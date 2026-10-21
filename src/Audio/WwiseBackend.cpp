#include "Audio/WwiseBackend.h"
#include "Testing/D3AudioProbe.h"
#include "Testing/D3WwiseTrace.h"

#include "Audio/PcmWem.h"

#include <cctype>
#include <charconv>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <malloc.h>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

// miniaudio is used only as a decoder; device I/O is compiled out. Not /Wall-clean.
#pragma warning(push, 0)
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_DEVICE_IO
#include <miniaudio.h>
#pragma warning(pop)

// A miniaudio platform header defines ERROR, which breaks REX::ERROR.
#ifdef ERROR
#undef ERROR
#endif

namespace OSF::Audio::Wwise
{
	namespace
	{
		// Shipped event with an "External_Source" slot, resident in a loaded bank (no LoadBank needed).
		// It is a dialogue event, so posts route through the dialogue bus (dialogue volume, may duck).
		constexpr std::uint32_t kExternalSourceEvent = 0x5DE4F1F3;  // Ak hash of "Dialogue_6_Combat"

		constexpr std::string_view kEventPrefix = "event:";

		std::string LowerExtension(std::string_view a_path)
		{
			const auto dot = a_path.find_last_of('.');
			if (dot == std::string_view::npos) {
				return {};
			}
			std::string ext{ a_path.substr(dot + 1) };
			for (auto& c : ext) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return ext;
		}

		// AK reads pInMemory zero-copy for the whole playback, so cached buffers are never freed.
		using MediaBuffer = PreparedMedia;

		constexpr std::size_t kMaxMediaBytes = 64u << 20;       // per clip
		constexpr std::size_t kMaxMediaCacheBytes = 256u << 20;  // aggregate, process lifetime
		constexpr std::size_t kMaxMediaCacheEntries = 4096;

		struct MediaCacheEntry
		{
			std::mutex mutex;
			std::condition_variable readyCV;
			bool ready = false;
			MediaBuffer media;
		};

		std::mutex g_mediaCacheMutex;
		std::unordered_map<std::wstring, std::shared_ptr<MediaCacheEntry>> g_mediaCache;
		std::size_t g_mediaCacheBytes = 0;

		// AkCodecID from a .wem's RIFF 'fmt ' tag: 0xFFFF = Wwise Vorbis, 0xFFFE = Wwise PCM.
		// Unrecognized -> Vorbis.
		RE::BGSAudio::AkCodecID DeriveWemCodec(const void* a_data, std::uint32_t a_size)
		{
			const auto* b = static_cast<const std::uint8_t*>(a_data);
			if (a_size < 12 || std::memcmp(b, "RIFF", 4) != 0 || std::memcmp(b + 8, "WAVE", 4) != 0) {
				return RE::BGSAudio::AkCodecID::kVorbis;
			}
			// 64-bit index: a corrupt chunkSize near UINT32_MAX would wrap a 32-bit index.
			for (std::uint64_t i = 12; i + 8 <= a_size;) {
				std::uint32_t chunkSize = 0;
				std::memcpy(&chunkSize, b + i + 4, 4);
				if (std::memcmp(b + i, "fmt ", 4) == 0) {
					if (i + 10 > a_size) {
						break;
					}
					std::uint16_t tag = 0;
					std::memcpy(&tag, b + i + 8, 2);
					return (tag == 0xFFFE) ? RE::BGSAudio::AkCodecID::kPCM
					                       : RE::BGSAudio::AkCodecID::kVorbis;
				}
				i += 8ull + chunkSize + (chunkSize & 1);
			}
			return RE::BGSAudio::AkCodecID::kVorbis;
		}

		// 16-byte aligned (AK requirement), read via the process file API so MO2/USVFS loose files resolve.
		MediaBuffer LoadWemFile(const std::wstring& a_path)
		{
			std::ifstream file{ a_path, std::ios::binary | std::ios::ate };
			if (!file) {
				REX::DEBUG("[Audio] cannot open .wem '{}'", std::filesystem::path(a_path).string());
				return {};
			}
			const auto size = static_cast<std::size_t>(file.tellg());
			if (size == 0 || size > kMaxMediaBytes) {
				REX::DEBUG("[Audio] .wem '{}' unusable size {}", std::filesystem::path(a_path).string(), size);
				return {};
			}
			void* buffer = _aligned_malloc(size, 16);
			if (buffer == nullptr) {
				return {};
			}
			file.seekg(0);
			if (!file.read(static_cast<char*>(buffer), static_cast<std::streamsize>(size))) {
				_aligned_free(buffer);
				REX::DEBUG("[Audio] .wem read failed '{}'", std::filesystem::path(a_path).string());
				return {};
			}
			const auto bytes = static_cast<std::uint32_t>(size);
			return MediaBuffer{ buffer, bytes, DeriveWemCodec(buffer, bytes) };
		}

