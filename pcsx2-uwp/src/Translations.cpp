// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/Config.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/PrecompiledHeader.h"
#include "UWPHost.h"
#include "UWPUtils.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/RedtapeWindows.h"
#include "common/StringUtil.h"

#include <winrt/Windows.Data.Xml.Dom.h>

#include <array>
#include <map>
#include <tuple>

// clang-format off
namespace XmlDom = winrt::Windows::Data::Xml::Dom;
// clang-format on

enum class FontScript : u8;

namespace UWPHost
{
	static bool LoadTranslationCatalog(const std::string& path);
	static bool UpdateGlyphRangesAndClearCache(std::string_view language);
	static FontScript GetFontScript(std::string_view language);
	static size_t GetPluralForm(std::string_view language, int count);
} // namespace UWPHost

using TranslationKey = std::tuple<std::string, std::string, std::string>;
static std::map<TranslationKey, std::vector<std::string>> s_translations;
static std::string s_current_language = "en-US";

static std::string NormalizeLanguage(std::string language)
{
	std::replace(language.begin(), language.end(), '_', '-');
	if (StringUtil::StartsWithNoCase(language, "zh-Hant") || StringUtil::Strcasecmp(language.c_str(), "zh-HK") == 0 ||
		StringUtil::Strcasecmp(language.c_str(), "zh-MO") == 0)
		return "zh-TW";
	if (StringUtil::StartsWithNoCase(language, "zh-Hans") || StringUtil::Strcasecmp(language.c_str(), "zh-SG") == 0)
		return "zh-CN";
	// Xbox uses nb for Norwegian, but PCSX2's translation catalogs use no.
	if (StringUtil::Strcasecmp(language.c_str(), "nb") == 0 || StringUtil::StartsWithNoCase(language, "nb-"))
		language.replace(0, 2, "no");
	return language;
}

static std::string getSystemLanguage()
{
	ULONG count = 0, length = 0;
	if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &length) || !length)
		return "en-US";
	std::vector<wchar_t> names(length);
	if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, names.data(), &length))
		return "en-US";
	std::vector<std::string> locales;
	for (auto pos = names.begin(); locales.size() < count && pos != names.end() && *pos;)
	{
		const auto end = std::find(pos, names.end(), L'\0');
		if (end == names.end())
			break;
		locales.push_back(NormalizeLanguage(StringUtil::WideStringToUTF8String(
			std::wstring_view(&*pos, static_cast<size_t>(end - pos)))));
		pos = end + 1;
	}
	if (locales.empty())
		return "en-US";
	const std::vector<std::string> available = UWPHost::GetAvailableLanguageList();
	for (const std::string& locale : locales)
	{
		// Can we find an exact match?
		for (const std::string& language : available)
		{
			if (StringUtil::Strcasecmp(language.c_str(), locale.c_str()) == 0)
				return language;
		}
		// How about a partial match?
		const std::string base = locale.substr(0, locale.find('-'));
		// English source text is preferable.
		if (StringUtil::Strcasecmp(base.c_str(), "en") == 0)
			return "en-US";
		for (const std::string& language : available)
		{
			const std::string candidate = language.substr(0, language.find('-'));
			if (StringUtil::Strcasecmp(candidate.c_str(), base.c_str()) == 0)
			{
				Console.Warning("Couldn't find translation for system language %s, using %s instead", locale.c_str(), language.c_str());
				return language;
			}
		}
	}
	// No matches :(
	Console.Warning("Couldn't find translation for system language %s, using en instead", locales.front().c_str());
	return "en-US";
}

bool UWPHost::InstallTranslator()
{
	pxAssertRel(!g_emu_thread, "Install translations before starting the CPU thread");
	s_translations.clear();
	std::string language = Host::GetBaseStringSettingValue("UI", "Language", GetDefaultLanguage());
	if (language.empty() || language == "system")
		language = getSystemLanguage();
	else
		language = NormalizeLanguage(std::move(language));
	s_current_language = language;

	// Load PCSX2's existing Qt catalogs so UWP can reuse the same contexts and translations.
	for (const std::string& available : GetAvailableLanguageList())
	{
		if (StringUtil::Strcasecmp(available.c_str(), language.c_str()) != 0)
			continue;
		s_current_language = available;
		const std::string directory = Path::Combine(EmuFolders::AppRoot, "translations");
		const std::string path = Path::Combine(directory, fmt::format("pcsx2-qt_{}.ts", available));
		if (FileSystem::FileExists(path.c_str()))
		{
			if (LoadTranslationCatalog(path))
				Console.WriteLn("Loaded translation file for language %s", available.c_str());
			else
				Console.ErrorFmt("Failed to load translation catalog '{}'.", path);
		}
		break;
	}
	return UpdateGlyphRangesAndClearCache(s_current_language);
}

