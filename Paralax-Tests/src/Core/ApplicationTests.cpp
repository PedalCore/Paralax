#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Core/Application.h>
#include <Paralax/Core/Input.h>
#include <Paralax/Events/KeyEvent.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace Paralax;
using Paralax::Testing::AssertionFailedError;
using Paralax::Testing::ScopedAssertionCapture;

namespace {

	ApplicationSpecification TestSpecification(uint64_t maxFrames = 0)
	{
		ApplicationSpecification specification;
		specification.Name = "Test Application";
		specification.Window.Title = "Test Application";
		specification.Window.Width = 64;
		specification.Window.Height = 64;
		specification.Window.Headless = true;
		specification.MaxFrames = maxFrames;
		specification.FixedTimestep = 1.0f / 60.0f;
		return specification;
	}

	// Records lifecycle calls into a shared log.
	class RecordingLayer : public Layer
	{
	public:
		RecordingLayer(std::string name, std::vector<std::string>& log, bool handleEvents = false)
			: Layer(std::move(name))
			, m_Log(log)
			, m_HandleEvents(handleEvents)
		{
		}

		void OnAttach() override { m_Log.push_back(GetName() + ":attach"); }
		void OnDetach() override { m_Log.push_back(GetName() + ":detach"); }
		void OnUpdate(Timestep ts) override
		{
			Updates++;
			LastTimestep = ts;
		}
		void OnEvent(Event& event) override
		{
			m_Log.push_back(GetName() + ":" + event.GetName());
			if (m_HandleEvents)
				event.Handled = true;
		}

		int Updates = 0;
		float LastTimestep = 0.0f;

	private:
		std::vector<std::string>& m_Log;
		bool m_HandleEvents;
	};

}

TEST_CASE("Application: runs for MaxFrames with a fixed timestep")
{
	std::vector<std::string> log;
	Application app(TestSpecification(5));

	REQUIRE(Application::Exists());
	CHECK(&Application::Get() == &app);
	CHECK(app.GetWindow().IsHeadless());

	auto* layer = app.PushLayer<RecordingLayer>("Game", log);
	app.Run();

	CHECK(app.GetFrameCount() == 5);
	CHECK(layer->Updates == 5);
	CHECK(layer->LastTimestep == 1.0f / 60.0f);
	CHECK_FALSE(app.IsRunning());
}

TEST_CASE("Application: Close stops the loop after the current frame")
{
	class ClosingLayer : public Layer
	{
	public:
		void OnUpdate(Timestep) override
		{
			if (++Updates == 3)
				Application::Get().Close();
		}
		int Updates = 0;
	};

	Application app(TestSpecification());
	auto* layer = app.PushLayer<ClosingLayer>();
	app.Run();
	CHECK(layer->Updates == 3);
	CHECK(app.GetFrameCount() == 3);
}

TEST_CASE("Application: layers attach in push order and detach in reverse on destruction")
{
	std::vector<std::string> log;
	{
		Application app(TestSpecification(1));
		app.PushOverlay<RecordingLayer>("Overlay", log);
		app.PushLayer<RecordingLayer>("Game", log);
	}
	CHECK(log == std::vector<std::string>{"Overlay:attach", "Game:attach", "Overlay:detach", "Game:detach"});
	CHECK_FALSE(Application::Exists());
}

TEST_CASE("Application: events reach overlays first and stop once handled")
{
	std::vector<std::string> log;
	Application app(TestSpecification(1));
	app.PushLayer<RecordingLayer>("Game", log);
	app.PushOverlay<RecordingLayer>("Overlay", log, true);
	log.clear();

	KeyPressedEvent event(Key::A);
	app.OnEvent(event);

	CHECK(event.Handled);
	CHECK(log == std::vector<std::string>{"Overlay:KeyPressed"});
	// Input state is updated even when a layer handles the event.
	CHECK(Input::IsKeyDown(Key::A));
}