		std::wstring LowerExtensionW(const std::wstring& a_path)
		{
			const auto dot = a_path.find_last_of(L'.');
			if (dot == std::wstring::npos) {
				return {};
			}
			std::wstring ext = a_path.substr(dot + 1);
			for (auto& c : ext) {
				c = static_cast<wchar_t>(std::towlower(c));
			}
			return ext;
		}

		// Layout in Audio/PcmWem.h; copied into a 16-byte-aligned, cache-owned buffer.
		MediaBuffer BuildPcmWem(const std::int16_t* a_pcm, std::size_t a_frames, std::uint32_t a_channels, std::uint32_t a_sampleRate)
		{
			const std::vector<std::uint8_t> bytes = OSF::Audio::BuildPcmWemBytes(a_pcm, a_frames, a_channels, a_sampleRate);
			auto* buf = static_cast<std::uint8_t*>(_aligned_malloc(bytes.size(), 16));
			if (buf == nullptr) {
				return {};
			}
			std::memcpy(buf, bytes.data(), bytes.size());
			return MediaBuffer{ buf, static_cast<std::uint32_t>(bytes.size()), RE::BGSAudio::AkCodecID::kPCM };
		}

		// Empty on decode failure; the cue is skipped.
		MediaBuffer DecodeToPcmWem(const std::wstring& a_path)
		{
			ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 0, 0);
			ma_decoder decoder;
			if (ma_decoder_init_file_w(a_path.c_str(), &cfg, &decoder) != MA_SUCCESS) {
				REX::DEBUG("[Audio] cannot decode '{}'", std::filesystem::path(a_path).string());
				return {};
			}

			std::uint32_t channels = decoder.outputChannels;
			// Only mono/stereo AkChannelConfig masks are RE-proven; re-open forcing a stereo downmix.
			if (channels > 2) {
				ma_decoder_uninit(&decoder);
				cfg = ma_decoder_config_init(ma_format_s16, 2, 0);
				if (ma_decoder_init_file_w(a_path.c_str(), &cfg, &decoder) != MA_SUCCESS) {
					REX::DEBUG("[Audio] cannot re-open '{}' for stereo downmix",
						std::filesystem::path(a_path).string());
					return {};
				}
				channels = decoder.outputChannels;
			}
			const std::uint32_t sampleRate = decoder.outputSampleRate;

			// Chunked: mp3/vorbis report only an estimated length.
			std::vector<std::int16_t> pcm;
			constexpr std::size_t kFramesPerRead = 4096;
			std::vector<std::int16_t> chunk(kFramesPerRead * channels);
			for (;;) {
				ma_uint64 got = 0;
				const ma_result r = ma_decoder_read_pcm_frames(&decoder, chunk.data(), kFramesPerRead, &got);
				if (got != 0) {
					pcm.insert(pcm.end(), chunk.begin(),
						chunk.begin() + static_cast<std::ptrdiff_t>(got * channels));
				}
				if (pcm.size() * sizeof(std::int16_t) > kMaxMediaBytes) {
					REX::DEBUG("[Audio] decoded '{}' exceeds {} bytes — aborting",
						std::filesystem::path(a_path).string(), kMaxMediaBytes);
					ma_decoder_uninit(&decoder);
					return {};
				}
				if (r != MA_SUCCESS || got < kFramesPerRead) {
					break;
				}
			}
			ma_decoder_uninit(&decoder);

			if (pcm.empty()) {
				REX::DEBUG("[Audio] '{}' decoded to 0 frames", std::filesystem::path(a_path).string());
				return {};
			}
			return BuildPcmWem(pcm.data(), pcm.size() / channels, channels, sampleRate);
		}