const char* UWPHost::GetDefaultLanguage()
{
	return "system";
}

size_t UWPHost::GetPluralForm(std::string_view language, int count)
{
	// Plural rules from Qt's numerus.cpp
	const std::string_view base = language.substr(0, language.find('-'));
	const int n = std::max(count, 0);
	const int last = n % 10;
	const int last_two = n % 100;

	// Single form (CJK, Turkish, etc.)
	if (base == "zh" || base == "ja" || base == "ko" || base == "hu" || base == "id" ||
		base == "fa" || base == "tr" || base == "vi" || base == "gn")
	{
		return 0;
	}

	// French & Brazilian Portuguese (0 and 1 are singular)
	if (base == "fr" || language == "pt-BR")
		return n > 1 ? 1 : 0;

	// Slavic (Russian, Ukrainian, Croatian, Serbian)
	if (base == "ru" || base == "uk" || base == "hr" || base == "sr")
	{
		if (last == 1 && last_two != 11)
			return 0;
		if (last >= 2 && last <= 4 && (last_two < 10 || last_two >= 20))
			return 1;
		return 2;
	}

	// Polish
	if (base == "pl")
	{
		if (n == 1)
			return 0;
		if (last >= 2 && last <= 4 && (last_two < 10 || last_two >= 20))
			return 1;
		return 2;
	}

	// Czech & Slovak
	if (base == "cs")
	{
		if (n == 1)
			return 0;
		if (n >= 2 && n <= 4)
			return 1;
		return 2;
	}

	// Lithuanian
	if (base == "lt")
	{
		if (last == 1 && last_two != 11)
			return 0;
		if (last != 0 && (last_two < 10 || last_two >= 20))
			return 1;
		return 2;
	}

	// Latvian
	if (base == "lv")
	{
		if (last == 1 && last_two != 11)
			return 0;
		return (n != 0) ? 1 : 2;
	}

	// Romanian
	if (base == "ro")
	{
		if (n == 1)
			return 0;
		if (n == 0 || (last_two >= 1 && last_two <= 19))
			return 1;
		return 2;
	}

	// Arabic
	if (base == "ar")
	{
		if (n == 0)
			return 0;
		if (n == 1)
			return 1;
		if (n == 2)
			return 2;
		if (last_two >= 3 && last_two <= 10)
			return 3;
		if (last_two >= 11)
			return 4;
		return 5;
	}

	// Default (English, etc.): 0 = singular, 1 = plural
	return n != 1 ? 1 : 0;
}

s32 Host::Internal::GetTranslatedStringImpl(const std::string_view context, const std::string_view msg, char* tbuf, size_t tbuf_space)
{
	auto it = s_translations.lower_bound(TranslationKey{context, msg, std::string()});
	const std::string_view translated = it != s_translations.end() && std::get<0>(it->first) == context && std::get<1>(it->first) == msg ?
	                                        std::string_view(it->second.front()) :
	                                        msg;
	if (translated.size() > tbuf_space)
		return -1;
	if (!translated.empty())
		std::memcpy(tbuf, translated.data(), translated.size());
	return static_cast<s32>(translated.size());
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
	auto it = s_translations.find(TranslationKey{context, msg, disambiguation ? disambiguation : ""});
	if (it == s_translations.end() && disambiguation)
		it = s_translations.find(TranslationKey{context, msg, std::string()});
	const size_t form = count < 0 ? 0 : UWPHost::GetPluralForm(s_current_language, count);
	std::string result = it != s_translations.end() && form < it->second.size() ? it->second[form] : msg;
	if (count < 0)
		return result;
	const std::string number = std::to_string(count);
	for (size_t pos = 0; (pos = result.find("%n", pos)) != std::string::npos; pos += number.size())
		result.replace(pos, 2, number);
	return result;
}

