#include "pxpch.h"
#include "Paralax/Scripting/ScriptSystem.h"

#include "Paralax/Asset/AssetManager.h"
#include "Paralax/Audio/AudioSystem.h"
#include "Paralax/Core/Input.h"
#include "Paralax/Core/InputNames.h"
#include "Paralax/Core/JsonText.h"
#include "Paralax/Core/PathEncoding.h"
#include "Paralax/Game/GameState.h"
#include "Paralax/Math/Math.h"
#include "Paralax/Physics/PhysicsSystem.h"
#include "Paralax/Scene/ComponentRegistry.h"
#include "Paralax/Scene/Components.h"
#include "Paralax/Scene/Entity.h"
#include "Paralax/Scene/Prefab.h"
#include "Paralax/Scene/Scene.h"
#include "Paralax/Scripting/ScriptAsset.h"

#include <glm/gtc/quaternion.hpp>
#include <sol/sol.hpp>

#include <charconv>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

namespace Paralax {

	using json = nlohmann::json;

	namespace {

		// Instruction-count hook granularity: the budget is checked every this many VM instructions.
		constexpr int HookInterval = 1000;
		// Largest array a script may assign to an asset-array field.
		constexpr size_t MaxArrayElements = 4096;
		// Worst-case step estimate above which pattern-matching calls are refused (they are not instruction-metered).
		constexpr double PatternCostLimit = 1e9;

		// Accounts every allocation of a Lua state against a limit.
		struct LuaMemory
		{
			size_t Used = 0;
			size_t Limit = 0;
		};

		void* LuaAllocate(void* userData, void* pointer, size_t oldSize, size_t newSize)
		{
			auto* memory = static_cast<LuaMemory*>(userData);
			// When pointer is null, oldSize encodes the kind of object being allocated, not a size.
			const size_t previous = pointer ? oldSize : 0;
			if (newSize == 0)
			{
				std::free(pointer);
				memory->Used -= previous;
				return nullptr;
			}
			// Written so no intermediate can overflow: the growth must fit in the remaining budget.
			if (newSize > previous && newSize - previous > memory->Limit - memory->Used)
				return nullptr; // Lua turns this into a "not enough memory" error.
			void* result = std::realloc(pointer, newSize);
			if (result)
				memory->Used = memory->Used - previous + newSize;
			return result;
		}

		// xoshiro256** seeded through splitmix64: fast, high quality and identical on every platform.
		class Random
		{
		public:
			explicit Random(uint64_t seed = 0) { Seed(seed); }

			void Seed(uint64_t seed)
			{
				for (uint64_t& word : m_State)
				{
					seed += 0x9E3779B97F4A7C15ull;
					uint64_t z = seed;
					z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
					z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
					word = z ^ (z >> 31);
				}
			}

			uint64_t Next()
			{
				const uint64_t result = Rotate(m_State[1] * 5, 7) * 9;
				const uint64_t t = m_State[1] << 17;
				m_State[2] ^= m_State[0];
				m_State[3] ^= m_State[1];
				m_State[1] ^= m_State[2];
				m_State[0] ^= m_State[3];
				m_State[2] ^= t;
				m_State[3] = Rotate(m_State[3], 45);
				return result;
			}

			// Uniform in [0, 1).
			double NextDouble() { return static_cast<double>(Next() >> 11) * 0x1.0p-53; }

			// Uniform in [low, high] (requires low <= high), without modulo bias.
			int64_t NextInRange(int64_t low, int64_t high)
			{
				const uint64_t span = static_cast<uint64_t>(high) - static_cast<uint64_t>(low);
				if (span == UINT64_MAX)
					return static_cast<int64_t>(Next());
				const uint64_t range = span + 1;
				const uint64_t limit = UINT64_MAX - UINT64_MAX % range;
				uint64_t value = Next();
				while (value >= limit)
					value = Next();
				return static_cast<int64_t>(static_cast<uint64_t>(low) + value % range);
			}

		private:
			static uint64_t Rotate(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

			uint64_t m_State[4] = {};
		};

		// Scripts refer to entities by UUID; every access re-resolves it, so a stale reference to a destroyed entity
		// raises a clear error instead of touching freed data.
		struct ScriptEntity
		{
			UUID ID;
		};

		// A component of an entity, accessed field by field through reflection (values are copies).
		struct ComponentRef
		{
			UUID ID;
			const ComponentInfo* Info = nullptr;
		};

		std::string IdToString(UUID id)
		{
			return std::to_string(static_cast<uint64_t>(id));
		}

		std::optional<uint64_t> ParseId(const sol::object& value)
		{
			if (value.get_type() == sol::type::string)
			{
				const std::string text = value.as<std::string>();
				uint64_t parsed = 0;
				auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
				if (ec != std::errc() || ptr != text.data() + text.size() || text.empty())
					return std::nullopt;
				return parsed;
			}
			if (value.get_type() == sol::type::number && value.is<int64_t>())
			{
				const int64_t number = value.as<int64_t>();
				if (number >= 0)
					return static_cast<uint64_t>(number);
			}
			return std::nullopt;
		}

		// Raised inside bindings. This is the one deliberate use of exceptions in engine code (AGENTS rule 4): the
		// sol2 trampoline around every binding catches it and turns it into a Lua error, and every engine-initiated
		// call into Lua is protected, so it never reaches engine callers.
		[[noreturn]] void ScriptError(std::string message)
		{
			throw sol::error(std::move(message));
		}

		std::optional<float> AsFloat(const sol::object& value)
		{
			if (value.get_type() != sol::type::number)
				return std::nullopt;
			return static_cast<float>(value.as<double>());
		}

		// Reads N floats from a math userdata (glm type) or a Lua array table.
		template<typename Glm, size_t N>
		std::optional<json> FloatsToJson(const sol::object& value)
		{
			json array = json::array();
			if (value.is<Glm>())
			{
				const Glm& v = value.as<const Glm&>();
				if constexpr (std::is_same_v<Glm, glm::quat>)
					return json::array({v.x, v.y, v.z, v.w});
				else
				{
					for (size_t i = 0; i < N; i++)
						array.push_back(v[static_cast<typename Glm::length_type>(i)]);
					return array;
				}
			}
			if (value.get_type() != sol::type::table)
				return std::nullopt;
			// Raw access: a metamethod here would run script code in the middle of a component write.
			sol::table table = value.as<sol::table>();
			for (size_t i = 1; i <= N; i++)
			{
				std::optional<float> component = AsFloat(table.raw_get<sol::object>(i));
				if (!component)
					return std::nullopt;
				array.push_back(*component);
			}
			return array;
		}

		// Length without __len, which could run script code.
		size_t RawLength(const sol::table& table)
		{
			lua_State* L = table.lua_state();
			table.push();
			const size_t length = lua_rawlen(L, -1);
			lua_pop(L, 1);
			return length;
		}

		std::optional<json> HandleToJson(const sol::object& value)
		{
			if (std::optional<uint64_t> id = ParseId(value))
				return json(std::to_string(*id));
			return std::nullopt;
		}

		// Converts a Lua value to the JSON form of a reflected field, so ComponentRegistry::SetField can validate it
		// (types, ranges, enum names) exactly as it does for scene files and agents.
		std::optional<json> LuaToFieldJson(const sol::object& value, const FieldInfo& field)
		{
			switch (field.Type)
			{
				case FieldType::Bool:
					if (value.get_type() == sol::type::boolean)
						return json(value.as<bool>());
					return std::nullopt;
				case FieldType::Int:
					if (value.get_type() == sol::type::number && value.is<int64_t>())
						return json(value.as<int64_t>());
					return std::nullopt;
				case FieldType::Float:
					if (value.get_type() == sol::type::number)
						return json(value.as<double>());
					return std::nullopt;
				case FieldType::Vec2: return FloatsToJson<glm::vec2, 2>(value);
				case FieldType::Vec3: return FloatsToJson<glm::vec3, 3>(value);
				case FieldType::Vec4: return FloatsToJson<glm::vec4, 4>(value);
				case FieldType::Quat: return FloatsToJson<glm::quat, 4>(value);
				case FieldType::String:
					if (value.get_type() == sol::type::string)
						return json(value.as<std::string>());
					return std::nullopt;
				case FieldType::Asset:
					if (value.get_type() == sol::type::lua_nil)
						return json("0");
					return HandleToJson(value);
				case FieldType::AssetArray:
				{
					if (value.get_type() != sol::type::table)
						return std::nullopt;
					sol::table table = value.as<sol::table>();
					json array = json::array();
					const size_t count = RawLength(table);
					if (count > MaxArrayElements)
						return std::nullopt;
					for (size_t i = 1; i <= count; i++)
					{
						std::optional<json> handle = HandleToJson(table.raw_get<sol::object>(i));
						if (!handle)
							return std::nullopt;
						array.push_back(std::move(*handle));
					}
					return array;
				}
				case FieldType::Enum:
					if (value.get_type() == sol::type::string)
						return json(value.as<std::string>());
					return std::nullopt;
			}
			return std::nullopt;
		}

		sol::object FieldToLua(sol::state_view lua, const FieldInfo& field, void* component)
		{
			void* pointer = field.Access(component);
			switch (field.Type)
			{
				case FieldType::Bool: return sol::make_object(lua, *static_cast<bool*>(pointer));
				case FieldType::Int: return sol::make_object(lua, *static_cast<int*>(pointer));
				case FieldType::Float: return sol::make_object(lua, static_cast<double>(*static_cast<float*>(pointer)));
				case FieldType::Vec2: return sol::make_object(lua, *static_cast<glm::vec2*>(pointer));
				case FieldType::Vec3: return sol::make_object(lua, *static_cast<glm::vec3*>(pointer));
				case FieldType::Vec4: return sol::make_object(lua, *static_cast<glm::vec4*>(pointer));
				case FieldType::Quat: return sol::make_object(lua, *static_cast<glm::quat*>(pointer));
				case FieldType::String: return sol::make_object(lua, *static_cast<std::string*>(pointer));
				case FieldType::Asset: return sol::make_object(lua, IdToString(*static_cast<UUID*>(pointer)));
				case FieldType::AssetArray:
				{
					// Copy first: creating the table can run a garbage collection step, and nothing here may hold a
					// reference into component storage across a Lua allocation.
					const std::vector<UUID> handles = *static_cast<std::vector<UUID>*>(pointer);
					sol::table table = lua.create_table(static_cast<int>(handles.size()), 0);
					int index = 1;
					for (UUID handle : handles)
						table.raw_set(index++, IdToString(handle));
					return table;
				}
				case FieldType::Enum:
				{
					const int value = *static_cast<int*>(pointer);
					if (value >= 0 && static_cast<size_t>(value) < field.EnumNames.size())
						return sol::make_object(lua, field.EnumNames[static_cast<size_t>(value)]);
					return sol::make_object(lua, value);
				}
			}
			return sol::make_object(lua, sol::lua_nil);
		}

