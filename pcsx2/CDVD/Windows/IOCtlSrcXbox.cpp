// SPDX-FileCopyrightText: 2026 Skitzelles
// SPDX-FileCopyrightText: 2026 SternXD
// SPDX-License-Identifier: GPL-3.0+

#if defined(WINRT_XBOX)

#include "IOCtlSrcXbox.h"

#include "common/BitUtils.h"
#include "common/ByteSwap.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/ScopedGuard.h"
#include "common/Threading.h"
#include "pcsx2-uwp/src/UWPUtils.h"

#include <winioctl.h>
#include <ntddcdvd.h>
#include <ntddcdrm.h>
#include <ntddscsi.h>
#include <roapi.h>

#include <winrt/Windows.Devices.Custom.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

// clang-format off
namespace Custom 	  = winrt::Windows::Devices::Custom;
namespace Enumeration = winrt::Windows::Devices::Enumeration;
namespace Foundation  = winrt::Windows::Foundation;
namespace Streams 	  = winrt::Windows::Storage::Streams;
// clang-format on

// CD-ROM device interface GUID.
static constexpr const wchar_t* DRIVE_SELECTOR = L"System.Devices.InterfaceClassGuid:=\"{53f56308-b6bf-11d0-94f2-00a0c91efb8b}\"";
static constexpr u32 SENSE_SIZE = 32;
static constexpr u32 COMMAND_TIMEOUT = 20;
static constexpr u32 SPT_HEADER_SIZE = sizeof(SCSI_PASS_THROUGH) + SENSE_SIZE;
static constexpr u32 MAX_TRANSFER_SIZE = 64 * 1024;
static constexpr u32 DATA_SECTOR_SIZE = 2048;
static constexpr u32 RAW_SECTOR_SIZE = 2352;

static HRESULT FindDrive(winrt::hstring* device_id)
{
	const auto factory = winrt::try_get_activation_factory<Enumeration::DeviceInformation, Enumeration::IDeviceInformationStatics>();
	if (!factory)
		return E_NOTIMPL;

	Foundation::IAsyncOperation<Enumeration::DeviceInformationCollection> operation{nullptr};
	HRESULT hr = UWP::GetABI(factory)->FindAllAsyncAqsFilter(winrt::get_abi(winrt::hstring(DRIVE_SELECTOR)), winrt::put_abi(operation));
	Enumeration::DeviceInformationCollection devices{nullptr};
	if (SUCCEEDED(hr))
		hr = UWP::WaitForResult(operation, &devices);

	u32 count = 0;
	if (SUCCEEDED(hr))
		hr = UWP::GetABI(devices)->get_Size(&count);
	if (FAILED(hr) || count == 0)
		return hr;

	Enumeration::DeviceInformation device{nullptr};
	hr = UWP::GetABI(devices)->GetAt(0, winrt::put_abi(device));
	if (SUCCEEDED(hr))
		hr = UWP::GetABI(device)->get_Id(winrt::put_abi(*device_id));
	return hr;
}

bool XboxDiscDrive::HasDrive()
{
	// Enumerate on an MTA thread.
	HRESULT hr = E_FAIL;
	winrt::hstring device_id;
	Threading::Thread thread;
	if (!thread.Start([&]() {
			hr = RoInitialize(RO_INIT_MULTITHREADED);
			if (FAILED(hr))
				return;

			const ScopedGuard uninitialize([]() { RoUninitialize(); });
			hr = FindDrive(&device_id);
		}))
		return false;

	thread.Join();
	if (FAILED(hr))
	{
		Console.WarningFmt("CDVD: Drive enumeration failed: 0x{:08X}", static_cast<u32>(hr));
		return false;
	}
	return !device_id.empty();
}

struct XboxDrive
{
	Custom::CustomDevice device{nullptr};
	Custom::IOControlCode code{nullptr};
	Streams::Buffer input{nullptr};
	Streams::Buffer output{nullptr};
	std::mutex mutex;
	bool is_cd = false;
	u32 lead_out = 0;
	u64 position = 0;

	~XboxDrive()
	{
		const HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
		const ScopedGuard uninitialize([hr]() {
			if (SUCCEEDED(hr))
				RoUninitialize();
		});
		const auto closable = device.try_as<Foundation::IClosable>();
		if (closable)
			UWP::GetABI(closable)->Close();
	}

