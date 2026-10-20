// SPDX-FileCopyrightText: 2026 Skitzelles
// SPDX-License-Identifier: GPL-3.0+

// Reading the disc in an Xbox console's own drive (Dev Mode, UWP).
//
// An app on the console is refused every file and volume route to the drive (CreateFile on \\.\X:, the
// IOCTL_CDROM_* and IOCTL_DVD_* requests). What does open is the drive's device interface through
// Windows.Devices.Custom, once the package declares the CD-ROM interface class
// ({53f56308-b6bf-11d0-94f2-00a0c91efb8b}) as a DeviceCapability; opened for reading and writing it takes
// IOCTL_SCSI_PASS_THROUGH, so everything here is plain SCSI / MMC commands: READ(10), READ CD, READ TOC,
// READ DVD STRUCTURE, READ CAPACITY, TEST UNIT READY.
//
// Included by IOCtlSrc.cpp when WINRT_XBOX is set; it replaces the desktop implementation.

#include <winrt/Windows.Devices.Custom.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <cstring>

namespace
{
	using namespace winrt::Windows::Devices::Custom;
	using namespace winrt::Windows::Devices::Enumeration;
	using winrt::Windows::Storage::Streams::Buffer;

	// The selector for the CD-ROM device interface class.
	constexpr const wchar_t* XBOX_DRIVE_SELECTOR = L"System.Devices.InterfaceClassGuid:=\"{53f56308-b6bf-11d0-94f2-00a0c91efb8b}\"";

	constexpr u32 XBOX_SPT_HEADER = sizeof(SCSI_PASS_THROUGH) + 32; // the structure, then room for sense data
	constexpr u32 XBOX_MAX_DATA = 64 * 1024; // one command moves at most this much

	struct XboxDrive
	{
		CustomDevice device{nullptr};
		IOControlCode code{0x04 /* IOCTL_SCSI_BASE */, 0x401, IOControlAccessMode::ReadWrite, IOControlBufferingMethod::Buffered};
		Buffer in{XBOX_SPT_HEADER + XBOX_MAX_DATA};
		Buffer out{XBOX_SPT_HEADER + XBOX_MAX_DATA};
		bool is_cd = false;
	};

	// WinRT calls need the calling thread to be in an apartment; the emulator's own threads are not by default.
	void XboxEnsureApartment()
	{
		static thread_local bool done = false;
		if (done)
			return;
		done = true;
		try
		{
			winrt::init_apartment();
		}
		catch (...)
		{
		}
	}

	XboxDrive* XboxOpenDrive(Error* error)
	{
		XboxEnsureApartment();
		try
		{
			const auto found = DeviceInformation::FindAllAsync(XBOX_DRIVE_SELECTOR).get();
			if (found.Size() == 0)
			{
				Error::SetString(error, "This console has no disc drive.");
				return nullptr;
			}
			auto drive = std::make_unique<XboxDrive>();
			drive->device = CustomDevice::FromIdAsync(found.GetAt(0).Id(), DeviceAccessMode::ReadWrite, DeviceSharingMode::Shared).get();
			if (!drive->device)
			{
				Error::SetString(error, "The disc drive did not open.");
				return nullptr;
			}
			return drive.release();
		}
		catch (winrt::hresult_error const& e)
		{
			Error::SetString(error, fmt::format("The disc drive did not open (error {:08x}).", static_cast<u32>(e.code().value)));
			return nullptr;
		}
	}