		std::string DescribeExpected(FieldType type)
		{
			switch (type)
			{
				case FieldType::Bool: return "a boolean";
				case FieldType::Int: return "an integer";
				case FieldType::Float: return "a number";
				case FieldType::Vec2: return "a vec2";
				case FieldType::Vec3: return "a vec3";
				case FieldType::Vec4: return "a vec4";
				case FieldType::Quat: return "a quat";
				case FieldType::String: return "a string";
				case FieldType::Asset: return "an asset handle (string)";
				case FieldType::AssetArray: return "an array of asset handles";
				case FieldType::Enum: return "an enum name (string)";
			}
			return "a value";
		}

		// Every script-visible userdata type is trivially destructible, so its __gc has nothing to do. sol2's default
		// __gc does not type-check its argument and is reachable from scripts through the instance's __index
		// (vec3().__gc(other)), so it is replaced with a checked no-op.
		static_assert(std::is_trivially_destructible_v<ScriptEntity> && std::is_trivially_destructible_v<ComponentRef>);
		static_assert(std::is_trivially_destructible_v<glm::vec2> && std::is_trivially_destructible_v<glm::vec3>);
		static_assert(std::is_trivially_destructible_v<glm::vec4> && std::is_trivially_destructible_v<glm::quat>);
		template<typename T>
		auto NoDestructor()
		{
			return [](const T&) {};
		}

		// glm's vector members live in anonymous unions, which member pointers cannot bind; expose them as properties.
		template<typename V, int Index>
		auto ComponentProperty()
		{
			return sol::property([](const V& v) { return v[Index]; }, [](V& v, float value) { v[Index] = value; });
		}

		int Traceback(lua_State* L)
		{
			const char* message = lua_tostring(L, 1);
			if (!message)
			{
				// Non-string error objects: describe them with tostring (via __tostring when present).
				message = luaL_tolstring(L, 1, nullptr);
			}
			luaL_traceback(L, L, message, 1);
			return 1;
		}

	}

	struct ScriptSystem::Impl
	{
		struct Instance
		{
			AssetHandle Script = 0;
			sol::table Self;
			bool Created = false;
			bool Failed = false;
			// Defines OnFixedUpdate (looked up once OnCreate has run).
			bool FixedUpdate = false;
		};

		Impl(AssetManager& assets, ScriptSystemSpecification specification)
			: Assets(assets)
			, Specification(specification)
			, Rng(specification.RandomSeed)
		{
		}

		AssetManager& Assets;
		ScriptSystemSpecification Specification;
		Scene* ActiveScene = nullptr;
		// The running scene's physics, if it simulates any (for the Physics API and collision callbacks).
		PhysicsSystem* Physics = nullptr;
		// A scene change requested by Scene.Load / Scene.Reload during this update (0 = reload).
		std::optional<AssetHandle> SceneRequest;
		// The running scene's audio, if it plays any (for the Audio API).
		AudioSystem* Audio = nullptr;

		LuaMemory Memory;
		std::optional<sol::state> Lua;
		sol::function ErrorHandler;
		// Dispatch(self, name, ...) calls self[name](self, ...) if it is a function, and Defines(self, name) says
		// whether it is one (both defined by the prelude).
		sol::function Dispatch;
		sol::function Defines;
		// The Time table the engine updates (held here, so a script reassigning the global cannot break updates).
		sol::table TimeTable;
		// The global API tables, in creation order. GetApiNames enumerates exactly these, so a table made with
		// CreateApiTable is listed by script.api and required in docs/Scripting.md and the feature test
		// (FeatureTestCoverageTests) with no list to keep in step. Constant tables (Key, Mouse) are not API tables.
		std::vector<std::string> ApiTables;

		// Per top-level call (a callback, a chunk, an Evaluate): instructions executed, whether the budget ran out,
		// and entities created. Nested calls (e.g. OnDestroy run by a Destroy inside a callback) share the budget.
		uint32_t CallDepth = 0;
		uint64_t InstructionsUsed = 0;
		bool BudgetExceeded = false;
		uint64_t EntitiesCreated = 0;

		// Compiled script classes by asset handle; a failed load is cached as nil so it is reported once.
		std::unordered_map<uint64_t, sol::object> Classes;
		std::unordered_map<uint64_t, Instance> Instances;
		// require() results by normalized path, and the paths currently being loaded (cycle detection).
		std::unordered_map<std::string, sol::object> Modules;
		std::unordered_set<std::string> ModulesLoading;

		std::vector<std::string> Errors;
		Random Rng;
		double Elapsed = 0.0;
		uint64_t Frame = 0;

		// --- Error reporting ---------------------------------------------------------------------------------

		// The message holds what the script raised, of any size (error() takes any string), so it is capped before it
		// is logged and kept.
		void ReportError(std::string_view error)
		{
			std::string message = CapMessage(error);
			PX_ERROR("Script error: {}", message);
			Errors.push_back(std::move(message));
			if (Errors.size() > Specification.MaxRecordedErrors)
				Errors.erase(Errors.begin(), Errors.begin() + static_cast<std::ptrdiff_t>(Errors.size() - Specification.MaxRecordedErrors));
		}

		static void InstructionHook(lua_State* L, lua_Debug*)
		{
			Impl* impl = *static_cast<Impl**>(lua_getextraspace(L));
			impl->InstructionsUsed += static_cast<uint64_t>(lua_gethookcount(L));
			if (impl->BudgetExceeded || impl->InstructionsUsed > impl->Specification.InstructionBudget)
			{
				// From here on every instruction raises the error, so pcall, xpcall or a coroutine inside the script
				// cannot swallow it and keep going: the error reaches the engine's protected call. The hook is reset
				// at the next top-level call.
				impl->BudgetExceeded = true;
				lua_sethook(L, &InstructionHook, LUA_MASKCOUNT, 1);
				// lua_pushfstring has no 64-bit integer conversion: format here. (Lua is built as C++, so the error
				// unwinds through this frame and the string is destroyed properly.)
				const std::string message = fmt::format("instruction budget of {} exceeded (an endless loop?)", impl->Specification.InstructionBudget);
				luaL_error(L, "%s", message.c_str());
			}
		}

		// Brackets a top-level entry into Lua: the outermost one starts a fresh instruction budget and entity quota.
		class CallScope
		{
		public:
			explicit CallScope(Impl& impl)
				: m_Impl(impl)
			{
				if (m_Impl.CallDepth++ == 0)
				{
					m_Impl.InstructionsUsed = 0;
					m_Impl.BudgetExceeded = false;
					m_Impl.EntitiesCreated = 0;
					lua_sethook(m_Impl.Lua->lua_state(), &InstructionHook, LUA_MASKCOUNT, HookInterval);
				}
			}
			~CallScope() { m_Impl.CallDepth--; }

			CallScope(const CallScope&) = delete;
			CallScope& operator=(const CallScope&) = delete;

		private:
			Impl& m_Impl;
		};

		template<typename Fn>
		static int ProtectedTrampoline(lua_State* L)
		{
			Fn* function = static_cast<Fn*>(lua_touserdata(L, 1));
			std::string message;
			try
			{
				(*function)();
				return 0;
			}
			catch (const std::exception& e)
			{
				// sol2 reports errors in engine-side operations (and ScriptError) as C++ exceptions; turn them into a
				// Lua error so lua_pcall reports them. Lua's own errors are not std::exceptions and pass through.
				message = e.what();
			}
			lua_pushstring(L, message.c_str());
			return lua_error(L);
		}

		// Runs engine-side code that touches Lua (creating tables, looking up callbacks, which may run metamethods)
		// inside a protected call, so a Lua error or an allocation failure is reported instead of escaping into the
		// engine. Returns false (error reported with context) on failure.
		template<typename Fn>
		bool Protect(const std::string& context, Fn&& function)
		{
			using Function = std::remove_reference_t<Fn>;
			CallScope scope(*this);
			lua_State* L = Lua->lua_state();
			if (!lua_checkstack(L, 3))
			{
				ReportError(fmt::format("{}: Lua stack exhausted", context));
				return false;
			}
			lua_pushcfunction(L, &Traceback);
			const int handler = lua_gettop(L);
			lua_pushcfunction(L, &ProtectedTrampoline<Function>);
			lua_pushlightuserdata(L, &function);
			const int status = lua_pcall(L, 1, 0, handler);
			if (status != LUA_OK)
			{
				const char* message = lua_tostring(L, -1);
				ReportError(fmt::format("{}: {}", context, message ? message : "error"));
				lua_pop(L, 1);
			}
			lua_pop(L, 1); // The handler.
			return status == LUA_OK;
		}

		// Calls a Lua function with the error handler and the shared instruction budget, reporting errors with context.
		template<typename... Args>
		std::optional<sol::protected_function_result> Call(const sol::function& function, const std::string& context, Args&&... args)
		{
			CallScope scope(*this);
			sol::protected_function protectedFunction(function, ErrorHandler);
			sol::protected_function_result result = protectedFunction(std::forward<Args>(args)...);
			if (!result.valid())
			{
				sol::error error = result;
				ReportError(fmt::format("{}: {}", context, error.what()));
				return std::nullopt;
			}
			return result;
		}