	static std::unique_ptr<XboxDrive> Open(Error* error);
	s32 SendCommand(const u8* cdb, u8 cdb_length, u8* data, u32 data_length,
		u8* sense = nullptr, u32 sense_size = 0, u32 timeout = COMMAND_TIMEOUT);
	u32 GetSectorCount();
	bool ReadRawSector(u32 sector, u8* buffer);
	bool ReadSectors2048(u32 sector, u32 count, u8* buffer);
	bool ReadDVDStructure(const DVD_READ_STRUCTURE& request, void* buffer, u32 size, DWORD* bytes_returned);
	bool ReadTOC(void* buffer, u32 size, DWORD* bytes_returned);
};

std::unique_ptr<XboxDrive> XboxDrive::Open(Error* error)
{
	const HRESULT apartment_hr = RoInitialize(RO_INIT_MULTITHREADED);
	if (FAILED(apartment_hr))
	{
		Error::SetHResult(error, "Failed to initialize WinRT for disc access", apartment_hr);
		return nullptr;
	}
	const ScopedGuard uninitialize([]() { RoUninitialize(); });

	winrt::hstring device_id;
	HRESULT hr = FindDrive(&device_id);
	if (SUCCEEDED(hr) && device_id.empty())
	{
		Error::SetString(error, "No optical drive found.");
		return nullptr;
	}

	auto drive = std::make_unique<XboxDrive>();
	const auto device_factory = winrt::try_get_activation_factory<Custom::CustomDevice, Custom::ICustomDeviceStatics>();
	const auto code_factory = winrt::try_get_activation_factory<Custom::IOControlCode, Custom::IIOControlCodeFactory>();
	const auto buffer_factory = winrt::try_get_activation_factory<Streams::Buffer, Streams::IBufferFactory>();
	if (SUCCEEDED(hr) && (!device_factory || !code_factory || !buffer_factory))
		hr = E_NOTIMPL;

	Foundation::IAsyncOperation<Custom::CustomDevice> operation{nullptr};
	if (SUCCEEDED(hr))
		hr = UWP::GetABI(device_factory)->FromIdAsync(winrt::get_abi(device_id), static_cast<s32>(Custom::DeviceAccessMode::ReadWrite), static_cast<s32>(Custom::DeviceSharingMode::Shared), winrt::put_abi(operation));
	if (SUCCEEDED(hr))
		hr = UWP::WaitForResult(operation, &drive->device);
	if (SUCCEEDED(hr) && !drive->device)
		hr = E_ACCESSDENIED;
	if (SUCCEEDED(hr))
		hr = UWP::GetABI(code_factory)->CreateIOControlCode(IOCTL_SCSI_BASE, 0x401, static_cast<s32>(Custom::IOControlAccessMode::ReadWrite), static_cast<s32>(Custom::IOControlBufferingMethod::Buffered), winrt::put_abi(drive->code));
	if (SUCCEEDED(hr))
		hr = UWP::GetABI(buffer_factory)->Create(SPT_HEADER_SIZE + MAX_TRANSFER_SIZE, winrt::put_abi(drive->input));
	if (SUCCEEDED(hr))
		hr = UWP::GetABI(buffer_factory)->Create(SPT_HEADER_SIZE + MAX_TRANSFER_SIZE, winrt::put_abi(drive->output));
	if (FAILED(hr))
	{
		Error::SetHResult(error, "Failed to open optical drive", hr);
		return nullptr;
	}
	return drive;
}

