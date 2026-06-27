#include "Audio/WwiseBackend.h"

#include "Audio/PcmWem.h"

#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <malloc.h>
#include <mutex>
#include <unordered_map>
#include <vector>

// This TU compiles the miniaudio implementation (#define MINIAUDIO_IMPLEMENTATION). We use ONLY the decoder
// (ma_decoder) to turn an arbitrary .wav/.mp3/.ogg/.flac into raw PCM, which BuildPcmWem then wraps into a
// Wwise PCM .wem for the engine-mixed external-source post. The output-device backends are compiled out
// (MA_NO_DEVICE_IO): OSF never plays audio through its own device — everything rides the game's Wwise mix.
// The implementation is C and not /Wall-clean, drop it from our warning level.
#pragma warning(push, 0)
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_DEVICE_IO
#include <miniaudio.h>
#pragma warning(pop)

namespace OSF::Audio::Wwise
{
	namespace
	{
		//@TODO: use Commonlib?
		// AK::SoundEngine::PostEvent(AkUniqueID, AkGameObjectID, u32 flags, callback, cookie, u32 cExternals, AkExternalSourceInfo*, AkPlayingID)
		// Wwise 2021.1 ABI, statically linked into the game. called from a worker thread, the returned playing IDs continued the engine's own counter.
		constexpr REL::ID kAkPostEventByID{ 150391 };

		// First 16 bytes of 150391, byte-identical on 1.16.242 and 1.16.244
		// Mismatch on a future patch = self-disable.
		constexpr std::array<std::uint8_t, 16> kPostEventPrologue{
			0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10,
			0x48, 0x89, 0x74, 0x24, 0x18, 0x4C
		};

		// Shipped event that already carries an "External_Source" placeholder slot and streamed a loose file live.
		// It is already resident in a loaded bank, so NO LoadBank is needed: we just substitute our own media through its external-source slot.
		// (Equivalent at runtime: AkSoundEngine::GetIDFromString("Dialogue_6_Combat").)

		// LIMITATION: this is a dialogue/VO event, so posts route through the dialogue bus
		// (dialogue-volume gated, may duck other audio).
		constexpr std::uint32_t kExternalSourceEvent = 0x5DE4F1F3;  // Ak hash of "Dialogue_6_Combat"

		constexpr std::string_view kEventPrefix = "event:";

		// Lowercased file extension (without the dot), or "" if none.
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

		// Ready-to-post .wem bytes held in memory — either a real .wem loaded from disk, or one built
		// at runtime from decoded PCM (BuildPcmWem). AK references pInMemory zero-copy across the whole
		// playback, so the cache OWNS this memory for the process lifetime (never freed).
		// `codec` is the AkCodecID to post with — Vorbis/PCM read from a real .wem's fmt tag, or kPCM
		// for a runtime-built buffer.
		struct MediaBuffer
		{
			void* data{ nullptr };
			std::uint32_t size{ 0 };
			RE::BGSAudio::AkCodecID codec{ RE::BGSAudio::AkCodecID::kVorbis };
		};

		constexpr std::size_t kMaxMediaBytes = 64u << 20;  // 64 MiB guard per clip

		std::mutex g_mediaCacheMutex;
		std::unordered_map<std::wstring, MediaBuffer> g_mediaCache;