		// --- Scene access ----------------------------------------------------------------------------------

		Scene& GetScene() const
		{
			if (!ActiveScene)
				ScriptError("the scene is not running");
			return *ActiveScene;
		}

		Entity Resolve(const ScriptEntity& entity) const
		{
			std::optional<Entity> resolved = GetScene().TryGetEntityByUUID(entity.ID);
			if (!resolved)
				ScriptError(fmt::format("entity {} no longer exists", static_cast<uint64_t>(entity.ID)));
			return *resolved;
		}

		// Rules beyond the field's type, range and length, shared by every write path so scripts cannot bypass what
		// the editor and agents enforce (entity.rename, component.set): names cannot be empty. Their length is the
		// registry's MaxLength, checked by SetField like any other string field. (Fields of existing components are
		// writable, as in component.set; UserEditable only governs adding and removing components.)
		static void CheckComponentWrite(const ComponentInfo& info, const FieldInfo& field, const json& value)
		{
			if (info.Name == "Tag" && field.Name == "Name" && value.is_string() && value.get_ref<const std::string&>().empty())
				ScriptError("an entity name cannot be empty");
		}

		// The registry's limit and wording, for the setters that write the Tag component directly. (Names live outside
		// the Lua heap and its memory limit, so the bound matters here too.)
		static void CheckName(const std::string& text, const char* what)
		{
			if (text.size() > TagComponent::MaxLength)
				ScriptError(fmt::format("{} expects at most {} bytes", what, TagComponent::MaxLength));
		}

		// Entities live outside the Lua heap, so the memory limit does not bound them: cap creation per call instead.
		void CountCreatedEntities(size_t count)
		{
			EntitiesCreated += count;
			if (EntitiesCreated > Specification.MaxEntitiesCreatedPerCall)
				ScriptError(fmt::format("a single callback may create at most {} entities", Specification.MaxEntitiesCreatedPerCall));
		}

		const ComponentInfo& FindComponentInfo(const std::string& name) const
		{
			const ComponentInfo* info = ComponentRegistry::Find(name);
			if (!info)
				ScriptError(fmt::format("unknown component {}", QuoteText(name)));
			return *info;
		}

		// --- Script loading ----------------------------------------------------------------------------------

		// Compiles and runs a script asset's chunk and returns its first result (nil if it returned nothing). Returns
		// nullopt if compiling or running failed; the error has then been reported.
		std::optional<sol::object> RunChunk(const ScriptAsset& script)
		{
			sol::load_result loaded = Lua->load(script.Source, "@" + script.Name, sol::load_mode::text);
			if (!loaded.valid())
			{
				sol::error error = loaded;
				ReportError(fmt::format("{}: {}", script.Name, error.what()));
				return std::nullopt;
			}
			std::optional<sol::protected_function_result> result = Call(loaded.get<sol::function>(), script.Name);
			if (!result)
				return std::nullopt;
			if (result->return_count() == 0)
				return sol::make_object(*Lua, sol::lua_nil);
			return result->get<sol::object>(0);
		}

		sol::object GetClass(AssetHandle handle)
		{
			if (auto it = Classes.find(handle); it != Classes.end())
				return it->second;

			sol::object scriptClass = sol::make_object(*Lua, sol::lua_nil);
			Ref<ScriptAsset> script = Assets.GetAsset<ScriptAsset>(handle);
			if (!script)
			{
				ReportError(fmt::format("script asset {} not found", static_cast<uint64_t>(handle)));
			}
			else if (std::optional<sol::object> result = RunChunk(*script))
			{
				if (result->get_type() == sol::type::table)
					scriptClass = *result;
				else
					ReportError(fmt::format("{}: a script must return a table (its class)", script->Name));
			}
			Classes[handle] = scriptClass;
			return scriptClass;
		}

		// Runs inside a script (a Lua error raised here propagates to the requiring code, which reports it).
		sol::object Require(const std::string& name)
		{
			std::filesystem::path path = FromUtf8(name).lexically_normal();
			if (path.extension() != ".lua")
				path += ".lua";
			// Key by the registry path, so different spellings of the same file load it once.
			const std::optional<std::filesystem::path> registryPath = Assets.ToRegistryPath(path);
			const AssetHandle handle = registryPath ? Assets.GetHandleForPath(*registryPath) : AssetHandle(0);
			Ref<ScriptAsset> script = handle.IsValid() ? Assets.GetAsset<ScriptAsset>(handle) : nullptr;
			if (!script)
				ScriptError(fmt::format("require: no script {} in the project", QuoteText(ToUtf8(path))));
			const std::string key = ToUtf8(*registryPath);
			if (auto it = Modules.find(key); it != Modules.end())
				return it->second;
			if (ModulesLoading.contains(key))
				ScriptError(fmt::format("require: circular dependency on '{}'", key));

			sol::load_result loaded = Lua->load(script->Source, "@" + script->Name, sol::load_mode::text);
			if (!loaded.valid())
			{
				sol::error error = loaded;
				ScriptError(fmt::format("require: {}", error.what()));
			}
			ModulesLoading.insert(key);
			sol::protected_function module(loaded.get<sol::function>(), ErrorHandler);
			sol::protected_function_result result = module();
			ModulesLoading.erase(key);
			if (!result.valid())
			{
				sol::error error = result;
				ScriptError(fmt::format("require '{}': {}", key, error.what()));
			}
			sol::object value = result.return_count() > 0 ? result.get<sol::object>(0) : sol::make_object(*Lua, true);
			if (value.get_type() == sol::type::lua_nil)
				value = sol::make_object(*Lua, true);
			Modules[key] = value;
			return value;
		}

		// --- Instances -----------------------------------------------------------------------------------

		void DestroyInstance(UUID id, bool callOnDestroy)
		{
			auto it = Instances.find(id);
			if (it == Instances.end())
				return;
			Instance instance = std::move(it->second);
			Instances.erase(it);
			if (!callOnDestroy || !instance.Created || instance.Failed)
				return;
			RunCallback(instance, "OnDestroy");
		}

		std::string ContextName(const Instance& instance, const char* callback) const
		{
			std::string name = fmt::format("script {}", static_cast<uint64_t>(instance.Script));
			if (Ref<ScriptAsset> script = Assets.GetAsset<ScriptAsset>(instance.Script))
				name = script->Name;
			return fmt::format("{} {}", name, callback);
		}

		// Runs a callback if the script defines it. The lookup happens in Lua (Dispatch, see the prelude), inside the
		// protected call, so metamethods on the instance or class (e.g. inheritance, or a hostile __index) run as
		// script code with proper error reporting. Returns false if the lookup or the callback failed.
		template<typename... Args>
		bool RunCallback(Instance& instance, const char* callback, Args&&... args)
		{
			const std::string context = ContextName(instance, callback);
			bool succeeded = true;
			const bool protectedOk = Protect(context, [&] { succeeded = Call(Dispatch, context, instance.Self, callback, std::forward<Args>(args)...).has_value(); });
			return protectedOk && succeeded;
		}

		// Whether the instance (or its class) defines a callback. A failing lookup (a hostile __index) disables it.
		bool DefinesCallback(Instance& instance, const char* callback)
		{
			const std::string context = ContextName(instance, callback);
			bool defined = false;
			const bool protectedOk = Protect(context, [&] {
				std::optional<sol::protected_function_result> result = Call(Defines, context, instance.Self, callback);
				if (!result)
					instance.Failed = true;
				else
					defined = result->get<bool>(0);
			});
			if (!protectedOk)
				instance.Failed = true;
			return defined && !instance.Failed;
		}

		bool WantsFixedUpdate() const
		{
			return std::any_of(Instances.begin(), Instances.end(), [](const auto& entry) { return entry.second.Created && !entry.second.Failed && entry.second.FixedUpdate; });
		}

		// Delivers the contacts the physics step reported since the last update to both entities' scripts, in the
		// physics system's (deterministic) order: OnCollisionEnter/Exit or OnTriggerEnter/Exit(self, other).
		void DispatchContactEvents(Scene& scene)
		{
			if (!Physics)
				return;
			for (const ContactEvent& event : Physics->TakeContactEvents())
			{
				const char* callback = event.Type == ContactEventType::CollisionEnter  ? "OnCollisionEnter"
									   : event.Type == ContactEventType::CollisionExit ? "OnCollisionExit"
									   : event.Type == ContactEventType::TriggerEnter  ? "OnTriggerEnter"
																					   : "OnTriggerExit";
				for (const auto& [self, other] : {std::pair{event.A, event.B}, std::pair{event.B, event.A}})
				{
					auto it = Instances.find(self);
					if (it == Instances.end() || it->second.Failed || !it->second.Created || !scene.TryGetEntityByUUID(self))
						continue;
					// The other entity may already be gone (exits after destruction): it is still passed, invalid.
					if (!RunCallback(it->second, callback, ScriptEntity{other}))
					{
						if (auto again = Instances.find(self); again != Instances.end())
							again->second.Failed = true;
					}
				}
			}
		}