bool Host::LocaleCircleConfirm()
{
	const std::string_view language = std::string_view(s_current_language).substr(0, s_current_language.find('-'));
	return language == "ja" || language == "zh" || language == "ko";
}

std::vector<std::string> UWPHost::GetAvailableLanguageList()
{
	const std::string directory = Path::Combine(EmuFolders::AppRoot, "translations");
	FileSystem::FindResultsArray files;
	FileSystem::FindFiles(directory.c_str(), "pcsx2-qt_*.ts", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_SORT_BY_NAME, &files);
	std::vector<std::string> languages{"en-US"};
	for (const FILESYSTEM_FIND_DATA& file : files)
	{
		const std::string_view name = Path::GetFileName(file.FileName);
		constexpr std::string_view prefix = "pcsx2-qt_", suffix = ".ts";
		if (!name.starts_with(prefix) || !name.ends_with(suffix) || name.size() <= prefix.size() + suffix.size())
			continue;
		std::string language(name.substr(prefix.size(), name.size() - prefix.size() - suffix.size()));
		if (language != "en-US")
			languages.push_back(std::move(language));
	}
	return languages;
}

enum class FontScript : u8
{
	Arabic,
	ChineseSimplified,
	ChineseTraditional,
	Devanagari,
	Emoji,
	Hebrew,
	Japanese,
	Korean,
	Latin,
};
static constexpr size_t NumFontScripts = static_cast<size_t>(FontScript::Latin) + 1;

struct FontLoadInfo
{
	const char* file_name;
};

namespace FontNames
{
	static constexpr FontLoadInfo Arabic[] = {{"NotoSansArabic-Regular.ttf"}, {"Segoeui.ttf"}};
	static constexpr FontLoadInfo ChineseSimplified[] = {{"NotoSansSC-Regular.ttf"}, {"Msyh.ttc"}};
	static constexpr FontLoadInfo ChineseTraditional[] = {{"NotoSansTC-Regular.ttf"}, {"Msjh.ttc"}};
	static constexpr FontLoadInfo Devanagari[] = {{"NotoSansDevanagari-Regular.ttf"}, {"Nirmala.ttf"}};
	static constexpr FontLoadInfo Emoji[] = {{"Twemoji.Mozilla.ttf"}, {"Seguiemj.ttf"}};
	static constexpr FontLoadInfo Hebrew[] = {{"NotoSansHebrew-Regular.ttf"}, {"Segoeui.ttf"}};
	static constexpr FontLoadInfo Japanese[] = {{"NotoSansJP-Regular.ttf"}, {"YuGothR.ttc"}, {"Meiryo.ttc"}};
	static constexpr FontLoadInfo Korean[] = {{"NotoSansKR-Regular.ttf"}, {"Malgun.ttf"}};
	static constexpr FontLoadInfo Latin[] = {{"Roboto-Regular.ttf"}};
} // namespace FontNames

static constexpr std::span<const FontLoadInfo> GetFontNames(FontScript script)
{
	switch (script)
	{
		case FontScript::Arabic:
			return FontNames::Arabic;
		case FontScript::ChineseSimplified:
			return FontNames::ChineseSimplified;
		case FontScript::ChineseTraditional:
			return FontNames::ChineseTraditional;
		case FontScript::Devanagari:
			return FontNames::Devanagari;
		case FontScript::Emoji:
			return FontNames::Emoji;
		case FontScript::Hebrew:
			return FontNames::Hebrew;
		case FontScript::Japanese:
			return FontNames::Japanese;
		case FontScript::Korean:
			return FontNames::Korean;
		case FontScript::Latin:
			return FontNames::Latin;
	}
	return {};
}

static std::array<std::span<const u8>, NumFontScripts> s_font_data;

static std::span<const u8> TryLoadFont(const FontLoadInfo& info)
{
	const std::string resource = Path::Combine("fonts", info.file_name);
	const std::string user_path = Path::Combine(EmuFolders::UserResources, resource);
	if (const auto data = FileSystem::MapBinaryFileForRead(user_path.c_str()); !data.empty())
		return data;
	const std::string resource_path = EmuFolders::GetOverridableResourcePath(resource);
	if (const auto data = FileSystem::MapBinaryFileForRead(resource_path.c_str()); !data.empty())
		return data;
	const std::string system_path = Path::Combine("C:\\Windows\\Fonts", info.file_name);
	return FileSystem::MapBinaryFileForRead(system_path.c_str());
}

