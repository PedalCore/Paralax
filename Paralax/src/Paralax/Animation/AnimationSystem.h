#pragma once

#include "Paralax/Core/Base.h"
#include "Paralax/Scene/SceneSystem.h"

namespace Paralax {

	class AssetManager;

	// Plays the scene's Animation components while it runs. Add it to a scene before Scene::OnRuntimeStart, after the
	// script system (scripts start, stop or seek clips in OnUpdate) and before the physics system (so animated
	// kinematic bodies push what they touch in the same update).
	//
	// Each update, a playing component's Time advances by the time step times its Speed; a looping clip wraps around
	// (either way, for a negative Speed), and one that does not loop stops at its end (or start) and clears Playing.
	// The clip's channels then set the local Translation, Rotation or Scale of their targets: the entity with the
	// component, or entities below it named by the channel's path. Targets are looked up again while they are missing
	// and when the clip changes. A clip that cannot be loaded, or a target that cannot be found, is reported once and skipped.
	// Results depend only on the time steps, so playback is deterministic.
	//
	// Main-thread-only.
	class AnimationSystem final : public SceneSystem
	{
	public:
		// assets must outlive the system.
		explicit AnimationSystem(AssetManager& assets);
		~AnimationSystem() override;

		AnimationSystem(const AnimationSystem&) = delete;
		AnimationSystem& operator=(const AnimationSystem&) = delete;

		const char* GetName() const override { return "Animation"; }
		void OnRuntimeStart(Scene& scene) override;
		void OnUpdate(Scene& scene, Timestep timestep) override;
		void OnRuntimeStop(Scene& scene) override;
		void OnEntityDestroying(Scene& scene, const Entity& entity) override;

	private:
		struct Impl;
		Scope<Impl> m_Impl;
	};

}