		// Creates instances for new script components, then runs OnCreate and OnUpdate, all in hierarchy order.
		void Update(Scene& scene, float dt)
		{
			// Collect first: scripts may create, destroy or reparent entities while running.
			std::vector<std::pair<UUID, AssetHandle>> scripted;
			std::vector<UUID> stale; // Instances whose component was removed or now points at another script.
			scene.ForEachEntity([&](Entity entity) {
				const auto* component = entity.TryGetComponent<ScriptComponent>();
				if (component && component->Script.IsValid())
					scripted.emplace_back(entity.GetUUID(), component->Script);
				if (auto it = Instances.find(entity.GetUUID()); it != Instances.end() && (!component || component->Script != it->second.Script))
					stale.push_back(entity.GetUUID());
			});
			// Instances outliving their entity cannot normally exist (destruction notifies this system); drop any
			// without callbacks.
			std::erase_if(Instances, [&](const auto& entry) { return !scene.TryGetEntityByUUID(UUID(entry.first)); });
			for (UUID id : stale)
				DestroyInstance(id, true);

			for (const auto& [id, script] : scripted)
			{
				if (Instances.contains(id) || !scene.TryGetEntityByUUID(id))
					continue;
				Instance instance;
				instance.Script = script;
				// A class that failed to load is reported once (when it is first loaded); its instances stay inert.
				const bool created = Protect(ContextName(instance, "instantiation"), [&] {
					sol::object scriptClass = GetClass(script);
					if (scriptClass.get_type() != sol::type::table)
						return;
					sol::table self = Lua->create_table();
					sol::table metatable = Lua->create_table();
					metatable.raw_set("__index", scriptClass);
					self[sol::metatable_key] = metatable;
					self.raw_set("entity", ScriptEntity{id});
					instance.Self = self;
				});
				instance.Failed = !created || !instance.Self.valid();
				Instances.emplace(id, std::move(instance));
			}

			for (const auto& [id, script] : scripted)
			{
				auto it = Instances.find(id);
				if (it == Instances.end() || it->second.Failed || it->second.Created || !scene.TryGetEntityByUUID(id))
					continue;
				it->second.Created = true;
				Instance& instance = it->second;
				if (!RunCallback(instance, "OnCreate"))
				{
					// The callback may have destroyed (deferred) entities, but cannot have erased instances.
					if (auto again = Instances.find(id); again != Instances.end())
						again->second.Failed = true;
					continue;
				}
				// Looked up once, so the fixed-update machinery costs nothing for scripts without the callback.
				if (auto again = Instances.find(id); again != Instances.end() && !again->second.Failed)
					again->second.FixedUpdate = DefinesCallback(again->second, "OnFixedUpdate");
			}

			DispatchContactEvents(scene);

			for (const auto& [id, script] : scripted)
			{
				auto it = Instances.find(id);
				if (it == Instances.end() || it->second.Failed || !it->second.Created || !scene.TryGetEntityByUUID(id))
					continue;
				if (!RunCallback(it->second, "OnUpdate", static_cast<double>(dt)))
				{
					if (auto again = Instances.find(id); again != Instances.end())
						again->second.Failed = true;
				}
			}
		}

		// OnFixedUpdate(self, dt) for every created, working instance that defines it, in hierarchy order. Skipped:
		// entities destroyed during this update (they would otherwise run once more per remaining step), and instances
		// whose Script component was removed or replaced (they are destroyed at the next update).
		void FixedUpdate(Scene& scene, float dt)
		{
			std::vector<UUID> order; // Collected first: scripts may create, destroy or reparent entities while running.
			scene.ForEachEntity([&](Entity entity) {
				if (auto it = Instances.find(entity.GetUUID()); it != Instances.end() && it->second.Created && !it->second.Failed && it->second.FixedUpdate)
					order.push_back(entity.GetUUID());
			});
			for (UUID id : order)
			{
				auto it = Instances.find(id);
				if (it == Instances.end() || it->second.Failed)
					continue;
				std::optional<Entity> entity = scene.TryGetEntityByUUID(id);
				const auto* component = entity ? entity->TryGetComponent<ScriptComponent>() : nullptr;
				if (!component || component->Script != it->second.Script || scene.IsDestructionPending(*entity))
					continue;
				if (!RunCallback(it->second, "OnFixedUpdate", static_cast<double>(dt)))
				{
					if (auto again = Instances.find(id); again != Instances.end())
						again->second.Failed = true;
				}
			}
		}

		// --- Bindings ----------------------------------------------------------------------------------------

