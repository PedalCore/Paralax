#include "EditorCore/EditorContext.h"

#include <Paralax/Core/Input.h>
#include <Paralax/Events/ApplicationEvent.h>
#include <Paralax/Scene/Entity.h>
#include <Paralax/Scene/SceneSerializer.h>
#include <Paralax/Scripting/ScriptSystem.h>

#include <algorithm>
#include <cctype>
#include <charconv>

namespace Paralax::Editor {

	using json = nlohmann::json;

	EditorContext::EditorContext(Ref<Project> project)
		: m_Project(std::move(project))
		, m_EditorScene(CreateRef<Scene>("Untitled"))
	{
		RegisterBuiltinCommands(m_Commands);
		RegisterScriptingCommands(m_Commands);
		RegisterProjectCommands(m_Commands);
		RegisterInputCommands(m_Commands);
	}

	EditorContext::~EditorContext() = default;

	bool EditorContext::NewScene(const std::string& name)
	{
		CommitInteractiveEdit();
		if (IsChangeOpen())
		{
			PX_CORE_ERROR("Cannot replace the scene while a change is in progress");
			return false;
		}
		StopPlaying();
		m_EditorScene = CreateRef<Scene>(name.empty() ? "Untitled" : name);
		m_ScenePath.clear();
		m_SceneDirty = false;
		m_Selection.clear();
		ClearUndoHistory();
		return true;
	}

	std::optional<std::filesystem::path> EditorContext::ResolveProjectPath(const std::filesystem::path& path, std::string& outError) const
	{
		if (path.empty())
		{
			outError = "The path is empty";
			return std::nullopt;
		}
		if (!m_Project)
			return path;
		// ToRegistryPath resolves ".." and symlinks before checking containment.
		const AssetManager& assets = m_Project->GetAssetManager();
		std::optional<std::filesystem::path> registryPath = assets.ToRegistryPath(path);
		if (!registryPath)
		{
			outError = fmt::format("'{}' is outside the project's asset directory", path.generic_string());
			return std::nullopt;
		}
		return assets.ToAbsolutePath(*registryPath);
	}

