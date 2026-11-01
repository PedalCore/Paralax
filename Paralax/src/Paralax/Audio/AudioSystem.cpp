#include "pxpch.h"
#include "Paralax/Audio/AudioSystem.h"

#include "Paralax/Asset/AssetManager.h"
#include "Paralax/Audio/AudioClip.h"
#include "Paralax/Scene/Components.h"
#include "Paralax/Scene/Entity.h"
#include "Paralax/Scene/Scene.h"

#include <miniaudio.h>

#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace Paralax {

	namespace {

		bool IsFinite(const glm::vec3& v)
		{
			return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
		}

		// Keeps values from scene data (possibly hand-edited files) in the ranges miniaudio handles sensibly.
		float SafeVolume(float volume)
		{
			return std::isfinite(volume) ? std::clamp(volume, 0.0f, 10.0f) : 0.0f;
		}

		float SafePitch(float pitch)
		{
			return std::isfinite(pitch) ? std::clamp(pitch, 0.1f, 4.0f) : 1.0f;
		}

		float SafeDistance(float distance)
		{
			return std::isfinite(distance) ? std::clamp(distance, 0.01f, 100000.0f) : 1.0f;
		}

		constexpr uint32_t OutputChannels = 2;

		// One sounding clip. miniaudio keeps pointers to the buffer and the sound, so a voice never moves once created
		// (it lives behind a Scope), and the clip it reads from stays alive with it. A voice is made for each play and
		// discarded when it ends or is stopped, so a finished sound never lingers half-stopped inside miniaudio.
		struct Voice
		{
			Ref<AudioClip> Clip;
			ma_audio_buffer Buffer{};
			ma_sound Sound{};
			bool BufferReady = false;
			bool SoundReady = false;
			// How far into the clip the voice is, in clip frames, by simulated time. This, not the mixer, decides when
			// a voice ends, so what scripts observe is the same with a device as when mixing offline.
			double Position = 0.0;
			float Pitch = 1.0f;
			bool Looping = false;
			bool Paused = false;

			Voice() = default;
			Voice(const Voice&) = delete;
			Voice& operator=(const Voice&) = delete;
			~Voice()
			{
				// The sound reads from the buffer, so it goes first. Uninitialising a sound detaches it from the mixer
				// safely while a device thread is mixing.
				if (SoundReady)
					ma_sound_uninit(&Sound);
				if (BufferReady)
					ma_audio_buffer_uninit(&Buffer);
			}

			// Advances by simulated time; returns false once a non-looping voice has played to the end.
			bool Advance(float seconds)
			{
				if (Paused)
					return true;
				const double frames = static_cast<double>(Clip->GetFrameCount());
				Position += static_cast<double>(seconds) * static_cast<double>(Pitch) * static_cast<double>(Clip->GetSampleRate());
				if (Position < frames)
					return true;
				if (!Looping)
					return false;
				Position = std::fmod(Position, frames);
				return true;
			}
		};

		// An entity's AudioSource as the system tracks it.
		struct Source
		{
			Scope<Voice> Sounding; // While playing or paused.
			// The clip the source had when last seen, to notice a change (or a reload) while it plays.
			Ref<AudioClip> Clip;
			uint32_t ClipVersion = 0;
		};

	}

	struct AudioSystem::Impl
	{
		Impl(AssetManager& assets, AudioSystemSpecification specification)
			: Assets(assets)
			, Specification(specification)
		{
		}

		AssetManager& Assets;
		AudioSystemSpecification Specification;
		Scene* ActiveScene = nullptr;

		ma_engine Engine{};
		bool EngineReady = false;
		AudioOutput Output = AudioOutput::Offline;
		float MasterVolume = 1.0f;
		bool Paused = false;

		std::unordered_map<uint64_t, Source> Sources; // By entity UUID; every AudioSource seen while running.
		std::vector<Scope<Voice>> OneShots;
		bool WarnedVoiceLimit = false;

		// Offline mixing.
		double PendingFrames = 0.0;
		std::vector<float> Scratch;
		std::vector<float> Captured;

		uint32_t VoiceCount() const
		{
			size_t count = OneShots.size();
			for (const auto& [id, source] : Sources)
				count += source.Sounding ? 1 : 0;
			return static_cast<uint32_t>(count);
		}

		bool Start(Scene& scene)
		{
			ActiveScene = &scene;
			if (Specification.Output == AudioOutput::Device)
			{
				ma_engine_config config = ma_engine_config_init();
				if (ma_engine_init(&config, &Engine) == MA_SUCCESS)
				{
					EngineReady = true;
					Output = AudioOutput::Device;
				}
				else
					PX_CORE_WARN("Audio: no output device is available; sound is mixed offline and not heard");
			}
			if (!EngineReady)
			{
				ma_engine_config config = ma_engine_config_init();
				config.noDevice = MA_TRUE;
				config.channels = OutputChannels;
				config.sampleRate = Specification.SampleRate;
				if (ma_engine_init(&config, &Engine) != MA_SUCCESS)
				{
					PX_CORE_ERROR("Audio: the mixer could not be initialised; the scene plays without sound");
					ActiveScene = nullptr;
					return false;
				}
				EngineReady = true;
				Output = AudioOutput::Offline;
			}
			ma_engine_set_volume(&Engine, MasterVolume);
			if (Paused && Output == AudioOutput::Device)
				ma_engine_stop(&Engine);
			PendingFrames = 0.0;
			Captured.clear();
			Sync(scene);
			return true;
		}

		void Stop()
		{
			// Voices first: they are attached to the engine's node graph.
			Sources.clear();
			OneShots.clear();
			if (EngineReady)
				ma_engine_uninit(&Engine);
			EngineReady = false;
			Output = AudioOutput::Offline;
			WarnedVoiceLimit = false;
			ActiveScene = nullptr;
		}

		bool ReserveVoice()
		{
			if (VoiceCount() < Specification.MaxVoices)
				return true;
			if (!WarnedVoiceLimit)
				PX_CORE_WARN("Audio: {} sounds are already playing; further ones are skipped", Specification.MaxVoices);
			WarnedVoiceLimit = true;
			return false;
		}

		// A stopped voice for a clip; nullptr if the voice limit is reached or miniaudio refuses it (logged).
		Scope<Voice> CreateVoice(const Ref<AudioClip>& clip)
		{
			if (!ReserveVoice())
				return nullptr;
			auto voice = CreateScope<Voice>();
			voice->Clip = clip;
			ma_audio_buffer_config config = ma_audio_buffer_config_init(ma_format_f32, clip->GetChannels(), clip->GetFrameCount(), clip->GetSamples().data(), nullptr);
			config.sampleRate = clip->GetSampleRate();
			voice->BufferReady = ma_audio_buffer_init(&config, &voice->Buffer) == MA_SUCCESS;
			voice->SoundReady = voice->BufferReady && ma_sound_init_from_data_source(&Engine, &voice->Buffer, 0, nullptr, &voice->Sound) == MA_SUCCESS;
			if (!voice->SoundReady)
			{
				PX_CORE_ERROR("Audio: cannot create a voice");
				return nullptr;
			}
			// Positions are set directly each frame, without velocities: no Doppler shift.
			ma_sound_set_doppler_factor(&voice->Sound, 0.0f);
			return voice;
		}

		static void Apply(Voice& voice, const AudioSourceComponent& source, const glm::vec3& position)
		{
			voice.Pitch = SafePitch(source.Pitch);
			voice.Looping = source.Loop;
			ma_sound_set_volume(&voice.Sound, SafeVolume(source.Volume));
			ma_sound_set_pitch(&voice.Sound, voice.Pitch);
			ma_sound_set_looping(&voice.Sound, source.Loop ? MA_TRUE : MA_FALSE);
			ma_sound_set_spatialization_enabled(&voice.Sound, source.Spatial ? MA_TRUE : MA_FALSE);
			const float minDistance = SafeDistance(source.MinDistance);
			ma_sound_set_min_distance(&voice.Sound, minDistance);
			ma_sound_set_max_distance(&voice.Sound, std::max(SafeDistance(source.MaxDistance), minDistance));
			if (IsFinite(position))
				ma_sound_set_position(&voice.Sound, position.x, position.y, position.z);
		}

		Ref<AudioClip> LoadClip(const AudioSourceComponent& source)
		{
			Ref<AudioClip> clip = source.Clip.IsValid() ? Assets.GetAsset<AudioClip>(source.Clip) : nullptr;
			return clip && clip->GetFrameCount() > 0 ? clip : nullptr;
		}

		// Starts a fresh voice for an entity's source (replacing any it had). Returns false if it cannot sound.
		bool StartSource(Entity entity, const AudioSourceComponent& component, Source& source, bool paused = false)
		{
			source.Sounding.reset();
			source.Clip = LoadClip(component);
			source.ClipVersion = source.Clip ? source.Clip->Version : 0;
			if (!source.Clip)
				return false;
			Scope<Voice> voice = CreateVoice(source.Clip);
			if (!voice)
				return false;
			Apply(*voice, component, glm::vec3(ActiveScene->GetWorldTransform(entity)[3]));
			voice->Paused = paused;
			if (!paused)
				ma_sound_start(&voice->Sound);
			source.Sounding = std::move(voice);
			return true;
		}

		void UpdateListener(Scene& scene)
		{
			std::optional<Entity> listener;
			scene.ForEachEntity([&](Entity entity) {
				if (listener)
					return;
				const auto* component = entity.TryGetComponent<AudioListenerComponent>();
				if (component && component->Active)
					listener = entity;
			});
			if (!listener)
				listener = scene.GetPrimaryCameraEntity();
			glm::vec3 position(0.0f);
			glm::vec3 forward(0.0f, 0.0f, -1.0f);
			if (listener)
			{
				const glm::mat4 world = scene.GetWorldTransform(*listener);
				const glm::vec3 translation(world[3]);
				const glm::vec3 facing = -glm::vec3(world[2]);
				if (IsFinite(translation))
					position = translation;
				if (IsFinite(facing) && glm::length(facing) > 1e-6f)
					forward = glm::normalize(facing);
			}
			ma_engine_listener_set_position(&Engine, 0, position.x, position.y, position.z);
			ma_engine_listener_set_direction(&Engine, 0, forward.x, forward.y, forward.z);
			ma_engine_listener_set_world_up(&Engine, 0, 0.0f, 1.0f, 0.0f);
		}

		// Brings the voices in line with the scene's sources. A source seen for the first time starts if it plays on
		// start; one whose clip changed (or was reloaded) carries on with the new clip from its beginning, in the same
		// playing or paused state.
		void Sync(Scene& scene)
		{
			UpdateListener(scene);
			std::unordered_set<uint64_t> seen;
			scene.ForEachEntity([&](Entity entity) {
				const auto* component = entity.TryGetComponent<AudioSourceComponent>();
				if (!component)
					return;
				const uint64_t id = entity.GetUUID();
				seen.insert(id);
				auto [it, firstSight] = Sources.try_emplace(id);
				Source& source = it->second;
				if (firstSight)
				{
					if (component->PlayOnStart)
						StartSource(entity, *component, source);
					else
					{
						source.Clip = LoadClip(*component);
						source.ClipVersion = source.Clip ? source.Clip->Version : 0;
					}
					return;
				}
				Ref<AudioClip> clip = LoadClip(*component);
				if (clip != source.Clip || (clip && clip->Version != source.ClipVersion))
				{
					if (source.Sounding)
						StartSource(entity, *component, source, source.Sounding->Paused);
					else
					{
						source.Clip = clip;
						source.ClipVersion = clip ? clip->Version : 0;
					}
					return;
				}
				if (source.Sounding)
					Apply(*source.Sounding, *component, glm::vec3(scene.GetWorldTransform(entity)[3]));
			});
			// Sources whose component (or entity) is gone.
			std::erase_if(Sources, [&](const auto& entry) { return !seen.contains(entry.first); });
			if (VoiceCount() < Specification.MaxVoices)
				WarnedVoiceLimit = false;
		}

		// Ends voices that have played to the end, by simulated time.
		void Advance(float seconds)
		{
			if (!(seconds > 0.0f) || !std::isfinite(seconds))
				return;
			for (auto& [id, source] : Sources)
			{
				if (source.Sounding && !source.Sounding->Advance(seconds))
					source.Sounding.reset();
			}
			std::erase_if(OneShots, [&](const Scope<Voice>& voice) { return !voice->Advance(seconds); });
		}

		void Mix(float seconds)
		{
			if (Output != AudioOutput::Offline || !(seconds > 0.0f) || !std::isfinite(seconds))
				return;
			// Fractional frames carry over, so the mixed length follows simulated time.
			PendingFrames += static_cast<double>(seconds) * static_cast<double>(Specification.SampleRate);
			uint64_t frames = static_cast<uint64_t>(PendingFrames);
			PendingFrames -= static_cast<double>(frames);
			// Nothing hears offline output, so a long hitch is not mixed in full (at most one second per update); the
			// sounds then lag the simulation by the rest, while their playing state still follows simulated time.
			frames = std::min<uint64_t>(frames, Specification.SampleRate);
			constexpr uint64_t BlockFrames = 1024;
			Scratch.resize(BlockFrames * OutputChannels);
			while (frames > 0)
			{
				const uint64_t block = std::min(frames, BlockFrames);
				ma_uint64 read = 0;
				ma_engine_read_pcm_frames(&Engine, Scratch.data(), block, &read);
				// With nothing playing the mixer produces no frames at all: that stretch is silence.
				std::fill(Scratch.begin() + static_cast<std::ptrdiff_t>(read * OutputChannels), Scratch.begin() + static_cast<std::ptrdiff_t>(block * OutputChannels), 0.0f);
				const uint64_t capturedFrames = Captured.size() / OutputChannels;
				const uint64_t keep = capturedFrames < Specification.MaxCapturedFrames ? std::min<uint64_t>(block, Specification.MaxCapturedFrames - capturedFrames) : 0;
				Captured.insert(Captured.end(), Scratch.begin(), Scratch.begin() + static_cast<std::ptrdiff_t>(keep * OutputChannels));
				frames -= block;
			}
		}

		// The entity and its source component, if it has one, while running.
		std::optional<std::pair<Entity, const AudioSourceComponent*>> FindComponent(UUID id) const
		{
			if (!ActiveScene || !EngineReady)
				return std::nullopt;
			std::optional<Entity> entity = ActiveScene->TryGetEntityByUUID(id);
			const auto* component = entity ? entity->TryGetComponent<AudioSourceComponent>() : nullptr;
			if (!component)
				return std::nullopt;
			return std::pair{*entity, component};
		}
	};

	AudioSystem::AudioSystem(AssetManager& assets, AudioSystemSpecification specification)
		: m_Impl(CreateScope<Impl>(assets, specification))
	{
		PX_CORE_VERIFY(specification.SampleRate >= AudioClip::MinSampleRate && specification.SampleRate <= AudioClip::MaxSampleRate, "Unsupported audio sample rate");
		PX_CORE_VERIFY(specification.MaxVoices >= 1 && specification.MaxVoices <= 4096, "MaxVoices must be in [1, 4096]");
	}

	AudioSystem::~AudioSystem()
	{
		m_Impl->Stop();
	}

	void AudioSystem::OnRuntimeStart(Scene& scene)
	{
		m_Impl->Stop();
		m_Impl->Start(scene);
	}

	void AudioSystem::OnUpdate(Scene& scene, Timestep timestep)
	{
		if (m_Impl->ActiveScene != &scene || !m_Impl->EngineReady)
			return;
		m_Impl->Sync(scene);
		m_Impl->Mix(timestep.GetSeconds());
		m_Impl->Advance(timestep.GetSeconds());
	}

	void AudioSystem::OnRuntimeStop(Scene& scene)
	{
		if (m_Impl->ActiveScene == &scene)
			m_Impl->Stop();
	}

	void AudioSystem::OnEntityDestroying(Scene& scene, const Entity& entity)
	{
		if (m_Impl->ActiveScene == &scene)
			m_Impl->Sources.erase(entity.GetUUID());
	}

	bool AudioSystem::Play(UUID entity)
	{
		auto found = m_Impl->FindComponent(entity);
		if (!found)
			return false;
		// A source first touched here counts as seen, so PlayOnStart does not start it a second time.
		Source& source = m_Impl->Sources[entity];
		if (source.Sounding && source.Sounding->Paused)
		{
			ma_sound_start(&source.Sounding->Sound);
			source.Sounding->Paused = false;
			return true;
		}
		return m_Impl->StartSource(found->first, *found->second, source);
	}

	bool AudioSystem::Pause(UUID entity)
	{
		auto found = m_Impl->FindComponent(entity);
		if (!found || !m_Impl->LoadClip(*found->second))
			return false;
		// Seen from here on: PlayOnStart no longer applies to a source paused before it started.
		Source& source = m_Impl->Sources[entity];
		if (source.Sounding && !source.Sounding->Paused)
		{
			ma_sound_stop(&source.Sounding->Sound);
			source.Sounding->Paused = true;
		}
		return true;
	}

	bool AudioSystem::Stop(UUID entity)
	{
		auto found = m_Impl->FindComponent(entity);
		if (!found || !m_Impl->LoadClip(*found->second))
			return false;
		// Seen from here on: PlayOnStart no longer applies to a source stopped before it started.
		m_Impl->Sources[entity].Sounding.reset();
		return true;
	}

	bool AudioSystem::IsPlaying(UUID entity) const
	{
		auto it = m_Impl->Sources.find(entity);
		return it != m_Impl->Sources.end() && it->second.Sounding && !it->second.Sounding->Paused;
	}

	bool AudioSystem::PlayOneShot(AssetHandle clipHandle, std::optional<glm::vec3> position, float volume)
	{
		Impl& impl = *m_Impl;
		if (!impl.EngineReady || !impl.ActiveScene || !std::isfinite(volume) || volume < 0.0f || (position && !IsFinite(*position)))
			return false;
		Ref<AudioClip> clip = clipHandle.IsValid() ? impl.Assets.GetAsset<AudioClip>(clipHandle) : nullptr;
		if (!clip || clip->GetFrameCount() == 0)
			return false;
		Scope<Voice> voice = impl.CreateVoice(clip);
		if (!voice)
			return false;
		ma_sound_set_volume(&voice->Sound, SafeVolume(volume));
		ma_sound_set_spatialization_enabled(&voice->Sound, position ? MA_TRUE : MA_FALSE);
		if (position)
			ma_sound_set_position(&voice->Sound, position->x, position->y, position->z);
		ma_sound_start(&voice->Sound);
		impl.OneShots.push_back(std::move(voice));
		return true;
	}

	void AudioSystem::SetMasterVolume(float volume)
	{
		if (!std::isfinite(volume))
			return;
		m_Impl->MasterVolume = std::clamp(volume, 0.0f, 10.0f);
		if (m_Impl->EngineReady)
			ma_engine_set_volume(&m_Impl->Engine, m_Impl->MasterVolume);
	}

	float AudioSystem::GetMasterVolume() const
	{
		return m_Impl->MasterVolume;
	}

	void AudioSystem::SetPaused(bool paused)
	{
		Impl& impl = *m_Impl;
		if (impl.Paused == paused)
			return;
		impl.Paused = paused;
		// Offline output only advances with updates, which a paused game does not get; a device must be told.
		if (impl.EngineReady && impl.Output == AudioOutput::Device)
		{
			if (paused)
				ma_engine_stop(&impl.Engine);
			else
				ma_engine_start(&impl.Engine);
		}
	}

	bool AudioSystem::IsPaused() const
	{
		return m_Impl->Paused;
	}

	AudioOutput AudioSystem::GetOutput() const
	{
		return m_Impl->Output;
	}

	uint32_t AudioSystem::GetVoiceCount() const
	{
		return m_Impl->VoiceCount();
	}

	const std::vector<float>& AudioSystem::GetCapturedOutput() const
	{
		return m_Impl->Captured;
	}

	uint32_t AudioSystem::GetSampleRate() const
	{
		return m_Impl->EngineReady ? ma_engine_get_sample_rate(&m_Impl->Engine) : m_Impl->Specification.SampleRate;
	}

}
