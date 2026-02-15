// SPDX-FileCopyrightText: 2026 SternXD
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/PrecompiledHeader.h"

#include "pcsx2-uwp/UWPUtils.h"
#include "pcsx2/Config.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include <winrt/base.h>
#include <xaudio2.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct CachedSound final
{
	WAVEFORMATEXTENSIBLE fmt = {};
	bool is_extensible = false;
	std::vector<u8> file;
	size_t data_offset = 0;
	u32 data_size = 0;
};

struct PlaybackContext final
{
	IXAudio2SourceVoice* voice = nullptr;
	std::shared_ptr<CachedSound> sound;
};

struct VoiceCallback final : IXAudio2VoiceCallback
{
	volatile LONG ref_count = 0;

	void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
	void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
	void STDMETHODCALLTYPE OnStreamEnd() override {}
	void STDMETHODCALLTYPE OnBufferStart(void*) override {}
	void STDMETHODCALLTYPE OnLoopEnd(void*) override {}

	void CleanupContext(void* pContext)
	{
		auto* ctx = static_cast<PlaybackContext*>(pContext);
		if (!ctx)
			return;
		if (ctx->voice)
			ctx->voice->DestroyVoice();
		delete ctx;
	}

	void STDMETHODCALLTYPE OnVoiceError(void* pContext, HRESULT) override
	{
		CleanupContext(pContext);
	}

	void STDMETHODCALLTYPE OnBufferEnd(void* pContext) override
	{
		CleanupContext(pContext);
	}
};

static std::mutex s_mutex;
static std::unordered_map<std::string, std::shared_ptr<CachedSound>> s_cache;
static winrt::com_ptr<IXAudio2> s_xaudio;
static IXAudio2MasteringVoice* s_mastering_voice = nullptr;
static bool s_engine_attempted = false;
static VoiceCallback s_voice_callback;

// clang-format off
static constexpr u32 FOURCC_RIFF = 0x46464952;	// "RIFF"
static constexpr u32 FOURCC_WAVE = 0x45564157;	// "WAVE"
static constexpr u32 FOURCC_FMT	 = 0x20746D66;	// "fmt"
static constexpr u32 FOURCC_DATA = 0x61746164;	// "data"
// clang-format on

static u32 ReadU32LE(const u8* p)
{
	return (static_cast<u32>(p[0]) << 0) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
	       (static_cast<u32>(p[3]) << 24);
}

static u16 ReadU16LE(const u8* p)
{
	return static_cast<u16>((static_cast<u16>(p[0]) << 0) | (static_cast<u16>(p[1]) << 8));
}

static bool ParseWav(const std::vector<u8>& file, WAVEFORMATEXTENSIBLE* fmt, bool* is_extensible, size_t* data_offset, u32* data_size)
{
	if (!fmt || !is_extensible || !data_offset || !data_size)
		return false;
	if (file.size() < 12)
		return false;

	const u8* const base = file.data();
	if (ReadU32LE(base) != FOURCC_RIFF || ReadU32LE(base + 8) != FOURCC_WAVE)
		return false;

	bool have_fmt = false;
	bool have_data = false;
	size_t pos = 12;

	while (pos + 8 <= file.size())
	{
		const u32 id = ReadU32LE(base + pos);
		const u32 size = ReadU32LE(base + pos + 4);
		const size_t payload = pos + 8;
		if (payload + size > file.size())
			return false;

		if (id == FOURCC_FMT)
		{
			if (size < 16)
				return false;

			const u16 format_tag = ReadU16LE(base + payload + 0);
			if (format_tag == WAVE_FORMAT_EXTENSIBLE)
			{
				if (size < sizeof(WAVEFORMATEXTENSIBLE))
					return false;

				std::memcpy(fmt, base + payload, sizeof(WAVEFORMATEXTENSIBLE));
				*is_extensible = true;
			}
			else
			{
				std::memset(fmt, 0, sizeof(WAVEFORMATEXTENSIBLE));
				fmt->Format.wFormatTag = format_tag;
				fmt->Format.nChannels = ReadU16LE(base + payload + 2);
				fmt->Format.nSamplesPerSec = ReadU32LE(base + payload + 4);
				fmt->Format.nAvgBytesPerSec = ReadU32LE(base + payload + 8);
				fmt->Format.nBlockAlign = ReadU16LE(base + payload + 12);
				fmt->Format.wBitsPerSample = ReadU16LE(base + payload + 14);
				fmt->Format.cbSize = 0;
				*is_extensible = false;
			}

			have_fmt = true;
		}
		else if (id == FOURCC_DATA)
		{
			*data_offset = payload;
			*data_size = size;
			have_data = true;
		}

		pos = payload + ((static_cast<size_t>(size) + 1) & ~static_cast<size_t>(1));
	}

	return have_fmt && have_data;
}