		MediaBuffer LoadMedia(const std::wstring& a_path)
		{
			return (LowerExtensionW(a_path) == L"wem") ? LoadWemFile(a_path) : DecodeToPcmWem(a_path);
		}

		void FreeUnpublishedMedia(MediaBuffer& a_media)
		{
			if (a_media.data) {
				_aligned_free(a_media.data);
				a_media = {};
			}
		}

		// Each path loads once; concurrent callers for the same path share the in-flight load. Bytes and entries are capped.
		MediaBuffer GetOrLoadMedia(const std::wstring& a_path)
		{
			std::shared_ptr<MediaCacheEntry> entry;
			bool loader = false;
			{
				std::lock_guard cacheLock{ g_mediaCacheMutex };
				if (const auto it = g_mediaCache.find(a_path); it != g_mediaCache.end()) {
					entry = it->second;
				} else {
					if (g_mediaCache.size() >= kMaxMediaCacheEntries) {
						REX::WARN("[Audio] media cache entry limit ({}) reached — '{}' skipped",
							kMaxMediaCacheEntries, std::filesystem::path(a_path).string());
						return {};
					}
					entry = std::make_shared<MediaCacheEntry>();
					g_mediaCache.emplace(a_path, entry);
					loader = true;
				}
			}

			if (!loader) {
#ifdef OSF_ANIMATION_SMOKE
				Testing::D3AudioProbe::Point(Testing::D3AudioProbe::Current(), "cacheState", 2);
#endif
				std::unique_lock entryLock{ entry->mutex };
				entry->readyCV.wait(entryLock, [&] { return entry->ready; });
				return entry->media;
			}

			MediaBuffer media;
#ifdef OSF_ANIMATION_SMOKE
			Testing::D3AudioProbe::Point(Testing::D3AudioProbe::Current(), "cacheState", 1);
#endif
			struct ReadyPublisher
			{
				std::shared_ptr<MediaCacheEntry> entry;
				MediaBuffer& media;

				~ReadyPublisher()
				{
					{
						std::lock_guard entryLock{ entry->mutex };
						entry->media = media;
						entry->ready = true;
					}
					entry->readyCV.notify_all();
				}
			} publishReady{ entry, media };

			try {
				media = LoadMedia(a_path);
				if (media.data) {
					std::lock_guard cacheLock{ g_mediaCacheMutex };
					if (media.size > kMaxMediaCacheBytes - g_mediaCacheBytes) {
						REX::WARN("[Audio] media cache byte limit ({} MiB) reached — '{}' skipped",
							kMaxMediaCacheBytes >> 20, std::filesystem::path(a_path).string());
						FreeUnpublishedMedia(media);
					} else {
						g_mediaCacheBytes += media.size;
					}
				}
			} catch (const std::exception& e) {
				FreeUnpublishedMedia(media);
				REX::ERROR("[Audio] media preparation failed for '{}': {}",
					std::filesystem::path(a_path).string(), e.what());
			} catch (...) {
				FreeUnpublishedMedia(media);
				REX::ERROR("[Audio] media preparation failed for '{}' with an unknown exception",
					std::filesystem::path(a_path).string());
			}
			return media;
		}

