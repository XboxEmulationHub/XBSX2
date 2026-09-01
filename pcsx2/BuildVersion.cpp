// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "svnrev.h"

namespace BuildVersion
{
	// XBSX2 app version.
	const char* AppVersion = "2.0.9.0";

	// Upstream PCSX2 version this build is based on.
	const char* Pcsx2BaseVersion = "2.8.1";

	const char* GitTag = GIT_TAG;
	bool GitTaggedCommit = GIT_TAGGED_COMMIT;
	int GitTagHi = GIT_TAG_HI;
	int GitTagMid = GIT_TAG_MID;
	int GitTagLo = GIT_TAG_LO;
	const char* GitRev = GIT_REV;
	const char* GitHash = GIT_HASH;
	const char* GitDate = GIT_DATE;
} // namespace BuildVersion
