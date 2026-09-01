// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// This file provides the same information as svnrev.h except you don't need to
// recompile each object file using it when said information is updated.
namespace BuildVersion
{
	// XBSX2 app version.
	extern const char* AppVersion;

	// Upstream PCSX2 version this build is based on.
	extern const char* Pcsx2BaseVersion;

	extern const char* GitTag;
	extern bool GitTaggedCommit;
	extern int GitTagHi;
	extern int GitTagMid;
	extern int GitTagLo;
	extern const char* GitRev;
	extern const char* GitHash;
	extern const char* GitDate;
} // namespace BuildVersion
