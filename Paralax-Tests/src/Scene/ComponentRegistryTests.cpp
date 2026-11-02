#include <doctest/doctest.h>

#include <Paralax/Scene/ComponentRegistry.h>
#include <Paralax/Scene/Entity.h>
#include <Paralax/Scene/SceneSerializer.h>

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <set>
#include <vector>

using namespace Paralax;
using json = nlohmann::json;

TEST_CASE("ComponentRegistry: component and field names are unique and lookups work")
{
	std::set<std::string> names;
	for (const ComponentInfo& info : ComponentRegistry::GetAll())
	{
		CHECK(names.insert(info.Name).second);
		std::set<std::string> fields;
		for (const FieldInfo& field : info.Fields)
		{
			CHECK(fields.insert(field.Name).second);
			CHECK(field.Access != nullptr);
		}
		CHECK(ComponentRegistry::Find(info.Name) == &info);
	}
	CHECK(ComponentRegistry::Find("Transform") != nullptr);
	CHECK(ComponentRegistry::Find("NoSuchComponent") == nullptr);
	CHECK(ComponentRegistry::Find("Transform")->FindField("Translation") != nullptr);
	CHECK(ComponentRegistry::Find("Transform")->FindField("Nope") == nullptr);
}

TEST_CASE("ComponentRegistry: every component round-trips through JSON")
{
	Scene scene;
	Entity entity = scene.CreateEntity("Source");
	Entity target = scene.CreateEntity("Target");
	entt::registry& registry = scene.GetRegistry();

	for (const ComponentInfo& info : ComponentRegistry::GetAll())
	{
		CAPTURE(info.Name);
		void* source = info.Add(registry, entity.GetHandle());
		const json original = ComponentRegistry::ToJson(info, source);
		CHECK(original.is_object());
		CHECK(original.size() == info.Fields.size());

		void* destination = info.Add(registry, target.GetHandle());
		std::string error;
		CHECK(ComponentRegistry::FromJson(info, destination, original, error));
		CHECK(error.empty());
		CHECK(ComponentRegistry::ToJson(info, destination) == original);
	}
}

TEST_CASE("ComponentRegistry: SetField validates types, ranges and enums without modifying on failure")
{
	CameraComponent camera;
	const ComponentInfo& info = *ComponentRegistry::Find("Camera");
	std::string error;

	const FieldInfo& fov = *info.FindField("FieldOfView");
	const float before = camera.Camera.PerspectiveFOV;
	CHECK_FALSE(ComponentRegistry::SetField(fov, &camera, "wide", error));
	CHECK(error.find("FieldOfView") != std::string::npos);
	CHECK_FALSE(ComponentRegistry::SetField(fov, &camera, 10.0, error)); // radians, > 179 degrees
	CHECK(camera.Camera.PerspectiveFOV == before);
	CHECK(ComponentRegistry::SetField(fov, &camera, 1.0, error));
	CHECK(camera.Camera.PerspectiveFOV == 1.0f);

	const FieldInfo& projection = *info.FindField("Projection");
	CHECK(ComponentRegistry::GetField(projection, &camera) == "Perspective");
	CHECK(ComponentRegistry::SetField(projection, &camera, "Orthographic", error));
	CHECK(camera.Camera.Projection == SceneCamera::ProjectionType::Orthographic);
	CHECK_FALSE(ComponentRegistry::SetField(projection, &camera, "Fisheye", error));
	CHECK(error.find("Perspective, Orthographic") != std::string::npos);
	CHECK_FALSE(ComponentRegistry::SetField(projection, &camera, 1, error));

	const FieldInfo& primary = *info.FindField("Primary");
	CHECK_FALSE(ComponentRegistry::SetField(primary, &camera, 1, error));
	CHECK(ComponentRegistry::SetField(primary, &camera, false, error));
	CHECK_FALSE(camera.Primary);
}

