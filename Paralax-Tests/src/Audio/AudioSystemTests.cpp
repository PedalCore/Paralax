#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Audio/AudioClip.h>
#include <Paralax/Audio/AudioSystem.h>
#include <Paralax/Scene/Components.h>
#include <Paralax/Scene/Entity.h>
#include <Paralax/Scene/Scene.h>

#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

using namespace Paralax;

namespace {

	constexpr uint32_t Rate = 48000;
	constexpr float Frame = 1.0f / 60.0f;

	// A sine tone of the given length at amplitude 0.5.
	Ref<AudioClip> Tone(float seconds, uint32_t channels = 1)
	{
		const size_t frames = static_cast<size_t>(seconds * static_cast<float>(Rate));
		std::vector<float> samples(frames * channels);
		for (size_t i = 0; i < frames; i++)
		{
			const float value = 0.5f * std::sin(2.0f * std::numbers::pi_v<float> * 440.0f * static_cast<float>(i) / static_cast<float>(Rate));
			for (uint32_t c = 0; c < channels; c++)
				samples[i * channels + c] = value;
		}
		return AudioClip::Create(channels, Rate, std::move(samples));
	}

	// Root mean square of one channel (0 = left, 1 = right) of interleaved stereo output, over a range of frames.
	float Rms(const std::vector<float>& stereo, size_t firstFrame, size_t frameCount, int channel)
	{
		double sum = 0.0;
		size_t count = 0;
		for (size_t frame = firstFrame; frame < firstFrame + frameCount && frame * 2 + 1 < stereo.size(); frame++)
		{
			const double value = static_cast<double>(stereo[frame * 2 + static_cast<size_t>(channel)]);
			sum += value * value;
			count++;
		}
		return count ? static_cast<float>(std::sqrt(sum / static_cast<double>(count))) : 0.0f;
	}

	struct AudioFixture
	{
		explicit AudioFixture(AudioSystemSpecification specification = Offline())
		{
			Audio = static_cast<AudioSystem*>(World.AddSystem(CreateScope<AudioSystem>(Assets, specification)));
		}
		~AudioFixture() { World.OnRuntimeStop(); }

		static AudioSystemSpecification Offline()
		{
			AudioSystemSpecification specification;
			specification.SampleRate = Rate;
			specification.MaxCapturedFrames = Rate * 10;
			return specification;
		}

		AssetHandle AddClip(Ref<AudioClip> clip)
		{
			clip->Handle = AssetHandle(m_NextHandle++);
			Assets.AddMemoryAsset(clip);
			return clip->Handle;
		}

		Entity Source(const std::string& name, AssetHandle clip, glm::vec3 position = glm::vec3(0.0f), bool spatial = false)
		{
			Entity entity = World.CreateEntity(name);
			entity.GetComponent<TransformComponent>().Translation = position;
			auto& source = entity.AddComponent<AudioSourceComponent>();
			source.Clip = clip;
			source.Spatial = spatial;
			return entity;
		}

		void Run(float seconds)
		{
			const int frames = static_cast<int>(std::lround(seconds / Frame));
			for (int i = 0; i < frames; i++)
				World.OnUpdateRuntime(Timestep(Frame));
		}

		const std::vector<float>& Output() const { return Audio->GetCapturedOutput(); }
		size_t OutputFrames() const { return Output().size() / 2; }
		// RMS of the left channel over the most recent seconds of output.
		float RecentLevel(float seconds = 0.1f) const
		{
			const size_t frames = static_cast<size_t>(seconds * static_cast<float>(Rate));
			return Rms(Output(), OutputFrames() > frames ? OutputFrames() - frames : 0, frames, 0);
		}

		AssetManager Assets;
		Scene World{"AudioTest"};
		AudioSystem* Audio = nullptr;

	private:
		uint64_t m_NextHandle = 9000;
	};

}

