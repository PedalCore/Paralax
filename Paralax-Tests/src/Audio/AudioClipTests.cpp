#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Audio/AudioClip.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

using namespace Paralax;
using Paralax::Testing::MakeWav;

namespace {

	void WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	}

}

TEST_CASE("AudioClip: WAV files decode to float samples at their own channel count and rate")
{
	const std::vector<float> samples = {0.0f, 0.5f, -0.5f, 1.0f, 0.25f, -1.0f}; // 3 stereo frames.
	const std::vector<uint8_t> wav = MakeWav(2, 22050, samples);
	Ref<AudioClip> clip = AudioClip::LoadFromMemory(wav.data(), wav.size());
	REQUIRE(clip);
	CHECK(clip->GetType() == AssetType::Audio);
	CHECK(clip->GetChannels() == 2);
	CHECK(clip->GetSampleRate() == 22050);
	CHECK(clip->GetFrameCount() == 3);
	CHECK(clip->GetDuration() == doctest::Approx(3.0 / 22050.0));
	REQUIRE(clip->GetSamples().size() == samples.size());
	for (size_t i = 0; i < samples.size(); i++)
		CHECK(clip->GetSamples()[i] == doctest::Approx(static_cast<double>(samples[i])).epsilon(0.001));

	// 32-bit float WAV too; values beyond [-1, 1] and non-finite ones are made safe for the mixer.
	const std::vector<float> wild = {0.5f, 3.0f, -7.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()};
	const std::vector<uint8_t> floatWav = MakeWav(1, 48000, wild, 32);
	Ref<AudioClip> floats = AudioClip::LoadFromMemory(floatWav.data(), floatWav.size());
	REQUIRE(floats);
	CHECK(floats->GetSamples() == std::vector<float>{0.5f, 1.0f, -1.0f, 0.0f, 0.0f});
}

TEST_CASE("AudioClip: clips load from files and through the asset manager")
{
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("AudioClipFiles");
	WriteBytes(directory / "Sounds" / "Beep.wav", MakeWav(1, 44100, std::vector<float>(441, 0.25f)));
	Ref<AudioClip> clip = AudioClip::LoadFromFile(directory / "Sounds" / "Beep.wav");
	REQUIRE(clip);
	CHECK(clip->GetFrameCount() == 441);

	AssetManager assets(directory);
	const AssetHandle handle = assets.ImportAsset("Sounds/Beep.wav");
	REQUIRE(handle.IsValid());
	CHECK(assets.GetAssetType(handle) == AssetType::Audio);
	Ref<AudioClip> managed = assets.GetAsset<AudioClip>(handle);
	REQUIRE(managed);
	CHECK(managed->GetSampleRate() == 44100);
	CHECK(managed->Handle == handle);
}

TEST_CASE("AudioClip: hostile and unsupported files are rejected with an error")
{
	Paralax::Testing::ScopedLogCapture log;
	const std::filesystem::path directory = Paralax::Testing::CreateTestDirectory("AudioClipHostile");

	CHECK_FALSE(AudioClip::LoadFromMemory(nullptr, 10));
	const std::vector<uint8_t> garbage(512, 0x5A);
	CHECK_FALSE(AudioClip::LoadFromMemory(garbage.data(), garbage.size()));
	CHECK_FALSE(AudioClip::LoadFromFile(directory / "Missing.wav"));
	WriteBytes(directory / "Empty.wav", {});
	CHECK_FALSE(AudioClip::LoadFromFile(directory / "Empty.wav"));

	// Unsupported layouts: too many channels, a sample rate outside the supported range.
	const std::vector<uint8_t> nine = MakeWav(9, 48000, std::vector<float>(90, 0.0f));
	CHECK_FALSE(AudioClip::LoadFromMemory(nine.data(), nine.size()));
	const std::vector<uint8_t> slow = MakeWav(1, 1000, std::vector<float>(100, 0.0f));
	CHECK_FALSE(AudioClip::LoadFromMemory(slow.data(), slow.size()));

	// A header that claims far more data than the file holds decodes what is there, without reading past the end.
	std::vector<uint8_t> lying = MakeWav(1, 48000, std::vector<float>(100, 0.5f));
	lying[40] = 0xFF; // data chunk size: 0x7FFFFFxx bytes.
	lying[41] = 0xFF;
	lying[42] = 0xFF;
	lying[43] = 0x7F;
	Ref<AudioClip> truncated = AudioClip::LoadFromMemory(lying.data(), lying.size());
	REQUIRE(truncated);
	CHECK(truncated->GetFrameCount() == 100);

	// Truncated in the middle of the header.
	const std::vector<uint8_t> whole = MakeWav(1, 48000, std::vector<float>(100, 0.5f));
	const std::vector<uint8_t> cut(whole.begin(), whole.begin() + 30);
	CHECK_FALSE(AudioClip::LoadFromMemory(cut.data(), cut.size()));

	// Files above the encoded size limit are refused before anything is read (a sparse file, so this is cheap).
	const std::filesystem::path huge = directory / "Huge.wav";
	WriteBytes(huge, whole);
	std::filesystem::resize_file(huge, AudioClip::MaxEncodedBytes + 1);
	CHECK_FALSE(AudioClip::LoadFromFile(huge));
	CHECK(log.Contains(spdlog::level::err, "larger than"));
}