// Returns the number of bytes read, or -1 on failure.
s32 XboxDrive::SendCommand(const u8* cdb, u8 cdb_length, u8* data, u32 data_length,
	u8* sense, u32 sense_size, u32 timeout)
{
	if (cdb_length > sizeof(SCSI_PASS_THROUGH::Cdb) || data_length > MAX_TRANSFER_SIZE)
	{
		SetLastError(ERROR_INVALID_PARAMETER);
		return -1;
	}

	const HRESULT apartment_hr = RoInitialize(RO_INIT_MULTITHREADED);
	if (FAILED(apartment_hr))
	{
		SetLastError(ERROR_NOT_READY);
		return -1;
	}
	const ScopedGuard uninitialize([]() { RoUninitialize(); });

	SCSI_PASS_THROUGH spt{};
	spt.Length = sizeof(spt);
	spt.CdbLength = cdb_length;
	spt.SenseInfoLength = SENSE_SIZE;
	spt.DataIn = data ? SCSI_IOCTL_DATA_IN : SCSI_IOCTL_DATA_UNSPECIFIED;
	spt.DataTransferLength = data ? data_length : 0;
	spt.TimeOutValue = timeout;
	spt.DataBufferOffset = SPT_HEADER_SIZE;
	spt.SenseInfoOffset = sizeof(spt);
	std::memcpy(spt.Cdb, cdb, cdb_length);
	std::memset(input.data(), 0, SPT_HEADER_SIZE);
	std::memcpy(input.data(), &spt, sizeof(spt));

	HRESULT hr = UWP::GetABI(input)->put_Length(SPT_HEADER_SIZE + spt.DataTransferLength);
	if (SUCCEEDED(hr))
		hr = UWP::GetABI(output)->put_Length(0);
	Foundation::IAsyncOperation<u32> operation{nullptr};
	if (SUCCEEDED(hr))
		hr = UWP::GetABI(device)->SendIOControlAsync(winrt::get_abi(code), winrt::get_abi(input), winrt::get_abi(output), winrt::put_abi(operation));
	u32 received = 0;
	if (SUCCEEDED(hr))
		hr = UWP::WaitForResult(operation, &received);
	if (FAILED(hr) || received < sizeof(spt) || received > SPT_HEADER_SIZE + MAX_TRANSFER_SIZE)
	{
		SetLastError(FAILED(hr) && HRESULT_FACILITY(hr) == FACILITY_WIN32 ? HRESULT_CODE(hr) : ERROR_IO_DEVICE);
		return -1;
	}

	std::memcpy(&spt, output.data(), sizeof(spt));
	if (sense && spt.SenseInfoOffset >= sizeof(spt) && spt.SenseInfoOffset <= received)
	{
		const u32 length = std::min<u32>({sense_size, spt.SenseInfoLength, received - spt.SenseInfoOffset});
		std::memcpy(sense, output.data() + spt.SenseInfoOffset, length);
	}
	if (spt.ScsiStatus != 0)
	{
		SetLastError(ERROR_IO_DEVICE);
		return -1;
	}
	if (!data)
		return 0;
	if (received < SPT_HEADER_SIZE || spt.DataTransferLength > data_length || spt.DataTransferLength > received - SPT_HEADER_SIZE)
	{
		SetLastError(ERROR_READ_FAULT);
		return -1;
	}
	std::memcpy(data, output.data() + SPT_HEADER_SIZE, spt.DataTransferLength);
	return static_cast<s32>(spt.DataTransferLength);
}

u32 XboxDrive::GetSectorCount()
{
	// Read the disc capacity.
	const u8 cdb[10] = {0x25};
	u8 capacity[8];
	if (SendCommand(cdb, sizeof(cdb), capacity, sizeof(capacity)) != sizeof(capacity))
		return 0;
	return ByteSwap(GetBufferU32(capacity, 0)) + 1;
}

bool XboxDrive::ReadRawSector(u32 sector, u8* buffer)
{
	u8 cdb[12] = {};
	// READ CD command
	cdb[0] = 0xBE;
	// Don't care about sector type.
	cdb[1] = 0;
	cdb[2] = (sector >> 24) & 0xFF;
	cdb[3] = (sector >> 16) & 0xFF;
	cdb[4] = (sector >> 8) & 0xFF;
	cdb[5] = sector & 0xFF;
	// Number of sectors to read
	cdb[6] = 0;
	cdb[7] = 0;
	cdb[8] = 1;
	// Sync + all headers + user data + EDC/ECC. Excludes C2 + subchannel
	cdb[9] = 0xF8;
	return SendCommand(cdb, sizeof(cdb), buffer, RAW_SECTOR_SIZE) == RAW_SECTOR_SIZE;
}