		// pInMemory, not szFile: the engine's file resolver is registry-gated, so a szFile post is accepted but silent.
		std::uint32_t PostExternalOn(std::uint32_t a_eventID, const PreparedMedia& media, std::uint64_t a_gameObj)
		{
#ifdef OSF_ANIMATION_SMOKE
			const auto d3 = Testing::D3AudioProbe::Current();
#endif
			if (media.data == nullptr) {
				return 0;
			}
			RE::BGSAudio::AkExternalSourceInfo ext{};
			ext.iExternalSrcCookie = RE::BGSAudio::kExternalSourceCookie;
			ext.idCodec = static_cast<std::uint32_t>(media.codec);
			ext.szFile = nullptr;
			ext.pInMemory = media.data;
			ext.uiMemorySize = media.size;
			ext.idFile = 0;
#ifdef OSF_ANIMATION_SMOKE
			void* traceCookie=Testing::D3WwiseTrace::Cookie(d3);
			Testing::D3AudioProbe::Point(d3, "postNs");
			const auto playing = RE::BGSAudio::AkSoundEngine::PostEvent(a_eventID, a_gameObj,
				traceCookie ? 0x19u : 0u,
				traceCookie ? reinterpret_cast<void*>(&Testing::D3WwiseTrace::Callback) : nullptr,
				traceCookie, 1, &ext, 0);
#else
			const auto playing = RE::BGSAudio::AkSoundEngine::PostEvent(a_eventID, a_gameObj, 0, nullptr, nullptr, 1, &ext, 0);
#endif
#ifdef OSF_ANIMATION_SMOKE
			Testing::D3WwiseTrace::Posted(traceCookie,playing);
			Testing::D3AudioProbe::Point(d3, "postReturnNs");
			Testing::D3AudioProbe::Point(d3, playing ? "acceptedPlayingId" : "postRejected", playing ? playing : 1);
#endif
			return playing;
		}
	}

	std::uint32_t HashEventName(std::string_view a_name)
	{
		std::uint32_t hash = 0x811C9DC5;
		for (const char c : a_name) {
			hash *= 0x01000193;
			hash ^= static_cast<std::uint8_t>(std::tolower(static_cast<unsigned char>(c)));
		}
		return hash;
	}

	std::optional<std::uint32_t> ParseEventSpec(std::string_view a_spec)
	{
		if (!a_spec.starts_with(kEventPrefix)) {
			return std::nullopt;
		}
		auto body = a_spec.substr(kEventPrefix.size());
		if (body.empty()) {
			REX::WARN("[Audio] empty 'event:' cue sound spec");
			return std::nullopt;
		}
		if (body.starts_with("0x") || body.starts_with("0X")) {
			body.remove_prefix(2);
			std::uint32_t id = 0;
			const auto [ptr, ec] = std::from_chars(body.data(), body.data() + body.size(), id, 16);
			if (ec != std::errc{} || ptr != body.data() + body.size()) {
				REX::WARN("[Audio] malformed event ID in cue sound spec 'event:0x{}'", body);
				return std::nullopt;
			}
			return id;
		}
		return HashEventName(body);
	}

	std::uint32_t PostEvent(std::uint32_t a_eventID, std::uint64_t a_gameObject)
	{
		const auto gameObject = a_gameObject != 0 ? a_gameObject : RE::BGSAudio::AkSoundEngine::kPlayerGameObject;
		return RE::BGSAudio::AkSoundEngine::PostEvent(a_eventID, gameObject, 0, nullptr, nullptr, 0, nullptr, 0);
	}

	bool StopVoice(std::uint32_t a_playingID)
	{
		if (a_playingID == 0) return false;
#ifdef OSF_ANIMATION_SMOKE
		RE::BGSAudio::AkSoundEngine::StopPlayingID(a_playingID,Testing::D3WwiseTrace::StopTransition(a_playingID));
		Testing::D3AudioProbe::VoiceStopped(a_playingID);
#else
		RE::BGSAudio::AkSoundEngine::StopPlayingID(a_playingID);
#endif
		return true;
	}

	bool IsWwiseExternalSource(std::string_view a_path)
	{
		const std::string ext = LowerExtension(a_path);
		return ext == "wem" || ext == "wav" || ext == "mp3" || ext == "ogg" || ext == "flac";
	}

	PreparedMedia PrepareExternalFile(const std::wstring& a_path)
	{
#ifdef OSF_ANIMATION_SMOKE
		const auto d3 = Testing::D3AudioProbe::Current();
		Testing::D3AudioProbe::Point(d3, "prepareNs");
		Testing::D3AudioProbe::Checkpoint(d3, "during-prepare");
#endif
		const auto media = GetOrLoadMedia(a_path);
#ifdef OSF_ANIMATION_SMOKE
		Testing::D3AudioProbe::Point(d3, "mediaReadyNs");
		Testing::D3AudioProbe::Checkpoint(d3, "before-post");
#endif
		return media;
	}

	std::uint32_t PostPreparedFile(const PreparedMedia& a_media, std::uint64_t a_gameObject)
	{
		const auto gameObject = a_gameObject != 0 ? a_gameObject : RE::BGSAudio::AkSoundEngine::kPlayerGameObject;
		return PostExternalOn(kExternalSourceEvent, a_media, gameObject);
	}

	std::uint32_t PostExternalFile(const std::wstring& a_path, std::uint64_t a_gameObject)
	{
		return PostPreparedFile(PrepareExternalFile(a_path), a_gameObject);
	}
}