	// Sends one command. data_len bytes are read from the drive into data (none when data is null).
	// Returns the number of bytes that came back, or -1 when the drive refused the command.
	s32 XboxScsi(XboxDrive* d, const u8* cdb, u8 cdb_len, u8* data, u32 data_len, u32 timeout_seconds = 20)
	{
		if (!d || !d->device || data_len > XBOX_MAX_DATA)
			return -1;
		XboxEnsureApartment();

		SCSI_PASS_THROUGH spt{};
		spt.Length = sizeof(SCSI_PASS_THROUGH);
		spt.CdbLength = cdb_len;
		spt.SenseInfoLength = 32;
		spt.DataIn = data ? SCSI_IOCTL_DATA_IN : SCSI_IOCTL_DATA_UNSPECIFIED;
		spt.DataTransferLength = data ? data_len : 0;
		spt.TimeOutValue = timeout_seconds;
		spt.DataBufferOffset = XBOX_SPT_HEADER;
		spt.SenseInfoOffset = sizeof(SCSI_PASS_THROUGH);
		std::memcpy(spt.Cdb, cdb, cdb_len);

		u8* p = d->in.data();
		std::memset(p, 0, XBOX_SPT_HEADER);
		std::memcpy(p, &spt, sizeof(spt));
		d->in.Length(XBOX_SPT_HEADER + spt.DataTransferLength);
		try
		{
			const u32 n = d->device.SendIOControlAsync(d->code, d->in, d->out).get();
			if (n < sizeof(SCSI_PASS_THROUGH))
				return -1;
			std::memcpy(&spt, d->out.data(), sizeof(spt));
			if (spt.ScsiStatus != 0)
				return -1;
			if (!data)
				return 0;
			const u32 moved = std::min({static_cast<u32>(spt.DataTransferLength), data_len, n > XBOX_SPT_HEADER ? n - XBOX_SPT_HEADER : 0u});
			std::memcpy(data, d->out.data() + XBOX_SPT_HEADER, moved);
			return static_cast<s32>(moved);
		}
		catch (winrt::hresult_error const&)
		{
			return -1;
		}
	}

	constexpr u32 Be32(const u8* p)
	{
		return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) | (static_cast<u32>(p[2]) << 8) | p[3];
	}

	// READ CAPACITY: the number of 2048-byte sectors on the disc, 0 when there is none.
	u32 XboxSectorCount(XboxDrive* d)
	{
		const u8 cdb[10] = {0x25};
		u8 cap[8];
		if (XboxScsi(d, cdb, 10, cap, 8) != 8)
			return 0;
		return Be32(cap) + 1;
	}

	bool XboxReadRaw(XboxDrive* d, u32 sector, u32 count, u8* buffer)
	{
		const u8 cdb[12] = {0xBE, 0, static_cast<u8>(sector >> 24), static_cast<u8>(sector >> 16), static_cast<u8>(sector >> 8),
			static_cast<u8>(sector), 0, 0, static_cast<u8>(count), 0xF8, 0, 0}; // sync, headers, data, EDC/ECC
		return XboxScsi(d, cdb, 12, buffer, count * 2352) == static_cast<s32>(count * 2352);
	}
} // namespace

#define XBOX_DRIVE static_cast<XboxDrive*>(m_xbox)

IOCtlSrc::IOCtlSrc(std::string filename)
	: m_filename(std::move(filename))
{
}

IOCtlSrc::~IOCtlSrc()
{
	delete XBOX_DRIVE;
}

bool IOCtlSrc::Reopen(Error* error)
{
	std::lock_guard<std::mutex> guard(m_lock);
	delete XBOX_DRIVE;
	m_xbox = XboxOpenDrive(error);
	if (!m_xbox)
		return false;

	m_sectors = 0;
	m_layer_break = 0;
	m_media_type = 0;
	m_toc.clear();

	// A CD answers READ CD; a DVD refuses it at once.
	u8 probe[2352];
	XBOX_DRIVE->is_cd = XboxReadRaw(XBOX_DRIVE, 16, 1, probe);
	if (!(XBOX_DRIVE->is_cd ? ReadCDInfo() : ReadDVDInfo()))
		Console.Warning("CDVD: no readable disc in the console's drive");
	else
	{
		const char* kind = m_media_type < 0 ? "CD" : (m_media_type == 0 ? "DVD" : "DVD, two layers");
		Console.WriteLn(fmt::format("CDVD: disc in the console's drive: {}, {} sectors, layer break {}", kind, m_sectors, m_layer_break));
	}
	return true;
}

