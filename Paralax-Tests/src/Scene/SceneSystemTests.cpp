#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Scene/Entity.h>
#include <Paralax/Scene/Scene.h>
#include <Paralax/Scene/SceneSystem.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace Paralax;

namespace {

	// Records every callback; optionally destroys an entity from OnUpdate or OnEntityDestroying.
	class RecordingSystem : public SceneSystem
	{
	public:
		explicit RecordingSystem(std::vector<std::string>& log, std::string name = "Recorder")
			: m_Log(log)
			, m_Name(std::move(name))
		{
		}

		const char* GetName() const override { return m_Name.c_str(); }
		void OnRuntimeStart(Scene&) override { m_Log.push_back(m_Name + ":start"); }
		void OnUpdate(Scene& scene, Timestep timestep) override
		{
			m_Log.push_back(m_Name + ":update " + std::to_string(timestep.GetMilliseconds()));
			if (DestroyOnUpdate)
			{
				if (std::optional<Entity> entity = scene.FindEntityByName(*DestroyOnUpdate))
				{
					scene.DestroyEntity(*entity);
					// Deferred: still alive until the update finishes.
					m_Log.push_back(scene.FindEntityByName(*DestroyOnUpdate) ? "deferred" : "immediate");
				}
			}
		}
		void OnFixedUpdate(Scene& scene, Timestep timestep) override
		{
			m_Log.push_back(m_Name + ":fixed " + std::to_string(timestep.GetMilliseconds()));
			if (DestroyOnFixedUpdate)
			{
				if (std::optional<Entity> entity = scene.FindEntityByName(*DestroyOnFixedUpdate))
				{
					scene.DestroyEntity(*entity);
					m_Log.push_back(scene.FindEntityByName(*DestroyOnFixedUpdate) ? "deferred" : "immediate");
				}
			}
			if (ReenterFixedUpdate)
				scene.OnFixedUpdateRuntime(timestep);
		}
		bool WantsFixedUpdate() const override { return WantsFixed; }
		void OnRuntimeStop(Scene&) override { m_Log.push_back(m_Name + ":stop"); }
		void OnEntityDestroying(Scene& scene, const Entity& entity) override
		{
			m_Log.push_back(m_Name + ":destroying " + entity.GetName());
			if (DestroyFromCallback)
			{
				if (std::optional<Entity> other = scene.FindEntityByName(*DestroyFromCallback))
				{
					DestroyFromCallback.reset();
					scene.DestroyEntity(*other);
				}
			}
		}

		std::optional<std::string> DestroyOnUpdate;
		std::optional<std::string> DestroyOnFixedUpdate;
		bool ReenterFixedUpdate = false;
		bool WantsFixed = false;
		std::optional<std::string> DestroyFromCallback;

	private:
		std::vector<std::string>& m_Log;
		std::string m_Name;
	};

}

TEST_CASE("SceneSystem: systems run in order through the runtime lifecycle")
{
	std::vector<std::string> log;
	Scene scene;
	scene.AddSystem(CreateScope<RecordingSystem>(log, "A"));
	scene.AddSystem(CreateScope<RecordingSystem>(log, "B"));
	CHECK(scene.GetSystem<RecordingSystem>() != nullptr);

	scene.OnUpdateRuntime(Timestep(0.5f)); // Not running: ignored.
	scene.OnRuntimeStop();                 // Not running: ignored.
	scene.OnRuntimeStart();
	scene.OnRuntimeStart(); // Already running: ignored.
	scene.OnUpdateRuntime(Timestep(0.5f));
	scene.OnRuntimeStop();
	CHECK(log == std::vector<std::string>{"A:start", "B:start", "A:update 500.000000", "B:update 500.000000", "B:stop", "A:stop"});
}

TEST_CASE("SceneSystem: systems see destroyed subtrees parent first, while entities are still valid")
{
	std::vector<std::string> log;
	Scene scene;
	auto* system = static_cast<RecordingSystem*>(scene.AddSystem(CreateScope<RecordingSystem>(log)));
	Entity root = scene.CreateEntity("Root");
	Entity child = scene.CreateEntity("Child");
	Entity grandchild = scene.CreateEntity("Grandchild");
	scene.SetParent(child, root);
	scene.SetParent(grandchild, child);
	scene.CreateEntity("Bystander");

	scene.DestroyEntity(scene.CreateEntity("BeforeStart")); // Not running: no callbacks.
	scene.OnRuntimeStart();
	log.clear();

	// A callback destroying another entity is deferred while callbacks run, then carried out (and reported) before
	// DestroyEntity returns.
	system->DestroyFromCallback = "Bystander";
	scene.DestroyEntity(root);
	CHECK(log == std::vector<std::string>{"Recorder:destroying Root", "Recorder:destroying Child", "Recorder:destroying Grandchild",
										  "Recorder:destroying Bystander"});
	CHECK(scene.GetEntityCount() == 0);
	scene.OnRuntimeStop();
}

