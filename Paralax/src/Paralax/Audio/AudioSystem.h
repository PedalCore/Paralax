#pragma once

#include "Paralax/Asset/AssetHandle.h"
#include "Paralax/Core/Base.h"
#include "Paralax/Core/UUID.h"
#include "Paralax/Scene/SceneSystem.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace Paralax {

	class AssetManager;

	enum class AudioOutput
	{
		// The default output device. If there is none (no sound card or audio server), the system falls back to
		// Offline with a warning.
		Device,
		// Mixed in step with the simulation and not heard: headless runs, CI and tests. Deterministic for a given
		// sequence of time steps.
		Offline
	};

	struct AudioSystemSpecification
	{
		AudioOutput Output = AudioOutput::Offline;
		// Mixing rate in Offline mode (a device chooses its own). 8000 to 192000.
		uint32_t SampleRate = 48000;
		// Sources and one-shots playing at once; further ones are refused with a warning.
		uint32_t MaxVoices = 64;
		// Offline mode: the mixed output (stereo, interleaved) is kept for GetCapturedOutput, up to this many frames.
		uint64_t MaxCapturedFrames = 0;
	};

	// Plays the scene's AudioSource components (miniaudio) while it runs. Add it to a scene before
	// Scene::OnRuntimeStart, after the script and physics systems, so it hears where things ended up each frame.
	//
	// A source sounds through a voice while it plays (or is paused), kept in line with the component every update
	// (volume, pitch, looping, spatialisation, distances, and its world position). A source with PlayOnStart starts the
	// first time the system sees it: when the scene starts, or when the source is created while it runs. A source whose
	// clip changes (or is reloaded) carries on with the new clip from its start, in the same state. The listener is the
	// first active AudioListener in hierarchy order, else the primary camera. Clips come from the AssetManager; one that
	// cannot be loaded leaves the source silent (the asset manager reports it once).
	//
	// When a sound ends is decided by simulated time (the time steps the scene is updated with, scaled by pitch), not
	// by the mixer, so what scripts observe (IsPlaying) is the same whether the output is a device or offline.
	//
	// Main-thread-only. In Device mode miniaudio mixes on its own thread; clips are immutable while they play.
	class AudioSystem final : public SceneSystem
	{
	public:
		// assets must outlive the system. Verifies the specification.
		explicit AudioSystem(AssetManager& assets, AudioSystemSpecification specification = {});
		~AudioSystem() override;

		AudioSystem(const AudioSystem&) = delete;
		AudioSystem& operator=(const AudioSystem&) = delete;

		const char* GetName() const override { return "Audio"; }
		void OnRuntimeStart(Scene& scene) override;
		void OnUpdate(Scene& scene, Timestep timestep) override;
		void OnRuntimeStop(Scene& scene) override;
		void OnEntityDestroying(Scene& scene, const Entity& entity) override;

		// Source controls; each returns false if the entity has no AudioSource with a playable clip (or, for Play, the
		// voice limit is reached). Play resumes a paused source, otherwise starts it from the beginning; Pause keeps the
		// position; Stop ends the sound. A source controlled before its first update no longer plays on start.
		bool Play(UUID entity);
		bool Pause(UUID entity);
		bool Stop(UUID entity);
		bool IsPlaying(UUID entity) const;

		// Plays a clip once, not tied to an entity: at a world position (spatial), or as is without one. Returns false
		// if the scene is not running, the clip cannot be loaded, the volume is not finite and non-negative, the
		// position is not finite, or the voice limit is reached.
		bool PlayOneShot(AssetHandle clip, std::optional<glm::vec3> position = std::nullopt, float volume = 1.0f);

		// Silences a Device output while the game is paused (the editor's pause); offline output only advances with
		// updates anyway. Sounds resume where they were.
		void SetPaused(bool paused);
		bool IsPaused() const;

		// Overall gain, 0 to 10 (values outside are clamped; non-finite ones ignored).
		void SetMasterVolume(float volume);
		float GetMasterVolume() const;

		// The output actually in use (Device may have fallen back to Offline); Offline when not running.
		AudioOutput GetOutput() const;
		// Sounds playing or paused (sources and one-shots); at most MaxVoices.
		uint32_t GetVoiceCount() const;
		// Offline mode: the mixed output so far (stereo, interleaved), up to MaxCapturedFrames.
		const std::vector<float>& GetCapturedOutput() const;
		uint32_t GetSampleRate() const;

	private:
		struct Impl;
		Scope<Impl> m_Impl;
	};

}