		// AkCodecID from a .wem's RIFF 'fmt ' tag: 0xFFFF = Wwise Vorbis, 0xFFFE = Wwise PCM.
		// Anything unrecognized -> Vorbis (the most common case); a non-.wem file won't match here.
		RE::BGSAudio::AkCodecID DeriveWemCodec(const void* a_data, std::uint32_t a_size)
		{
			const auto* b = static_cast<const std::uint8_t*>(a_data);
			if (a_size < 12 || std::memcmp(b, "RIFF", 4) != 0 || std::memcmp(b + 8, "WAVE", 4) != 0) {
				return RE::BGSAudio::AkCodecID::kVorbis;
			}
			for (std::uint32_t i = 12; i + 8 <= a_size;) {
				std::uint32_t chunkSize = 0;
				std::memcpy(&chunkSize, b + i + 4, 4);
				if (std::memcmp(b + i, "fmt ", 4) == 0 && i + 10 <= a_size) {
					std::uint16_t tag = 0;
					std::memcpy(&tag, b + i + 8, 2);
					return (tag == 0xFFFE) ? RE::BGSAudio::AkCodecID::kPCM
					                       : RE::BGSAudio::AkCodecID::kVorbis;
				}
				i += 8 + chunkSize + (chunkSize & 1);
			}
			return RE::BGSAudio::AkCodecID::kVorbis;
		}

		// Loads a Wwise .wem into a 16-byte-aligned, process-lifetime buffer (AK requires aligned in-memory media and references it zero-copy). 
		// Opened through the process file API so MO2/USVFS-virtual loose files resolve, unlike szFile, 
		// which the registry-gated audio resolver never opens, the reason we post bytes.
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

		// Lowercased file extension of a wide path (no dot), or L"" if none.
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

		// Wraps raw interleaved 16-bit PCM in a Wwise PCM .wem (idCodec=kPCM) for an AK in-memory post.
		// The byte layout lives in the dep-free, unit-tested Audio/PcmWem.h (BuildPcmWemBytes); here we
		// just copy it into a 16-byte-aligned buffer (AK requires aligned in-memory media). The buffer is
		// _aligned_malloc'd and owned by the process-lifetime cache (never freed) — see GetOrLoadMedia.
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

		// Decodes any miniaudio-supported file (.wav/.mp3/.ogg/.flac) to interleaved 16-bit PCM and wraps it in a PCM .wem, so a vanilla audio file rides the SAME engine-mixed external-source path as a real .wem. 
		// Returns an empty buffer on any decode failure; the cue is then skipped (no private-device fallback).
		MediaBuffer DecodeToPcmWem(const std::wstring& a_path)
		{
			// Phase 1: open with native channels/rate (s16 output) to learn the channel count.
			ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 0, 0);
			ma_decoder decoder;
			if (ma_decoder_init_file_w(a_path.c_str(), &cfg, &decoder) != MA_SUCCESS) {
				REX::DEBUG("[Audio] cannot decode '{}'", std::filesystem::path(a_path).string());
				return {};
			}

			std::uint32_t channels = decoder.outputChannels;
			// Only mono + stereo AkChannelConfig masks are RE-proven; re-open forcing 2-channel output so miniaudio downmixes anything wider to stereo.
			if (channels > 2) {
				ma_decoder_uninit(&decoder);
				cfg = ma_decoder_config_init(ma_format_s16, 2, 0);
				if (ma_decoder_init_file_w(a_path.c_str(), &cfg, &decoder) != MA_SUCCESS) {
					REX::DEBUG("[Audio] cannot re-open '{}' for stereo downmix",
						std::filesystem::path(a_path).string());
					return {};
				}
				channels = decoder.outputChannels;  // == 2
			}
			const std::uint32_t sampleRate = decoder.outputSampleRate;

