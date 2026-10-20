// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "CDVD/CDVDdiscReader.h"

#if defined(WINRT_XBOX)
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <thread>

// The console's own drive is opened by its device interface, not by a drive letter (see IOCtlSrcXbox.inl).
// Series S has no drive, so nothing is listed there.
static constexpr const wchar_t* s_drive_selector = L"System.Devices.InterfaceClassGuid:=\"{53f56308-b6bf-11d0-94f2-00a0c91efb8b}\"";

static bool XboxHasDiscDrive()
{
	// Asked once, on a thread of its own: a blocking WinRT call is not allowed on the UI thread.
	static const bool has_drive = []() {
		bool found = false;
		std::thread([&found]() {
			try
			{
				winrt::init_apartment();
				using winrt::Windows::Devices::Enumeration::DeviceInformation;
				const auto drives = DeviceInformation::FindAllAsync(s_drive_selector).get();
				found = drives.Size() > 0;
			}
			catch (...)
			{
			}
		}).join();
		return found;
	}();
	return has_drive;
}
#endif

std::vector<std::string> GetOpticalDriveList()
{
#if defined(WINRT_XBOX)
	if (XboxHasDiscDrive())
		return {"disc:"};
	return {};
#else
	DWORD size = GetLogicalDriveStringsA(0, nullptr);
	std::vector<char> drive_strings(size);
	if (GetLogicalDriveStringsA(size, drive_strings.data()) != size - 1)
		return {};

	std::vector<std::string> drives;
	for (auto p = drive_strings.data(); *p; ++p)
	{
		if (GetDriveTypeA(p) == DRIVE_CDROM)
			drives.push_back(p);
		while (*p)
			++p;
	}
	return drives;
#endif
}

void GetValidDrive(std::string& drive)
{
#if defined(WINRT_XBOX)
	if (XboxHasDiscDrive())
		drive = "disc:";
	else
		drive.clear();
	return;
#endif
	if (drive.empty() || GetDriveTypeA(drive.c_str()) != DRIVE_CDROM)
	{
		auto drives = GetOpticalDriveList();
		if (drives.empty())
		{
			drive.clear();
			return;
		}
		drive = drives.front();
	}

	printf(" * CDVD: Opening drive '%s'...\n", drive.data());

	// The drive string has the form "X:\", but to open the drive, the string
	// has to be in the form "\\.\X:"
	drive.pop_back();
	drive.insert(0, "\\\\.\\");
}