TEST_CASE("AudioClip: FLAC and MP3 files decode too")
{
	// FLAC: the sample shipped with the pinned miniaudio sources (CMake passes its path).
	Ref<AudioClip> flac = AudioClip::LoadFromFile(PX_TEST_FLAC_FILE);
	REQUIRE(flac);
	CHECK(flac->GetChannels() == 2);
	CHECK(flac->GetSampleRate() == 44100);
	CHECK(flac->GetDuration() > 1.0f);
	float peak = 0.0f;
	for (float sample : flac->GetSamples())
		peak = std::max(peak, std::abs(sample));
	CHECK(peak > 0.01f);

	// MP3: MPEG-1 layer III frames (128 kbit/s, 44.1 kHz, mono) whose side information is all zero, which decode to
	// silence. 417 bytes each: 144 * 128000 / 44100, rounded down, without padding.
	std::vector<uint8_t> mp3;
	for (int frame = 0; frame < 20; frame++)
	{
		const uint8_t header[] = {0xFF, 0xFB, 0x90, 0xC0};
		mp3.insert(mp3.end(), header, header + 4);
		mp3.insert(mp3.end(), 417 - 4, 0);
	}
	Ref<AudioClip> silence = AudioClip::LoadFromMemory(mp3.data(), mp3.size());
	REQUIRE(silence);
	CHECK(silence->GetChannels() == 1);
	CHECK(silence->GetSampleRate() == 44100);
	CHECK(silence->GetFrameCount() >= 1152);
	for (float sample : silence->GetSamples())
		REQUIRE(sample == 0.0f);
}

TEST_CASE("AudioClip: decoding stops at the decoded size limit")
{
	Paralax::Testing::ScopedLogCapture log;
	const std::vector<uint8_t> wav = MakeWav(1, 8000, std::vector<float>(1000, 0.5f));
	CHECK(AudioClip::LoadFromMemory(wav.data(), wav.size(), 1000 * sizeof(float))); // Exactly fits.
	CHECK_FALSE(AudioClip::LoadFromMemory(wav.data(), wav.size(), 999 * sizeof(float)));
	CHECK(log.Contains(spdlog::level::err, "decoded size limit"));
	// The limit can be lowered, never raised.
	CHECK(AudioClip::LoadFromMemory(wav.data(), wav.size(), std::numeric_limits<size_t>::max()));
}

TEST_CASE("AudioClip: Create verifies channels, sample rate and whole frames")
{
	Ref<AudioClip> clip = AudioClip::Create(2, 48000, {0.1f, 0.2f, 0.3f, 0.4f});
	CHECK(clip->GetFrameCount() == 2);

	Paralax::Testing::ScopedAssertionCapture capture;
	CHECK_THROWS_AS(AudioClip::Create(0, 48000, {}), Paralax::Testing::AssertionFailedError);
	CHECK_THROWS_AS(AudioClip::Create(9, 48000, {}), Paralax::Testing::AssertionFailedError);
	CHECK_THROWS_AS(AudioClip::Create(1, 100, {0.0f}), Paralax::Testing::AssertionFailedError);
	CHECK_THROWS_AS(AudioClip::Create(2, 48000, {0.0f, 0.0f, 0.0f}), Paralax::Testing::AssertionFailedError);
	CHECK_THROWS_AS(AudioClip::Create(1, 48000, {}), Paralax::Testing::AssertionFailedError); // No frames: nothing to play.
}