			// Read all frames in chunks: mp3/vorbis report only an ESTIMATED length, so a single sized read is unreliable. 
			// Cap with the same per-clip guard as raw media.
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
					break;  // MA_AT_END or short tail read
				}
			}
			ma_decoder_uninit(&decoder);

			if (pcm.empty()) {
				REX::DEBUG("[Audio] '{}' decoded to 0 frames", std::filesystem::path(a_path).string());
				return {};
			}
			return BuildPcmWem(pcm.data(), pcm.size() / channels, channels, sampleRate);
		}

		// Produces the ready-to-post media for a_path: a real .wem is loaded as-is (codec from its fmt tag); 
		// any other decodable format is decoded to PCM and wrapped in a PCM .wem at runtime.
		MediaBuffer LoadMedia(const std::wstring& a_path)
		{
			return (LowerExtensionW(a_path) == L"wem") ? LoadWemFile(a_path) : DecodeToPcmWem(a_path);
		}

		// Loads + prepares a clip once and caches it for the WHOLE PROCESS (success AND failure, so a missing/bad file doesn't re-hit disk). 
		// The cache is NEVER evicted on purpose: AK references pInMemory zero-copy for the entire playback and the external-source duplicator copies only the 0x20 descriptor, 
		// NOT the bytes — freeing a buffer while a voice still reads it is an audio-thread use-after-free. Process-lifetime ownership is the invariant that keeps it safe.
		MediaBuffer GetOrLoadMedia(const std::wstring& a_path)
		{
			const std::scoped_lock lock{ g_mediaCacheMutex };
			if (const auto it = g_mediaCache.find(a_path); it != g_mediaCache.end()) {
				return it->second;
			}
			const MediaBuffer media = LoadMedia(a_path);
			g_mediaCache.emplace(a_path, media);
			return media;
		}

		// Posts a .wem as an external source via pInMemory, NOT szFile the engine's audio file resolver is registry-gated and never opens a loose file, so a szFile post is accepted but SILENT. 
		// pInMemory bypasses the resolver and renders engine-mixed. The cache owns the bytes for the process, so the descriptor only needs to outlive this call.
		std::uint32_t PostExternalOn(std::uint32_t a_eventID, const std::wstring& a_path, std::uint64_t a_gameObj)
		{
			const MediaBuffer media = GetOrLoadMedia(a_path);
			if (media.data == nullptr) {
				return 0;  // load failed (logged once by LoadWemFile)
			}
			RE::BGSAudio::AkExternalSourceInfo ext{};
			ext.iExternalSrcCookie = RE::BGSAudio::kExternalSourceCookie;
			ext.idCodec = static_cast<std::uint32_t>(media.codec);  // from the .wem's fmt tag
			ext.szFile = nullptr;
			ext.pInMemory = media.data;
			ext.uiMemorySize = media.size;
			ext.idFile = 0;
			return RE::BGSAudio::AkSoundEngine::PostEvent(a_eventID, a_gameObj, 0, nullptr, nullptr, 1, &ext, 0);
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

	bool Available()
	{
		static const bool available = []() {
			const auto* code = reinterpret_cast<const std::uint8_t*>(kAkPostEventByID.address());
			if (!code || std::memcmp(code, kPostEventPrologue.data(), kPostEventPrologue.size()) != 0) {
				REX::WARN("[Audio] Wwise audio disabled: AK PostEvent (ID {}) prologue mismatch on this runtime — cue sounds are skipped (no private-device fallback)", kAkPostEventByID.id());
				return false;
			}
			REX::DEBUG("[Audio] Wwise audio available: AK PostEvent prologue verified");
			return true;
		}();
		return available;
	}

	std::uint32_t PostEvent(std::uint32_t a_eventID)
	{
		if (!Available()) {
			return 0;
		}
		// cExternals 0 = no external source: a plain baked event on the player object.
		return RE::BGSAudio::AkSoundEngine::PostEvent(a_eventID, RE::BGSAudio::AkSoundEngine::kPlayerGameObject, 0, nullptr, nullptr, 0, nullptr, 0);
	}

	namespace
	{
		// AK::SoundEngine::ExecuteActionOnPlayingID = rel_id 150360 (0x142cd4130 on 1.16.244), msgType 0x39:
		//   void(AkActionOnEventType /*ecx*/, AkPlayingID /*edx*/, AkTimeMs /*r8d*/, AkCurveInterpolation /*r9d*/)
		// PROVEN (OSF RE 2026-06-25, Investigations/Responses/2026-06-25-wwise-stop-playing-voice.md):
		// unique off2id hit ids:[150360], verified 16-byte prologue, the ENGINE ITSELF calls it, AND
		// runtime-confirmed by ear (an in-flight tone cut instantly). Specifically the engine calls it —
		// 0x140f1c4e0 with actionType 0 (Stop), 0x140f1b5c5 / 0x140f1fc29 with actionType 1 (Pause), always
		// playingID in edx, transMs in r8d, curve 4 (Linear) in r9d. StopPlayingID (deprecated) is a thunk to
		// this; only ExecuteActionOnPlayingID survives in the build. It reserves an AK queue message
		// (QueueMgr::Reserve linchpin), so it is thread-safe to call from any thread, exactly like PostEvent.
		// ExecuteActionOnPlayingID(Stop, playingID, 0ms, Linear) cuts exactly that one voice instantly.
		constexpr REL::ID kAkStopVoiceID{ 150360 };

		// First 16 bytes of 150360 on 1.16.244 (mismatch on a future patch -> StopVoice self-disables).
		constexpr std::array<std::uint8_t, 16> kStopPrologue{
			0x85, 0xD2, 0x74, 0x6C, 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89
		};

		constexpr std::uint32_t kAkActionStop = 0;   // AkActionOnEventType_Stop
		constexpr std::uint32_t kAkCurveLinear = 4;  // AkCurveInterpolation_Linear (the value the engine passes)

		using ExecuteActionOnPlayingIDFn = void (*)(std::uint32_t /*action*/, std::uint32_t /*playingID*/,
		                                            std::int32_t /*transitionMs*/, std::uint32_t /*curve*/);
	}

	bool StopVoice(std::uint32_t a_playingID)
	{
		if (a_playingID == 0 || !Available()) {
			return false;  // nothing to stop, or the Wwise path is unavailable on this runtime
		}
		// Resolve + byte-gate once; self-disable on a moved/patched entry, exactly like Available()'s
		// PostEvent gate (we never call an unverified AK entry — a wrong target + playingID args could
		// corrupt AK's command queue).
		static const ExecuteActionOnPlayingIDFn stop = []() -> ExecuteActionOnPlayingIDFn {
			const auto* code = reinterpret_cast<const std::uint8_t*>(kAkStopVoiceID.address());
			if (!code || std::memcmp(code, kStopPrologue.data(), kStopPrologue.size()) != 0) {
				REX::WARN("[Audio] Wwise StopVoice disabled: AK ExecuteActionOnPlayingID (ID {}) prologue mismatch on this runtime — per-slot voice replace will let the prior clip play out", kAkStopVoiceID.id());
				return nullptr;
			}
			REX::DEBUG("[Audio] Wwise StopVoice available: AK ExecuteActionOnPlayingID prologue verified");
			return reinterpret_cast<ExecuteActionOnPlayingIDFn>(kAkStopVoiceID.address());
		}();
		if (stop == nullptr) {
			return false;
		}
		stop(kAkActionStop, a_playingID, 0, kAkCurveLinear);  // Stop, 0ms transition -> instant cut
		return true;
	}

	bool IsWwiseExternalSource(std::string_view a_path)
	{
		// Formats that ride the engine-mixed Wwise external-source path. A real .wem is posted as-is;
		// .wav/.mp3/.ogg/.flac are decoded to PCM and wrapped in a PCM .wem at runtime.
		// Anything else (a codec miniaudio can't decode) returns false and SoundService::Play skips the cue.
		const std::string ext = LowerExtension(a_path);
		return ext == "wem" || ext == "wav" || ext == "mp3" || ext == "ogg" || ext == "flac";
	}

	std::uint32_t PostExternalFile(const std::wstring& a_path)
	{
		if (!Available()) {
			return 0;
		}
		return PostExternalOn(kExternalSourceEvent, a_path, RE::BGSAudio::AkSoundEngine::kPlayerGameObject);
	}
}
