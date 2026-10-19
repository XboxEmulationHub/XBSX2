// SPDX-FileCopyrightText: 2026 SternXD
// SPDX-License-Identifier: GPL-3.0+

#include "UWPUtils.h"

#include "pcsx2/PrecompiledHeader.h"

#include "common/BitUtils.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/Threading.h"
#include "pcsx2/Config.h"

#include <winrt/base.h>
#include <xaudio2.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

struct CachedSound final
{
	WAVEFORMATEXTENSIBLE format = {};
	std::vector<u8> file;
	size_t data_offset = 0;
	u32 data_size = 0;
};

struct PlaybackContext final
{
	~PlaybackContext()
	{
		if (voice)
			voice->DestroyVoice();
	}

	IXAudio2SourceVoice* voice = nullptr;
	std::shared_ptr<CachedSound> sound;
	std::atomic<bool> completed{false};
};

class SoundPlayer final : public IXAudio2VoiceCallback
{
public:
	~SoundPlayer()
	{
		if (m_cleanup_thread.Joinable())
		{
			m_stopping.store(true);
			m_cleanup_sema.NotifyOfWork();
			m_cleanup_thread.Join();
		}
		if (m_mastering_voice)
			m_mastering_voice->DestroyVoice();
	}

	bool Play(std::shared_ptr<CachedSound> sound)
	{
		auto ctx = std::make_unique<PlaybackContext>();
		ctx->sound = std::move(sound);
		std::lock_guard lock(m_mutex);
		if (!Initialize())
			return false;

		const HRESULT hr = m_xaudio->CreateSourceVoice(&ctx->voice, &ctx->sound->format.Format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, this);
		if (FAILED(hr))
		{
			Console.WarningFmt("UWPSound: CreateSourceVoice() failed: 0x{:08X}", static_cast<u32>(hr));
			return false;
		}

		XAUDIO2_BUFFER buffer = {};
		buffer.Flags = XAUDIO2_END_OF_STREAM;
		buffer.AudioBytes = ctx->sound->data_size;
		buffer.pAudioData = ctx->sound->file.data() + ctx->sound->data_offset;
		buffer.pContext = ctx.get();
		if (FAILED(ctx->voice->SubmitSourceBuffer(&buffer)) || FAILED(ctx->voice->Start(0)))
			return false;

		m_playbacks.push_back(std::move(ctx));
		return true;
	}

	void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
	void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
	void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
	void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
	void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
	void STDMETHODCALLTYPE OnVoiceError(void* context, HRESULT) noexcept override { Complete(context); }
	void STDMETHODCALLTYPE OnBufferEnd(void* context) noexcept override { Complete(context); }

private:
	bool Initialize()
	{
		if (m_engine_attempted)
			return m_cleanup_thread.Joinable();
		m_engine_attempted = true;

		const HRESULT hr = XAudio2Create(m_xaudio.put(), 0, XAUDIO2_DEFAULT_PROCESSOR);
		if (FAILED(hr))
		{
			Console.ErrorFmt("UWPSound: XAudio2Create() failed: 0x{:08X}", static_cast<u32>(hr));
			return false;
		}
		const HRESULT mastering_hr = m_xaudio->CreateMasteringVoice(&m_mastering_voice);
		if (FAILED(mastering_hr))
		{
			Console.ErrorFmt("UWPSound: CreateMasteringVoice() failed: 0x{:08X}", static_cast<u32>(mastering_hr));
			return false;
		}

		if (!m_cleanup_thread.Start([this]() { CleanupThread(); }))
		{
			Console.Error("UWPSound: Failed to start cleanup thread.");
			return false;
		}
		return true;
	}

	void Complete(void* context)
	{
		if (!context)
			return;

		// DestroyVoice() cannot be called from an XAudio2 callback.
		static_cast<PlaybackContext*>(context)->completed.store(true);
		m_cleanup_sema.NotifyOfWork();
	}

	void CleanupThread()
	{
		Threading::SetNameOfCurrentThread("UWP Sound Cleanup");

		for (;;)
		{
			m_cleanup_sema.WaitForWork();

			std::vector<std::unique_ptr<PlaybackContext>> finished;
			const bool stopping = m_stopping.load();
			{
				std::lock_guard lock(m_mutex);
				for (auto it = m_playbacks.begin(); it != m_playbacks.end();)
				{
					if (stopping || (*it)->completed.load())
					{
						finished.push_back(std::move(*it));
						it = m_playbacks.erase(it);
					}
					else
					{
						++it;
					}
				}
			}
			if (stopping)
				return;
		}
	}