void IOCtlSrc::SetSpindleSpeed(bool restore_defaults) const
{
	// The console's drive refuses SET CD SPEED and SET STREAMING; it picks its own speed.
}

u32 IOCtlSrc::GetSectorCount() const
{
	return m_sectors;
}

u32 IOCtlSrc::GetLayerBreakAddress() const
{
	return m_layer_break;
}

s32 IOCtlSrc::GetMediaType() const
{
	return m_media_type;
}

const std::vector<toc_entry>& IOCtlSrc::ReadTOC() const
{
	return m_toc;
}

bool IOCtlSrc::ReadSectors2048(u32 sector, u32 count, u8* buffer) const
{
	std::lock_guard<std::mutex> guard(m_lock);
	if (!m_xbox)
		return false;

	if (XBOX_DRIVE->is_cd)
	{
		// A PlayStation CD has mode 2 sectors, which READ(10) refuses: read them raw and take the data out
		// (after 16 bytes in a mode 1 sector, after 24 in mode 2).
		u8 raw[2352];
		for (u32 n = 0; n < count; n++)
		{
			if (!XboxReadRaw(XBOX_DRIVE, sector + n, 1, raw))
			{
				Console.Error(fmt::format(" * CDVD: reading CD sector {} failed", sector + n));
				return false;
			}
			std::memcpy(buffer + 2048 * n, raw + (raw[15] == 2 ? 24 : 16), 2048);
		}
		return true;
	}

	constexpr u32 per_command = 31; // 62 KB
	for (u32 done = 0; done < count;)
	{
		const u32 n = std::min(count - done, per_command);
		const u32 lba = sector + done;
		const u8 cdb[10] = {0x28, 0, static_cast<u8>(lba >> 24), static_cast<u8>(lba >> 16), static_cast<u8>(lba >> 8),
			static_cast<u8>(lba), 0, static_cast<u8>(n >> 8), static_cast<u8>(n), 0};
		s32 got = -1;
		for (int attempt = 0; attempt < 3 && got != static_cast<s32>(n * 2048); attempt++)
			got = XboxScsi(XBOX_DRIVE, cdb, 10, buffer + 2048 * done, n * 2048);
		if (got != static_cast<s32>(n * 2048))
		{
			Console.Error(fmt::format(" * CDVD: reading sectors {}-{} failed", lba, lba + n - 1));
			return false;
		}
		done += n;
	}
	return true;
}

bool IOCtlSrc::ReadSectors2352(u32 sector, u32 count, u8* buffer) const
{
	std::lock_guard<std::mutex> guard(m_lock);
	if (!m_xbox)
		return false;

	// Several sectors in one command are quicker; a read that crosses into a track of another type fails,
	// and then they are read one by one.
	constexpr u32 per_command = 27; // 63.5 KB
	for (u32 done = 0; done < count;)
	{
		const u32 n = std::min(count - done, per_command);
		if (XboxReadRaw(XBOX_DRIVE, sector + done, n, buffer + 2352 * done))
		{
			done += n;
			continue;
		}
		for (u32 i = 0; i < n; i++)
		{
			if (!XboxReadRaw(XBOX_DRIVE, sector + done + i, 1, buffer + 2352 * (done + i)))
			{
				Console.Error(fmt::format(" * CDVD: reading raw sector {} failed", sector + done + i));
				return false;
			}
		}
		done += n;
	}
	return true;
}