static void TryLoadFonts()
{
	for (size_t i = 0; i < NumFontScripts; i++)
	{
		if (!s_font_data[i].empty())
			continue;
		for (const FontLoadInfo& info : GetFontNames(static_cast<FontScript>(i)))
		{
			if (const auto data = TryLoadFont(info); !data.empty())
			{
				s_font_data[i] = data;
				break;
			}
		}
	}
}

static FontScript GetSecondaryScript(FontScript primary)
{
	switch (primary)
	{
		case FontScript::ChineseSimplified:
			return FontScript::ChineseTraditional;
		case FontScript::ChineseTraditional:
			return FontScript::ChineseSimplified;
		default:
			return primary;
	}
}

static constexpr FontScript g_fallback_list[] = {
	FontScript::Japanese,
	FontScript::Korean,
	FontScript::ChineseSimplified,
	FontScript::ChineseTraditional,
	FontScript::Devanagari,
	FontScript::Arabic,
	FontScript::Hebrew,
	FontScript::Emoji,
};

static bool HasCenteredElipsis(FontScript script)
{
	switch (script)
	{
		case FontScript::ChineseSimplified:
		case FontScript::ChineseTraditional:
		case FontScript::Japanese:
		case FontScript::Korean:
			return true;
		default:
			return false;
	}
}

bool UWPHost::UpdateGlyphRangesAndClearCache(std::string_view language)
{
	const FontScript primary = GetFontScript(language);
	const FontScript secondary = GetSecondaryScript(primary);
	TryLoadFonts();
	if (s_font_data[static_cast<size_t>(FontScript::Latin)].empty())
	{
		Console.Error("Failed to load Roboto-Regular.ttf.");
		Host::ClearTranslationCache();
		return false;
	}

	std::vector<ImGuiManager::FontInfo> fonts;
	fonts.reserve(NumFontScripts);
	auto AddFont = [&fonts](FontScript script) -> ImGuiManager::FontInfo* {
		const auto data = s_font_data[static_cast<size_t>(script)];
		if (data.empty())
			return nullptr;
		fonts.push_back({data, {}, nullptr, script == FontScript::Emoji});
		return &fonts.back();
	};

	// Use latin script for its characters regardless of language
	ImGuiManager::FontInfo* latin = AddFont(FontScript::Latin);
	if (HasCenteredElipsis(primary))
	{
		// Ellipsis is vertically centered in e.g. Japanese fonts, use the main language's version instead of the latin one
		static constexpr u32 exclude_ellipsis[] = {0x2026, 0x2026};
		latin->exclude_ranges = exclude_ellipsis;
	}
	if (primary != FontScript::Latin)
		AddFont(primary);
	if (secondary != primary)
		AddFont(secondary);
	for (FontScript script : g_fallback_list)
	{
		if (script != primary && script != secondary)
			AddFont(script);
	}
	// Startup, safe to set directly.
	ImGuiManager::SetFonts(std::move(fonts));
	Host::ClearTranslationCache();
	return true;
}

FontScript UWPHost::GetFontScript(std::string_view language)
{
	if (language == "ar-SA" || language == "fa-IR")
		return FontScript::Arabic;
	if (language == "hi-IN")
		return FontScript::Devanagari;
	if (language == "he-IL")
		return FontScript::Hebrew;
	if (language == "ja-JP")
		return FontScript::Japanese;
	if (language == "ko-KR")
		return FontScript::Korean;
	if (language == "zh-CN")
		return FontScript::ChineseSimplified;
	if (language == "zh-TW")
		return FontScript::ChineseTraditional;
	return FontScript::Latin;
}

int Host::LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs)
{
	if (lhs.empty() || rhs.empty())
		return lhs.empty() ? (rhs.empty() ? 0 : -1) : 1;
	const std::wstring wide_lhs = StringUtil::UTF8StringToWideString(lhs);
	const std::wstring wide_rhs = StringUtil::UTF8StringToWideString(rhs);
	const std::wstring locale = StringUtil::UTF8StringToWideString(s_current_language);
	const int result = CompareStringEx(locale.c_str(), 0, wide_lhs.data(), static_cast<int>(wide_lhs.size()),
		wide_rhs.data(), static_cast<int>(wide_rhs.size()), nullptr, nullptr, 0);
	return result ? result - CSTR_EQUAL : lhs.compare(rhs);
}