static BOOL Fail(DWORD error)
{
	SetLastError(error);
	return FALSE;
}

static std::mutex s_drives_mutex;
static std::unordered_map<HANDLE, std::shared_ptr<XboxDrive>> s_drives;

static std::shared_ptr<XboxDrive> GetDrive(HANDLE handle)
{
	std::lock_guard<std::mutex> guard(s_drives_mutex);
	const auto it = s_drives.find(handle);
	if (it == s_drives.end())
	{
		SetLastError(ERROR_INVALID_HANDLE);
		return {};
	}
	return it->second;
}

bool XboxDrive::ReadSectors2048(u32 sector, u32 count, u8* buffer)
{
	if (is_cd)
	{
		// READ(10) cannot read Mode 2 data sectors.
		u8 raw[RAW_SECTOR_SIZE];
		for (u32 n = 0; n < count; ++n)
		{
			if (!ReadRawSector(sector + n, raw))
				return false;
			if (raw[15] != 1 && raw[15] != 2)
				return Fail(ERROR_UNRECOGNIZED_MEDIA);
			std::memcpy(buffer + DATA_SECTOR_SIZE * n, raw + (raw[15] == 2 ? 24 : 16), DATA_SECTOR_SIZE);
		}
		return true;
	}

	constexpr u32 per_command = (MAX_TRANSFER_SIZE - SPT_HEADER_SIZE) / DATA_SECTOR_SIZE;
	for (u32 done = 0; done < count;)
	{
		const u32 n = std::min(count - done, per_command);
		const u32 lba = sector + done;
		const u8 cdb[10] = {0x28, 0, static_cast<u8>(lba >> 24), static_cast<u8>(lba >> 16), static_cast<u8>(lba >> 8),
			static_cast<u8>(lba), 0, static_cast<u8>(n >> 8), static_cast<u8>(n), 0};
		const u32 bytes_to_read = n * DATA_SECTOR_SIZE;
		const s32 bytes_read = SendCommand(cdb, sizeof(cdb), buffer + DATA_SECTOR_SIZE * done, bytes_to_read);
		if (bytes_read < 0)
			return false;
		if (bytes_read != static_cast<s32>(bytes_to_read))
			return Fail(ERROR_READ_FAULT);
		done += n;
	}
	return true;
}

bool XboxDrive::ReadDVDStructure(const DVD_READ_STRUCTURE& request, void* buffer, u32 size, DWORD* bytes_returned)
{
	if (request.Format != DvdPhysicalDescriptor || request.LayerNumber > 1 || size < sizeof(DVD_DESCRIPTOR_HEADER) + sizeof(DVD_LAYER_DESCRIPTOR))
		return Fail(ERROR_INVALID_PARAMETER);

	std::array<u8, 36> info{};
	const u8 cdb[12] = {0xAD, 0, 0, 0, 0, 0, request.LayerNumber, 0, 0, static_cast<u8>(info.size()), 0, 0};
	const s32 bytes_read = SendCommand(cdb, sizeof(cdb), info.data(), info.size());
	if (bytes_read < 0)
		return false;
	if (bytes_read < static_cast<s32>(sizeof(DVD_DESCRIPTOR_HEADER) + sizeof(DVD_LAYER_DESCRIPTOR)))
		return Fail(ERROR_READ_FAULT);

	DVD_LAYER_DESCRIPTOR layer = GetBufferT<DVD_LAYER_DESCRIPTOR>(info.data(), sizeof(DVD_DESCRIPTOR_HEADER));
	const u32 start_sector = _byteswap_ulong(layer.StartingDataSector) & 0xFFFFFF;
	const u32 end_sector = _byteswap_ulong(layer.EndDataSector) & 0xFFFFFF;
	const u32 end_sector_layer0 = _byteswap_ulong(layer.EndLayerZeroSector) & 0xFFFFFF;
	if (end_sector < start_sector || (request.LayerNumber == 0 && layer.NumberOfLayers > 1))
		return Fail(ERROR_UNRECOGNIZED_MEDIA);
	if (request.LayerNumber == 0 && layer.NumberOfLayers == 1 && layer.TrackPath != 0 &&
		(end_sector_layer0 < start_sector || end_sector < (~end_sector_layer0 & 0xFFFFFFU)))
		return Fail(ERROR_UNRECOGNIZED_MEDIA);

	layer.StartingDataSector = _byteswap_ulong(start_sector);
	layer.EndDataSector = _byteswap_ulong(end_sector);
	layer.EndLayerZeroSector = _byteswap_ulong(end_sector_layer0);
	std::memcpy(info.data() + sizeof(DVD_DESCRIPTOR_HEADER), &layer, sizeof(layer));
	*bytes_returned = std::min<u32>(size, bytes_read);
	std::memcpy(buffer, info.data(), *bytes_returned);
	is_cd = false;
	return true;
}

