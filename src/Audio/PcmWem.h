#pragma once

// Wwise 2021.1 PCM .wem (AkCodecID kPCM=1) from interleaved s16 PCM; matches WwiseConsole `convert-external-source`.
// AK requires in-memory media 16-byte aligned, so a JUNK pad puts `data` at offset 64:
//   0   'RIFF' / size / 'WAVE'
//   12  'fmt ' / 24 / { tag 0xFFFE, channels, rate, avgBytes, blockAlign, 16, cbSize 6, 0, AkChannelConfig }
//   44  'JUNK' / 4 / <4 zero bytes>
//   56  'data' / dataBytes / <s16 PCM @ 64>

#include <cstdint>
#include <cstring>
#include <vector>

namespace OSF::Audio
{
	// numChannels | (eConfigType_Standard(1) << 8) | (speakerMask << 12). Only mono/stereo are RE-proven; callers downmix.
	inline std::uint32_t AkChannelConfigFor(std::uint32_t a_channels)
	{
		const std::uint32_t speakerMask = (a_channels == 1) ? 0x4u : 0x3u;
		return a_channels | (1u << 8) | (speakerMask << 12);
	}

	// Not aligned; the caller copies into a 16-byte-aligned buffer for the AK post.
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
		putU32(24u);
		putU16(0xFFFEu);                                      // wFormatTag = Wwise PCM marker
		putU16(static_cast<std::uint16_t>(a_channels));
		putU32(a_sampleRate);
		putU32(a_sampleRate * a_channels * 2u);
		putU16(static_cast<std::uint16_t>(a_channels * 2u));
		putU16(16u);
		putU16(6u);
		putU16(0u);
		putU32(AkChannelConfigFor(a_channels));

		putBytes("JUNK", 4);
		putU32(4u);
		putU32(0u);  // data content lands at offset 64

		putBytes("data", 4);
		putU32(dataBytes);
		if (dataBytes != 0) {
			putBytes(a_pcm, dataBytes);
		}

		return out;
	}
}