TEST_CASE("AudioSystem: offline mixing follows simulated time and carries the playing sources")
{
	AudioFixture fixture;
	const AssetHandle tone = fixture.AddClip(Tone(2.0f));
	Entity source = fixture.Source("Speaker", tone);
	fixture.World.OnRuntimeStart();
	CHECK(fixture.Audio->GetOutput() == AudioOutput::Offline);
	CHECK(fixture.Audio->IsPlaying(source.GetUUID())); // PlayOnStart.
	CHECK(fixture.Audio->GetVoiceCount() == 1);

	fixture.Run(0.5f);
	CHECK(fixture.OutputFrames() == Rate / 2); // Exactly the simulated time, fractions carried between frames.
	// A non-spatial mono source at volume 1: the tone's RMS (0.5 / sqrt 2) reaches both channels.
	CHECK(fixture.RecentLevel() == doctest::Approx(0.5 / std::numbers::sqrt2).epsilon(0.05));
	CHECK(Rms(fixture.Output(), 0, fixture.OutputFrames(), 1) == doctest::Approx(static_cast<double>(Rms(fixture.Output(), 0, fixture.OutputFrames(), 0))).epsilon(0.01));

	CHECK(fixture.Audio->Stop(source.GetUUID()));
	CHECK_FALSE(fixture.Audio->IsPlaying(source.GetUUID()));
	fixture.Run(0.2f);
	CHECK(fixture.RecentLevel() < 1e-4f);
	CHECK(fixture.OutputFrames() == Rate / 2 + Rate / 5); // Silence is still mixed, so output keeps time.
}

TEST_CASE("AudioSystem: volume, master volume and pitch shape the output")
{
	auto level = [](float volume, float master) {
		AudioFixture fixture;
		Entity source = fixture.Source("Speaker", fixture.AddClip(Tone(2.0f)));
		source.GetComponent<AudioSourceComponent>().Volume = volume;
		fixture.Audio->SetMasterVolume(master);
		fixture.World.OnRuntimeStart();
		fixture.Run(0.5f);
		return fixture.RecentLevel();
	};
	const float full = level(1.0f, 1.0f);
	CHECK(level(0.25f, 1.0f) == doctest::Approx(static_cast<double>(full) * 0.25).epsilon(0.02));
	CHECK(level(1.0f, 0.5f) == doctest::Approx(static_cast<double>(full) * 0.5).epsilon(0.02));
	CHECK(level(0.0f, 1.0f) < 1e-5f);

	// A 0.2 s clip at double speed is over after 0.1 s; at normal speed it is still playing then.
	AudioFixture fixture;
	const AssetHandle shortTone = fixture.AddClip(Tone(0.2f));
	Entity fast = fixture.Source("Fast", shortTone);
	fast.GetComponent<AudioSourceComponent>().Pitch = 2.0f;
	Entity normal = fixture.Source("Normal", shortTone);
	fixture.World.OnRuntimeStart();
	fixture.Run(0.15f);
	CHECK_FALSE(fixture.Audio->IsPlaying(fast.GetUUID()));
	CHECK(fixture.Audio->IsPlaying(normal.GetUUID()));

	fixture.Audio->SetMasterVolume(std::numeric_limits<float>::quiet_NaN()); // Ignored.
	CHECK(fixture.Audio->GetMasterVolume() == 1.0f);
	fixture.Audio->SetMasterVolume(50.0f); // Clamped.
	CHECK(fixture.Audio->GetMasterVolume() == 10.0f);
}

TEST_CASE("AudioSystem: clips end unless looping, and play, pause and stop control a source")
{
	AudioFixture fixture;
	const AssetHandle shortTone = fixture.AddClip(Tone(0.1f));
	Entity once = fixture.Source("Once", shortTone);
	Entity looping = fixture.Source("Looping", shortTone);
	looping.GetComponent<AudioSourceComponent>().Loop = true;
	Entity waiting = fixture.Source("Waiting", shortTone);
	waiting.GetComponent<AudioSourceComponent>().PlayOnStart = false;
	fixture.World.OnRuntimeStart();
	CHECK_FALSE(fixture.Audio->IsPlaying(waiting.GetUUID()));

	fixture.Run(0.5f);
	CHECK_FALSE(fixture.Audio->IsPlaying(once.GetUUID()));
	CHECK(fixture.Audio->IsPlaying(looping.GetUUID()));

	// Pause keeps the position; Play resumes. Play on a stopped (or finished) source starts it again.
	CHECK(fixture.Audio->Pause(looping.GetUUID()));
	CHECK_FALSE(fixture.Audio->IsPlaying(looping.GetUUID()));
	CHECK(fixture.Audio->Play(looping.GetUUID()));
	CHECK(fixture.Audio->IsPlaying(looping.GetUUID()));
	CHECK(fixture.Audio->Play(once.GetUUID()));
	CHECK(fixture.Audio->Play(waiting.GetUUID()));
	CHECK(fixture.Audio->IsPlaying(waiting.GetUUID()));
	fixture.Run(0.05f);
	CHECK(fixture.Audio->IsPlaying(once.GetUUID()));

	// Entities without a playable source.
	Entity silent = fixture.World.CreateEntity("Silent");
	CHECK_FALSE(fixture.Audio->Play(silent.GetUUID()));
	CHECK_FALSE(fixture.Audio->Stop(silent.GetUUID()));
	CHECK_FALSE(fixture.Audio->Pause(UUID(12345)));
	Entity unloaded = fixture.Source("Unloaded", AssetHandle(424242));
	fixture.Run(Frame);
	CHECK_FALSE(fixture.Audio->Play(unloaded.GetUUID()));
}