TEST_CASE("Application: injected input is visible to layers during the frame and cleared after it")
{
	class InputProbeLayer : public Layer
	{
	public:
		void OnUpdate(Timestep) override
		{
			PressedSeen.push_back(Input::WasKeyPressed(Key::Space));
			DownSeen.push_back(Input::IsKeyDown(Key::Space));
		}
		std::vector<bool> PressedSeen, DownSeen;
	};

	Application app(TestSpecification(3));
	auto* probe = app.PushLayer<InputProbeLayer>();
	app.SubmitToMainThread([&app] {
		KeyPressedEvent event(Key::Space);
		app.OnEvent(event); });
	app.Run();

	CHECK(probe->PressedSeen == std::vector<bool>{true, false, false});
	CHECK(probe->DownSeen == std::vector<bool>{true, true, true});
}

TEST_CASE("Application: window close event stops the loop unless a layer vetoes it")
{
	std::vector<std::string> log;

	SUBCASE("not vetoed")
	{
		Application app(TestSpecification(100));
		app.PushLayer<RecordingLayer>("Game", log);
		app.SubmitToMainThread([&app] {
			WindowCloseEvent event;
			app.OnEvent(event); });
		app.Run();
		CHECK(app.GetFrameCount() == 1);
	}

	SUBCASE("vetoed by a layer")
	{
		Application app(TestSpecification(4));
		app.PushLayer<RecordingLayer>("Editor", log, true);
		app.SubmitToMainThread([&app] {
			WindowCloseEvent event;
			app.OnEvent(event); });
		app.Run();
		CHECK(app.GetFrameCount() == 4);
	}
}

TEST_CASE("Application: a zero-size resize suspends layer updates until the window is restored")
{
	class MinimizeDriver : public Layer
	{
	public:
		void OnUpdate(Timestep) override
		{
			if (++Updates != 1)
				return;

			Application& app = Application::Get();
			WindowResizeEvent minimize(0, 0);
			app.OnEvent(minimize);

			// Restore two frames later: the outer task runs at the start of frame 2, the inner one at frame 3.
			app.SubmitToMainThread([&app] { app.SubmitToMainThread([&app] {
					WindowResizeEvent restore(64, 64);
					app.OnEvent(restore); }); });
		}
		int Updates = 0;
	};

	std::vector<std::string> log;
	Application app(TestSpecification(6));
	auto* game = app.PushLayer<RecordingLayer>("Game", log);
	app.PushLayer<MinimizeDriver>();
	app.Run();

	CHECK(app.GetFrameCount() == 6);
	// Updated on frame 1, skipped on frame 2 (minimized), updated again on frames 3-6.
	CHECK(game->Updates == 5);
}

TEST_CASE("Application: work submitted from another thread while the loop runs executes on the main thread in order")
{
	constexpr int taskCount = 1000;
	// Large frame cap only as a safety net; the test closes the app once every task has run.
	Application app(TestSpecification(1'000'000));
	std::vector<int> order;
	const std::thread::id mainThread = std::this_thread::get_id();
	bool ranOnMainThread = true;

	class StartSignalLayer : public Layer
	{
	public:
		explicit StartSignalLayer(std::atomic<bool>& started)
			: m_Started(started)
		{
		}
		void OnUpdate(Timestep) override { m_Started = true; }

	private:
		std::atomic<bool>& m_Started;
	};

	std::atomic<bool> started = false;
	app.PushLayer<StartSignalLayer>(started);

	std::thread worker([&] {
		while (!started)
			std::this_thread::yield();
		for (int i = 0; i < taskCount; i++)
		{
			app.SubmitToMainThread([&, i] {
				if (std::this_thread::get_id() != mainThread)
					ranOnMainThread = false;
				order.push_back(i);
				if (i == taskCount - 1)
					app.Close();
			});
		}
	});

	app.Run();
	worker.join();

	REQUIRE(order.size() == static_cast<size_t>(taskCount));
	for (int i = 0; i < taskCount; i++)
		REQUIRE(order[static_cast<size_t>(i)] == i);
	CHECK(ranOnMainThread);
}

