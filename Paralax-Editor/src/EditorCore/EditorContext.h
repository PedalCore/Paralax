#pragma once

#include "EditorCore/CommandRegistry.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Audio/AudioSystem.h>
#include <Paralax/Core/Base.h>
#include <Paralax/Core/Timestep.h>
#include <Paralax/Core/UUID.h>
#include <Paralax/Events/Event.h>
#include <Paralax/Game/GameSession.h>
#include <Paralax/Project/Project.h>
#include <Paralax/Scene/Scene.h>
#include <Paralax/Scripting/ScriptSystem.h>

#include <nlohmann/json.hpp>

#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Paralax::Editor {

	class ConsoleSink;

	// All editor state that is independent of the UI: project, open scene, selection, play mode and undo history.
	// The UI panels, the command registry and the agent server all operate on this. Main-thread-only.
	class EditorContext
	{
	public:
		// project may be null: the editor then works on an unsaved in-memory scene with built-in assets only.
		explicit EditorContext(Ref<Project> project = nullptr);
		~EditorContext();

		EditorContext(const EditorContext&) = delete;
		EditorContext& operator=(const EditorContext&) = delete;

		// --- Project and assets ---
		Project* GetProject() const { return m_Project.get(); }
		AssetManager& GetAssets() { return m_Project ? m_Project->GetAssetManager() : m_ScratchAssets; }

		// --- Scene ---
		// The scene being edited, or the running copy while playing.
		Scene& GetActiveScene() { return m_Game ? m_Game->GetScene() : *m_EditorScene; }
		Scene& GetEditorScene() { return *m_EditorScene; }
		const std::filesystem::path& GetScenePath() const { return m_ScenePath; }
		bool IsSceneDirty() const { return m_SceneDirty; }

		// Replaces the scene with an empty one (stopping play mode and clearing the undo history). Returns false while
		// an undoable change is open.
		bool NewScene(const std::string& name = "Untitled");
		// Loads a .pxscene; on failure the current scene is kept and false is returned (errors logged). With a project
		// the path is relative to its asset directory and must stay inside it. Returns false while a change is open.
		bool OpenScene(const std::filesystem::path& path);
		// Saves to path (or the current path if empty). With a project the path must lie inside its asset
		// directory. Returns false if there is no path, the path is rejected or writing fails.
		bool SaveScene(const std::filesystem::path& path = {});
		// Resolves a user-supplied path for a project file: relative paths are taken from the asset directory, and
		// anything outside it (after resolving "..") is rejected with an error. Without a project, paths are used
		// as given. Agents reach this through commands, so it is the containment boundary for scene files.
		std::optional<std::filesystem::path> ResolveProjectPath(const std::filesystem::path& path, std::string& outError) const;
		// Resolves where a command may write an output file (captures, exports): relative paths are taken from the
		// project directory and must stay inside it; without a project they are used as given. The extension must
		// match (case-insensitive), so a command can never overwrite an arbitrary file type.
		std::optional<std::filesystem::path> ResolveOutputPath(const std::filesystem::path& path, std::string_view extension, std::string& outError) const;

		// --- Selection (by UUID so it survives undo/redo, which recreate the scene) ---
		const std::vector<UUID>& GetSelection() const { return m_Selection; }
		std::optional<UUID> GetPrimarySelection() const;
		void SetSelection(std::vector<UUID> selection);
		void Select(UUID id, bool additive = false);
		void ClearSelection() { m_Selection.clear(); }
		bool IsSelected(UUID id) const;

		// --- Play mode ---
		bool IsPlaying() const { return m_Game != nullptr; }
		// How play mode sounds: Offline (the default: headless runs, agents and tests mix silently and
		// deterministically) or Device. Applies from the next StartPlaying.
		void SetAudioOutput(AudioOutput output) { m_AudioOutput = output; }
		AudioOutput GetAudioOutput() const { return m_AudioOutput; }
		// Copies the editor scene and starts simulating the copy (with scripting, physics and audio). Returns false if
		// already playing.
		// A paused session only advances through explicit steps (play.step), which keeps agent-driven runs
		// deterministic.
		bool StartPlaying(bool paused = false);
		// Discards the running copy and returns to editing. Returns false if not playing.
		bool StopPlaying();
		// Advances the running scene by one step (ignores the pause state and queued input).
		void UpdateRuntime(Timestep ts);
		// Advances a running game by whole frames (play.step), each one input frame: queued input (QueueInput) is
		// delivered to the first step, which sees its presses and releases; later steps see keys as held. Real input
		// shares the same state, and per-frame transitions are cleared after each step. Returns false if not playing.
		bool StepRuntime(int64_t frames, Timestep ts);
		// Advances the running scene from the editor's frame loop (delivering queued input first): does nothing while
		// paused.
		void TickRuntime(Timestep ts);
		// Queues synthetic input for the running game. It is delivered at the start of its next update, so the game
		// sees every press and release however many editor frames pass in between. Returns false if not playing.
		bool QueueInput(Scope<Event> event);
		size_t GetQueuedInputCount() const { return m_QueuedInput.size(); }
		// Rewrites happen on disk at once, but a running game keeps the code it started with: the reload is deferred
		// to the end of play mode (immediate when not playing).
		void ReloadAssetWhenStopped(AssetHandle handle);
		bool IsPaused() const { return m_Paused; }
		// Pausing outside play mode is ignored.
		// Pausing also silences the game's audio device (sounds resume where they were).
		void SetPaused(bool paused);
		// The running game (play mode), or nullptr. Its scene changes when scripts switch scenes, so never hold it, or
		// its scene or systems, across Execute or an update.
		GameSession* GetGame() const { return m_Game.get(); }
		ScriptSystem* GetScriptSystem() const { return m_Game ? m_Game->GetScripts() : nullptr; }
		AudioSystem* GetAudioSystem() const { return m_Game ? m_Game->GetAudio() : nullptr; }

		// --- Undo (scene snapshots) ---
		// Brackets a scene change. Nested pairs are merged into the outermost; every Begin must be matched by an End.
		// A bracket opened while playing records nothing (the editor scene is not being edited). An uncommitted
		// change is rolled back if it modified the editor scene. Note that undo, redo and rollback replace the
		// Scene object: never hold a Scene& or component references across a call that may change the scene.
		void BeginUndoableChange(const std::string& description);
		void EndUndoableChange(bool committed);
		bool IsChangeOpen() const { return m_ChangeDepth > 0; }

		// Interactive edits (dragging a value, typing, moving a gizmo) span many frames but form one undo step.
		// Call ApplyInteractiveEdit with a stable key immediately *before* writing each change (it opens the bracket
		// on the first one), TouchInteractiveEdit every frame the interaction continues, and EndStaleInteractiveEdit
		// once per frame after all UI has run: an edit that was neither applied nor touched that frame (and not kept
		// alive, e.g. because a related popup widget is active) is closed. Scene-modifying commands, undo, redo and
		// play mode changes commit an open interactive edit first.
		void ApplyInteractiveEdit(uint64_t key, const std::string& description);
		void TouchInteractiveEdit(uint64_t key);
		void EndStaleInteractiveEdit(bool keepAlive = false);
		// Closes any open interactive edit now (e.g. before undo or when the scene is replaced).
		void CommitInteractiveEdit();
		bool CanUndo() const { return !m_UndoStack.empty(); }
		bool CanRedo() const { return !m_RedoStack.empty(); }
		std::string GetUndoDescription() const { return m_UndoStack.empty() ? std::string() : m_UndoStack.back().Description; }
		std::string GetRedoDescription() const { return m_RedoStack.empty() ? std::string() : m_RedoStack.back().Description; }
		bool Undo();
		bool Redo();
		// The history keeps at most this many steps, and drops the oldest beyond the byte budget (always keeping one).
		static constexpr size_t MaxUndoSteps = 200;
		static constexpr size_t MaxUndoBytes = 256ull * 1024 * 1024;

		// --- Commands ---
		CommandRegistry& GetCommands() { return m_Commands; }
		CommandResult Execute(std::string_view command, const nlohmann::json& params = nlohmann::json::object());
		// Queues a command to run in ExecuteDeferred. UI code uses this while it holds references into the scene.
		void Defer(std::string command, nlohmann::json params = nlohmann::json::object());
		// Runs queued commands in order; failures are logged. Returns the number of commands that failed.
		size_t ExecuteDeferred();

		// Rendering is owned by the UI layer; it installs this hook so "render.capture" works for agents.
		using CaptureHandler = std::function<CommandResult(const nlohmann::json& params)>;
		void SetCaptureHandler(CaptureHandler handler) { m_CaptureHandler = std::move(handler); }
		const CaptureHandler& GetCaptureHandler() const { return m_CaptureHandler; }

		// Resolves an entity id string/number to an entity of the active scene.
		std::optional<Entity> ResolveEntity(const nlohmann::json& id);

	private:
		struct UndoEntry
		{
			std::string Description;
			std::string Scene; // Serialized scene JSON: far smaller than a JSON DOM, and cheap to compare.
			std::vector<UUID> Selection;
		};

		std::string SnapshotScene();
		void RestoreScene(const std::string& snapshot);
		void PushUndo(UndoEntry entry);
		void TrimUndoHistory();
		void PruneSelection();
		void DeliverQueuedInput();

	private:
		Ref<Project> m_Project;
		AssetManager m_ScratchAssets;
		Ref<Scene> m_EditorScene;
		Scope<GameSession> m_Game;
		bool m_Paused = false;
		AudioOutput m_AudioOutput = AudioOutput::Offline;
		std::vector<Scope<Event>> m_QueuedInput;
		std::vector<AssetHandle> m_DeferredReloads;
		std::filesystem::path m_ScenePath;
		bool m_SceneDirty = false;

		std::vector<UUID> m_Selection;

		std::deque<UndoEntry> m_UndoStack;
		std::vector<UndoEntry> m_RedoStack;
		size_t m_UndoBytes = 0; // Snapshot bytes in m_UndoStack.
		uint32_t m_ChangeDepth = 0;
		std::optional<UndoEntry> m_PendingChange;
		std::optional<uint64_t> m_InteractiveEdit;
		bool m_InteractiveEditTouched = false;
		std::vector<std::pair<std::string, nlohmann::json>> m_DeferredCommands;

		CommandRegistry m_Commands;
		CaptureHandler m_CaptureHandler;
	};

	// Registers the built-in editor commands (scene, entity, component, asset, selection, undo, play, render).
	void RegisterBuiltinCommands(CommandRegistry& registry);
	// Registers the script and prefab commands (script.create/eval/errors, prefab.create/instantiate).
	void RegisterScriptingCommands(CommandRegistry& registry);
	// Registers the project commands (project.info, project.set_start_scene, project.export).
	void RegisterProjectCommands(CommandRegistry& registry);
	// Registers the input commands (input.key, input.mouse_button, input.mouse_move, input.scroll, input.release_all,
	// input.state): synthetic input for playtesting.
	void RegisterInputCommands(CommandRegistry& registry);
	// Registers console.get and console.clear, which read and clear the log kept by sink (the editor's console). The
	// editor application owns the sink and calls this; the commands keep it alive. Verifies sink is set.
	void RegisterConsoleCommands(CommandRegistry& registry, std::shared_ptr<ConsoleSink> sink);

}