TEST_CASE("ComponentRegistry: vectors, quaternions and asset handles are validated and normalised")
{
	TransformComponent transform;
	const ComponentInfo& info = *ComponentRegistry::Find("Transform");
	std::string error;

	CHECK(ComponentRegistry::SetField(*info.FindField("Translation"), &transform, json::array({1, 2.5, -3}), error));
	CHECK(transform.Translation == glm::vec3(1.0f, 2.5f, -3.0f));
	CHECK_FALSE(ComponentRegistry::SetField(*info.FindField("Translation"), &transform, json::array({1, 2}), error));
	CHECK_FALSE(ComponentRegistry::SetField(*info.FindField("Translation"), &transform, json::array({1, "a", 3}), error));

	CHECK(ComponentRegistry::SetField(*info.FindField("Rotation"), &transform, json::array({0, 0, 0, 2}), error));
	CHECK(transform.Rotation == glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
	CHECK_FALSE(ComponentRegistry::SetField(*info.FindField("Rotation"), &transform, json::array({0, 0, 0, 0}), error));
	CHECK_FALSE(ComponentRegistry::SetField(*info.FindField("Rotation"), &transform, json::array({1e-30, 0, 0, 0}), error));
	CHECK(transform.Rotation == glm::quat(1.0f, 0.0f, 0.0f, 0.0f));

	// Components whose squares overflow a float still normalise (in float, the length became infinity and a zero
	// quaternion was stored), and the stored unit quaternion round-trips.
	CHECK(ComponentRegistry::SetField(*info.FindField("Rotation"), &transform, json::array({1e20, 0, 0, 0}), error));
	CHECK(transform.Rotation == glm::quat(0.0f, 1.0f, 0.0f, 0.0f));
	const double largest = static_cast<double>(FLT_MAX);
	CHECK(ComponentRegistry::SetField(*info.FindField("Rotation"), &transform, json::array({largest, -largest, largest, largest}), error));
	CHECK(transform.Rotation == glm::quat(0.5f, 0.5f, -0.5f, 0.5f));
	const json stored = ComponentRegistry::GetField(*info.FindField("Rotation"), &transform);
	CHECK(stored == json::array({0.5, -0.5, 0.5, 0.5}));
	TransformComponent reloaded;
	CHECK(ComponentRegistry::SetField(*info.FindField("Rotation"), &reloaded, stored, error));
	CHECK(reloaded.Rotation == transform.Rotation);

	// Storing is idempotent, so saving and loading never drifts a rotation. [3, 3, 3, 1] normalises to a quaternion
	// whose length is 4e-8 short of 1: dividing it by that length again would move w from 0.18898223 to 0.18898225,
	// so this pins the rule that a quaternion already unit to float precision is kept bit for bit.
	CHECK(ComponentRegistry::SetField(*info.FindField("Rotation"), &transform, json::array({3, 3, 3, 1}), error));
	const glm::quat normalised(0.18898223340511322f, 0.5669466853141785f, 0.5669466853141785f, 0.5669466853141785f);
	CHECK(transform.Rotation == normalised);
	CHECK(ComponentRegistry::SetField(*info.FindField("Rotation"), &reloaded, ComponentRegistry::GetField(*info.FindField("Rotation"), &transform), error));
	CHECK(reloaded.Rotation == normalised);
	// A rotation made in code round-trips too.
	const glm::quat quarterTurn = glm::angleAxis(glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
	transform.Rotation = quarterTurn;
	CHECK(ComponentRegistry::SetField(*info.FindField("Rotation"), &reloaded, ComponentRegistry::GetField(*info.FindField("Rotation"), &transform), error));
	CHECK(reloaded.Rotation == quarterTurn);

	MeshComponent mesh;
	const ComponentInfo& meshInfo = *ComponentRegistry::Find("Mesh");
	CHECK(ComponentRegistry::SetField(*meshInfo.FindField("Mesh"), &mesh, "18446744073709551615", error));
	CHECK(static_cast<uint64_t>(mesh.Mesh) == UINT64_MAX);
	CHECK(ComponentRegistry::GetField(*meshInfo.FindField("Mesh"), &mesh) == "18446744073709551615");
	CHECK_FALSE(ComponentRegistry::SetField(*meshInfo.FindField("Mesh"), &mesh, "12x", error));
	CHECK_FALSE(ComponentRegistry::SetField(*meshInfo.FindField("Mesh"), &mesh, -5, error));
	CHECK(ComponentRegistry::SetField(*meshInfo.FindField("Materials"), &mesh, json::array({"1", "2"}), error));
	CHECK(mesh.Materials.size() == 2);
	CHECK_FALSE(ComponentRegistry::SetField(*meshInfo.FindField("Materials"), &mesh, json::array({"1", true}), error));
	CHECK(mesh.Materials.size() == 2);
}

TEST_CASE("ComponentRegistry: FromJson applies valid fields and reports unknown or invalid ones")
{
	PointLightComponent light;
	const ComponentInfo& info = *ComponentRegistry::Find("PointLight");
	std::string error;
	CHECK_FALSE(ComponentRegistry::FromJson(info, &light, json{{"Intensity", 42}, {"Bogus", 1}, {"Range", "far"}}, error));
	CHECK(light.Intensity == 42.0f);
	CHECK(error.find("Bogus") != std::string::npos);
	CHECK(error.find("Range") != std::string::npos);
	CHECK_FALSE(ComponentRegistry::FromJson(info, &light, json::array(), error));
}

TEST_CASE("ComponentRegistry: numbers beyond float range are rejected")
{
	TransformComponent transform;
	const ComponentInfo& info = *ComponentRegistry::Find("Transform");
	std::string error;
	CHECK_FALSE(ComponentRegistry::SetField(*info.FindField("Translation"), &transform, json::array({1e300, 0, 0}), error));
	CHECK(transform.Translation == glm::vec3(0.0f));

	CameraComponent camera;
	CHECK_FALSE(ComponentRegistry::SetField(*ComponentRegistry::Find("Camera")->FindField("OrthographicNear"), &camera, -1e300, error));
}

TEST_CASE("ComponentRegistry: values at the edge of every range are refused or survive a reload unchanged")
{
	// A value accepted by SetField but refused when its scene is loaded again would be dropped by the next save and
	// open, undo or redo. Near a small bound, narrowing to float moves a value by more than the range's slack.
	size_t accepted = 0;
	size_t refused = 0;
	for (const ComponentInfo& info : ComponentRegistry::GetAll())
	{
		for (const FieldInfo& field : info.Fields)
		{
			const bool vector = field.Type == FieldType::Vec2 || field.Type == FieldType::Vec3 || field.Type == FieldType::Vec4;
			if ((field.Type != FieldType::Float && !vector) || !(field.Min < field.Max))
				continue;
			const double min = static_cast<double>(field.Min);
			const double max = static_cast<double>(field.Max);
			for (double candidate : {min - 1e-6, max + 1e-6, std::nextafter(min - 1e-6, -1e30), std::nextafter(max + 1e-6, 1e30), min, max, 0.009999, 0.000099})
			{
				CAPTURE(info.Name);
				CAPTURE(field.Name);
				CAPTURE(candidate);
				Scene scene;
				Entity entity = scene.CreateEntity("Probe");
				void* component = info.Add(scene.GetRegistry(), entity.GetHandle());
				const size_t components = field.Type == FieldType::Vec2 ? 2 : field.Type == FieldType::Vec3 ? 3
																											: 4;
				const json value = vector ? json(std::vector<double>(components, candidate)) : json(candidate);
				std::string error;
				if (!ComponentRegistry::SetField(field, component, value, error))
				{
					refused++;
					continue;
				}
				accepted++;
				const json stored = ComponentRegistry::GetField(field, component);
				Scene loaded;
				std::vector<std::string> warnings;
				REQUIRE(SceneSerializer::Deserialize(loaded, SceneSerializer::Serialize(scene), error, &warnings));
				CHECK(warnings.empty());
				Entity reloaded = loaded.GetEntityByUUID(entity.GetUUID());
				CHECK(ComponentRegistry::GetField(field, info.Get(loaded.GetRegistry(), reloaded.GetHandle())) == stored);
			}
		}
	}
	CHECK(accepted > 0);
	CHECK(refused > 0);
}

namespace {

	struct IntHolder
	{
		int Value = 7;
	};

}

TEST_CASE("ComponentRegistry: an unsigned value too large for an int field is refused, not wrapped")
{
	// Without a range only the 32-bit bound applies, so a wrapped value (UINT64_MAX read as signed is -1) would pass.
	const FieldInfo field = Reflection::MakeField<&IntHolder::Value>("Value");
	IntHolder holder;
	std::string error;
	CHECK_FALSE(ComponentRegistry::SetField(field, &holder, json(UINT64_MAX), error));
	CHECK_FALSE(ComponentRegistry::SetField(field, &holder, json(uint64_t(1) << 32), error));
	CHECK(holder.Value == 7);
	CHECK(ComponentRegistry::SetField(field, &holder, json(uint64_t(INT32_MAX)), error));
	CHECK(holder.Value == INT32_MAX);
	CHECK(ComponentRegistry::SetField(field, &holder, json(int64_t(INT32_MIN)), error));
	CHECK(holder.Value == INT32_MIN);
}

TEST_CASE("ComponentRegistry: orthographic clip planes have a range like the perspective ones")
{
	CameraComponent camera;
	const ComponentInfo& info = *ComponentRegistry::Find("Camera");
	std::string error;
	for (const char* name : {"OrthographicNear", "OrthographicFar"})
	{
		CAPTURE(name);
		const FieldInfo& field = *info.FindField(name);
		CHECK(field.Min < field.Max);
		CHECK_FALSE(ComponentRegistry::SetField(field, &camera, -1e7, error));
		CHECK_FALSE(ComponentRegistry::SetField(field, &camera, 1e7, error));
		CHECK(ComponentRegistry::SetField(field, &camera, -250.0, error));
	}
	CHECK(camera.Camera.OrthographicNear == -250.0f);
	CHECK(camera.Camera.OrthographicFar == -250.0f);
}

TEST_CASE("ComponentRegistry: names and tags are limited to 256 bytes by the registry")
{
	TagComponent tag;
	const ComponentInfo& info = *ComponentRegistry::Find("Tag");
	const FieldInfo& name = *info.FindField("Name");
	const FieldInfo& label = *info.FindField("Tag");
	CHECK(name.MaxLength == 256);
	CHECK(label.MaxLength == 256);
	CHECK(TagComponent::MaxLength == 256);

	std::string error;
	const std::string longest(256, 'n');
	CHECK(ComponentRegistry::SetField(name, &tag, longest, error));
	CHECK(tag.Name == longest);
	CHECK_FALSE(ComponentRegistry::SetField(name, &tag, longest + "n", error));
	CHECK(error.find("at most 256 bytes") != std::string::npos);
	CHECK(tag.Name == longest);
	CHECK_FALSE(ComponentRegistry::SetField(label, &tag, std::string(257, 't'), error));
	CHECK(tag.Tag.empty());
	CHECK(ComponentRegistry::SetField(label, &tag, std::string(256, 't'), error));
	CHECK(tag.Tag.size() == 256);
}

TEST_CASE("ComponentRegistry: engine-managed components cannot be removed through the registry")
{
	for (const char* name : {"Tag", "Transform", "PrefabInstance"})
	{
		CAPTURE(name);
		CHECK(ComponentRegistry::Find(name)->Remove == nullptr);
	}
	CHECK(ComponentRegistry::Find("PointLight")->Remove != nullptr);
	for (const ComponentInfo& info : ComponentRegistry::GetAll())
		CHECK(info.Copy != nullptr);
}

TEST_CASE("ComponentRegistry: a range on a vector field applies to every component")
{
	ColliderComponent collider;
	const ComponentInfo& info = *ComponentRegistry::Find("Collider");
	const FieldInfo& size = *info.FindField("Size");
	std::string error;

	CHECK_FALSE(ComponentRegistry::SetField(size, &collider, json::array({1, -1, 1}), error));
	CHECK(error.find("each in") != std::string::npos);
	CHECK_FALSE(ComponentRegistry::SetField(size, &collider, json::array({1, 0, 1}), error));   // Below the minimum.
	CHECK_FALSE(ComponentRegistry::SetField(size, &collider, json::array({1, 1, 1e9}), error)); // Above the maximum.
	CHECK(collider.Size == glm::vec3(1.0f));
	CHECK(ComponentRegistry::SetField(size, &collider, json::array({2, 0.5, 3}), error));
	CHECK(collider.Size == glm::vec3(2.0f, 0.5f, 3.0f));

	// Without a range (Min == Max) any finite components are accepted.
	CHECK(ComponentRegistry::SetField(*info.FindField("Offset"), &collider, json::array({-5, 0, 1e6}), error));
	CHECK(collider.Offset == glm::vec3(-5.0f, 0.0f, 1e6f));
}

TEST_CASE("ComponentRegistry: a GI volume keeps its probe counts within the renderer's limits")
{
	const ComponentInfo* info = ComponentRegistry::Find("GIVolume");
	REQUIRE(info);
	GIVolumeComponent volume;
	std::string error;
	CHECK(ComponentRegistry::SetField(*info->FindField("ProbeCountX"), &volume, 12, error));
	CHECK(volume.ProbeCountX == 12);
	CHECK_FALSE(ComponentRegistry::SetField(*info->FindField("ProbeCountX"), &volume, 13, error));
	CHECK_FALSE(ComponentRegistry::SetField(*info->FindField("ProbeCountY"), &volume, 1, error));
	CHECK_FALSE(ComponentRegistry::SetField(*info->FindField("ProbeSpacing"), &volume, 0.0f, error));
	CHECK(volume.ProbeCountX == 12);
	CHECK(volume.ProbeCountY == 4);
	CHECK(volume.ProbeSpacing == 1.0f);
}