TEST_CASE("Application: layers pushed during OnUpdate are attached after the update pass")
{
	std::vector<std::string> log;

	class SpawnerLayer : public Layer
	{
	public:
		explicit SpawnerLayer(std::vector<std::string>& log)
			: Layer("Spawner")
			, m_Log(log)
		{
		}
		void OnUpdate(Timestep) override
		{
			if (Updates++ != 0)
				return;
			// Enough pushes to force the layer vector to reallocate mid-iteration if pushes were applied immediately.
			for (int i = 0; i < 16; i++)
				Spawned.push_back(Application::Get().PushLayer<RecordingLayer>("Spawned" + std::to_string(i), m_Log));
			Overlay = Application::Get().PushOverlay<RecordingLayer>("SpawnedOverlay", m_Log);
			AttachedDuringUpdate = !m_Log.empty();
		}
		int Updates = 0;
		bool AttachedDuringUpdate = false;
		std::vector<RecordingLayer*> Spawned;
		RecordingLayer* Overlay = nullptr;

	private:
		std::vector<std::string>& m_Log;
	};

	Application app(TestSpecification(3));
	auto* spawner = app.PushLayer<SpawnerLayer>(log);
	auto* after = app.PushLayer<RecordingLayer>("After", log);
	log.clear();
	app.Run();

	CHECK_FALSE(spawner->AttachedDuringUpdate);
	REQUIRE(spawner->Spawned.size() == 16);
	CHECK(log.front() == "Spawned0:attach");
	CHECK(log.back() == "SpawnedOverlay:attach");
	// Spawned layers first update on the next frame; the layer after the spawner still updated on frame 1.
	for (RecordingLayer* spawned : spawner->Spawned)
		CHECK(spawned->Updates == 2);
	CHECK(spawner->Overlay->Updates == 2);
	CHECK(after->Updates == 3);
}

TEST_CASE("Application: a layer can pop itself during OnEvent")
{
	std::vector<std::string> log;

	class SelfRemovingLayer : public RecordingLayer
	{
	public:
		using RecordingLayer::RecordingLayer;
		void OnEvent(Event& event) override
		{
			RecordingLayer::OnEvent(event);
			Application::Get().PopOverlay(this);
		}
	};

	Application app(TestSpecification(2));
	app.PushLayer<RecordingLayer>("Game", log);
	app.PushOverlay<SelfRemovingLayer>("Popup", log);
	log.clear();

	KeyPressedEvent first(Key::A);
	app.OnEvent(first);
	KeyPressedEvent second(Key::B);
	app.OnEvent(second);

	CHECK(log == std::vector<std::string>{"Popup:KeyPressed", "Game:KeyPressed", "Popup:detach", "Game:KeyPressed"});
}

TEST_CASE("Application: a layer popped by another during OnEvent receives no further events")
{
	std::vector<std::string> log;

	class RemoverOverlay : public RecordingLayer
	{
	public:
		RemoverOverlay(std::vector<std::string>& log, Layer*& victim)
			: RecordingLayer("Remover", log)
			, m_Victim(victim)
		{
		}
		void OnEvent(Event& event) override
		{
			RecordingLayer::OnEvent(event);
			if (m_Victim)
				Application::Get().PopLayer(std::exchange(m_Victim, nullptr));
		}

	private:
		Layer*& m_Victim;
	};

	Application app(TestSpecification(1));
	Layer* victim = app.PushLayer<RecordingLayer>("Victim", log);
	app.PushOverlay<RemoverOverlay>(log, victim);
	log.clear();

	KeyPressedEvent event(Key::A);
	app.OnEvent(event);

	CHECK(log == std::vector<std::string>{"Remover:KeyPressed", "Victim:detach"});
}

