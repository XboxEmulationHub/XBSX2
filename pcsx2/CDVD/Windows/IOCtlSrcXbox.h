// SPDX-FileCopyrightText: 2026 Skitzelles
// SPDX-FileCopyrightText: 2026 SternXD
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#if defined(WINRT_XBOX)
#include "common/RedtapeWindows.h"
#include "common/Pcsx2Types.h"

class Error;
#endif

namespace XboxDiscDrive
{
	inline constexpr const char* DEVICE_PATH = "disc:";
	bool HasDrive();

#if defined(WINRT_XBOX)
	HANDLE Open(Error* error);
	// Handles returned by Open() must be released through this adapter.
	BOOL CloseHandle(HANDLE handle);
	BOOL SetFilePointerEx(HANDLE handle, LARGE_INTEGER distance, LARGE_INTEGER* position, DWORD method);
	BOOL ReadFile(HANDLE handle, void* buffer, DWORD size, DWORD* bytes_read, OVERLAPPED* overlapped);
	BOOL DeviceIoControl(HANDLE handle, DWORD code, void* input, DWORD input_size,
		void* output, DWORD output_size, DWORD* bytes_returned, OVERLAPPED* overlapped);
#endif
} // namespace XboxDiscDrive