TEST_CASE("SceneSystem: destruction requested during an update is deferred to its end")
{
	std::vector<std::string> log;
	Scene scene;
	auto* system = static_cast<RecordingSystem*>(scene.AddSystem(CreateScope<RecordingSystem>(log)));
	scene.CreateEntity("Target");
	system->DestroyOnUpdate = "Target";
	scene.OnRuntimeStart();
	scene.OnUpdateRuntime(Timestep(0.0f));
	CHECK(std::find(log.begin(), log.end(), "deferred") != log.end());
	CHECK_FALSE(scene.FindEntityByName("Target"));
	scene.OnRuntimeStop();
}

TEST_CASE("SceneSystem: fixed updates run every system in order, only while running")
{
	std::vector<std::string> log;
	Scene scene;
	scene.AddSystem(CreateScope<RecordingSystem>(log, "A"));
	scene.AddSystem(CreateScope<RecordingSystem>(log, "B"));
	scene.OnFixedUpdateRuntime(Timestep(0.25f)); // Not running: ignored.
	scene.OnRuntimeStart();
	scene.OnFixedUpdateRuntime(Timestep(0.25f));
	scene.OnRuntimeStop();
	scene.OnFixedUpdateRuntime(Timestep(0.25f));
	CHECK(log == std::vector<std::string>{"A:start", "B:start", "A:fixed 250.000000", "B:fixed 250.000000", "B:stop", "A:stop"});
}

TEST_CASE("SceneSystem: destruction requested during a fixed update waits for the update around it")
{
	std::vector<std::string> log;
	Scene scene;
	auto* system = static_cast<RecordingSystem*>(scene.AddSystem(CreateScope<RecordingSystem>(log)));
	scene.CreateEntity("Target");
	scene.CreateEntity("Other");
	system->DestroyOnFixedUpdate = "Target";
	scene.OnRuntimeStart();

	// On its own (not inside an update), the destruction happens when the fixed update finishes.
	scene.OnFixedUpdateRuntime(Timestep(0.0f));
	CHECK(std::find(log.begin(), log.end(), "deferred") != log.end());
	CHECK_FALSE(scene.FindEntityByName("Target"));

	// Inside an update (as the PhysicsSystem calls it), it waits for the update.
	struct FixedStepper : SceneSystem
	{
		const char* GetName() const override { return "Stepper"; }
		void OnUpdate(Scene& running, Timestep) override
		{
			running.OnFixedUpdateRuntime(Timestep(0.0f));
			StillThere = running.FindEntityByName("Other").has_value();
		}
		bool StillThere = false;
	};
	scene.OnRuntimeStop();
	Scene nested;
	auto* recorder = static_cast<RecordingSystem*>(nested.AddSystem(CreateScope<RecordingSystem>(log)));
	auto* stepper = static_cast<FixedStepper*>(nested.AddSystem(CreateScope<FixedStepper>()));
	nested.CreateEntity("Other");
	recorder->DestroyOnFixedUpdate = "Other";
	nested.OnRuntimeStart();
	nested.OnUpdateRuntime(Timestep(0.0f));
	CHECK(stepper->StillThere);
	CHECK_FALSE(nested.FindEntityByName("Other"));
	nested.OnRuntimeStop();
}

TEST_CASE("SceneSystem: pending destruction covers the entity and its descendants until the update ends")
{
	Scene scene;
	Entity root = scene.CreateEntity("Root");
	Entity child = scene.CreateEntity("Child");
	Entity grandchild = scene.CreateEntity("Grandchild");
	Entity other = scene.CreateEntity("Other");
	scene.SetParent(child, root);
	scene.SetParent(grandchild, child);
	struct Destroyer : SceneSystem
	{
		const char* GetName() const override { return "Destroyer"; }
		void OnUpdate(Scene& running, Timestep) override
		{
			Entity target = *running.FindEntityByName("Child");
			Before = running.IsDestructionPending(target);
			running.DestroyEntity(target);
			for (const char* name : {"Root", "Child", "Grandchild", "Other"})
				After.push_back(running.IsDestructionPending(*running.FindEntityByName(name)));
		}
		bool Before = true;
		std::vector<bool> After;
	};
	auto* destroyer = static_cast<Destroyer*>(scene.AddSystem(CreateScope<Destroyer>()));
	CHECK_FALSE(scene.IsDestructionPending(root));
	CHECK_FALSE(scene.IsDestructionPending(Entity()));
	scene.OnRuntimeStart();
	scene.OnUpdateRuntime(Timestep(0.0f));
	CHECK_FALSE(destroyer->Before);
	CHECK(destroyer->After == std::vector<bool>{false, true, true, false});
	CHECK_FALSE(scene.IsValid(child));
	CHECK_FALSE(scene.IsValid(grandchild));
	CHECK_FALSE(scene.IsDestructionPending(other));
	CHECK_FALSE(scene.IsDestructionPending(child)); // Gone, not pending.
	scene.OnRuntimeStop();
}