	std::optional<std::filesystem::path> EditorContext::ResolveOutputPath(const std::filesystem::path& path, std::string_view extension, std::string& outError) const
	{
		if (path.empty() || !path.has_filename())
		{
			outError = "The output path is empty or names a directory";
			return std::nullopt;
		}
		auto hasExtension = [extension](const std::filesystem::path& candidate) {
			std::string actual = candidate.extension().string();
			std::transform(actual.begin(), actual.end(), actual.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return actual == extension;
		};
		// Checked on the resolved path too: a symlink named "x.png" must not redirect the write to another file type.
		std::error_code error;
		const std::filesystem::path base = m_Project ? m_Project->GetDirectory() : std::filesystem::path();
		const std::filesystem::path resolved = std::filesystem::weakly_canonical(path.is_absolute() || !m_Project ? path : base / path, error);
		if (!hasExtension(path) || error || !hasExtension(resolved))
		{
			outError = fmt::format("The output file must have the '{}' extension", extension);
			return std::nullopt;
		}
		if (!m_Project)
			return path;

		const std::filesystem::path relative = resolved.lexically_relative(base);
		if (relative.empty() || *relative.begin() == "..")
		{
			outError = fmt::format("'{}' is outside the project directory", path.generic_string());
			return std::nullopt;
		}
		return resolved;
	}

	bool EditorContext::OpenScene(const std::filesystem::path& path)
	{
		CommitInteractiveEdit();
		if (IsChangeOpen())
		{
			PX_CORE_ERROR("Cannot open a scene while a change is in progress");
			return false;
		}
		std::string error;
		std::optional<std::filesystem::path> resolvedPath = ResolveProjectPath(path, error);
		if (!resolvedPath)
		{
			PX_CORE_ERROR("Cannot open scene: {}", error);
			return false;
		}
		const std::filesystem::path& resolved = *resolvedPath;

		Ref<Scene> scene = SceneSerializer::LoadFromFile(resolved);
		if (!scene)
			return false;

		StopPlaying();
		m_EditorScene = scene;
		m_ScenePath = resolved;
		m_SceneDirty = false;
		m_Selection.clear();
		ClearUndoHistory();
		return true;
	}

	bool EditorContext::SaveScene(const std::filesystem::path& path)
	{
		std::filesystem::path target = path.empty() ? m_ScenePath : path;
		if (target.empty())
		{
			PX_CORE_ERROR("SaveScene: the scene has no file path yet");
			return false;
		}
		if (target.extension() != ".pxscene")
			target += ".pxscene";
		std::string error;
		std::optional<std::filesystem::path> resolved = ResolveProjectPath(target, error);
		if (!resolved)
		{
			PX_CORE_ERROR("Cannot save scene: {}", error);
			return false;
		}
		target = *resolved;

		if (!SceneSerializer::SaveToFile(*m_EditorScene, target))
			return false;
		m_ScenePath = target;
		m_SceneDirty = false;
		if (m_Project)
			m_Project->GetAssetManager().ImportAsset(target);
		return true;
	}

	std::optional<UUID> EditorContext::GetPrimarySelection() const
	{
		if (m_Selection.empty())
			return std::nullopt;
		return m_Selection.back();
	}

	void EditorContext::SetSelection(std::vector<UUID> selection)
	{
		m_Selection = std::move(selection);
		PruneSelection();
	}

	void EditorContext::Select(UUID id, bool additive)
	{
		if (!additive)
			m_Selection.clear();
		std::erase(m_Selection, id);
		m_Selection.push_back(id);
		PruneSelection();
	}

	bool EditorContext::IsSelected(UUID id) const
	{
		return std::find(m_Selection.begin(), m_Selection.end(), id) != m_Selection.end();
	}

	void EditorContext::PruneSelection()
	{
		Scene& scene = GetActiveScene();
		std::erase_if(m_Selection, [&](UUID id) { return !scene.TryGetEntityByUUID(id).has_value(); });
	}

	bool EditorContext::StartPlaying(bool paused)
	{
		if (m_Game)
			return false;
		// An interactive edit belongs to the scene it started in; later writes must not join it across the switch.
		CommitInteractiveEdit();
		// Keys held before the game started belong to the editor, not the game: release them (keeping the cursor),
		// as when the window loses focus.
		m_QueuedInput.clear();
		Input::OnEvent(WindowLostFocusEvent());
		GameSessionSpecification specification;
		specification.Audio = m_AudioOutput;
		m_Game = CreateScope<GameSession>(GetAssets(), *m_EditorScene, specification);
		SetPaused(paused);
		return true;
	}

	void EditorContext::SetPaused(bool paused)
	{
		m_Paused = paused && IsPlaying();
		if (AudioSystem* audio = GetAudioSystem())
			audio->SetPaused(m_Paused);
	}

	bool EditorContext::StopPlaying()
	{
		if (!m_Game)
			return false;
		CommitInteractiveEdit();
		m_Paused = false;
		m_Game = nullptr;
		// Nothing injected for the game may stay held afterwards (it would steer the editor camera).
		m_QueuedInput.clear();
		Input::OnEvent(WindowLostFocusEvent());
		for (AssetHandle handle : m_DeferredReloads)
			GetAssets().ReloadAsset(handle);
		m_DeferredReloads.clear();
		PruneSelection();
		return true;
	}

	void EditorContext::UpdateRuntime(Timestep ts)
	{
		if (!m_Game)
			return;
		const uint32_t switches = m_Game->GetSceneSwitches();
		m_Game->Update(ts);
		// A scene switch (Scene.Load or Scene.Reload) replaced every entity: the selection refers to the old ones.
		if (m_Game->GetSceneSwitches() != switches)
			PruneSelection();
	}

	bool EditorContext::StepRuntime(int64_t frames, Timestep ts)
	{
		if (!m_Game)
			return false;
		for (int64_t i = 0; i < frames; i++)
		{
			DeliverQueuedInput();
			UpdateRuntime(ts);
			Input::EndFrame();
		}
		return true;
	}

	void EditorContext::TickRuntime(Timestep ts)
	{
		if (m_Paused || !m_Game)
			return;
		// Seen by this update; the application clears the transitions at the end of the frame.
		DeliverQueuedInput();
		UpdateRuntime(ts);
	}

	bool EditorContext::QueueInput(Scope<Event> event)
	{
		if (!m_Game || !event)
			return false;
		m_QueuedInput.push_back(std::move(event));
		return true;
	}

	void EditorContext::DeliverQueuedInput()
	{
		// Through the same path as real input, so a game cannot tell the two apart.
		for (const Scope<Event>& event : m_QueuedInput)
			Input::OnEvent(*event);
		m_QueuedInput.clear();
	}

	void EditorContext::ReloadAssetWhenStopped(AssetHandle handle)
	{
		if (!m_Game)
			GetAssets().ReloadAsset(handle);
		else if (std::find(m_DeferredReloads.begin(), m_DeferredReloads.end(), handle) == m_DeferredReloads.end())
			m_DeferredReloads.push_back(handle);
	}

	std::string EditorContext::SnapshotScene()
	{
		// Replacing invalid UTF-8 keeps dump() from throwing on names that came from files.
		return SceneSerializer::Serialize(*m_EditorScene).dump(-1, ' ', false, json::error_handler_t::replace);
	}

	void EditorContext::RestoreScene(const std::string& snapshot)
	{
		auto scene = CreateRef<Scene>(m_EditorScene->GetName());
		std::string error;
		// Snapshots come from Serialize, so they always load; verify to catch serializer regressions.
		const json document = json::parse(snapshot, nullptr, false);
		PX_CORE_VERIFY(!document.is_discarded() && SceneSerializer::Deserialize(*scene, document, error), "Undo snapshot failed to load: {}", error);
		scene->SetViewportSize(m_EditorScene->GetViewportWidth(), m_EditorScene->GetViewportHeight());
		m_EditorScene = scene;
		PruneSelection();
	}

	void EditorContext::BeginUndoableChange(const std::string& description)
	{
		// Depth is tracked even while playing so brackets stay balanced across play mode changes.
		if (m_ChangeDepth++ == 0 && !IsPlaying())
			m_PendingChange = UndoEntry{description, SnapshotScene(), m_Selection};
	}

	void EditorContext::EndUndoableChange(bool committed)
	{
		PX_CORE_ASSERT(m_ChangeDepth > 0, "EndUndoableChange without a matching BeginUndoableChange");
		if (m_ChangeDepth == 0 || --m_ChangeDepth != 0)
			return;
		if (!m_PendingChange)
			return; // Opened while playing: nothing was recorded.

		UndoEntry entry = std::move(*m_PendingChange);
		m_PendingChange.reset();
		const bool changed = SnapshotScene() != entry.Scene;
		if (!committed)
		{
			// Failed commands may have partially modified the scene; roll back. Most failures change nothing, and
			// then the scene object is kept.
			if (changed)
				RestoreScene(entry.Scene);
			m_Selection = entry.Selection;
			PruneSelection();
			return;
		}
		if (!changed)
			return; // No undo step.

		PushUndo(std::move(entry));
		m_RedoStack.clear();
		m_SceneDirty = true;
	}

	void EditorContext::PushUndo(UndoEntry entry)
	{
		m_UndoBytes += entry.Scene.size();
		m_UndoStack.push_back(std::move(entry));
		TrimUndoHistory();
	}

	void EditorContext::ClearUndoHistory()
	{
		m_UndoStack.clear();
		m_UndoBytes = 0;
		m_RedoStack.clear();
	}

	bool EditorContext::ResetHistory()
	{
		CommitInteractiveEdit();
		if (IsChangeOpen())
		{
			PX_CORE_ERROR("Cannot reset the undo history while a change is in progress");
			return false;
		}
		ClearUndoHistory();
		m_SceneDirty = false;
		return true;
	}

	void EditorContext::TrimUndoHistory()
	{
		while (m_UndoStack.size() > 1 && (m_UndoStack.size() > MaxUndoSteps || m_UndoBytes > MaxUndoBytes))
		{
			m_UndoBytes -= m_UndoStack.front().Scene.size();
			m_UndoStack.pop_front();
		}
	}

	void EditorContext::ApplyInteractiveEdit(uint64_t key, const std::string& description)
	{
		if (m_InteractiveEdit && *m_InteractiveEdit != key)
			CommitInteractiveEdit();
		if (!m_InteractiveEdit)
		{
			BeginUndoableChange(description);
			m_InteractiveEdit = key;
		}
		m_InteractiveEditTouched = true;
	}

	void EditorContext::TouchInteractiveEdit(uint64_t key)
	{
		if (m_InteractiveEdit && *m_InteractiveEdit == key)
			m_InteractiveEditTouched = true;
	}

	void EditorContext::EndStaleInteractiveEdit(bool keepAlive)
	{
		if (m_InteractiveEdit && !m_InteractiveEditTouched && !keepAlive)
			CommitInteractiveEdit();
		m_InteractiveEditTouched = false;
	}

	void EditorContext::CommitInteractiveEdit()
	{
		if (!m_InteractiveEdit)
			return;
		m_InteractiveEdit.reset();
		m_InteractiveEditTouched = false;
		EndUndoableChange(true);
	}

	bool EditorContext::Undo()
	{
		CommitInteractiveEdit();
		if (IsPlaying() || IsChangeOpen() || m_UndoStack.empty())
			return false;
		UndoEntry entry = std::move(m_UndoStack.back());
		m_UndoStack.pop_back();
		m_UndoBytes -= entry.Scene.size();
		m_RedoStack.push_back({entry.Description, SnapshotScene(), m_Selection});
		RestoreScene(entry.Scene);
		m_Selection = entry.Selection;
		PruneSelection();
		m_SceneDirty = true;
		return true;
	}

	bool EditorContext::Redo()
	{
		CommitInteractiveEdit();
		if (IsPlaying() || IsChangeOpen() || m_RedoStack.empty())
			return false;
		UndoEntry entry = std::move(m_RedoStack.back());
		m_RedoStack.pop_back();
		PushUndo({entry.Description, SnapshotScene(), m_Selection});
		RestoreScene(entry.Scene);
		m_Selection = entry.Selection;
		PruneSelection();
		m_SceneDirty = true;
		return true;
	}

	CommandResult EditorContext::Execute(std::string_view command, const json& params)
	{
		return m_Commands.Execute(*this, command, params);
	}

	void EditorContext::Defer(std::string command, json params)
	{
		m_DeferredCommands.emplace_back(std::move(command), std::move(params));
	}

	size_t EditorContext::ExecuteDeferred()
	{
		size_t failures = 0;
		// Commands may defer further commands; those run in the next call.
		std::vector<std::pair<std::string, json>> commands;
		commands.swap(m_DeferredCommands);
		if (!commands.empty())
			CommitInteractiveEdit();
		for (const auto& [command, params] : commands)
		{
			CommandResult result = Execute(command, params);
			if (!result.Success)
			{
				PX_CORE_WARN("{}: {}", command, result.Error);
				failures++;
			}
		}
		return failures;
	}

	std::optional<Entity> EditorContext::ResolveEntity(const json& id)
	{
		uint64_t value = 0;
		if (id.is_number_unsigned())
		{
			value = id.get<uint64_t>();
		}
		else if (id.is_string())
		{
			const std::string& text = id.get_ref<const std::string&>();
			auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
			if (ec != std::errc() || ptr != text.data() + text.size())
				return std::nullopt;
		}
		else
		{
			return std::nullopt;
		}
		return GetActiveScene().TryGetEntityByUUID(UUID(value));
	}

}
