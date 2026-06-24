// Offline byte-layout tests for the runtime Wwise PCM .wem builder (Audio/PcmWem.h). Locks the
// RE-proven format (module systems.audio_wwise) so a future edit can't silently drift the header that
// makes a decoded loose file engine-mixable. No game / no WwiseConsole needed.
#include "framework/TestHarness.h"

#include "Audio/PcmWem.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using OSF::Audio::AkChannelConfigFor;
using OSF::Audio::BuildPcmWemBytes;

namespace
{
	std::uint16_t ReadU16(const std::vector<std::uint8_t>& a_b, std::size_t a_at)
	{
		return static_cast<std::uint16_t>(a_b[a_at] | (a_b[a_at + 1] << 8));
	}

	std::uint32_t ReadU32(const std::vector<std::uint8_t>& a_b, std::size_t a_at)
	{
		return static_cast<std::uint32_t>(a_b[a_at]) | (static_cast<std::uint32_t>(a_b[a_at + 1]) << 8) |
		       (static_cast<std::uint32_t>(a_b[a_at + 2]) << 16) | (static_cast<std::uint32_t>(a_b[a_at + 3]) << 24);
	}

	std::string Tag(const std::vector<std::uint8_t>& a_b, std::size_t a_at)
	{
		return std::string(reinterpret_cast<const char*>(a_b.data() + a_at), 4);
	}
}

OSF_TEST_CASE(AkChannelConfig_mono_and_stereo)
{
	// numChannels | (Standard<<8) | (speakerMask<<12): mono FC=0x4, stereo L|R=0x3.
	CHECK_EQ(AkChannelConfigFor(1), 0x00004101u);
	CHECK_EQ(AkChannelConfigFor(2), 0x00003102u);
}

OSF_TEST_CASE(PcmWem_mono_layout)
{
	const std::int16_t pcm[] = { 100, -200, 300 };  // 3 frames, mono
	const std::uint32_t rate = 44100;
	const auto b = BuildPcmWemBytes(pcm, 3, 1, rate);

	const std::uint32_t dataBytes = 3u * 1u * 2u;
	CHECK_EQ(b.size(), static_cast<std::size_t>(64u + dataBytes));

	// RIFF / WAVE
	CHECK_EQ(Tag(b, 0), std::string("RIFF"));
	CHECK_EQ(ReadU32(b, 4), static_cast<std::uint32_t>(b.size() - 8));
	CHECK_EQ(Tag(b, 8), std::string("WAVE"));

	// fmt chunk: EXTENSIBLE, 24-byte payload
	CHECK_EQ(Tag(b, 12), std::string("fmt "));
	CHECK_EQ(ReadU32(b, 16), 24u);
	CHECK_EQ(ReadU16(b, 20), 0xFFFEu);                 // wFormatTag
	CHECK_EQ(ReadU16(b, 22), 1u);                      // nChannels
	CHECK_EQ(ReadU32(b, 24), rate);                    // nSamplesPerSec
	CHECK_EQ(ReadU32(b, 28), rate * 1u * 2u);          // nAvgBytesPerSec
	CHECK_EQ(ReadU16(b, 32), 2u);                      // nBlockAlign = channels*2
	CHECK_EQ(ReadU16(b, 34), 16u);                     // wBitsPerSample
	CHECK_EQ(ReadU16(b, 36), 6u);                      // cbSize
	CHECK_EQ(ReadU16(b, 38), 0u);                      // reserved
	CHECK_EQ(ReadU32(b, 40), 0x00004101u);             // AkChannelConfig (mono)

	// JUNK pad aligns data content to 16
	CHECK_EQ(Tag(b, 44), std::string("JUNK"));
	CHECK_EQ(ReadU32(b, 48), 4u);
	CHECK_EQ(ReadU32(b, 52), 0u);                      // 4 zero pad bytes

	// data chunk: payload at offset 64, 16-aligned, equal to the input PCM
	CHECK_EQ(Tag(b, 56), std::string("data"));
	CHECK_EQ(ReadU32(b, 60), dataBytes);
	CHECK_EQ(static_cast<std::size_t>(64) % 16u, static_cast<std::size_t>(0));
	CHECK(std::memcmp(b.data() + 64, pcm, dataBytes) == 0);
}

OSF_TEST_CASE(PcmWem_stereo_header)
{
	const std::int16_t pcm[] = { 1, 2, 3, 4 };  // 2 frames, stereo
	const std::uint32_t rate = 48000;
	const auto b = BuildPcmWemBytes(pcm, 2, 2, rate);

	const std::uint32_t dataBytes = 2u * 2u * 2u;
	CHECK_EQ(b.size(), static_cast<std::size_t>(64u + dataBytes));
	CHECK_EQ(ReadU16(b, 22), 2u);                      // nChannels
	CHECK_EQ(ReadU32(b, 28), rate * 2u * 2u);          // nAvgBytesPerSec
	CHECK_EQ(ReadU16(b, 32), 4u);                      // nBlockAlign = channels*2
	CHECK_EQ(ReadU32(b, 40), 0x00003102u);             // AkChannelConfig (stereo)
	CHECK_EQ(ReadU32(b, 60), dataBytes);
	CHECK(std::memcmp(b.data() + 64, pcm, dataBytes) == 0);
}

OSF_TEST_CASE(PcmWem_empty_is_header_only)
{
	const auto b = BuildPcmWemBytes(nullptr, 0, 1, 22050);
	CHECK_EQ(b.size(), static_cast<std::size_t>(64));  // header only, data content lands at 64
	CHECK_EQ(ReadU32(b, 60), 0u);                      // dataBytes == 0
}
