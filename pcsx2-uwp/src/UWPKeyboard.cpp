// SPDX-CopyrightText: 2026 SternXD
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/PrecompiledHeader.h"

#include <windows.h>

#include "IconsPromptFont.h"
#include "UWPKeyboard.h"

namespace UWPKeyboard
{
	struct KeyNameCode
	{
		const char* name;
		u32 code;
	};

	struct KeyCodeIcon
	{
		u32 code;
		const char* icon;
	};

	static constexpr KeyNameCode s_special_keys[] = {
		{"Left", VK_LEFT},
		{"Right", VK_RIGHT},
		{"Up", VK_UP},
		{"Down", VK_DOWN},
		{"PageUp", VK_PRIOR},
		{"PageDown", VK_NEXT},
		{"Home", VK_HOME},
		{"End", VK_END},
		{"Insert", VK_INSERT},
		{"Delete", VK_DELETE},
		{"Backspace", VK_BACK},
		{"Space", VK_SPACE},
		{"Return", VK_RETURN},
		{"Escape", VK_ESCAPE},
		{"LeftCtrl", VK_LCONTROL},
		{"Ctrl", VK_CONTROL},
		{"LeftShift", VK_LSHIFT},
		{"Shift", VK_SHIFT},
		{"LeftAlt", VK_LMENU},
		{"Alt", VK_MENU},
		{"LeftSuper", VK_LWIN},
		{"RightCtrl", VK_RCONTROL},
		{"RightShift", VK_RSHIFT},
		{"RightAlt", VK_RMENU},
		{"RightSuper", VK_RWIN},
		{"Menu", VK_APPS},
		{"F1", VK_F1},
		{"F2", VK_F2},
		{"F3", VK_F3},
		{"F4", VK_F4},
		{"F5", VK_F5},
		{"F6", VK_F6},
		{"F7", VK_F7},
		{"F8", VK_F8},
		{"F9", VK_F9},
		{"F10", VK_F10},
		{"F11", VK_F11},
		{"F12", VK_F12},
		{"Apostrophe", VK_OEM_7},
		{"Comma", VK_OEM_COMMA},
		{"Minus", VK_OEM_MINUS},
		{"Period", VK_OEM_PERIOD},
		{"Slash", VK_OEM_2},
		{"Semicolon", VK_OEM_1},
		{"Equal", VK_OEM_PLUS},
		{"BracketLeft", VK_OEM_4},
		{"Backslash", VK_OEM_5},
		{"BracketRight", VK_OEM_6},
		{"QuoteLeft", VK_OEM_3},
		{"CapsLock", VK_CAPITAL},
		{"ScrollLock", VK_SCROLL},
		{"NumLock", VK_NUMLOCK},
		{"PrintScreen", VK_SNAPSHOT},
		{"Pause", VK_PAUSE},
		{"Keypad0", VK_NUMPAD0},
		{"Keypad1", VK_NUMPAD1},
		{"Keypad2", VK_NUMPAD2},
		{"Keypad3", VK_NUMPAD3},
		{"Keypad4", VK_NUMPAD4},
		{"Keypad5", VK_NUMPAD5},
		{"Keypad6", VK_NUMPAD6},
		{"Keypad7", VK_NUMPAD7},
		{"Keypad8", VK_NUMPAD8},
		{"Keypad9", VK_NUMPAD9},
		{"KeypadPeriod", VK_DECIMAL},
		{"KeypadDivide", VK_DIVIDE},
		{"KeypadMultiply", VK_MULTIPLY},
		{"KeypadMinus", VK_SUBTRACT},
		{"KeypadPlus", VK_ADD},
		{"KeypadReturn", VK_RETURN},
	};

	static constexpr KeyCodeIcon s_key_icons[] = {
		{VK_ESCAPE, ICON_PF_ESC},
		{VK_TAB, ICON_PF_TAB},
		{VK_BACK, ICON_PF_BACKSPACE},
		{VK_RETURN, ICON_PF_ENTER},
		{VK_INSERT, ICON_PF_INSERT},
		{VK_DELETE, ICON_PF_DELETE},
		{VK_PAUSE, ICON_PF_PAUSE},
		{VK_SNAPSHOT, ICON_PF_PRTSC},
		{VK_HOME, ICON_PF_HOME},
		{VK_END, ICON_PF_END},
		{VK_LEFT, ICON_PF_ARROW_LEFT},
		{VK_UP, ICON_PF_ARROW_UP},
		{VK_RIGHT, ICON_PF_ARROW_RIGHT},
		{VK_DOWN, ICON_PF_ARROW_DOWN},
		{VK_PRIOR, ICON_PF_PAGE_UP},
		{VK_NEXT, ICON_PF_PAGE_DOWN},
		{VK_SHIFT, ICON_PF_SHIFT},
		{VK_LSHIFT, ICON_PF_SHIFT},
		{VK_RSHIFT, ICON_PF_SHIFT},
		{VK_CONTROL, ICON_PF_CTRL},
		{VK_LCONTROL, ICON_PF_CTRL},
		{VK_RCONTROL, ICON_PF_CTRL},
		{VK_MENU, ICON_PF_ALT},
		{VK_LMENU, ICON_PF_ALT},
		{VK_RMENU, ICON_PF_ALT},
		{VK_LWIN, ICON_PF_SUPER},
		{VK_RWIN, ICON_PF_SUPER},
		{VK_CAPITAL, ICON_PF_CAPS},
		{VK_NUMLOCK, ICON_PF_NUMLOCK},
		{VK_SCROLL, ICON_PF_SCRLK},
		{VK_SPACE, ICON_PF_SPACE},
		{VK_F1, ICON_PF_F1},
		{VK_F2, ICON_PF_F2},
		{VK_F3, ICON_PF_F3},
		{VK_F4, ICON_PF_F4},
		{VK_F5, ICON_PF_F5},
		{VK_F6, ICON_PF_F6},
		{VK_F7, ICON_PF_F7},
		{VK_F8, ICON_PF_F8},
		{VK_F9, ICON_PF_F9},
		{VK_F10, ICON_PF_F10},
		{VK_F11, ICON_PF_F11},
		{VK_F12, ICON_PF_F12},
	};

	std::optional<u32> NameToCode(const std::string_view name)
	{
		if (name.size() == 1)
		{
			const u32 code = static_cast<u32>(name[0]);
			if ((code >= '0' && code <= '9') || (code >= 'A' && code <= 'Z'))
				return code;
		}

		for (const KeyNameCode& key : s_special_keys)
		{
			if (name == key.name)
				return key.code;
		}

		return std::nullopt;
	}

	std::optional<std::string> CodeToName(const u32 code)
	{
		if ((code >= '0' && code <= '9') || (code >= 'A' && code <= 'Z'))
			return std::string(1, static_cast<char>(code));

		for (const KeyNameCode& key : s_special_keys)
		{
			if (code == key.code)
				return std::string(key.name);
		}

		return std::nullopt;
	}

	const char* CodeToIcon(const u32 code)
	{
		for (const KeyCodeIcon& key : s_key_icons)
		{
			if (code == key.code)
				return key.icon;
		}

		return nullptr;
	}

	bool IsValidKey(const u32 code)
	{
		return CodeToName(code).has_value();
	}
} // namespace UWPKeyboard
