#pragma once

#include "Paralax/Core/Timestep.h"

namespace Paralax {

	class Entity;
	class Scene;

	// A runtime system attached to a running scene (scripting, physics, audio). The scene owns its systems and calls
	// them in the order they were added. Systems are attached to a scene before Scene::OnRuntimeStart and are not
	// copied by Scene::Copy. Main-thread-only.
	//
	// While a callback runs, entity destruction requested through the scene is deferred until the callback returns,
	// so systems never observe a half-destroyed entity.
	class SceneSystem
	{
	public:
		virtual ~SceneSystem() = default;

		// Short name for logs and diagnostics, e.g. "Scripting".
		virtual const char* GetName() const = 0;

		virtual void OnRuntimeStart(Scene& /*scene*/) {}
		// One simulation step. Called by Scene::OnUpdateRuntime.
		virtual void OnUpdate(Scene& /*scene*/, Timestep /*timestep*/) {}
		virtual void OnRuntimeStop(Scene& /*scene*/) {}

		// Called for every entity of a subtree that is about to be destroyed while the scene is running, while the
		// entity and its components are still valid (parents before children).
		virtual void OnEntityDestroying(Scene& /*scene*/, const Entity& /*entity*/) {}
	};

}