static XmlDom::IXmlNode SelectNode(const XmlDom::IXmlNode& node, const wchar_t* path)
{
	XmlDom::IXmlNode result{nullptr};
	if (const auto selector = node.try_as<XmlDom::IXmlNodeSelector>())
		UWP::GetABI(selector)->SelectSingleNode(winrt::get_abi(winrt::hstring(path)), winrt::put_abi(result));
	return result;
}

static std::vector<XmlDom::IXmlNode> SelectNodes(const XmlDom::IXmlNode& node, const wchar_t* path)
{
	const auto selector = node.try_as<XmlDom::IXmlNodeSelector>();
	XmlDom::XmlNodeList nodes{nullptr};
	if (!selector || FAILED(UWP::GetABI(selector)->SelectNodes(winrt::get_abi(winrt::hstring(path)), winrt::put_abi(nodes))))
		return {};
	u32 length = 0;
	UWP::GetABI(nodes)->get_Length(&length);
	std::vector<XmlDom::IXmlNode> result;
	result.reserve(length);
	for (u32 i = 0; i < length; i++)
	{
		XmlDom::IXmlNode item{nullptr};
		if (SUCCEEDED(UWP::GetABI(nodes)->Item(i, winrt::put_abi(item))) && item)
			result.push_back(std::move(item));
	}
	return result;
}

static std::string GetNodeText(const XmlDom::IXmlNode& node)
{
	winrt::hstring text;
	if (node)
	{
		if (const auto serializer = node.try_as<XmlDom::IXmlNodeSerializer>())
			UWP::GetABI(serializer)->get_InnerText(winrt::put_abi(text));
	}
	return winrt::to_string(text);
}

static std::string GetAttribute(const XmlDom::IXmlNode& node, const wchar_t* name)
{
	winrt::hstring text;
	if (const auto element = node.try_as<XmlDom::IXmlElement>())
		UWP::GetABI(element)->GetAttribute(winrt::get_abi(winrt::hstring(name)), winrt::put_abi(text));
	return winrt::to_string(text);
}

bool UWPHost::LoadTranslationCatalog(const std::string& path)
{
	auto xml = FileSystem::ReadFileToString(path.c_str());
	if (!xml)
		return false;
	// TS files declare a doctype, but do not need DTD processing.
	if (const size_t pos = xml->find("<!DOCTYPE TS>"); pos != std::string::npos)
		xml->erase(pos, std::strlen("<!DOCTYPE TS>"));
	const auto factory = winrt::try_get_activation_factory<XmlDom::XmlDocument>();
	winrt::Windows::Foundation::IInspectable object{nullptr};
	if (!factory || FAILED(UWP::GetABI(factory)->ActivateInstance(winrt::put_abi(object))))
		return false;
	const auto document = object.try_as<XmlDom::IXmlDocumentIO>();
	const auto root = object.try_as<XmlDom::IXmlNode>();
	if (!document || !root || FAILED(UWP::GetABI(document)->LoadXml(winrt::get_abi(winrt::to_hstring(*xml)))))
		return false;
	if (!SelectNode(root, L"/TS"))
		return false;

	for (const XmlDom::IXmlNode& context : SelectNodes(root, L"/TS/context"))
	{
		const std::string name = GetNodeText(SelectNode(context, L"name"));
		for (const XmlDom::IXmlNode& message : SelectNodes(context, L"message"))
		{
			const auto translation = SelectNode(message, L"translation");
			if (!translation || !GetAttribute(translation, L"type").empty())
				continue;
			std::vector<std::string> forms;
			if (GetAttribute(message, L"numerus") == "yes")
			{
				for (const XmlDom::IXmlNode& form : SelectNodes(translation, L"numerusform"))
					forms.push_back(GetNodeText(form));
			}
			else
			{
				forms.push_back(GetNodeText(translation));
			}
			if (forms.empty() || std::any_of(forms.begin(), forms.end(), [](const std::string& form) { return form.empty(); }))
				continue;
			s_translations.emplace(TranslationKey{name, GetNodeText(SelectNode(message, L"source")),
									   GetNodeText(SelectNode(message, L"comment"))},
				std::move(forms));
		}
	}
	return true;
}
