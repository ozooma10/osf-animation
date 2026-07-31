#include "Audio/PcmWem.h"
#include "Registry/SoundRegistry.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{
	void Check(bool a_condition, const char* a_message)
	{
		if (!a_condition) {
			std::cerr << "FAIL: " << a_message << '\n';
			std::exit(1);
		}
	}

	std::uint16_t ReadU16(const std::vector<std::uint8_t>& a_bytes, std::size_t a_offset)
	{
		std::uint16_t value = 0;
		std::memcpy(&value, a_bytes.data() + a_offset, sizeof(value));
		return value;
	}

	std::uint32_t ReadU32(const std::vector<std::uint8_t>& a_bytes, std::size_t a_offset)
	{
		std::uint32_t value = 0;
		std::memcpy(&value, a_bytes.data() + a_offset, sizeof(value));
		return value;
	}
}

int main()
{
	// xmake runs this target with test/fixtures as cwd, so LoadAll sees Data/OSF.
	auto& reg = OSF::Registry::SoundRegistry::GetSingleton();
	reg.LoadAll();
	const auto files = reg.FileStats();
	bool sawFixture = false;
	for (const auto& file : files) {
		if (file.file == "sounds_text.sounds.json") {
			sawFixture = true;
			Check(file.errors == 0 && file.warnings == 0 && file.problems.empty(), "sound file stats: clean fixture");
			Check(file.path == "sounds_text.sounds.json", "sound file stats: Data/OSF-relative path");
		}
	}
	Check(sawFixture, "sound file stats: fixture present");

	Check(reg.TextForClip("Sound/OSF/Test/a.wav") == "Line A spoken.", "object-form subtitle");
	Check(reg.TextForClip("Sound/OSF/Test/silent.wav").empty(), "empty object-form subtitle");
	Check(reg.TextForClip("Sound/OSF/Test/b.wav") == "Line B spoken.", "array-form subtitle");
	Check(reg.TextForClip("Sound/OSF/Test/c.wav").empty(), "array clip without subtitle");
	Check(reg.TextForClip("Sound/OSF/Test/nope.wav").empty(), "unknown clip subtitle");
	Check(reg.Pick({ "test", "objform" }).has_value(), "object-form pool resolves");
	Check(reg.Pick({ "test", "arrform" }).has_value(), "array-form pool resolves");

	{
		const std::array<std::int16_t, 4> pcm{ 0, 1, -2, 0x1234 };
		const auto wem = OSF::Audio::BuildPcmWemBytes(pcm.data(), 2, 2, 48000);
		Check(wem.size() == 72, "PCM WEM includes its fixed header and stereo payload");
		Check(std::memcmp(wem.data(), "RIFF", 4) == 0 &&
			std::memcmp(wem.data() + 8, "WAVE", 4) == 0 &&
			std::memcmp(wem.data() + 12, "fmt ", 4) == 0, "PCM WEM RIFF chunks are placed correctly");
		Check(ReadU32(wem, 4) == wem.size() - 8 && ReadU32(wem, 16) == 24,
			"PCM WEM RIFF and extensible-format sizes are correct");
		Check(ReadU16(wem, 20) == 0xFFFE && ReadU16(wem, 22) == 2 && ReadU32(wem, 24) == 48000,
			"PCM WEM stereo format identity is correct");
		Check(ReadU32(wem, 28) == 192000 && ReadU16(wem, 32) == 4 && ReadU16(wem, 34) == 16,
			"PCM WEM stereo byte rate and sample geometry are correct");
		Check(ReadU16(wem, 36) == 6 && ReadU16(wem, 38) == 0 && ReadU32(wem, 40) == 0x00003102,
			"PCM WEM stereo AkChannelConfig is correct");
		Check(std::memcmp(wem.data() + 44, "JUNK", 4) == 0 && ReadU32(wem, 48) == 4 && ReadU32(wem, 52) == 0,
			"PCM WEM padding aligns the sample payload");
		Check(std::memcmp(wem.data() + 56, "data", 4) == 0 && ReadU32(wem, 60) == sizeof(pcm),
			"PCM WEM data chunk describes the payload");
		Check(std::memcmp(wem.data() + 64, pcm.data(), sizeof(pcm)) == 0,
			"PCM WEM copies samples at the aligned payload offset");
	}
	{
		const auto wem = OSF::Audio::BuildPcmWemBytes(nullptr, 0, 1, 44100);
		Check(wem.size() == 64 && ReadU32(wem, 60) == 0, "empty PCM WEM is a header-only file");
		Check(ReadU16(wem, 22) == 1 && ReadU32(wem, 28) == 88200 && ReadU16(wem, 32) == 2,
			"PCM WEM mono sample geometry is correct");
		Check(ReadU32(wem, 40) == 0x00004101, "PCM WEM mono AkChannelConfig is correct");
	}

	std::cout << "Sound registry and PCM/WEM tests passed\n";
	return 0;
}
