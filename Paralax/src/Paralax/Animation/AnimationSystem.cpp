#include "pxpch.h"
#include "Paralax/Animation/AnimationSystem.h"

#include "Paralax/Animation/AnimationClip.h"
#include "Paralax/Asset/AssetManager.h"
#include "Paralax/Scene/Components.h"
#include "Paralax/Scene/Entity.h"
#include "Paralax/Scene/Scene.h"

#include <cmath>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Paralax {

	struct AnimationSystem::Impl
	{
		explicit Impl(AssetManager& assets)
			: Assets(assets)
		{
		}

		// What a component's clip resolves to: its channels' target entities, found once, and again while one is
		// missing or when the clip (or its version, after a reload) changes.
		struct Binding
		{
			AssetHandle Clip = 0;
			uint32_t Version = 0;
			std::vector<std::optional<UUID>> Targets;
			std::optional<float> AppliedTime; // The time last applied, so a paused clip is posed again when seeked.
		};

		AssetManager& Assets;
		Scene* ActiveScene = nullptr;
		std::unordered_map<uint64_t, Binding> Bindings;
		// Entities whose current problem of each kind has been logged, so it is reported once rather than every update;
		// cleared when the problem goes away, so a later one is reported again.
		std::unordered_set<uint64_t> ReportedClip;
		std::unordered_set<uint64_t> ReportedTargets;

		// Whether a bound target is still the entity the path names: below the host by those names (it may have
		// been renamed or moved elsewhere since). Walks up from the target, so it costs the path's length.
		static bool StillMatches(Scene& scene, Entity host, Entity target, const std::vector<std::string>& path)
		{
			Entity current = target;
			for (auto name = path.rbegin(); name != path.rend(); ++name)
			{
				if (current.GetName() != *name)
					return false;
				std::optional<Entity> parent = scene.GetParent(current);
				if (!parent)
					return false;
				current = *parent;
			}
			return current == host;
		}

		static std::optional<Entity> FindTarget(Scene& scene, Entity host, const std::vector<std::string>& path)
		{
			Entity current = host;
			for (const std::string& name : path)
			{
				std::optional<Entity> next;
				for (UUID child : current.GetComponent<RelationshipComponent>().Children)
				{
					std::optional<Entity> candidate = scene.TryGetEntityByUUID(child);
					if (candidate && candidate->GetName() == name)
					{
						next = candidate;
						break;
					}
				}
				if (!next)
					return std::nullopt;
				current = *next;
			}
			return current;
		}

		Binding& Bind(Scene& scene, Entity host, const AnimationClipAsset& clip)
		{
			Binding& binding = Bindings[host.GetUUID()];
			bool stale = binding.Clip != clip.Handle || binding.Version != clip.Version || binding.Targets.size() != clip.Channels.size();
			// A target that is gone, no longer matches its path, or was never found, is looked for again.
			for (size_t i = 0; !stale && i < binding.Targets.size(); i++)
			{
				std::optional<Entity> target = binding.Targets[i] ? scene.TryGetEntityByUUID(*binding.Targets[i]) : std::nullopt;
				stale = !target || !StillMatches(scene, host, *target, clip.Channels[i].Target);
			}
			if (!stale)
				return binding;

			std::vector<std::optional<UUID>> targets(clip.Channels.size());
			bool missing = false;
			for (size_t i = 0; i < clip.Channels.size(); i++)
			{
				if (std::optional<Entity> target = FindTarget(scene, host, clip.Channels[i].Target))
					targets[i] = target->GetUUID();
				else
					missing = true;
			}
			// Pose again (even a paused clip) only when something changed: another clip, or different targets.
			if (binding.Clip != clip.Handle || binding.Version != clip.Version || targets != binding.Targets)
				binding.AppliedTime.reset();
			binding.Clip = clip.Handle;
			binding.Version = clip.Version;
			binding.Targets = std::move(targets);
			if (!missing)
				ReportedTargets.erase(host.GetUUID());
			else if (ReportedTargets.insert(host.GetUUID()).second)
				PX_CORE_WARN("Animation: '{}' plays '{}', but some of the entities it animates are not below it; they are skipped", host.GetName(), clip.Name);
			return binding;
		}

		static void Apply(Scene& scene, const AnimationClipAsset& clip, const Binding& binding, float time)
		{
			for (size_t i = 0; i < clip.Channels.size(); i++)
			{
				if (!binding.Targets[i])
					continue;
				std::optional<Entity> target = scene.TryGetEntityByUUID(*binding.Targets[i]);
				if (!target)
					continue;
				const AnimationChannel& channel = clip.Channels[i];
				const glm::vec4 value = AnimationClipAsset::Sample(channel, time);
				auto& transform = target->GetComponent<TransformComponent>();
				switch (channel.Property)
				{
					case AnimationProperty::Translation: transform.Translation = glm::vec3(value); break;
					case AnimationProperty::Rotation: transform.Rotation = glm::quat(value.w, value.x, value.y, value.z); break;
					case AnimationProperty::Scale: transform.Scale = glm::vec3(value); break;
				}
			}
		}

		// Advances one component (by seconds of scene time; 0 only poses it) and applies its clip.
		void Animate(Scene& scene, Entity host, float seconds)
		{
			auto& component = host.GetComponent<AnimationComponent>();
			if (!component.Clip.IsValid())
				return;
			Ref<AnimationClipAsset> clip = Assets.GetAsset<AnimationClipAsset>(component.Clip);
			if (!clip)
			{
				if (ReportedClip.insert(host.GetUUID()).second)
					PX_CORE_WARN("Animation: '{}' has no usable clip ({}); nothing is played", host.GetName(), static_cast<uint64_t>(component.Clip));
				return;
			}
			ReportedClip.erase(host.GetUUID());
			const float duration = clip->Duration;

			if (component.Playing)
			{
				const float speed = std::isfinite(component.Speed) ? component.Speed : 0.0f;
				float time = std::isfinite(component.Time) ? component.Time + seconds * speed : 0.0f;
				if (duration <= 0.0f)
				{
					// A single pose: shown, and done at once unless it loops.
					time = 0.0f;
					if (!component.Loop)
						component.Playing = false;
				}
				else if (component.Loop)
				{
					time = std::fmod(time, duration);
					if (time < 0.0f)
						time += duration;
				}
				else if (time >= duration || time <= 0.0f)
				{
					// Ends at the end it was heading for.
					const bool finished = (time >= duration && speed > 0.0f) || (time <= 0.0f && speed < 0.0f);
					time = std::clamp(time, 0.0f, duration);
					if (finished)
						component.Playing = false;
				}
				component.Time = time;
			}

			Binding& binding = Bind(scene, host, *clip);
			// The pose for the component's time: wrapped into the clip for a looping clip (as playing it would), clamped
			// otherwise; a seek while paused shows the same pose playing from there would.
			float time = std::isfinite(component.Time) ? component.Time : 0.0f;
			if (component.Loop && duration > 0.0f)
			{
				time = std::fmod(time, duration);
				if (time < 0.0f)
					time += duration;
			}
			time = std::clamp(time, 0.0f, std::max(duration, 0.0f));
			// A paused clip is posed only when its time is changed (a seek), so it leaves the entities alone otherwise.
			if (!component.Playing && binding.AppliedTime == time)
				return;
			Apply(scene, *clip, binding, time);
			binding.AppliedTime = time;
		}

		void AnimateAll(Scene& scene, float seconds)
		{
			// Hierarchy order, so two clips animating the same entity resolve the same way every time.
			std::vector<Entity> animated;
			std::unordered_set<uint64_t> hosts;
			scene.ForEachEntity([&](Entity entity) {
				if (entity.HasComponent<AnimationComponent>())
				{
					animated.push_back(entity);
					hosts.insert(entity.GetUUID());
				}
			});
			// A removed component takes its state with it: added again (even paused, at the time last applied), it is
			// posed afresh like a new one, and a problem with it is reported again.
			std::erase_if(Bindings, [&](const auto& entry) { return !hosts.contains(entry.first); });
			std::erase_if(ReportedClip, [&](uint64_t host) { return !hosts.contains(host); });
			std::erase_if(ReportedTargets, [&](uint64_t host) { return !hosts.contains(host); });
			for (Entity entity : animated)
			{
				if (scene.IsValid(entity) && entity.HasComponent<AnimationComponent>())
					Animate(scene, entity, seconds);
			}
		}
	};

	AnimationSystem::AnimationSystem(AssetManager& assets)
		: m_Impl(CreateScope<Impl>(assets))
	{
	}

	AnimationSystem::~AnimationSystem() = default;

	void AnimationSystem::OnRuntimeStart(Scene& scene)
	{
		m_Impl->ActiveScene = &scene;
		m_Impl->Bindings.clear();
		m_Impl->ReportedClip.clear();
		m_Impl->ReportedTargets.clear();
		// The first frame shows every clip at its starting time.
		m_Impl->AnimateAll(scene, 0.0f);
	}

	void AnimationSystem::OnUpdate(Scene& scene, Timestep timestep)
	{
		if (m_Impl->ActiveScene != &scene)
			return;
		const float seconds = std::isfinite(timestep.GetSeconds()) ? std::max(timestep.GetSeconds(), 0.0f) : 0.0f;
		m_Impl->AnimateAll(scene, seconds);
	}

	void AnimationSystem::OnRuntimeStop(Scene& scene)
	{
		if (m_Impl->ActiveScene != &scene)
			return;
		m_Impl->ActiveScene = nullptr;
		m_Impl->Bindings.clear();
		m_Impl->ReportedClip.clear();
		m_Impl->ReportedTargets.clear();
	}

	void AnimationSystem::OnEntityDestroying(Scene& scene, const Entity& entity)
	{
		if (m_Impl->ActiveScene != &scene)
			return;
		m_Impl->Bindings.erase(entity.GetUUID());
		m_Impl->ReportedClip.erase(entity.GetUUID());
		m_Impl->ReportedTargets.erase(entity.GetUUID());
	}

}