TEST_CASE("Application: a layer popped by an earlier layer during OnUpdate is not updated that frame")
{
	std::vector<std::string> log;

	class KillerLayer : public Layer
	{
	public:
		explicit KillerLayer(Layer*& victim)
			: m_Victim(victim)
		{
		}
		void OnUpdate(Timestep) override
		{
			if (m_Victim)
				Application::Get().PopLayer(std::exchange(m_Victim, nullptr));
		}

	private:
		Layer*& m_Victim;
	};

	class UpdateLoggingLayer : public Layer
	{
	public:
		UpdateLoggingLayer(std::string name, std::vector<std::string>& log)
			: Layer(std::move(name))
			, m_Log(log)
		{
		}
		void OnUpdate(Timestep) override { m_Log.push_back(GetName() + ":update"); }
		void OnDetach() override { m_Log.push_back(GetName() + ":detach"); }

	private:
		std::vector<std::string>& m_Log;
	};

	Application app(TestSpecification(3));
	Layer* victimPointer = nullptr;
	app.PushLayer<KillerLayer>(victimPointer);
	auto* victim = app.PushLayer<UpdateLoggingLayer>("Victim", log);
	auto* survivor = app.PushLayer<RecordingLayer>("Survivor", log);
	victimPointer = victim;
	log.clear();

	app.Run();

	CHECK(log == std::vector<std::string>{"Victim:detach"});
	CHECK(survivor->Updates == 3);
}

TEST_CASE("Application: layer changes requested from OnDetach during destruction are safe and discarded")
{
	std::vector<std::string> log;

	class GreedyLayer : public RecordingLayer
	{
	public:
		GreedyLayer(std::vector<std::string>& log, Layer*& other)
			: RecordingLayer("Greedy", log)
			, m_Log(log)
			, m_Other(other)
		{
		}
		void OnDetach() override
		{
			RecordingLayer::OnDetach();
			Application& app = Application::Get();
			app.PopLayer(m_Other);
			for (int i = 0; i < 16; i++)
				app.PushLayer<RecordingLayer>("Late", m_Log);
		}

	private:
		std::vector<std::string>& m_Log;
		Layer*& m_Other;
	};

	Layer* first = nullptr;
	{
		Application app(TestSpecification(1));
		first = app.PushLayer<RecordingLayer>("First", log);
		app.PushLayer<GreedyLayer>(log, first);
		app.PushLayer<RecordingLayer>("Last", log);
		log.clear();
	}

	// Every layer detaches exactly once, in reverse order; late pushes are never attached.
	CHECK(log == std::vector<std::string>{"Last:detach", "Greedy:detach", "First:detach"});
}

TEST_CASE("Application: PopLayer outside iteration detaches immediately; unknown layers are ignored with a warning")
{
	Paralax::Testing::ScopedLogCapture capture;
	std::vector<std::string> log;
	Application app(TestSpecification(1));
	auto* layer = app.PushLayer<RecordingLayer>("Game", log);
	auto* overlay = app.PushOverlay<RecordingLayer>("Overlay", log);

	app.PopOverlay(layer); // wrong kind
	CHECK(capture.Contains(spdlog::level::warn, "PopOverlay ignored"));

	app.PopLayer(layer);
	app.PopOverlay(overlay);
	CHECK(log == std::vector<std::string>{"Game:attach", "Overlay:attach", "Game:detach", "Overlay:detach"});
}

TEST_CASE("Application: pushing a null layer is rejected with an error")
{
	Paralax::Testing::ScopedLogCapture capture;
	Application app(TestSpecification(1));
	CHECK(app.PushLayer(Scope<Layer>()) == nullptr);
	CHECK(app.PushOverlay(Scope<Layer>()) == nullptr);
	CHECK(capture.Contains(spdlog::level::err, "Cannot push a null layer"));
}

TEST_CASE("Application: a minimize event suspends layer updates until restored")
{
	std::vector<std::string> log;
	Application app(TestSpecification(4));
	auto* game = app.PushLayer<RecordingLayer>("Game", log);

	WindowMinimizeEvent minimize(true);
	app.OnEvent(minimize);
	app.SubmitToMainThread([&app] {
		app.SubmitToMainThread([&app] {
			WindowMinimizeEvent restore(false);
			app.OnEvent(restore);
		});
	});
	app.Run();

	// Frame 1 is skipped (minimized); the outer task runs at the start of frame 1 and the inner restore at the
	// start of frame 2, so frames 2-4 update.
	CHECK(game->Updates == 3);
}