static bool EnsureXAudio2()
{
	std::lock_guard<std::mutex> lock(s_mutex);
	if (s_engine_attempted)
		return static_cast<bool>(s_xaudio) && (s_mastering_voice != nullptr);

	s_engine_attempted = true;
	if (FAILED(XAudio2Create(s_xaudio.put(), 0, XAUDIO2_DEFAULT_PROCESSOR)))
		return false;
	if (FAILED(s_xaudio->CreateMasteringVoice(&s_mastering_voice)))
	{
		s_xaudio = nullptr;
		return false;
	}

	return true;
}

static std::shared_ptr<CachedSound> GetOrLoadSound(std::string_view full_path)
{
	const std::string path_str(full_path);
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		auto it = s_cache.find(path_str);
		if (it != s_cache.end())
			return it->second;
	}

	auto file = FileSystem::ReadBinaryFile(path_str.c_str());
	if (!file.has_value())
		return {};

	auto sound = std::make_shared<CachedSound>();
	sound->file = std::move(file.value());
	if (!ParseWav(sound->file, &sound->fmt, &sound->is_extensible, &sound->data_offset, &sound->data_size))
		return {};
	if (sound->data_offset + sound->data_size > sound->file.size())
		return {};

	{
		std::lock_guard<std::mutex> lock(s_mutex);
		auto [it, inserted] = s_cache.emplace(path_str, sound);
		if (!inserted)
			return it->second;
	}

	return sound;
}

bool UWP::PlaySoundAsync(const char* path)
{
	if (!path || path[0] == '\0')
		return false;
	if (!EnsureXAudio2())
		return false;

	const std::string fullPath = Path::IsAbsolute(path) ? path : Path::Combine(EmuFolders::Resources, path);
	auto sound = GetOrLoadSound(fullPath);
	if (!sound)
		return false;

	const WAVEFORMATEX* fmt = sound->is_extensible ? reinterpret_cast<const WAVEFORMATEX*>(&sound->fmt) : &sound->fmt.Format;

	IXAudio2SourceVoice* voice = nullptr;
	if (FAILED(s_xaudio->CreateSourceVoice(&voice, fmt, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &s_voice_callback)))
		return false;

	auto* ctx = new PlaybackContext();
	ctx->voice = voice;
	ctx->sound = std::move(sound);

	XAUDIO2_BUFFER buffer = {};
	buffer.Flags = XAUDIO2_END_OF_STREAM;
	buffer.AudioBytes = ctx->sound->data_size;
	buffer.pAudioData = reinterpret_cast<const BYTE*>(ctx->sound->file.data() + ctx->sound->data_offset);
	buffer.pContext = ctx;

	if (FAILED(voice->SubmitSourceBuffer(&buffer)))
	{
		voice->DestroyVoice();
		delete ctx;
		return false;
	}

	if (FAILED(voice->Start(0)))
	{
		voice->DestroyVoice();
		delete ctx;
		return false;
	}

	return true;
}