bool XboxDrive::ReadTOC(void* buffer, u32 size, DWORD* bytes_returned)
{
	if (size < sizeof(CDROM_TOC))
		return Fail(ERROR_INSUFFICIENT_BUFFER);

	auto& toc = *static_cast<CDROM_TOC*>(buffer);
	toc = {};
	const u8 cdb[10] = {0x43, 0, 0, 0, 0, 0, 0, static_cast<u8>(sizeof(toc) >> 8), static_cast<u8>(sizeof(toc) & 0xFF), 0};
	const s32 bytes_read = SendCommand(cdb, sizeof(cdb), reinterpret_cast<u8*>(&toc), sizeof(toc));
	if (bytes_read < 0)
		return false;
	if (bytes_read < 4)
		return Fail(ERROR_READ_FAULT);

	// Ignore incomplete TOC descriptors.
	const s32 end = std::min<s32>(bytes_read, ((toc.Length[0] << 8) | toc.Length[1]) + 2);
	const u32 track_count = end > 4 ? (end - 4) / sizeof(TRACK_DATA) : 0;
	const u32 length = 2 + track_count * sizeof(TRACK_DATA);
	toc.Length[0] = length >> 8;
	toc.Length[1] = length & 0xFF;
	lead_out = 0;
	for (u32 n = 0; n < track_count; ++n)
	{
		TRACK_DATA& track = toc.TrackData[n];
		track.Address[0] = 0;
		if (track.TrackNumber == 0xAA)
			lead_out = ByteSwap(GetBufferU32(track.Address, 0));
	}
	*bytes_returned = length + 2;
	is_cd = true;
	return true;
}

HANDLE XboxDiscDrive::Open(Error* error)
{
	std::shared_ptr<XboxDrive> drive(XboxDrive::Open(error));
	if (!drive)
		return INVALID_HANDLE_VALUE;

	const HANDLE handle = reinterpret_cast<HANDLE>(drive.get());
	std::lock_guard<std::mutex> guard(s_drives_mutex);
	s_drives.emplace(handle, std::move(drive));
	return handle;
}

BOOL XboxDiscDrive::CloseHandle(HANDLE handle)
{
	// Pending operations keep the device alive after its handle is closed.
	std::shared_ptr<XboxDrive> drive;
	{
		std::lock_guard<std::mutex> guard(s_drives_mutex);
		const auto it = s_drives.find(handle);
		if (it == s_drives.end())
			return Fail(ERROR_INVALID_HANDLE);
		drive = std::move(it->second);
		s_drives.erase(it);
	}
	return TRUE;
}

BOOL XboxDiscDrive::SetFilePointerEx(HANDLE handle, LARGE_INTEGER distance, LARGE_INTEGER* position, DWORD method)
{
	const auto drive = GetDrive(handle);
	if (!drive)
		return FALSE;
	if (method != FILE_BEGIN || distance.QuadPart < 0 || distance.QuadPart % DATA_SECTOR_SIZE != 0)
		return Fail(ERROR_INVALID_PARAMETER);

	std::lock_guard<std::mutex> guard(drive->mutex);
	drive->position = distance.QuadPart;
	if (position)
		position->QuadPart = distance.QuadPart;
	return TRUE;
}