bool IOCtlSrc::ReadDVDInfo()
{
	// READ DVD STRUCTURE, format 0 (physical format information) of layer 0: a 4-byte header, then the
	// layer descriptor.
	u8 info[36] = {};
	const u8 cdb[12] = {0xAD, 0, 0, 0, 0, 0, 0, 0x00, 0, sizeof(info), 0, 0};
	if (XboxScsi(XBOX_DRIVE, cdb, 12, info, sizeof(info)) >= 20)
	{
		const u8* layer = info + 4;
		const u32 layers = (layer[2] >> 5) & 3, track_path = (layer[2] >> 4) & 1;
		const u32 start_sector = Be32(layer + 4), end_sector = Be32(layer + 8), end_sector_layer0 = Be32(layer + 12);
		if (end_sector > start_sector)
		{
			if (layers == 0)
			{
				m_media_type = 0;
				m_layer_break = 0;
				m_sectors = end_sector - start_sector + 1;
			}
			else if (track_path == 0)
			{
				// Two layers, parallel track path: the second layer has its own descriptor
				u8 info1[36] = {};
				const u8 cdb1[12] = {0xAD, 0, 0, 0, 0, 0, 1, 0x00, 0, sizeof(info1), 0, 0};
				if (XboxScsi(XBOX_DRIVE, cdb1, 12, info1, sizeof(info1)) < 20)
					return false;
				const u32 layer1_start_sector = Be32(info1 + 4 + 4), layer1_end_sector = Be32(info1 + 4 + 8);
				m_media_type = 1;
				m_layer_break = end_sector - start_sector;
				m_sectors = end_sector - start_sector + 1 + layer1_end_sector - layer1_start_sector + 1;
			}
			else
			{
				// Two layers, opposite track path
				m_media_type = 2;
				m_layer_break = end_sector_layer0 - start_sector;
				m_sectors = end_sector_layer0 - start_sector + 1 + end_sector - (~end_sector_layer0 & 0xFFFFFFU) + 1;
			}
			return true;
		}
	}

	// The drive did not hand out the structure: go by the size it reports and take the disc as one layer.
	m_sectors = XboxSectorCount(XBOX_DRIVE);
	m_media_type = 0;
	m_layer_break = 0;
	if (m_sectors)
		Console.Warning("CDVD: READ DVD STRUCTURE refused, taking the disc as a single-layer DVD");
	return m_sectors != 0;
}

bool IOCtlSrc::ReadCDInfo()
{
	// READ TOC, format 0, addresses as sector numbers: a 4-byte header, then 8 bytes for every track.
	u8 toc[804] = {};
	const u8 cdb[10] = {0x43, 0, 0, 0, 0, 0, 0, static_cast<u8>(sizeof(toc) >> 8), static_cast<u8>(sizeof(toc) & 0xff), 0};
	const s32 got = XboxScsi(XBOX_DRIVE, cdb, 10, toc, sizeof(toc));
	if (got < 4)
		return false;

	m_toc.clear();
	u32 lead_out = 0;
	const s32 end = std::min<s32>(got, ((toc[0] << 8) | toc[1]) + 2);
	for (s32 o = 4; o + 8 <= end; o += 8)
	{
		const u32 lba = Be32(toc + o + 4);
		if (toc[o + 2] == 0xAA)
		{
			lead_out = lba; // the lead-out is not a track
			continue;
		}
		m_toc.push_back({lba, toc[o + 2], static_cast<u8>(toc[o + 1] >> 4), static_cast<u8>(toc[o + 1] & 15)});
	}

	m_sectors = XboxSectorCount(XBOX_DRIVE);
	if (!m_sectors)
		m_sectors = lead_out;
	m_media_type = -1;
	return m_sectors != 0;
}

bool IOCtlSrc::ReadTrackSubQ(cdvdSubQ* subQ) const
{
	// The console's drive refuses subchannel requests. A data disc sits in its first track.
	subQ->adr = 1;
	subQ->trackNum = 1;
	subQ->trackIndex = 1;
	return true;
}

bool IOCtlSrc::DiscReady()
{
	bool ready;
	{
		std::lock_guard<std::mutex> guard(m_lock);
		if (!m_xbox)
			return false;
		const u8 test_unit_ready[6] = {};
		ready = XboxScsi(XBOX_DRIVE, test_unit_ready, 6, nullptr, 0) == 0;
	}

	if (ready)
	{
		if (!m_sectors)
			Reopen(nullptr);
	}
	else
	{
		m_sectors = 0;
		m_layer_break = 0;
		m_media_type = 0;
	}

	return !!m_sectors;
}

#undef XBOX_DRIVE
