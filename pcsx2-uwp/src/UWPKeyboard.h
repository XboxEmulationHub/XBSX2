// SPDX-CopyrightText: 2026 SternXD
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "common/Pcsx2Types.h"

namespace UWPKeyboard
{
	std::optional<u32> NameToCode(std::string_view name);
	std::optional<std::string> CodeToName(u32 code);
	const char* CodeToIcon(u32 code);
} // namespace UWPKeyboard
