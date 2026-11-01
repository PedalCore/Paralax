#pragma once

#include "Paralax/Core/Base.h"
#include "Paralax/Asset/AssetHandle.h"
#include "Paralax/Scene/SceneSystem.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Paralax {

	class AssetManager;
	class GameState;

	struct ScriptSystemSpecification
	{
		// Seed of the deterministic random generator behind math.random (scripts may reseed with math.randomseed).
		uint64_t RandomSeed = 0x5EED;
		// Lua instructions one callback (OnCreate, OnUpdate, ...) may execute before it is aborted with an error, so a
		// runaway loop cannot freeze the engine. At least 100000.
		uint64_t InstructionBudget = 200'000'000;
		// Total memory the scripts of one scene may allocate; allocations beyond it fail with a Lua memory error.
		// At least 4 MiB.
		size_t MemoryLimitBytes = 256ull * 1024 * 1024;
		// Entities one callback may create (entities live outside the Lua heap, so the memory limit does not cover them).
		uint64_t MaxEntitiesCreatedPerCall = 10'000;
		// Errors kept for GetErrors (older ones are dropped; all errors are logged).
		size_t MaxRecordedErrors = 100;
		// Values that outlive the scene (the Game table), owned by the GameSession; must outlive the system. Without
		// one, the Game functions raise an error.
		GameState* State = nullptr;
	};

	// Runs Lua scripts (ScriptComponent) in a running scene. Add it to a scene before Scene::OnRuntimeStart.
	//
	// A script is a Lua chunk that returns a table (its "class"). Every entity with a ScriptComponent gets an
	// instance: a table whose metatable indexes the class, with `self.entity` set. The system calls, in hierarchy
	// order:
	//   OnCreate(self)        before the instance's first update (entities created while running start next update)
	//   OnUpdate(self, dt)    every update, with dt in seconds
	//   OnFixedUpdate(self, dt)
	//                         before each step of the scene's PhysicsSystem (dt is the fixed step), after OnUpdate:
	//                         zero or more times per update, at the physics rate, so frame-rate independent.
	//                         Whether a script defines it is looked up once its OnCreate has run. Entities destroyed
	//                         during the update are skipped.
	//   OnDestroy(self)       when the entity (or its ScriptComponent) is removed, or when the scene stops running
	//   OnCollisionEnter/Exit(self, other), OnTriggerEnter/Exit(self, other)
	//                         contacts from the scene's PhysicsSystem, delivered before OnUpdate of the next update
	// A callback that raises an error disables its instance (the error, with a traceback, is logged and recorded).
	//
	// Scripts run in a sandbox: no io, os, debug, load or bytecode, no finalizers or access to engine metatables, a
	// deterministic math.random, an instruction budget per callback (which pcall cannot swallow), bounded pattern
	// matching, a memory limit, and a cap on entities created per callback. `require("Scripts/Util")` loads another
	// script from the project's assets.
	// The API (Entity, Scene, Physics, Input, Time, Log and the vec2/vec3/vec4/quat math types) is documented in
	// docs/Scripting.md. Add a PhysicsSystem after the ScriptSystem so forces applied in OnUpdate act in the same
	// update's simulation step.
	//
	// Main-thread-only. Errors in scripts, and Lua errors in the engine's own calls into Lua (e.g. out of memory), are
	// reported and recorded; they never propagate to C++ callers.
	class ScriptSystem final : public SceneSystem
	{
	public:
		explicit ScriptSystem(AssetManager& assets, ScriptSystemSpecification specification = {});
		~ScriptSystem() override;

		ScriptSystem(const ScriptSystem&) = delete;
		ScriptSystem& operator=(const ScriptSystem&) = delete;

		const char* GetName() const override { return "Scripting"; }
		void OnRuntimeStart(Scene& scene) override;
		void OnUpdate(Scene& scene, Timestep timestep) override;
		void OnFixedUpdate(Scene& scene, Timestep timestep) override;
		// True while a live instance defines OnFixedUpdate (looked up once its OnCreate has run).
		bool WantsFixedUpdate() const override;
		void OnRuntimeStop(Scene& scene) override;
		void OnEntityDestroying(Scene& scene, const Entity& entity) override;

		// Most recent script errors (message and traceback), oldest first.
		const std::vector<std::string>& GetErrors() const;
		void ClearErrors();

		// Number of live script instances.
		size_t GetInstanceCount() const;

		// A scene change scripts asked for (Scene.Load or Scene.Reload): the scene to switch to, or 0 to restart the
		// current one. The host performs it after the update (see GameSession); the last request of an update wins.
		std::optional<AssetHandle> TakeSceneRequest();

		// Runs a Lua chunk in the running scene's script environment (for the console and agents) and returns its
		// results converted with tostring, separated by tabs. Returns nullopt (with outError set) if the scene is not
		// running or the code fails. The instruction budget and memory limit apply.
		std::optional<std::string> Evaluate(std::string_view code, std::string& outError);

		// Every name scripts can use, as "Table.Function" for the global API tables and "Type:Member" for methods and
		// properties of userdata types (Entity, Component, vec2/3/4, quat). Works whether or not the scene runs.
		std::vector<std::string> GetApiNames();

	private:
		struct Impl;
		Scope<Impl> m_Impl;
	};

}