TEST_CASE("AudioSystem: sources follow the scene as components and entities change")
{
	AudioFixture fixture;
	const AssetHandle first = fixture.AddClip(Tone(2.0f));
	const AssetHandle second = fixture.AddClip(Tone(2.0f, 2));
	Entity source = fixture.Source("Speaker", first);
	fixture.World.OnRuntimeStart();
	fixture.Run(0.1f);

	// A new clip carries on playing.
	source.GetComponent<AudioSourceComponent>().Clip = second;
	fixture.Run(Frame);
	CHECK(fixture.Audio->IsPlaying(source.GetUUID()));
	CHECK(fixture.Audio->GetVoiceCount() == 1);

	// A source added while running starts (PlayOnStart), and Play on it before its first update counts.
	Entity late = fixture.Source("Late", first);
	late.GetComponent<AudioSourceComponent>().PlayOnStart = false;
	CHECK(fixture.Audio->Play(late.GetUUID()));
	CHECK(fixture.Audio->IsPlaying(late.GetUUID()));
	fixture.Run(Frame);
	CHECK(fixture.Audio->IsPlaying(late.GetUUID()));
	CHECK(fixture.Audio->GetVoiceCount() == 2);

	// Removing the component or destroying the entity silences it.
	source.RemoveComponent<AudioSourceComponent>();
	fixture.Run(Frame);
	CHECK(fixture.Audio->GetVoiceCount() == 1);
	CHECK_FALSE(fixture.Audio->IsPlaying(source.GetUUID()));
	const UUID lateId = late.GetUUID();
	fixture.World.DestroyEntity(late);
	CHECK(fixture.Audio->GetVoiceCount() == 0);
	CHECK_FALSE(fixture.Audio->IsPlaying(lateId));
	fixture.Run(0.1f);
	CHECK(fixture.RecentLevel(0.05f) < 1e-4f);
}

TEST_CASE("AudioSystem: spatial sources fade with distance and pan toward their side")
{
	AudioFixture fixture;
	const AssetHandle tone = fixture.AddClip(Tone(2.0f));
	Entity listener = fixture.World.CreateEntity("Ears");
	listener.AddComponent<AudioListenerComponent>(); // At the origin, facing -Z.
	Entity near = fixture.Source("Near", tone, glm::vec3(2.0f, 0.0f, 0.0f), true);
	Entity far = fixture.Source("Far", tone, glm::vec3(20.0f, 0.0f, 0.0f), true);
	for (Entity entity : {near, far})
	{
		auto& source = entity.GetComponent<AudioSourceComponent>();
		source.MinDistance = 1.0f;
		source.MaxDistance = 100.0f;
		source.PlayOnStart = false;
	}
	fixture.World.OnRuntimeStart();

	auto measure = [&](Entity entity) {
		fixture.Audio->Play(entity.GetUUID());
		fixture.Run(0.3f);
		const size_t frames = Rate / 10;
		const float left = Rms(fixture.Output(), fixture.OutputFrames() - frames, frames, 0);
		const float right = Rms(fixture.Output(), fixture.OutputFrames() - frames, frames, 1);
		fixture.Audio->Stop(entity.GetUUID());
		fixture.Run(0.1f);
		return std::pair{left, right};
	};
	const auto [nearLeft, nearRight] = measure(near);
	const auto [farLeft, farRight] = measure(far);
	CHECK(nearRight > nearLeft * 1.5f);  // On the listener's right.
	CHECK(farRight < nearRight * 0.25f); // Ten times further: much quieter.
	CHECK(farRight > 0.0f);

	// Turning the listener around puts the source on its left.
	listener.GetComponent<TransformComponent>().Rotation = glm::angleAxis(std::numbers::pi_v<float>, glm::vec3(0.0f, 1.0f, 0.0f));
	const auto [turnedLeft, turnedRight] = measure(near);
	CHECK(turnedLeft > turnedRight * 1.5f);
}

