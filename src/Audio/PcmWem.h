#pragma once

// Dependency-free builder for a Wwise 2021.1 PCM .wem (AkCodecID kPCM=1) from raw interleaved
// 16-bit PCM. Kept free of CommonLibSF/miniaudio/RE on purpose so the byte layout is verifiable
// offline (no game, no WwiseConsole). WwiseBackend.cpp wraps the bytes into a
// 16-byte-aligned AK in-memory buffer for the actual external-source post.
//
// The format is RE-proven (module systems.audio_wwise: "external-source media MUST be a Wwise-encoded
// .wem") and byte-for-byte matches WwiseConsole's `convert-external-source` PCM output. The `data`
// payload deliberately starts at file offset 64 (16-byte aligned, which AK requires for in-memory
// media), produced by a fixed 4-byte JUNK pad after the 24-byte WAVE_FORMAT_EXTENSIBLE fmt chunk:
//   0   'RIFF' / size / 'WAVE'
//   12  'fmt ' / 24 / { tag 0xFFFE, channels, rate, avgBytes, blockAlign, 16, cbSize 6, 0, AkChannelConfig }
//   44  'JUNK' / 4 / <4 zero bytes>          (aligns data content to offset 64)
//   56  'data' / dataBytes / <s16 PCM @ 64>

#include <cstdint>
#include <cstring>
#include <vector>

namespace OSF::Audio
{
	// AkChannelConfig (Wwise 2021.1 AkSpeakerConfig packing): numChannels | (eConfigType_Standard(1) << 8)
	// | (speakerMask << 12). Only mono (front-center mask 0x4 -> 0x00004101) and stereo (L|R mask 0x3 ->
	// 0x00003102) are RE-proven; callers downmix anything wider to stereo, so a_channels is 1 or 2.
	inline std::uint32_t AkChannelConfigFor(std::uint32_t a_channels)
	{
		const std::uint32_t speakerMask = (a_channels == 1) ? 0x4u : 0x3u;
		return a_channels | (1u << 8) | (speakerMask << 12);
	}

	// Builds the full .wem byte image (header + PCM). The returned bytes are NOT specially aligned; the
	// caller copies them into a 16-byte-aligned buffer for the AK post.
	inline std::vector<std::uint8_t> BuildPcmWemBytes(const std::int16_t* a_pcm, std::size_t a_frames,
	                                                  std::uint32_t a_channels, std::uint32_t a_sampleRate)
	{
		const std::uint32_t dataBytes =
			static_cast<std::uint32_t>(a_frames * a_channels * sizeof(std::int16_t));
		const std::uint32_t totalSize = 64u + dataBytes;

		std::vector<std::uint8_t> out(totalSize);
		std::size_t at = 0;
		const auto putBytes = [&](const void* a_src, std::size_t a_n) {
			std::memcpy(out.data() + at, a_src, a_n);
			at += a_n;
		};
		const auto putU16 = [&](std::uint16_t a_v) { putBytes(&a_v, 2); };
		const auto putU32 = [&](std::uint32_t a_v) { putBytes(&a_v, 4); };

		putBytes("RIFF", 4);
		putU32(totalSize - 8u);  // RIFF chunk size = file size - 8
		putBytes("WAVE", 4);

		putBytes("fmt ", 4);
		putU32(24u);                                          // EXTENSIBLE fmt payload size
		putU16(0xFFFEu);                                      // wFormatTag = Wwise PCM marker
		putU16(static_cast<std::uint16_t>(a_channels));       // nChannels
		putU32(a_sampleRate);                                 // nSamplesPerSec
		putU32(a_sampleRate * a_channels * 2u);               // nAvgBytesPerSec
		putU16(static_cast<std::uint16_t>(a_channels * 2u));  // nBlockAlign
		putU16(16u);                                          // wBitsPerSample
		putU16(6u);                                           // cbSize
		putU16(0u);                                           // reserved
		putU32(AkChannelConfigFor(a_channels));               // AkChannelConfig

		putBytes("JUNK", 4);
		putU32(4u);
		putU32(0u);  // 4 zero pad bytes -> data content lands at offset 64

		putBytes("data", 4);
		putU32(dataBytes);
		if (dataBytes != 0) {
			putBytes(a_pcm, dataBytes);
		}

		return out;
	}
}