BOOL XboxDiscDrive::ReadFile(HANDLE handle, void* buffer, DWORD size, DWORD* bytes_read, OVERLAPPED* overlapped)
{
	if (!bytes_read)
		return Fail(ERROR_INVALID_PARAMETER);
	*bytes_read = 0;
	const auto drive = GetDrive(handle);
	if (!drive)
		return FALSE;
	if (overlapped || (!buffer && size) || size % DATA_SECTOR_SIZE != 0)
		return Fail(ERROR_INVALID_PARAMETER);

	std::lock_guard<std::mutex> guard(drive->mutex);
	const u64 sector = drive->position / DATA_SECTOR_SIZE;
	const u32 count = size / DATA_SECTOR_SIZE;
	if (sector > UINT32_MAX || count > UINT32_MAX - sector)
		return Fail(ERROR_INVALID_PARAMETER);
	if (!drive->ReadSectors2048(static_cast<u32>(sector), count, static_cast<u8*>(buffer)))
		return FALSE;
	*bytes_read = size;
	drive->position += size;
	return TRUE;
}

BOOL XboxDiscDrive::DeviceIoControl(HANDLE handle, DWORD code, void* input, DWORD input_size,
	void* output, DWORD output_size, DWORD* bytes_returned, OVERLAPPED* overlapped)
{
	if (!bytes_returned)
		return Fail(ERROR_INVALID_PARAMETER);
	*bytes_returned = 0;
	const auto drive = GetDrive(handle);
	if (!drive)
		return FALSE;
	if (overlapped)
		return Fail(ERROR_NOT_SUPPORTED);

	std::lock_guard<std::mutex> guard(drive->mutex);
	switch (code)
	{
		case FSCTL_ALLOW_EXTENDED_DASD_IO:
			return TRUE;

		case IOCTL_STORAGE_CHECK_VERIFY:
		{
			// Check whether the drive is ready.
			const u8 cdb[6] = {};
			if (drive->SendCommand(cdb, sizeof(cdb), nullptr, 0) == 0)
				return TRUE;
			drive->lead_out = 0;
			return FALSE;
		}

		case IOCTL_DVD_READ_STRUCTURE:
			if (!input || input_size < sizeof(DVD_READ_STRUCTURE) || !output)
				return Fail(ERROR_INVALID_PARAMETER);
			return drive->ReadDVDStructure(*static_cast<const DVD_READ_STRUCTURE*>(input), output, output_size, bytes_returned);

		case IOCTL_CDROM_SET_SPEED:
		{
			if (!input || input_size < sizeof(CDROM_SET_SPEED))
				return Fail(ERROR_INVALID_PARAMETER);
			const auto& request = *static_cast<const CDROM_SET_SPEED*>(input);
			if (request.RequestType != CdromSetSpeed)
				return Fail(ERROR_NOT_SUPPORTED);
			if (request.RotationControl != CdromDefaultRotation && request.RotationControl != CdromCAVRotation)
				return Fail(ERROR_INVALID_PARAMETER);

			// Set the drive speed.
			const u8 cdb[12] = {0xBB, static_cast<u8>(request.RotationControl),
				static_cast<u8>(request.ReadSpeed >> 8), static_cast<u8>(request.ReadSpeed),
				static_cast<u8>(request.WriteSpeed >> 8), static_cast<u8>(request.WriteSpeed), 0, 0, 0, 0, 0, 0};
			return drive->SendCommand(cdb, sizeof(cdb), nullptr, 0) == 0;
		}

		case IOCTL_CDROM_READ_TOC_EX:
		{
			if (!input || input_size < sizeof(CDROM_READ_TOC_EX) || !output)
				return Fail(ERROR_INVALID_PARAMETER);
			const auto& request = *static_cast<const CDROM_READ_TOC_EX*>(input);
			if (request.Format != CDROM_READ_TOC_EX_FORMAT_TOC || request.Msf)
				return Fail(ERROR_NOT_SUPPORTED);
			return drive->ReadTOC(output, output_size, bytes_returned);
		}

		case IOCTL_CDROM_READ_Q_CHANNEL:
		{
			if (!input || input_size < sizeof(CDROM_SUB_Q_DATA_FORMAT) || !output)
				return Fail(ERROR_INVALID_PARAMETER);
			const auto& request = *static_cast<const CDROM_SUB_Q_DATA_FORMAT*>(input);
			if (request.Format != IOCTL_CDROM_CURRENT_POSITION)
				return Fail(ERROR_NOT_SUPPORTED);
			if (output_size < sizeof(SUB_Q_CURRENT_POSITION))
				return Fail(ERROR_INSUFFICIENT_BUFFER);

			SUB_Q_CURRENT_POSITION position{};
			// Read the current subchannel position in MSF format.
			const u8 cdb[10] = {0x42, 0x02, 0x40, IOCTL_CDROM_CURRENT_POSITION, 0, 0, 0, 0, static_cast<u8>(sizeof(position)), 0};
			const s32 bytes_read = drive->SendCommand(cdb, sizeof(cdb), reinterpret_cast<u8*>(&position), sizeof(position));
			if (bytes_read < 0)
				return FALSE;
			if (bytes_read != static_cast<s32>(sizeof(position)))
				return Fail(ERROR_READ_FAULT);
			const u32 data_length = (position.Header.DataLength[0] << 8) | position.Header.DataLength[1];
			if (data_length != sizeof(position) - sizeof(position.Header) || position.FormatCode != IOCTL_CDROM_CURRENT_POSITION)
				return Fail(ERROR_INVALID_DATA);

			std::memcpy(output, &position, sizeof(position));
			*bytes_returned = sizeof(position);
			return TRUE;
		}

		case IOCTL_DISK_GET_LENGTH_INFO:
		{
			if (!output || output_size < sizeof(GET_LENGTH_INFORMATION))
				return Fail(ERROR_INVALID_PARAMETER);
			u32 sectors = drive->GetSectorCount();
			if (!sectors)
				sectors = drive->lead_out;
			if (!sectors)
				return Fail(ERROR_NOT_READY);
			static_cast<GET_LENGTH_INFORMATION*>(output)->Length.QuadPart = static_cast<u64>(sectors) * DATA_SECTOR_SIZE;
			*bytes_returned = sizeof(GET_LENGTH_INFORMATION);
			return TRUE;
		}

		case IOCTL_SCSI_PASS_THROUGH_DIRECT:
		{
			if (!input || input_size < sizeof(SCSI_PASS_THROUGH_DIRECT) || !output || output_size < sizeof(SCSI_PASS_THROUGH_DIRECT))
				return Fail(ERROR_INVALID_PARAMETER);
			auto spt = GetBufferT<SCSI_PASS_THROUGH_DIRECT>(static_cast<const u8*>(input), 0);
			if (spt.Length != sizeof(spt) || spt.CdbLength == 0 || spt.CdbLength > sizeof(spt.Cdb) ||
				(spt.DataIn != SCSI_IOCTL_DATA_IN && spt.DataIn != SCSI_IOCTL_DATA_UNSPECIFIED) ||
				(spt.DataTransferLength && (!spt.DataBuffer || spt.DataIn != SCSI_IOCTL_DATA_IN)))
				return Fail(ERROR_INVALID_PARAMETER);
			if (spt.SenseInfoLength && (spt.SenseInfoOffset < sizeof(spt) || spt.SenseInfoOffset > output_size ||
										   spt.SenseInfoLength > output_size - spt.SenseInfoOffset))
				return Fail(ERROR_INVALID_PARAMETER);

			std::array<u8, SENSE_SIZE> sense{};
			const u32 sense_size = std::min<u32>(spt.SenseInfoLength, sense.size());
			const s32 bytes_read = drive->SendCommand(spt.Cdb, spt.CdbLength, static_cast<u8*>(spt.DataBuffer),
				spt.DataTransferLength, sense.data(), sense_size, spt.TimeOutValue);
			spt.DataTransferLength = bytes_read < 0 ? 0 : bytes_read;
			spt.ScsiStatus = bytes_read < 0 ? 2 : 0;
			spt.SenseInfoLength = sense_size;
			std::memcpy(output, &spt, sizeof(spt));
			if (sense_size)
				std::memcpy(static_cast<u8*>(output) + spt.SenseInfoOffset, sense.data(), sense_size);
			*bytes_returned = sense_size ? std::max<u32>(sizeof(spt), spt.SenseInfoOffset + sense_size) : sizeof(spt);
			return bytes_read >= 0;
		}

		default:
			return Fail(ERROR_NOT_SUPPORTED);
	}
}

#endif // WINRT_XBOX
