#pragma once

#include "Paralax/Asset/Asset.h"
#include "Paralax/Core/Base.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace Paralax {

	// A sound, decoded in full to 32-bit float samples (interleaved by channel) when loaded: playing it then never
	// touches the disk or a decoder, and the same clip can play on any number of sources at once. Loads WAV, FLAC
	// and MP3. Owns its samples; immutable once loaded, so it may be read from any thread (the audio mixer does).
	class AudioClip : public Asset
	{
	public:
		// Untrusted files are bounded before decoding (and while decoding, for formats whose length is not known up
		// front).
		static constexpr size_t MaxEncodedBytes = 64ull * 1024 * 1024;
		static constexpr size_t MaxDecodedBytes = 256ull * 1024 * 1024;
		static constexpr uint32_t MaxChannels = 8;
		static constexpr uint32_t MinSampleRate = 8000;
		static constexpr uint32_t MaxSampleRate = 192000;

		AssetType GetType() const override { return AssetType::Audio; }

		// Returns nullptr (with an error logged) if the file cannot be read, is not a supported format, is malformed,
		// or exceeds the limits above.
		static Ref<AudioClip> LoadFromFile(const std::filesystem::path& path);
		// maxDecodedBytes lowers the decoded size limit (never raises it beyond MaxDecodedBytes).
		static Ref<AudioClip> LoadFromMemory(const void* data, size_t size, size_t maxDecodedBytes = MaxDecodedBytes);
		// A clip from samples already in memory (generated sounds, tests). Verifies the channel count, sample rate and
		// that samples holds at least one whole frame and only whole frames.
		static Ref<AudioClip> Create(uint32_t channels, uint32_t sampleRate, std::vector<float> samples);

		uint32_t GetChannels() const { return m_Channels; }
		uint32_t GetSampleRate() const { return m_SampleRate; }
		uint64_t GetFrameCount() const { return m_Channels ? m_Samples.size() / m_Channels : 0; }
		float GetDuration() const { return m_SampleRate ? static_cast<float>(GetFrameCount()) / static_cast<float>(m_SampleRate) : 0.0f; }
		// Interleaved samples: frame f, channel c is at f * channels + c.
		const std::vector<float>& GetSamples() const { return m_Samples; }

	private:
		uint32_t m_Channels = 0;
		uint32_t m_SampleRate = 0;
		std::vector<float> m_Samples;
	};

}
