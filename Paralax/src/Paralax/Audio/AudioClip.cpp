#include "pxpch.h"
#include "Paralax/Audio/AudioClip.h"

#include "Paralax/Core/PathEncoding.h"

#include <miniaudio.h>

#include <fstream>

namespace Paralax {

	Ref<AudioClip> AudioClip::LoadFromFile(const std::filesystem::path& path)
	{
		const std::string name = ToUtf8(path);
		std::error_code error;
		const uintmax_t size = std::filesystem::file_size(path, error);
		if (error || size == 0 || size > MaxEncodedBytes)
		{
			PX_CORE_ERROR("Cannot load audio '{}': {}", name, error ? error.message() : (size == 0 ? std::string("empty file") : fmt::format("larger than {} MiB", MaxEncodedBytes >> 20)));
			return nullptr;
		}
		std::vector<char> bytes(static_cast<size_t>(size));
		std::ifstream file(path, std::ios::binary);
		if (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
		{
			PX_CORE_ERROR("Cannot load audio '{}': the file could not be read", name);
			return nullptr;
		}
		Ref<AudioClip> clip = LoadFromMemory(bytes.data(), bytes.size());
		if (!clip)
			PX_CORE_ERROR("Cannot load audio '{}'", name);
		return clip;
	}

	Ref<AudioClip> AudioClip::LoadFromMemory(const void* data, size_t size, size_t maxDecodedBytes)
	{
		maxDecodedBytes = std::min(maxDecodedBytes, MaxDecodedBytes);
		if (!data || size == 0 || size > MaxEncodedBytes)
		{
			PX_CORE_ERROR("Cannot decode audio: empty or larger than {} MiB", MaxEncodedBytes >> 20);
			return nullptr;
		}
		// Decoded to 32-bit float at the file's own channel count and sample rate (0 = native).
		const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
		ma_decoder decoder;
		if (ma_decoder_init_memory(data, size, &config, &decoder) != MA_SUCCESS)
		{
			PX_CORE_ERROR("Cannot decode audio: not a WAV, FLAC or MP3 file, or malformed");
			return nullptr;
		}
		const uint32_t channels = decoder.outputChannels;
		const uint32_t sampleRate = decoder.outputSampleRate;
		if (channels == 0 || channels > MaxChannels || sampleRate < MinSampleRate || sampleRate > MaxSampleRate)
		{
			ma_decoder_uninit(&decoder);
			PX_CORE_ERROR("Cannot decode audio: {} channels at {} Hz is not supported (1-{} channels, {}-{} Hz)", channels, sampleRate, MaxChannels, MinSampleRate, MaxSampleRate);
			return nullptr;
		}

		// Read in blocks rather than trusting a length from the header (it may be absent or a lie), stopping at the
		// decoded size limit.
		// Growing the buffer block by block can briefly hold about 1.5 times this while it reallocates; bounded either way.
		const size_t maxFrames = maxDecodedBytes / (sizeof(float) * channels);
		constexpr size_t BlockFrames = 4096;
		std::vector<float> samples;
		bool tooLong = false;
		for (;;)
		{
			const size_t frames = samples.size() / channels;
			if (frames >= maxFrames)
			{
				float probe[MaxChannels];
				ma_uint64 extra = 0;
				tooLong = ma_decoder_read_pcm_frames(&decoder, probe, 1, &extra) == MA_SUCCESS && extra > 0;
				break;
			}
			const size_t request = std::min(BlockFrames, maxFrames - frames);
			samples.resize((frames + request) * channels);
			ma_uint64 read = 0;
			const ma_result result = ma_decoder_read_pcm_frames(&decoder, samples.data() + frames * channels, request, &read);
			samples.resize((frames + static_cast<size_t>(read)) * channels);
			if (result != MA_SUCCESS || read < request)
			{
				if (result != MA_SUCCESS && result != MA_AT_END)
				{
					ma_decoder_uninit(&decoder);
					PX_CORE_ERROR("Cannot decode audio: the data is corrupt ({})", ma_result_description(result));
					return nullptr;
				}
				break;
			}
		}
		ma_decoder_uninit(&decoder);
		if (tooLong)
		{
			PX_CORE_ERROR("Cannot decode audio: longer than the decoded size limit ({} bytes)", maxDecodedBytes);
			return nullptr;
		}
		if (samples.empty())
		{
			PX_CORE_ERROR("Cannot decode audio: it contains no samples");
			return nullptr;
		}
		// Decoders pass through whatever the file holds; keep the mixer away from NaN, infinities and absurd peaks.
		for (float& sample : samples)
			sample = std::isfinite(sample) ? std::clamp(sample, -1.0f, 1.0f) : 0.0f;
		return Create(channels, sampleRate, std::move(samples));
	}

	Ref<AudioClip> AudioClip::Create(uint32_t channels, uint32_t sampleRate, std::vector<float> samples)
	{
		PX_CORE_VERIFY(channels >= 1 && channels <= MaxChannels, "An audio clip needs 1 to 8 channels");
		PX_CORE_VERIFY(sampleRate >= MinSampleRate && sampleRate <= MaxSampleRate, "Unsupported audio sample rate");
		PX_CORE_VERIFY(!samples.empty() && samples.size() % channels == 0, "Audio samples must hold one or more whole frames");
		auto clip = CreateRef<AudioClip>();
		clip->m_Channels = channels;
		clip->m_SampleRate = sampleRate;
		clip->m_Samples = std::move(samples);
		return clip;
	}

}