	std::mutex m_mutex;
	winrt::com_ptr<IXAudio2> m_xaudio;
	IXAudio2MasteringVoice* m_mastering_voice = nullptr;
	Threading::WorkSema m_cleanup_sema;
	Threading::Thread m_cleanup_thread;
	std::atomic<bool> m_stopping{false};
	bool m_engine_attempted = false;
	std::vector<std::unique_ptr<PlaybackContext>> m_playbacks;
};

static std::mutex s_cache_mutex;
static std::unordered_map<std::string, std::shared_ptr<CachedSound>> s_cache;

// clang-format off
static constexpr u32 FOURCC_RIFF = 0x46464952; // "RIFF"
static constexpr u32 FOURCC_WAVE = 0x45564157; // "WAVE"
static constexpr u32 FOURCC_FMT  = 0x20746D66; // "fmt "
static constexpr u32 FOURCC_DATA = 0x61746164; // "data"
// clang-format on

static bool ParseWav(CachedSound& sound)
{
	const std::vector<u8>& file = sound.file;
	if (file.size() < 12)
		return false;

	const u8* const base = file.data();
	if (GetBufferU32(base, 0) != FOURCC_RIFF || GetBufferU32(base, 8) != FOURCC_WAVE)
		return false;

	bool have_fmt = false;
	bool have_data = false;
	size_t pos = 12;

	while (pos + 8 <= file.size())
	{
		const u32 id = GetBufferU32(base + pos, 0);
		const u32 size = GetBufferU32(base + pos, 4);
		const size_t payload = pos + 8;
		if (payload + size > file.size())
			return false;

		if (id == FOURCC_FMT)
		{
			if (size < 16)
				return false;

			const u8* const chunk = base + payload;
			const u16 format_tag = GetBufferU16(chunk, 0);
			if (format_tag == WAVE_FORMAT_EXTENSIBLE)
			{
				if (size < sizeof(WAVEFORMATEXTENSIBLE))
					return false;

				std::memcpy(&sound.format, chunk, sizeof(WAVEFORMATEXTENSIBLE));
			}
			else
			{
				sound.format = {};
				sound.format.Format.wFormatTag = format_tag;
				sound.format.Format.nChannels = GetBufferU16(chunk, 2);
				sound.format.Format.nSamplesPerSec = GetBufferU32(chunk, 4);
				sound.format.Format.nAvgBytesPerSec = GetBufferU32(chunk, 8);
				sound.format.Format.nBlockAlign = GetBufferU16(chunk, 12);
				sound.format.Format.wBitsPerSample = GetBufferU16(chunk, 14);
			}

			have_fmt = true;
		}
		else if (id == FOURCC_DATA)
		{
			sound.data_offset = payload;
			sound.data_size = size;
			have_data = true;
		}

		pos = payload + ((static_cast<size_t>(size) + 1) & ~static_cast<size_t>(1));
	}

	return have_fmt && have_data;
}

static std::shared_ptr<CachedSound> GetOrLoadSound(const std::string& path)
{
	{
		std::lock_guard lock(s_cache_mutex);
		const auto it = s_cache.find(path);
		if (it != s_cache.end())
			return it->second;
	}

	auto file = FileSystem::ReadBinaryFile(path.c_str());
	if (!file)
	{
		Console.WarningFmt("UWPSound: Failed to read WAV file '{}'", path);
		return {};
	}

	auto sound = std::make_shared<CachedSound>();
	sound->file = std::move(*file);
	if (!ParseWav(*sound))
	{
		Console.WarningFmt("UWPSound: Invalid WAV file '{}'", path);
		return {};
	}

	std::lock_guard lock(s_cache_mutex);
	return s_cache.emplace(path, sound).first->second;
}

bool UWP::PlaySoundAsync(const char* path)
{
	if (!path || path[0] == '\0')
		return false;

	const std::string full_path = Path::IsAbsolute(path) ? path : Path::Combine(EmuFolders::Resources, path);
	auto sound = GetOrLoadSound(full_path);
	if (!sound)
		return false;

	static SoundPlayer player;
	return player.Play(std::move(sound));
}