		// Runs once per state, before scripts, with the raw functions still available; captures them in locals.
		static constexpr const char* s_SandboxPrelude = R"lua(
			local type, error, rawget, getmetatable_, setmetatable_ = type, error, rawget, getmetatable, setmetatable
			-- Metatables of strings and engine types are internal: changing them would affect every script (and the
			-- engine's own types). Tables keep the standard behaviour, including __metatable.
			getmetatable = function(value)
				if type(value) ~= "table" then return nil end
				return getmetatable_(value)
			end
			-- Finalizers would run script code at arbitrary points (any allocation, or engine shutdown).
			setmetatable = function(t, metatable)
				if type(metatable) == "table" and rawget(metatable, "__gc") ~= nil then
					error("__gc finalizers are not available to scripts", 2)
				end
				return setmetatable_(t, metatable)
			end
			-- Pattern matching runs in C, outside the instruction budget, and can backtrack exponentially. Refuse
			-- calls whose worst case ((#subject + 1) ^ (quantifiers + 1)) is too large; plain finds are unaffected.
			local find, match, gmatch, gsub, byte = string.find, string.match, string.gmatch, string.gsub, string.byte
			local function guard(name, subject, pattern)
				if type(subject) ~= "string" or type(pattern) ~= "string" then return end
				local quantifiers, i, n = 0, 1, #pattern
				while i <= n do
					local c = byte(pattern, i)
					if c == 37 then
						i = i + 2 -- %x escapes the next character
					else
						if c == 42 or c == 43 or c == 45 or c == 63 then quantifiers = quantifiers + 1 end
						i = i + 1
					end
				end
				if quantifiers > 0 and (#subject + 1) ^ (quantifiers + 1) > PATTERN_COST_LIMIT then
					error(name .. ": pattern too costly for a " .. #subject .. "-byte subject (use fewer quantifiers or a shorter string)", 3)
				end
			end
			string.find = function(s, p, init, plain)
				if not plain then guard("string.find", s, p) end
				return find(s, p, init, plain)
			end
			string.match = function(s, p, init) guard("string.match", s, p) return match(s, p, init) end
			string.gmatch = function(s, p, init) guard("string.gmatch", s, p) return gmatch(s, p, init) end
			string.gsub = function(s, p, replacement, n) guard("string.gsub", s, p) return gsub(s, p, replacement, n) end
			-- Returned to the engine (not globals): look up and call an instance's callback, and look one up.
			return function(self, name, ...)
				local callback = self[name]
				if type(callback) == "function" then return callback(self, ...) end
			end, function(self, name)
				return type(self[name]) == "function"
			end
		)lua";

		void CreateState()
		{
			Memory = {};
			Memory.Limit = Specification.MemoryLimitBytes;
			ApiTables.clear();
			Lua.emplace(sol::default_at_panic, &LuaAllocate, &Memory);
			sol::state& lua = *Lua;
			*static_cast<Impl**>(lua_getextraspace(lua.lua_state())) = this;
			lua_sethook(lua.lua_state(), &InstructionHook, LUA_MASKCOUNT, HookInterval);

			lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table, sol::lib::utf8, sol::lib::coroutine);
			lua.set_function("__paralax_traceback", &Traceback);
			ErrorHandler = lua["__paralax_traceback"];
			lua["__paralax_traceback"] = sol::lua_nil;

			const std::string prelude = fmt::format("local PATTERN_COST_LIMIT = {}\n{}", PatternCostLimit, s_SandboxPrelude);
			sol::load_result loaded = lua.load(prelude, "=sandbox", sol::load_mode::text);
			PX_CORE_VERIFY(loaded.valid(), "The script sandbox prelude must compile");
			std::optional<sol::protected_function_result> preludeResult = Call(loaded.get<sol::function>(), "sandbox");
			PX_CORE_VERIFY(preludeResult && preludeResult->return_count() == 2, "The script sandbox prelude must run");
			Dispatch = preludeResult->get<sol::function>(0);
			Defines = preludeResult->get<sol::function>(1);

			// Sandbox: no file access, no loading of arbitrary code (or bytecode), no raw environment tampering.
			for (const char* name : {"dofile", "loadfile", "load", "loadstring", "collectgarbage", "rawequal", "rawget", "rawset", "rawlen"})
				lua[name] = sol::lua_nil;
			lua["string"]["dump"] = sol::lua_nil;

			BindMath(lua);
			BindLogging(lua);
			BindEntity(lua);
			BindScene(lua);
			BindInput(lua);
			BindPhysics(lua);
			BindAudio(lua);
			BindGame(lua);

			TimeTable = CreateApiTable(lua, "Time");
			TimeTable.raw_set("delta", 0.0, "elapsed", 0.0, "frame", 0);
			lua.set_function("require", [this](const std::string& name) { return Require(name); });
		}

		// Creates a global table of API functions (or fields) and records it for GetApiNames.
		sol::table CreateApiTable(sol::state& lua, const char* name)
		{
			ApiTables.emplace_back(name);
			return lua.create_named_table(name);
		}

		void BindMath(sol::state& lua)
		{
			sol::table math = lua["math"];
			math.set_function("random", [this](sol::variadic_args args) -> sol::object {
				sol::state_view view(*Lua);
				if (args.size() == 0)
					return sol::make_object(view, Rng.NextDouble());
				auto integer = [](const sol::object& value) -> int64_t {
					if (value.get_type() != sol::type::number || !value.is<int64_t>())
						ScriptError("math.random: arguments must be integers");
					return value.as<int64_t>();
				};
				int64_t low = 1;
				int64_t high = integer(args[0]);
				if (args.size() >= 2)
				{
					low = high;
					high = integer(args[1]);
				}
				if (low > high)
					ScriptError("math.random: interval is empty");
				return sol::make_object(view, Rng.NextInRange(low, high));
			});
			math.set_function("randomseed", [this](sol::optional<int64_t> seed) { Rng.Seed(static_cast<uint64_t>(seed.value_or(0))); });

			lua.new_usertype<glm::vec2>("vec2", sol::call_constructor, sol::factories([] { return glm::vec2(0.0f); }, [](float s) { return glm::vec2(s); }, [](float x, float y) { return glm::vec2(x, y); }), "x", ComponentProperty<glm::vec2, 0>(), "y", ComponentProperty<glm::vec2, 1>(), sol::meta_function::addition, [](const glm::vec2& a, const glm::vec2& b) { return a + b; }, sol::meta_function::subtraction, [](const glm::vec2& a, const glm::vec2& b) { return a - b; }, sol::meta_function::multiplication, sol::overload([](const glm::vec2& a, const glm::vec2& b) { return a * b; }, [](const glm::vec2& a, float s) { return a * s; }, [](float s, const glm::vec2& a) { return s * a; }), sol::meta_function::division, sol::overload([](const glm::vec2& a, const glm::vec2& b) { return a / b; }, [](const glm::vec2& a, float s) { return a / s; }), sol::meta_function::unary_minus, [](const glm::vec2& a) { return -a; }, sol::meta_function::equal_to, [](const glm::vec2& a, const glm::vec2& b) { return a == b; }, sol::meta_function::to_string, [](const glm::vec2& v) { return fmt::format("vec2({}, {})", v.x, v.y); }, "Length", [](const glm::vec2& v) { return glm::length(v); }, "Normalized", [](const glm::vec2& v) { return glm::length(v) > 0.0f ? glm::normalize(v) : v; }, "Dot", [](const glm::vec2& a, const glm::vec2& b) { return glm::dot(a, b); }, "Lerp", [](const glm::vec2& a, const glm::vec2& b, float t) { return glm::mix(a, b, t); }, sol::meta_function::garbage_collect, NoDestructor<glm::vec2>());

			lua.new_usertype<glm::vec3>("vec3", sol::call_constructor, sol::factories([] { return glm::vec3(0.0f); }, [](float s) { return glm::vec3(s); }, [](float x, float y, float z) { return glm::vec3(x, y, z); }), "x", ComponentProperty<glm::vec3, 0>(), "y", ComponentProperty<glm::vec3, 1>(), "z", ComponentProperty<glm::vec3, 2>(), sol::meta_function::addition, [](const glm::vec3& a, const glm::vec3& b) { return a + b; }, sol::meta_function::subtraction, [](const glm::vec3& a, const glm::vec3& b) { return a - b; }, sol::meta_function::multiplication, sol::overload([](const glm::vec3& a, const glm::vec3& b) { return a * b; }, [](const glm::vec3& a, float s) { return a * s; }, [](float s, const glm::vec3& a) { return s * a; }), sol::meta_function::division, sol::overload([](const glm::vec3& a, const glm::vec3& b) { return a / b; }, [](const glm::vec3& a, float s) { return a / s; }), sol::meta_function::unary_minus, [](const glm::vec3& a) { return -a; }, sol::meta_function::equal_to, [](const glm::vec3& a, const glm::vec3& b) { return a == b; }, sol::meta_function::to_string, [](const glm::vec3& v) { return fmt::format("vec3({}, {}, {})", v.x, v.y, v.z); }, "Length", [](const glm::vec3& v) { return glm::length(v); }, "Normalized", [](const glm::vec3& v) { return glm::length(v) > 0.0f ? glm::normalize(v) : v; }, "Dot", [](const glm::vec3& a, const glm::vec3& b) { return glm::dot(a, b); }, "Cross", [](const glm::vec3& a, const glm::vec3& b) { return glm::cross(a, b); }, "Lerp", [](const glm::vec3& a, const glm::vec3& b, float t) { return glm::mix(a, b, t); }, "Distance", [](const glm::vec3& a, const glm::vec3& b) { return glm::distance(a, b); }, sol::meta_function::garbage_collect, NoDestructor<glm::vec3>());

			lua.new_usertype<glm::vec4>("vec4", sol::call_constructor, sol::factories([] { return glm::vec4(0.0f); }, [](float s) { return glm::vec4(s); }, [](float x, float y, float z, float w) { return glm::vec4(x, y, z, w); }), "x", ComponentProperty<glm::vec4, 0>(), "y", ComponentProperty<glm::vec4, 1>(), "z", ComponentProperty<glm::vec4, 2>(), "w", ComponentProperty<glm::vec4, 3>(), sol::meta_function::addition, [](const glm::vec4& a, const glm::vec4& b) { return a + b; }, sol::meta_function::subtraction, [](const glm::vec4& a, const glm::vec4& b) { return a - b; }, sol::meta_function::multiplication, sol::overload([](const glm::vec4& a, const glm::vec4& b) { return a * b; }, [](const glm::vec4& a, float s) { return a * s; }, [](float s, const glm::vec4& a) { return s * a; }), sol::meta_function::division, sol::overload([](const glm::vec4& a, const glm::vec4& b) { return a / b; }, [](const glm::vec4& a, float s) { return a / s; }), sol::meta_function::unary_minus, [](const glm::vec4& a) { return -a; }, sol::meta_function::equal_to, [](const glm::vec4& a, const glm::vec4& b) { return a == b; }, sol::meta_function::to_string, [](const glm::vec4& v) { return fmt::format("vec4({}, {}, {}, {})", v.x, v.y, v.z, v.w); }, "Length", [](const glm::vec4& v) { return glm::length(v); }, "Normalized", [](const glm::vec4& v) { return glm::length(v) > 0.0f ? glm::normalize(v) : v; }, "Dot", [](const glm::vec4& a, const glm::vec4& b) { return glm::dot(a, b); }, "Lerp", [](const glm::vec4& a, const glm::vec4& b, float t) { return glm::mix(a, b, t); }, sol::meta_function::garbage_collect, NoDestructor<glm::vec4>());

			lua.new_usertype<glm::quat>("quat", sol::call_constructor, sol::factories([] { return glm::quat(1.0f, 0.0f, 0.0f, 0.0f); }, [](float w, float x, float y, float z) { return glm::quat(w, x, y, z); }), "x", ComponentProperty<glm::quat, 0>(), "y", ComponentProperty<glm::quat, 1>(), "z", ComponentProperty<glm::quat, 2>(), "w", ComponentProperty<glm::quat, 3>(), sol::meta_function::multiplication, sol::overload([](const glm::quat& a, const glm::quat& b) { return a * b; }, [](const glm::quat& q, const glm::vec3& v) { return q * v; }), sol::meta_function::equal_to, [](const glm::quat& a, const glm::quat& b) { return a == b; }, sol::meta_function::to_string, [](const glm::quat& q) { return fmt::format("quat(w={}, x={}, y={}, z={})", q.w, q.x, q.y, q.z); }, "FromEuler", [](const glm::vec3& radians) { return Math::EulerToQuat(radians); }, "AngleAxis", [](float radians, const glm::vec3& axis) {
					if (!(glm::length(axis) > 0.0f))
						ScriptError("quat.AngleAxis: the axis must not be zero");
					return glm::angleAxis(radians, glm::normalize(axis)); }, "ToEuler", [](const glm::quat& q) { return Math::QuatToEuler(q); }, "Normalized", [](const glm::quat& q) { return glm::normalize(q); }, "Inverse", [](const glm::quat& q) { return glm::inverse(q); }, "Slerp", [](const glm::quat& a, const glm::quat& b, float t) { return glm::slerp(a, b, t); }, sol::meta_function::garbage_collect, NoDestructor<glm::quat>());
		}

		void BindLogging(sol::state& lua)
		{
			// Arguments are converted with Lua's tostring, so tables with __tostring print as their authors intend. A
			// line keeps at most MaxMessageBytes of its text, and only that much (and a byte more, for CapMessage to
			// find a character boundary) is copied out of Lua: a script can build strings up to its memory limit.
			auto join = [](sol::variadic_args args, sol::this_state state) {
				sol::state_view view(state);
				sol::function toString = view["tostring"];
				std::string message;
				size_t fullBytes = 0;
				auto append = [&](std::string_view text) {
					fullBytes += text.size();
					if (message.size() <= MaxMessageBytes)
						message.append(text.substr(0, MaxMessageBytes + 1 - message.size()));
				};
				for (size_t i = 0; i < args.size(); i++)
				{
					if (i > 0)
						append("\t");
					// The converted string stays on the Lua stack, and so valid, while `converted` lives.
					const sol::function_result converted = toString(args[i]);
					append(converted.get<std::string_view>());
				}
				return CapMessage(message, MaxMessageBytes, fullBytes);
			};
			sol::table log = CreateApiTable(lua, "Log");
			log.set_function("Trace", [join](sol::variadic_args args, sol::this_state s) { PX_TRACE("{}", join(args, s)); });
			log.set_function("Info", [join](sol::variadic_args args, sol::this_state s) { PX_INFO("{}", join(args, s)); });
			log.set_function("Warn", [join](sol::variadic_args args, sol::this_state s) { PX_WARN("{}", join(args, s)); });
			log.set_function("Error", [join](sol::variadic_args args, sol::this_state s) { PX_ERROR("{}", join(args, s)); });
			lua.set_function("print", [join](sol::variadic_args args, sol::this_state s) { PX_INFO("{}", join(args, s)); });
		}

		void BindEntity(sol::state& lua)
		{
			auto index = [this](const ComponentRef& component, const std::string& key, sol::this_state state) -> sol::object {
				if (key == "type")
					return sol::make_object(state, component.Info->Name);
				const FieldInfo* field = component.Info->FindField(key);
				if (!field)
					ScriptError(fmt::format("component '{}' has no field {}", component.Info->Name, QuoteText(key)));
				Entity entity = Resolve(ScriptEntity{component.ID});
				void* data = component.Info->Get(GetScene().GetRegistry(), entity.GetHandle());
				if (!data)
					ScriptError(fmt::format("the '{}' component was removed", component.Info->Name));
				return FieldToLua(state, *field, data);
			};
			auto newIndex = [this](const ComponentRef& component, const std::string& key, const sol::object& value) {
				const FieldInfo* field = component.Info->FindField(key);
				if (!field)
					ScriptError(fmt::format("component '{}' has no field {}", component.Info->Name, QuoteText(key)));
				// Convert first, then resolve the component: nothing may hold a pointer into component storage while Lua
				// code could run (the conversion reads the value; allocations may trigger a collection step).
				std::optional<json> converted = LuaToFieldJson(value, *field);
				if (!converted)
					ScriptError(fmt::format("{}.{} expects {}", component.Info->Name, key, DescribeExpected(field->Type)));
				CheckComponentWrite(*component.Info, *field, *converted);
				Entity entity = Resolve(ScriptEntity{component.ID});
				void* data = component.Info->Get(GetScene().GetRegistry(), entity.GetHandle());
				if (!data)
					ScriptError(fmt::format("the '{}' component was removed", component.Info->Name));
				std::string error;
				if (!ComponentRegistry::SetField(*field, data, *converted, error))
					ScriptError(fmt::format("{}.{}: {}", component.Info->Name, key, error));
			};
			lua.new_usertype<ComponentRef>("Component", sol::no_constructor, sol::meta_function::index, index, sol::meta_function::new_index, newIndex, sol::meta_function::to_string, [](const ComponentRef& component) { return fmt::format("Component({})", component.Info->Name); }, sol::meta_function::garbage_collect, NoDestructor<ComponentRef>());

			auto getComponent = [this](const ScriptEntity& self, const std::string& name) -> sol::optional<ComponentRef> {
				Entity entity = Resolve(self);
				const ComponentInfo& info = FindComponentInfo(name);
				if (!info.Has(GetScene().GetRegistry(), entity.GetHandle()))
					return sol::nullopt;
				return ComponentRef{self.ID, &info};
			};
			auto addComponent = [this](const ScriptEntity& self, const std::string& name, sol::optional<sol::table> fields) {
				Entity entity = Resolve(self);
				const ComponentInfo& info = FindComponentInfo(name);
				if (!info.UserEditable && !info.Has(GetScene().GetRegistry(), entity.GetHandle()))
					ScriptError(fmt::format("the '{}' component cannot be added by scripts", name));
				// Validate and convert every field before touching the entity, so a bad field changes nothing.
				std::vector<std::pair<const FieldInfo*, json>> values;
				if (fields)
				{
					for (const auto& [key, value] : *fields)
					{
						if (key.get_type() != sol::type::string)
							ScriptError("AddComponent: field names must be strings");
						const std::string fieldName = key.as<std::string>();
						const FieldInfo* field = info.FindField(fieldName);
						if (!field)
							ScriptError(fmt::format("component '{}' has no field {}", name, QuoteText(fieldName)));
						std::optional<json> converted = LuaToFieldJson(value, *field);
						if (!converted)
							ScriptError(fmt::format("{}.{} expects {}", name, fieldName, DescribeExpected(field->Type)));
						CheckComponentWrite(info, *field, *converted);
						values.emplace_back(field, std::move(*converted));
					}
				}
				entity = Resolve(self); // Re-resolve after running conversions.
				entt::registry& registry = GetScene().GetRegistry();
				const bool existed = info.Has(registry, entity.GetHandle());
				// An exact copy of the component as it was, to put back if a field is refused. Restoring field by field
				// would validate the old values, and leave behind any the ranges refuse (values set in code may be).
				std::optional<entt::registry> saved;
				entt::entity savedEntity = entt::null;
				if (existed && !values.empty())
				{
					saved.emplace();
					savedEntity = saved->create();
					info.Copy(registry, entity.GetHandle(), *saved, savedEntity);
				}
				void* data = info.Add(registry, entity.GetHandle());
				for (const auto& [field, value] : values)
				{
					std::string error;
					if (ComponentRegistry::SetField(*field, data, value, error))
						continue;
					// Undo: remove the new component, or put the existing one back as it was.
					if (!existed && info.Remove)
						info.Remove(registry, entity.GetHandle());
					else if (existed)
						info.Copy(*saved, savedEntity, registry, entity.GetHandle());
					ScriptError(fmt::format("{}.{}: {}", name, field->Name, error));
				}
				return ComponentRef{self.ID, &info};
			};
			auto removeComponent = [this](const ScriptEntity& self, const std::string& name) {
				Entity entity = Resolve(self);
				const ComponentInfo& info = FindComponentInfo(name);
				if (!info.Remove)
					ScriptError(fmt::format("the '{}' component cannot be removed", name));
				if (!info.Has(GetScene().GetRegistry(), entity.GetHandle()))
					return false;
				info.Remove(GetScene().GetRegistry(), entity.GetHandle());
				return true;
			};
			auto getParent = [this](const ScriptEntity& self) -> sol::optional<ScriptEntity> {
				std::optional<Entity> parent = GetScene().GetParent(Resolve(self));
				if (!parent)
					return sol::nullopt;
				return ScriptEntity{parent->GetUUID()};
			};
			auto setParent = [this](const ScriptEntity& self, sol::optional<ScriptEntity> parent, sol::optional<bool> keepWorldTransform) {
				std::optional<Entity> resolvedParent;
				if (parent)
					resolvedParent = Resolve(*parent);
				if (!GetScene().SetParent(Resolve(self), resolvedParent, keepWorldTransform.value_or(true)))
					ScriptError("SetParent: an entity cannot be parented to itself or a descendant");
			};
			auto getChildren = [this](const ScriptEntity& self, sol::this_state state) {
				// Copy: creating Lua values allocates, and nothing may reference scene storage across that.
				const std::vector<UUID> ids = Resolve(self).GetComponent<RelationshipComponent>().Children;
				sol::table children = sol::state_view(state).create_table(static_cast<int>(ids.size()), 0);
				int position = 1;
				for (UUID child : ids)
					children.raw_set(position++, ScriptEntity{child});
				return children;
			};
			auto getScript = [this](const ScriptEntity& self, sol::this_state state) -> sol::object {
				Resolve(self);
				auto it = Instances.find(self.ID);
				if (it == Instances.end() || it->second.Failed)
					return sol::make_object(state, sol::lua_nil);
				return it->second.Self;
			};
			auto name = sol::property([this](const ScriptEntity& self) { return Resolve(self).GetName(); },
									  [this](const ScriptEntity& self, const std::string& value) {
										  if (value.empty())
											  ScriptError("an entity name cannot be empty");
										  CheckName(value, "entity.name");
										  Resolve(self).GetComponent<TagComponent>().Name = value;
									  });
			auto tag = sol::property([this](const ScriptEntity& self) { return Resolve(self).GetComponent<TagComponent>().Tag; },
									 [this](const ScriptEntity& self, const std::string& value) {
										 CheckName(value, "entity.tag");
										 Resolve(self).GetComponent<TagComponent>().Tag = value;
									 });
			auto toString = [this](const ScriptEntity& self) {
				std::optional<Entity> entity = ActiveScene ? ActiveScene->TryGetEntityByUUID(self.ID) : std::nullopt;
				return entity ? fmt::format("Entity({}, \"{}\")", static_cast<uint64_t>(self.ID), entity->GetName())
							  : fmt::format("Entity({}, destroyed)", static_cast<uint64_t>(self.ID));
			};

			lua.new_usertype<ScriptEntity>("Entity", sol::no_constructor, "id", sol::readonly_property([](const ScriptEntity& self) { return IdToString(self.ID); }), "name", std::move(name), "tag", std::move(tag), "IsValid", [this](const ScriptEntity& self) { return ActiveScene && ActiveScene->TryGetEntityByUUID(self.ID).has_value(); }, "HasComponent", [this](const ScriptEntity& self, const std::string& component) { return FindComponentInfo(component).Has(GetScene().GetRegistry(), Resolve(self).GetHandle()); }, "GetComponent", getComponent, "AddComponent", addComponent, "RemoveComponent", removeComponent, "Destroy", [this](const ScriptEntity& self) { GetScene().DestroyEntity(Resolve(self)); }, "GetParent", getParent, "SetParent", setParent, "GetChildren", getChildren, "GetWorldPosition", [this](const ScriptEntity& self) { return glm::vec3(GetScene().GetWorldTransform(Resolve(self))[3]); }, "GetScript", getScript, sol::meta_function::equal_to, [](const ScriptEntity& a, const ScriptEntity& b) { return a.ID == b.ID; }, sol::meta_function::to_string, toString, sol::meta_function::garbage_collect, NoDestructor<ScriptEntity>());
		}

		void BindScene(sol::state& lua)
		{
			sol::table scene = CreateApiTable(lua, "Scene");
			scene.set_function("CreateEntity", [this](sol::optional<std::string> name, sol::optional<ScriptEntity> parent) {
				Scene& active = GetScene();
				std::optional<Entity> resolvedParent;
				if (parent)
					resolvedParent = Resolve(*parent);
				if (name)
					CheckName(*name, "Scene.CreateEntity's name");
				CountCreatedEntities(1);
				Entity entity = active.CreateEntity(name && !name->empty() ? *name : "Entity");
				if (resolvedParent)
					active.SetParent(entity, resolvedParent, false);
				return ScriptEntity{entity.GetUUID()};
			});
			scene.set_function("GetEntity", [this](const sol::object& id) -> sol::optional<ScriptEntity> {
				std::optional<uint64_t> parsed = ParseId(id);
				if (!parsed || !GetScene().TryGetEntityByUUID(UUID(*parsed)))
					return sol::nullopt;
				return ScriptEntity{UUID(*parsed)};
			});
			scene.set_function("FindEntityByName", [this](const std::string& name) -> sol::optional<ScriptEntity> {
				std::optional<Entity> entity = GetScene().FindEntityByName(name);
				if (!entity)
					return sol::nullopt;
				return ScriptEntity{entity->GetUUID()};
			});
			scene.set_function("FindEntitiesByTag", [this](const std::string& tag, sol::this_state state) {
				std::vector<UUID> ids;
				for (Entity entity : GetScene().FindEntitiesByTag(tag))
					ids.push_back(entity.GetUUID());
				sol::table result = sol::state_view(state).create_table(static_cast<int>(ids.size()), 0);
				int position = 1;
				for (UUID id : ids)
					result.raw_set(position++, ScriptEntity{id});
				return result;
			});
			scene.set_function("Destroy", [this](const ScriptEntity& entity) { GetScene().DestroyEntity(Resolve(entity)); });
			scene.set_function("GetEntityCount", [this] { return GetScene().GetEntityCount(); });
			scene.set_function("Load", [this](const sol::object& target) {
				AssetHandle handle = 0;
				if (std::optional<uint64_t> id = ParseId(target))
					handle = UUID(*id);
				else if (target.get_type() == sol::type::string)
					handle = Assets.GetHandleForPath(FromUtf8(target.as<std::string>()));
				if (!handle.IsValid() || Assets.GetAssetType(handle) != AssetType::Scene)
					ScriptError("Scene.Load: no scene with that handle or path");
				GetScene(); // Only while running.
				SceneRequest = handle;
			});
			scene.set_function("Reload", [this] {
				GetScene();
				SceneRequest = AssetHandle(0);
			});
			scene.set_function("Instantiate", [this](const sol::object& prefab, sol::optional<sol::table> options) {
				AssetHandle handle = 0;
				if (std::optional<uint64_t> id = ParseId(prefab))
					handle = UUID(*id);
				else if (prefab.get_type() == sol::type::string)
					handle = Assets.GetHandleForPath(FromUtf8(prefab.as<std::string>()));
				Ref<PrefabAsset> asset = handle.IsValid() ? Assets.GetAsset<PrefabAsset>(handle) : nullptr;
				if (!asset)
					ScriptError("Scene.Instantiate: no prefab with that handle or path");

				std::optional<Entity> parent;
				std::optional<glm::vec3> position;
				if (options)
				{
					// Raw reads: option tables are plain data, and metamethods must not run mid-call.
					sol::object parentValue = options->raw_get<sol::object>("parent");
					if (parentValue.valid() && parentValue.get_type() != sol::type::lua_nil)
					{
						if (!parentValue.is<ScriptEntity>())
							ScriptError("Scene.Instantiate: options.parent must be an Entity");
						parent = Resolve(parentValue.as<ScriptEntity>());
					}
					sol::object positionValue = options->raw_get<sol::object>("position");
					if (positionValue.valid() && positionValue.get_type() != sol::type::lua_nil)
					{
						if (!positionValue.is<glm::vec3>())
							ScriptError("Scene.Instantiate: options.position must be a vec3");
						position = positionValue.as<glm::vec3>();
						// The position bypasses reflection (PrefabAsset::Instantiate assigns it), so reflection's rule
						// applies here: a non-finite translation would silently leave the instance without a body and
						// with a NaN world transform. The components are floats already, so finite also means within
						// FLT_MAX, the rest of that rule.
						if (!std::isfinite(position->x) || !std::isfinite(position->y) || !std::isfinite(position->z))
							ScriptError("Scene.Instantiate: options.position must be finite");
					}
				}
				CountCreatedEntities(asset->GetEntityCount());
				return ScriptEntity{asset->Instantiate(GetScene(), parent, position).GetUUID()};
			});
		}

		PhysicsSystem& GetPhysics() const
		{
			if (!Physics)
				ScriptError("physics is not running in this scene");
			return *Physics;
		}

		void BindPhysics(sol::state& lua)
		{
			sol::table physics = CreateApiTable(lua, "Physics");
			// Body controls return false when the entity has no (dynamic) body; that is not an error.
			physics.set_function("AddForce", [this](const ScriptEntity& entity, const glm::vec3& force) { return GetPhysics().AddForce(Resolve(entity).GetUUID(), force); });
			physics.set_function("AddImpulse", [this](const ScriptEntity& entity, const glm::vec3& impulse) { return GetPhysics().AddImpulse(Resolve(entity).GetUUID(), impulse); });
			physics.set_function("SetLinearVelocity",
								 [this](const ScriptEntity& entity, const glm::vec3& velocity) { return GetPhysics().SetLinearVelocity(Resolve(entity).GetUUID(), velocity); });
			physics.set_function("GetLinearVelocity", [this](const ScriptEntity& entity) -> sol::optional<glm::vec3> {
				std::optional<glm::vec3> velocity = GetPhysics().GetLinearVelocity(Resolve(entity).GetUUID());
				return velocity ? sol::optional<glm::vec3>(*velocity) : sol::nullopt;
			});
			physics.set_function("SetAngularVelocity",
								 [this](const ScriptEntity& entity, const glm::vec3& velocity) { return GetPhysics().SetAngularVelocity(Resolve(entity).GetUUID(), velocity); });
			physics.set_function("GetAngularVelocity", [this](const ScriptEntity& entity) -> sol::optional<glm::vec3> {
				std::optional<glm::vec3> velocity = GetPhysics().GetAngularVelocity(Resolve(entity).GetUUID());
				return velocity ? sol::optional<glm::vec3>(*velocity) : sol::nullopt;
			});
			physics.set_function("Raycast", [this](const glm::vec3& origin, const glm::vec3& direction, float maxDistance, sol::this_state state) -> sol::object {
				std::optional<RaycastHit> hit = GetPhysics().Raycast(origin, direction, maxDistance);
				if (!hit)
					return sol::make_object(state, sol::lua_nil);
				sol::table result = sol::state_view(state).create_table();
				result.raw_set("entity", ScriptEntity{hit->Entity}, "point", hit->Point, "normal", hit->Normal, "distance", static_cast<double>(hit->Distance));
				return result;
			});
			physics.set_function("GetGravity", [this] { return GetPhysics().GetGravity(); });
			physics.set_function("SetGravity", [this](const glm::vec3& gravity) {
				if (!std::isfinite(gravity.x) || !std::isfinite(gravity.y) || !std::isfinite(gravity.z))
					ScriptError("Physics.SetGravity: gravity must be finite");
				GetPhysics().SetGravity(gravity);
			});
		}

		AudioSystem& GetAudio() const
		{
			if (!Audio)
				ScriptError("audio is not running in this scene");
			return *Audio;
		}

		void BindAudio(sol::state& lua)
		{
			sol::table audio = CreateApiTable(lua, "Audio");
			// Source controls return false when the entity has no AudioSource with a playable clip; not an error.
			audio.set_function("Play", [this](const ScriptEntity& entity) { return GetAudio().Play(Resolve(entity).GetUUID()); });
			audio.set_function("Pause", [this](const ScriptEntity& entity) { return GetAudio().Pause(Resolve(entity).GetUUID()); });
			audio.set_function("Stop", [this](const ScriptEntity& entity) { return GetAudio().Stop(Resolve(entity).GetUUID()); });
			audio.set_function("IsPlaying", [this](const ScriptEntity& entity) { return GetAudio().IsPlaying(Resolve(entity).GetUUID()); });
			// Plain objects (not optionals) for the optional arguments: each binds to its own position even when nil.
			audio.set_function("PlayOneShot", [this](const sol::object& clip, const sol::object& position, const sol::object& volume) {
				AssetHandle handle = 0;
				if (std::optional<uint64_t> id = ParseId(clip))
					handle = UUID(*id);
				else if (clip.get_type() == sol::type::string)
					handle = Assets.GetHandleForPath(FromUtf8(clip.as<std::string>()));
				if (!handle.IsValid() || Assets.GetAssetType(handle) != AssetType::Audio)
					ScriptError("Audio.PlayOneShot: no audio clip with that handle or path");
				std::optional<glm::vec3> where;
				if (position.valid() && position.get_type() != sol::type::lua_nil)
				{
					if (!position.is<glm::vec3>())
						ScriptError("Audio.PlayOneShot: position must be a vec3 or nil");
					where = position.as<glm::vec3>();
				}
				double gain = 1.0;
				if (volume.valid() && volume.get_type() != sol::type::lua_nil)
				{
					if (volume.get_type() != sol::type::number)
						ScriptError("Audio.PlayOneShot: volume must be a number");
					gain = volume.as<double>();
				}
				if (!std::isfinite(gain) || gain < 0.0)
					ScriptError("Audio.PlayOneShot: volume must be a non-negative number");
				return GetAudio().PlayOneShot(handle, where, static_cast<float>(gain));
			});
			audio.set_function("GetMasterVolume", [this] { return GetAudio().GetMasterVolume(); });
			audio.set_function("SetMasterVolume", [this](double volume) {
				if (!std::isfinite(volume) || volume < 0.0)
					ScriptError("Audio.SetMasterVolume: volume must be a non-negative number");
				GetAudio().SetMasterVolume(static_cast<float>(volume));
			});
		}

		GameState& GetState() const
		{
			if (!Specification.State)
				ScriptError("there is no game session to keep values in");
			return *Specification.State;
		}

		void BindGame(sol::state& lua)
		{
			sol::table game = CreateApiTable(lua, "Game");
			// Values that survive Scene.Load and Scene.Reload: booleans, numbers and strings. Setting nil removes.
			game.set_function("Set", [this](const std::string& key, const sol::object& value, sol::this_state caller) {
				GameState& state = GetState();
				if (!value.valid() || value.get_type() == sol::type::lua_nil)
				{
					state.Remove(key);
					return;
				}
				GameState::Value stored;
				switch (value.get_type())
				{
					case sol::type::boolean: stored = value.as<bool>(); break;
					case sol::type::number:
					{
						// Lua 5.4 tells integers from floats; keep the distinction.
						value.push(caller);
						const bool integer = lua_isinteger(caller, -1) != 0;
						lua_pop(caller, 1);
						if (integer)
							stored = value.as<int64_t>();
						else
							stored = value.as<double>();
						break;
					}
					case sol::type::string: stored = value.as<std::string>(); break;
					default: ScriptError("Game.Set: values must be booleans, numbers, strings or nil");
				}
				if (!state.Set(key, std::move(stored)))
					ScriptError(fmt::format("Game.Set: the key must be 1-{} bytes, numbers finite, strings at most {} bytes, and the store at most {} keys and {} bytes",
											GameState::MaxKeyBytes, GameState::MaxStringBytes, GameState::MaxKeys, GameState::MaxTotalBytes));
			});
			game.set_function("Get", [this](const std::string& key, const sol::object& fallback, sol::this_state state) -> sol::object {
				std::optional<GameState::Value> value = GetState().Get(key);
				if (!value)
					return fallback.valid() ? fallback : sol::make_object(state, sol::lua_nil);
				return std::visit([&](const auto& stored) { return sol::make_object(state, stored); }, *value);
			});
		}

		void BindInput(sol::state& lua)
		{
			// Constant tables, not API tables: their members are the names from GetKeyNames/GetMouseButtonNames, which
			// docs/Scripting.md describes as ranges, so script.api and the coverage test do not list them one by one.
			sol::table keys = lua.create_named_table("Key");
			for (const InputName<KeyCode>& name : GetKeyNames())
				keys[name.Name] = name.Code;
			sol::table mouse = lua.create_named_table("Mouse");
			for (const InputName<MouseCode>& name : GetMouseButtonNames())
				mouse[name.Name] = name.Code;

			auto key = [](int64_t code) {
				if (code < 0 || code >= Key::Count)
					ScriptError(fmt::format("unknown key code {}", code));
				return static_cast<KeyCode>(code);
			};
			auto button = [](int64_t code) {
				if (code < 0 || code >= Mouse::Count)
					ScriptError(fmt::format("unknown mouse button {}", code));
				return static_cast<MouseCode>(code);
			};
			sol::table input = CreateApiTable(lua, "Input");
			input.set_function("IsKeyDown", [key](int64_t code) { return Input::IsKeyDown(key(code)); });
			input.set_function("WasKeyPressed", [key](int64_t code) { return Input::WasKeyPressed(key(code)); });
			input.set_function("WasKeyReleased", [key](int64_t code) { return Input::WasKeyReleased(key(code)); });
			input.set_function("IsMouseButtonDown", [button](int64_t code) { return Input::IsMouseButtonDown(button(code)); });
			input.set_function("WasMouseButtonPressed", [button](int64_t code) { return Input::WasMouseButtonPressed(button(code)); });
			input.set_function("WasMouseButtonReleased", [button](int64_t code) { return Input::WasMouseButtonReleased(button(code)); });
			input.set_function("GetMousePosition", [] { return Input::GetMousePosition(); });
			input.set_function("GetMouseScrollDelta", [] { return Input::GetMouseScrollDelta(); });
		}

		void Shutdown()
		{
			// Detach from the scene first: nothing that runs while the state closes may reach it.
			ActiveScene = nullptr;
			Physics = nullptr;
			Audio = nullptr;
			SceneRequest.reset();
			Instances.clear();
			Classes.clear();
			Modules.clear();
			ModulesLoading.clear();
			TimeTable = sol::table();
			Dispatch = sol::function();
			Defines = sol::function();
			ErrorHandler = sol::function();
			Lua.reset();
		}
	};

	ScriptSystem::ScriptSystem(AssetManager& assets, ScriptSystemSpecification specification)
		: m_Impl(CreateScope<Impl>(assets, specification))
	{
		// The engine's own setup (libraries, bindings) needs some of the budget; below these it cannot even start.
		PX_CORE_VERIFY(specification.MemoryLimitBytes >= 4ull * 1024 * 1024, "ScriptSystem needs a memory limit of at least 4 MiB");
		PX_CORE_VERIFY(specification.InstructionBudget >= 100'000, "ScriptSystem needs an instruction budget of at least 100000");
	}

	ScriptSystem::~ScriptSystem()
	{
		// Script references must be released before the Lua state they belong to.
		m_Impl->Shutdown();
	}

	void ScriptSystem::OnRuntimeStart(Scene& scene)
	{
		m_Impl->Shutdown();
		m_Impl->ActiveScene = &scene;
		m_Impl->Physics = scene.GetSystem<PhysicsSystem>();
		m_Impl->Audio = scene.GetSystem<AudioSystem>();
		m_Impl->Rng.Seed(m_Impl->Specification.RandomSeed);
		m_Impl->Elapsed = 0.0;
		m_Impl->Frame = 0;
		m_Impl->CreateState();
	}

	void ScriptSystem::OnUpdate(Scene& scene, Timestep timestep)
	{
		if (!m_Impl->Lua || m_Impl->ActiveScene != &scene)
			return;
		m_Impl->Elapsed += static_cast<double>(timestep.GetSeconds());
		m_Impl->Frame++;
		m_Impl->Protect("Time", [&] {
			m_Impl->TimeTable.raw_set("delta", static_cast<double>(timestep.GetSeconds()), "elapsed", m_Impl->Elapsed, "frame", m_Impl->Frame);
		});
		m_Impl->Update(scene, timestep.GetSeconds());
	}

	bool ScriptSystem::WantsFixedUpdate() const
	{
		return m_Impl->Lua && m_Impl->WantsFixedUpdate();
	}

	void ScriptSystem::OnFixedUpdate(Scene& scene, Timestep timestep)
	{
		if (m_Impl->Lua && m_Impl->ActiveScene == &scene)
			m_Impl->FixedUpdate(scene, timestep.GetSeconds());
	}

	void ScriptSystem::OnRuntimeStop(Scene& scene)
	{
		if (!m_Impl->Lua || m_Impl->ActiveScene != &scene)
			return;
		// OnDestroy for every live instance, in hierarchy order, while their entities still exist.
		std::vector<UUID> order;
		scene.ForEachEntity([&](Entity entity) {
			if (m_Impl->Instances.contains(entity.GetUUID()))
				order.push_back(entity.GetUUID());
		});
		for (UUID id : order)
			m_Impl->DestroyInstance(id, true);
		m_Impl->Shutdown();
	}

	void ScriptSystem::OnEntityDestroying(Scene& scene, const Entity& entity)
	{
		if (m_Impl->Lua && m_Impl->ActiveScene == &scene)
			m_Impl->DestroyInstance(entity.GetUUID(), true);
	}

	const std::vector<std::string>& ScriptSystem::GetErrors() const
	{
		return m_Impl->Errors;
	}

	void ScriptSystem::ClearErrors()
	{
		m_Impl->Errors.clear();
	}

	size_t ScriptSystem::GetInstanceCount() const
	{
		return m_Impl->Instances.size();
	}

	std::optional<std::string> ScriptSystem::Evaluate(std::string_view code, std::string& outError)
	{
		if (!m_Impl->Lua)
		{
			outError = "The scene is not running";
			return std::nullopt;
		}
		std::string output;
		std::string failure;
		const size_t errorsBefore = m_Impl->Errors.size();
		const bool ran = m_Impl->Protect("eval", [&] {
			sol::state& lua = *m_Impl->Lua;
			// Expressions are accepted as well as statements: try "return <code>" first, as a console does.
			sol::load_result loaded = lua.load("return " + std::string(code), "=eval", sol::load_mode::text);
			if (!loaded.valid())
				loaded = lua.load(code, "=eval", sol::load_mode::text);
			if (!loaded.valid())
			{
				sol::error error = loaded;
				failure = error.what();
				return;
			}
			sol::protected_function function(loaded.get<sol::function>(), m_Impl->ErrorHandler);
			sol::protected_function_result result = function();
			if (!result.valid())
			{
				sol::error error = result;
				failure = error.what();
				return;
			}
			sol::protected_function toString(lua["tostring"], m_Impl->ErrorHandler);
			for (int i = 0; i < result.return_count(); i++)
			{
				if (i > 0)
					output += '\t';
				sol::protected_function_result text = toString(result.get<sol::object>(i));
				output += text.valid() && text.get_type() == sol::type::string ? text.get<std::string>() : std::string("<error>");
			}
		});
		if (!ran)
		{
			// Engine-side failure (e.g. out of memory): Protect reported it; return it to the caller instead.
			failure = m_Impl->Errors.size() > errorsBefore ? m_Impl->Errors.back() : "evaluation failed";
			m_Impl->Errors.resize(errorsBefore);
		}
		if (!failure.empty())
		{
			// What the code raised, of any size; the results, which the caller asked for, are returned whole.
			outError = CapMessage(failure);
			return std::nullopt;
		}
		return output;
	}

	std::optional<AssetHandle> ScriptSystem::TakeSceneRequest()
	{
		return std::exchange(m_Impl->SceneRequest, std::nullopt);
	}

	std::vector<std::string> ScriptSystem::GetApiNames()
	{
		// Enumerated from the bindings themselves, so the list can never drift from the implementation.
		const bool temporary = !m_Impl->Lua;
		if (temporary)
			m_Impl->CreateState();
		std::vector<std::string> names;
		m_Impl->Protect("api", [&] {
			sol::state& lua = *m_Impl->Lua;
			lua_State* L = lua.lua_state();
			auto collect = [&](const std::string& prefix, char separator, bool skipMetamethods) {
				// Expects a table on the top of the stack; leaves it there.
				lua_pushnil(L);
				while (lua_next(L, -2) != 0)
				{
					if (lua_type(L, -2) == LUA_TSTRING)
					{
						const std::string key = lua_tostring(L, -2);
						const bool internal = key.starts_with("__") || key == "class_cast" || key == "class_check" || key == "new";
						if (!(skipMetamethods && internal))
							names.push_back(prefix + separator + key);
					}
					lua_pop(L, 1);
				}
			};
			for (const std::string& table : m_Impl->ApiTables)
			{
				lua_getglobal(L, table.c_str());
				if (lua_istable(L, -1))
					collect(table, '.', false);
				lua_pop(L, 1);
			}
			names.insert(names.end(), {"math.random", "math.randomseed", "require", "print"});
			auto userdata = [&](const char* typeName, sol::object sample) {
				sample.push();
				if (lua_getmetatable(L, -1))
				{
					collect(typeName, ':', true);
					lua_pop(L, 1);
				}
				lua_pop(L, 1);
			};
			userdata("Entity", sol::make_object(lua, ScriptEntity{UUID(0)}));
			userdata("Component", sol::make_object(lua, ComponentRef{UUID(0), &ComponentRegistry::GetAll().front()}));
			userdata("vec2", sol::make_object(lua, glm::vec2(0.0f)));
			userdata("vec3", sol::make_object(lua, glm::vec3(0.0f)));
			userdata("vec4", sol::make_object(lua, glm::vec4(0.0f)));
			userdata("quat", sol::make_object(lua, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
		});
		if (temporary)
			m_Impl->Shutdown();
		std::sort(names.begin(), names.end());
		return names;
	}

}