TEST_CASE("AudioSystem: one-shots play once, and the voice limit counts only sounding voices")
{
	AudioSystemSpecification specification = AudioFixture::Offline();
	specification.MaxVoices = 2;
	AudioFixture fixture(specification);
	const AssetHandle shortTone = fixture.AddClip(Tone(0.1f));
	Paralax::Testing::ScopedLogCapture log;
	Entity a = fixture.Source("A", shortTone);
	a.GetComponent<AudioSourceComponent>().Loop = true;
	// Many idle sources (doors, pickups) take no voices.
	for (int i = 0; i < 10; i++)
		fixture.Source("Idle" + std::to_string(i), shortTone).GetComponent<AudioSourceComponent>().PlayOnStart = false;
	fixture.World.OnRuntimeStart();
	CHECK(fixture.Audio->GetVoiceCount() == 1);

	CHECK(fixture.Audio->PlayOneShot(shortTone));
	CHECK(fixture.Audio->GetVoiceCount() == 2);
	CHECK_FALSE(fixture.Audio->PlayOneShot(shortTone, glm::vec3(1.0f))); // Limit reached.
	Entity b = fixture.Source("B", shortTone);
	fixture.Run(Frame);
	CHECK_FALSE(fixture.Audio->IsPlaying(b.GetUUID())); // Refused a voice for PlayOnStart...
	CHECK(log.Contains(spdlog::level::warn, "sounds are already playing"));

	fixture.Run(0.3f); // ...and not started later behind the game's back when the one-shot frees its voice.
	CHECK(fixture.Audio->GetVoiceCount() == 1);
	CHECK_FALSE(fixture.Audio->IsPlaying(b.GetUUID()));
	CHECK(fixture.Audio->Play(b.GetUUID()));
	CHECK_FALSE(fixture.Audio->Play(fixture.World.FindEntityByName("Idle0")->GetUUID())); // Full again.

	CHECK_FALSE(fixture.Audio->PlayOneShot(AssetHandle(424242)));
	CHECK_FALSE(fixture.Audio->PlayOneShot(shortTone, glm::vec3(std::numeric_limits<float>::quiet_NaN())));
	CHECK_FALSE(fixture.Audio->PlayOneShot(shortTone, std::nullopt, -1.0f));
}

TEST_CASE("AudioSystem: a source can be replayed the moment its clip ends")
{
	// 0.25 s at 60 updates per second: the clip ends on an update boundary, and Play comes on the very next one.
	AudioFixture fixture;
	Entity source = fixture.Source("Beep", fixture.AddClip(Tone(0.25f)));
	fixture.World.OnRuntimeStart();
	fixture.Run(0.25f);
	CHECK_FALSE(fixture.Audio->IsPlaying(source.GetUUID()));
	CHECK(fixture.Audio->Play(source.GetUUID()));
	CHECK(fixture.Audio->IsPlaying(source.GetUUID()));
	fixture.Run(0.1f);
	CHECK(fixture.Audio->IsPlaying(source.GetUUID()));
	CHECK(fixture.RecentLevel(0.05f) > 0.3f); // Actually sounding, not just reported as playing.
}

TEST_CASE("AudioSystem: play on start applies once, and state survives clip changes")
{
	AudioFixture fixture;
	const AssetHandle first = fixture.AddClip(Tone(2.0f));
	const AssetHandle second = fixture.AddClip(Tone(2.0f, 2));
	Entity stopped = fixture.Source("Stopped", first);
	Entity paused = fixture.Source("Paused", first);
	Entity early = fixture.Source("Early", first); // Paused by a script before its first update.
	fixture.World.OnRuntimeStart();
	fixture.Run(0.1f);
	CHECK(fixture.Audio->Stop(stopped.GetUUID()));
	CHECK(fixture.Audio->Pause(paused.GetUUID()));

	// A new clip: a stopped source stays stopped (PlayOnStart does not fire again), a paused one stays paused.
	stopped.GetComponent<AudioSourceComponent>().Clip = second;
	paused.GetComponent<AudioSourceComponent>().Clip = second;
	fixture.Run(0.1f);
	CHECK_FALSE(fixture.Audio->IsPlaying(stopped.GetUUID()));
	CHECK_FALSE(fixture.Audio->IsPlaying(paused.GetUUID()));
	CHECK(fixture.Audio->Play(paused.GetUUID())); // Resumes (with the new clip).
	CHECK(fixture.Audio->IsPlaying(paused.GetUUID()));
	CHECK(fixture.Audio->IsPlaying(early.GetUUID()));

	Entity late = fixture.Source("Late", first);
	CHECK(fixture.Audio->Pause(late.GetUUID())); // Controlled before its first update: no automatic start.
	fixture.Run(0.1f);
	CHECK_FALSE(fixture.Audio->IsPlaying(late.GetUUID()));
}