TEST_CASE("Application: window creation failure leaves a non-running, uninitialised application")
{
	Paralax::Testing::ScopedLogCapture capture;
	ApplicationSpecification specification = TestSpecification(5);
	specification.Window.Width = 0;
	{
		Application app(specification);
		CHECK_FALSE(app.IsInitialized());
		CHECK_FALSE(app.IsRunning());
		CHECK(Application::Exists());

		app.Run();
		CHECK(app.GetFrameCount() == 0);

		Paralax::Testing::ScopedAssertionCapture assertCapture;
		CHECK_THROWS_AS(app.GetWindow(), AssertionFailedError);
	}
	CHECK_FALSE(Application::Exists());
	CHECK(capture.Contains(spdlog::level::err, "failed to initialise"));

	// A new application can be created afterwards.
	Application next(TestSpecification(1));
	CHECK(next.IsInitialized());
}

TEST_CASE("Application: Get without an application is a verified precondition")
{
	REQUIRE_FALSE(Application::Exists());
	ScopedAssertionCapture capture;
	CHECK_THROWS_AS(Application::Get(), AssertionFailedError);
}

TEST_CASE("Application: the log file is written for the application's lifetime")
{
	const std::filesystem::path path = Paralax::Testing::CreateTestDirectory("AppLog") / "App.log";
	{
		ApplicationSpecification specification = TestSpecification(1);
		specification.LogFile = path;
		Application app(specification);
		PX_INFO("inside application");
	}
	PX_INFO("after application");

	const std::string contents = Paralax::Testing::ReadFileText(path);
	CHECK(contents.find("inside application") != std::string::npos);
	CHECK(contents.find("after application") == std::string::npos);
}

TEST_CASE("Application: only one instance may exist")
{
	Application app(TestSpecification(1));
	ScopedAssertionCapture capture;
	CHECK_THROWS_AS(Application second(TestSpecification(1)), AssertionFailedError);
	CHECK(&Application::Get() == &app);
}

TEST_CASE("ApplyCommandLineOptions: parses standard engine options")
{
	ApplicationSpecification specification;
	specification.CommandLineArgs = CommandLine({"--headless", "--frames", "12", "--fixed-timestep=0.5"});
	REQUIRE(ApplyCommandLineOptions(specification));
	CHECK(specification.Window.Headless);
	CHECK(specification.MaxFrames == 12);
	CHECK(specification.FixedTimestep == 0.5f);
}

TEST_CASE("ApplyCommandLineOptions: --headless followed by a positional argument keeps the argument")
{
	ApplicationSpecification specification;
	specification.CommandLineArgs = CommandLine({"--headless", "project.pxproj"}, GetStandardFlagOptions());
	REQUIRE(ApplyCommandLineOptions(specification));
	CHECK(specification.Window.Headless);
	CHECK(specification.CommandLineArgs.GetPositional() == std::vector<std::string>{"project.pxproj"});
}

TEST_CASE("ApplyCommandLineOptions: --headless accepts explicit boolean values")
{
	for (auto [arguments, expected] : {std::pair<const char*, bool>{"--headless=true", true}, {"--headless=1", true}, {"--headless=false", false}, {"--headless=0", false}})
	{
		CAPTURE(arguments);
		ApplicationSpecification specification;
		specification.Window.Headless = !expected;
		specification.CommandLineArgs = CommandLine({std::string(arguments)});
		REQUIRE(ApplyCommandLineOptions(specification));
		CHECK(specification.Window.Headless == expected);
	}
}

TEST_CASE("ApplyCommandLineOptions: rejects malformed values and leaves the specification unchanged")
{
	for (const char* arguments : {"--frames=abc", "--fixed-timestep=0", "--fixed-timestep=-1", "--fixed-timestep=1x", "--fixed-timestep", "--fixed-timestep=inf"})
	{
		CAPTURE(arguments);
		ApplicationSpecification specification;
		specification.CommandLineArgs = CommandLine({"--headless", arguments});
		CHECK_FALSE(ApplyCommandLineOptions(specification));
		CHECK_FALSE(specification.Window.Headless);
		CHECK(specification.MaxFrames == 0);
		CHECK(specification.FixedTimestep == 0.0f);
	}
}
