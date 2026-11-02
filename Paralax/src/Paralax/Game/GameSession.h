#pragma once

#include "Paralax/Asset/AssetHandle.h"
#include "Paralax/Audio/AudioSystem.h"
#include "Paralax/Core/Base.h"
#include "Paralax/Core/Timestep.h"
#include "Paralax/Core/UUID.h"
#include "Paralax/Game/GameState.h"
#include "Paralax/Scripting/ScriptSystem.h"

#include <string>
#include <vector>

namespace Paralax {

	class AnimationSystem;
	class AssetManager;
	class PhysicsSystem;
	class Scene;

	struct GameSessionSpecification
	{
		AudioOutput Audio = AudioOutput::Offline;
		ScriptSystemSpecification Scripts;
		// Seed of the ids of entities created while the game runs (Scene.CreateEntity, Scene.Instantiate, editor
		// commands in play mode). One UUIDSequence serves the whole session and continues across scene switches, so the
		// same inputs give the same ids in the same order, and the session never hands out an id twice. The runtime and
		// the editor's play mode both use this default, so a playtest spawns the ids the exported game spawns.
		//
		// A seed of its own rather than one derived from Scripts.RandomSeed: the two streams live differently
		// (math.random restarts with every level and scripts reseed it; ids continue), entities are created by more
		// than scripts, and retuning a game's random numbers should not renumber its entities, which would reorder
		// their contact callbacks (those are sorted by id).
		uint64_t EntityIdSeed = 0x1D5EED;
	};

	// A running game: a copy of a scene with the runtime systems (scripts, then physics, then audio), as played by the
	// runtime and by the editor's play mode.
	//
	// Scripts may switch scenes (Scene.Load) or restart the current one (Scene.Reload). The switch happens after the
	// update that asked for it: the running scene stops and a fresh copy of the requested scene (as authored, or as
	// it was when this level started) starts with new systems. Script errors, the Game table's values (GetState) and
	// the entity id sequence (see GameSessionSpecification::EntityIdSeed) are kept across switches. The running scene's
	// id sequence belongs to the session, which takes it back at a switch: replacing it through GetScene() would make
	// the next level hand out ids again. The session keeps no global state of its own, so several may run side by side.
	//
	// Main-thread-only. The scene object changes on a switch, so never hold GetScene() across Update.
	class GameSession
	{
	public:
		// assets must outlive the session. scene is the level to play; the session plays a copy and keeps the original
		// for Scene.Reload.
		GameSession(AssetManager& assets, const Scene& scene, GameSessionSpecification specification = {});
		~GameSession();

		GameSession(const GameSession&) = delete;
		GameSession& operator=(const GameSession&) = delete;

		// Advances the game one step, then performs a scene switch if scripts asked for one. Verifies the session
		// has not ended.
		void Update(Timestep timestep);
		// Stops the game (scripts get OnDestroy) so that every error, including those raised while stopping, is in
		// GetScriptErrors. Called by the destructor if not before; later calls do nothing.
		void End();
		bool HasEnded() const { return m_Scene == nullptr; }

		// Verifies the session has not ended (the systems below are null then).
		Scene& GetScene() const
		{
			PX_CORE_VERIFY(m_Scene, "GameSession::GetScene after End");
			return *m_Scene;
		}
		ScriptSystem* GetScripts() const { return m_Scripts; }
		PhysicsSystem* GetPhysics() const { return m_Physics; }
		AnimationSystem* GetAnimation() const { return m_Animation; }
		AudioSystem* GetAudio() const { return m_Audio; }

		// Script errors of this session, from every scene played so far, and scene switches that failed (oldest first,
		// capped like the script system's).
		std::vector<std::string> GetScriptErrors() const;
		void ClearScriptErrors();
		// Scene switches performed (Scene.Load and Scene.Reload).
		uint32_t GetSceneSwitches() const { return m_Switches; }
		// The asset the current level was loaded from, or 0 for the scene the session started with.
		AssetHandle GetSceneAsset() const { return m_SceneAsset; }
		// Values scripts keep across scene switches (the Game table).
		GameState& GetState() { return m_State; }

	private:
		void Start(Ref<Scene> level);
		void Stop();
		void SwitchScene(AssetHandle target);
		void RecordErrors(const std::vector<std::string>& errors);

	private:
		AssetManager& m_Assets;
		GameSessionSpecification m_Specification;
		// Between levels; while one runs, the running scene holds the live sequence.
		UUIDSequence m_EntityIds;
		GameState m_State;
		Ref<Scene> m_Level; // The current level as it was when it started (never run).
		Ref<Scene> m_Scene; // The running copy.
		AssetHandle m_SceneAsset = 0;
		ScriptSystem* m_Scripts = nullptr;
		AnimationSystem* m_Animation = nullptr;
		PhysicsSystem* m_Physics = nullptr;
		AudioSystem* m_Audio = nullptr;
		bool m_AudioPaused = false;
		std::vector<std::string> m_EarlierErrors;
		uint32_t m_Switches = 0;
	};

}