TEST_CASE("AudioSystem: pausing the game holds sounds, and playback follows simulated time")
{
	AudioFixture fixture;
	Entity source = fixture.Source("Tone", fixture.AddClip(Tone(0.5f)));
	fixture.World.OnRuntimeStart();
	fixture.Run(0.2f);
	fixture.Audio->SetPaused(true);
	CHECK(fixture.Audio->IsPaused());
	// A paused game gets no updates; nothing ends meanwhile, however long the pause.
	CHECK(fixture.Audio->IsPlaying(source.GetUUID()));
	fixture.Audio->SetPaused(false);
	fixture.Run(0.25f);
	CHECK(fixture.Audio->IsPlaying(source.GetUUID())); // 0.45 s of 0.5 s.
	fixture.Run(0.1f);
	CHECK_FALSE(fixture.Audio->IsPlaying(source.GetUUID()));
	CHECK(fixture.Audio->GetVoiceCount() == 0);
}

TEST_CASE("AudioSystem: offline output is identical across runs, and hostile values are made safe")
{
	auto render = []() {
		AudioFixture fixture;
		const AssetHandle tone = fixture.AddClip(Tone(1.0f));
		Entity listener = fixture.World.CreateEntity("Ears");
		listener.AddComponent<AudioListenerComponent>();
		Entity source = fixture.Source("Moving", tone, glm::vec3(-5.0f, 0.0f, 0.0f), true);
		source.GetComponent<AudioSourceComponent>().Loop = true;
		fixture.World.OnRuntimeStart();
		for (int i = 0; i < 30; i++)
		{
			source.GetComponent<TransformComponent>().Translation.x += 0.3f;
			fixture.World.OnUpdateRuntime(Timestep(Frame));
		}
		return fixture.Output();
	};
	const std::vector<float> first = render();
	CHECK(first == render());

	AudioFixture fixture;
	Entity source = fixture.Source("Wild", fixture.AddClip(Tone(1.0f)), glm::vec3(std::numeric_limits<float>::infinity()), true);
	auto& component = source.GetComponent<AudioSourceComponent>();
	component.Volume = std::numeric_limits<float>::quiet_NaN();
	component.Pitch = 1e9f;
	component.MinDistance = -5.0f;
	component.MaxDistance = std::numeric_limits<float>::quiet_NaN();
	fixture.World.OnRuntimeStart();
	CHECK_NOTHROW(fixture.Run(0.2f));
	for (float sample : fixture.Output())
		REQUIRE(std::isfinite(sample));
}

TEST_CASE("AudioSystem: without a running scene nothing plays, and the specification is verified")
{
	AssetManager assets;
	AudioSystem audio(assets);
	CHECK(audio.GetOutput() == AudioOutput::Offline);
	CHECK_FALSE(audio.Play(UUID(1)));
	CHECK_FALSE(audio.IsPlaying(UUID(1)));
	CHECK_FALSE(audio.PlayOneShot(AssetHandle(1)));
	CHECK(audio.GetVoiceCount() == 0);

	// A device is used if there is one; otherwise mixing falls back to offline. Either way the scene runs.
	AudioSystemSpecification device;
	device.Output = AudioOutput::Device;
	AudioFixture withDevice(device);
	withDevice.Source("Speaker", withDevice.AddClip(Tone(0.5f)));
	withDevice.World.OnRuntimeStart();
	CHECK_NOTHROW(withDevice.Run(0.1f));
	CHECK(withDevice.Audio->GetVoiceCount() == 1);
	withDevice.Audio->SetPaused(true);
	withDevice.Audio->SetPaused(false);
	// Playback state follows simulated time on a device too: the 0.5 s clip is over after 0.6 s of updates.
	withDevice.Run(0.5f);
	CHECK(withDevice.Audio->GetVoiceCount() == 0);

	Paralax::Testing::ScopedAssertionCapture capture;
	AudioSystemSpecification slow;
	slow.SampleRate = 100;
	CHECK_THROWS_AS(AudioSystem(assets, slow), Paralax::Testing::AssertionFailedError);
	AudioSystemSpecification none;
	none.MaxVoices = 0;
	CHECK_THROWS_AS(AudioSystem(assets, none), Paralax::Testing::AssertionFailedError);
}
