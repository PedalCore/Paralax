#include "pxpch.h"
#include "Paralax/Game/GameSession.h"

#include "Paralax/Animation/AnimationSystem.h"
#include "Paralax/Asset/AssetManager.h"
#include "Paralax/Core/PathEncoding.h"
#include "Paralax/Physics/PhysicsSystem.h"
#include "Paralax/Scene/Scene.h"
#include "Paralax/Scene/SceneSerializer.h"

namespace Paralax {

	GameSession::GameSession(AssetManager& assets, const Scene& scene, GameSessionSpecification specification)
		: m_Assets(assets)
		, m_Specification(std::move(specification))
		, m_EntityIds(m_Specification.EntityIdSeed)
	{
		Start(Scene::Copy(scene));
	}

	GameSession::~GameSession()
	{
		Stop();
	}

	void GameSession::Start(Ref<Scene> level)
	{
		m_Level = std::move(level);
		m_Scene = Scene::Copy(*m_Level);
		m_Scene->SetEntityIdSequence(m_EntityIds);
		// Scripts first: forces they apply in OnUpdate act in the same update's physics step. Animation next, so clips
		// scripts start or seek take effect at once and animated kinematic bodies move in the same physics step. Audio
		// last, so it hears where everything else left things this frame.
		ScriptSystemSpecification scripts = m_Specification.Scripts;
		scripts.State = &m_State;
		m_Scripts = static_cast<ScriptSystem*>(m_Scene->AddSystem(CreateScope<ScriptSystem>(m_Assets, scripts)));
		m_Animation = static_cast<AnimationSystem*>(m_Scene->AddSystem(CreateScope<AnimationSystem>(m_Assets)));
		m_Physics = static_cast<PhysicsSystem*>(m_Scene->AddSystem(CreateScope<PhysicsSystem>()));
		AudioSystemSpecification audio;
		audio.Output = m_Specification.Audio;
		m_Audio = static_cast<AudioSystem*>(m_Scene->AddSystem(CreateScope<AudioSystem>(m_Assets, audio)));
		m_Audio->SetPaused(m_AudioPaused);
		m_Scene->OnRuntimeStart();
	}

	void GameSession::Stop()
	{
		if (!m_Scene)
			return;
		m_AudioPaused = m_Audio->IsPaused();
		// Stopping runs OnDestroy, which can fail too: collect the errors after it.
		m_Scene->OnRuntimeStop();
		RecordErrors(m_Scripts->GetErrors());
		// Taken back after OnDestroy, which may create entities too; the next level continues from here.
		if (const std::optional<UUIDSequence>& ids = m_Scene->GetEntityIdSequence())
			m_EntityIds = *ids;
		m_Scene = nullptr;
		m_Scripts = nullptr;
		m_Animation = nullptr;
		m_Physics = nullptr;
		m_Audio = nullptr;
	}

	void GameSession::End()
	{
		Stop();
	}

	void GameSession::RecordErrors(const std::vector<std::string>& errors)
	{
		m_EarlierErrors.insert(m_EarlierErrors.end(), errors.begin(), errors.end());
		const size_t limit = m_Specification.Scripts.MaxRecordedErrors;
		if (m_EarlierErrors.size() > limit)
			m_EarlierErrors.erase(m_EarlierErrors.begin(), m_EarlierErrors.end() - static_cast<std::ptrdiff_t>(limit));
	}

	void GameSession::Update(Timestep timestep)
	{
		PX_CORE_VERIFY(m_Scene, "GameSession::Update after End");
		m_Scene->OnUpdateRuntime(timestep);
		if (std::optional<AssetHandle> request = m_Scripts->TakeSceneRequest())
			SwitchScene(*request);
	}

	void GameSession::SwitchScene(AssetHandle target)
	{
		Ref<Scene> level = m_Level;
		if (target.IsValid())
		{
			// The script checked that the handle is a registered scene; the file may still be missing or broken.
			const AssetMetadata* metadata = m_Assets.GetMetadata(target);
			level = metadata ? SceneSerializer::LoadFromFile(m_Assets.ToAbsolutePath(metadata->FilePath)) : nullptr;
			if (!level)
			{
				// Reported like a script error, so agents and --fail-on-script-error see it.
				const std::string message = fmt::format("Scene.Load: the scene '{}' could not be loaded; the game stays in the current scene",
														metadata ? ToUtf8(metadata->FilePath) : std::to_string(static_cast<uint64_t>(target)));
				PX_CORE_ERROR("{}", message);
				RecordErrors({message});
				return;
			}
		}
		Stop();
		if (target.IsValid())
			m_SceneAsset = target;
		m_Switches++;
		Start(std::move(level));
	}

	std::vector<std::string> GameSession::GetScriptErrors() const
	{
		std::vector<std::string> errors = m_EarlierErrors;
		if (m_Scripts)
		{
			const std::vector<std::string>& current = m_Scripts->GetErrors();
			errors.insert(errors.end(), current.begin(), current.end());
		}
		const size_t limit = m_Specification.Scripts.MaxRecordedErrors;
		if (errors.size() > limit)
			errors.erase(errors.begin(), errors.end() - static_cast<std::ptrdiff_t>(limit));
		return errors;
	}

	void GameSession::ClearScriptErrors()
	{
		m_EarlierErrors.clear();
		if (m_Scripts)
			m_Scripts->ClearErrors();
	}

}