TEST_CASE("SceneSystem: the scene wants fixed updates while running if any system does")
{
	std::vector<std::string> log;
	Scene scene;
	scene.AddSystem(CreateScope<RecordingSystem>(log));
	auto* wanting = static_cast<RecordingSystem*>(scene.AddSystem(CreateScope<RecordingSystem>(log, "Wanting")));
	wanting->WantsFixed = true;
	CHECK_FALSE(scene.WantsFixedUpdate()); // Not running.
	scene.OnRuntimeStart();
	CHECK(scene.WantsFixedUpdate());
	wanting->WantsFixed = false;
	CHECK_FALSE(scene.WantsFixedUpdate());
	scene.OnRuntimeStop();
}

TEST_CASE("SceneSystem: a fixed update started from inside one is refused")
{
	std::vector<std::string> log;
	Scene scene;
	auto* system = static_cast<RecordingSystem*>(scene.AddSystem(CreateScope<RecordingSystem>(log)));
	system->ReenterFixedUpdate = true;
	scene.OnRuntimeStart();
#if defined(PX_ENABLE_ASSERTS)
	{
		Paralax::Testing::ScopedAssertionCapture capture;
		CHECK_THROWS_AS(scene.OnFixedUpdateRuntime(Timestep(0.0f)), Paralax::Testing::AssertionFailedError);
	}
#else
	// Dist: the inner call is ignored.
	scene.OnFixedUpdateRuntime(Timestep(0.0f));
	CHECK(std::count(log.begin(), log.end(), "Recorder:fixed 0.000000") == 1);
#endif
	// The scene is still usable afterwards.
	system->ReenterFixedUpdate = false;
	log.clear();
	scene.OnFixedUpdateRuntime(Timestep(0.0f));
	CHECK(log == std::vector<std::string>{"Recorder:fixed 0.000000"});
	scene.OnRuntimeStop();
}

TEST_CASE("SceneSystem: systems are not copied and cannot be added while running")
{
	std::vector<std::string> log;
	Scene scene;
	scene.AddSystem(CreateScope<RecordingSystem>(log));
	Ref<Scene> copy = Scene::Copy(scene);
	CHECK(copy->GetSystem<RecordingSystem>() == nullptr);

	scene.OnRuntimeStart();
	Paralax::Testing::ScopedAssertionCapture capture;
	CHECK_THROWS_AS(scene.AddSystem(CreateScope<RecordingSystem>(log)), Paralax::Testing::AssertionFailedError);
	CHECK_THROWS_AS(scene.AddSystem(nullptr), Paralax::Testing::AssertionFailedError);
	scene.OnRuntimeStop();
}

TEST_CASE("SceneSystem: CopySubtreeFrom copies between scenes with new IDs")
{
	Scene source;
	Entity root = source.CreateEntity("Root");
	Entity child = source.CreateEntity("Child");
	source.SetParent(child, root);
	Scene destination;
	Entity parent = destination.CreateEntity("Parent");
	Entity copy = destination.CopySubtreeFrom(source, root, parent);
	CHECK(copy.GetUUID() != root.GetUUID());
	CHECK(destination.GetParent(copy) == parent);
	CHECK(destination.GetEntityCount() == 3);
	CHECK(destination.GetRootEntities().size() == 1);
	Entity rootCopy = destination.CopySubtreeFrom(source, root);
	CHECK(destination.GetRootEntities().back() == rootCopy.GetUUID());

	Paralax::Testing::ScopedAssertionCapture capture;
	Entity stranger = source.CreateEntity("Stranger");
	CHECK_THROWS_AS(destination.CopySubtreeFrom(source, root, stranger), Paralax::Testing::AssertionFailedError);
}

namespace {

	// Moves an outside entity into the dying subtree from its destruction callback.
	class AdoptingSystem : public SceneSystem
	{
	public:
		const char* GetName() const override { return "Adopter"; }
		void OnEntityDestroying(Scene& scene, const Entity& entity) override
		{
			Notified.push_back(entity.GetName());
			if (entity.GetName() == "Root")
			{
				if (std::optional<Entity> orphan = scene.FindEntityByName("Orphan"))
					scene.SetParent(*orphan, entity);
			}
		}
		std::vector<std::string> Notified;
	};

}

TEST_CASE("SceneSystem: an entity moved into a dying subtree during callbacks is notified and destroyed")
{
	Scene scene;
	auto* system = static_cast<AdoptingSystem*>(scene.AddSystem(CreateScope<AdoptingSystem>()));
	Entity root = scene.CreateEntity("Root");
	scene.CreateEntity("Orphan");
	scene.OnRuntimeStart();
	scene.DestroyEntity(root);
	CHECK(system->Notified == std::vector<std::string>{"Root", "Orphan"});
	CHECK(scene.GetEntityCount() == 0);
	scene.OnRuntimeStop();
}

TEST_CASE("SceneSystem: destroying a running scene stops its systems first")
{
	std::vector<std::string> log;
	{
		Scene scene;
		scene.AddSystem(CreateScope<RecordingSystem>(log));
		scene.OnRuntimeStart();
	}
	CHECK(log == std::vector<std::string>{"Recorder:start", "Recorder:stop"});
}
