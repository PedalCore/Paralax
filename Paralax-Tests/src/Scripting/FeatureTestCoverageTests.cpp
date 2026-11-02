#include <doctest/doctest.h>

#include "TestUtilities.h"

#include <Paralax/Asset/AssetManager.h>
#include <Paralax/Scene/ComponentRegistry.h>
#include <Paralax/Scene/Entity.h>
#include <Paralax/Scene/Scene.h>
#include <Paralax/Scene/SceneSerializer.h>
#include <Paralax/Scripting/ScriptSystem.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>

// AGENTS.md: every component and every scripting API function must appear in the feature test scene. These tests
// enforce it, so the rule cannot be forgotten: they fail when something new is added without extending the scene.

using namespace Paralax;

namespace {

	std::filesystem::path RepositoryRoot()
	{
		// This file lives at <root>/Paralax-Tests/src/Scripting/.
		return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
	}

	std::filesystem::path FeatureTestDirectory()
	{
		return RepositoryRoot() / "Paralax-Tests" / "FeatureTest";
	}

	// The level of a Lua long bracket ("[[" is 0, "[==[" is 2) opening at `at`, or nullopt if none opens there.
	std::optional<size_t> LongBracketLevel(const std::string& source, size_t at)
	{
		if (at >= source.size() || source[at] != '[')
			return std::nullopt;
		size_t level = 0;
		while (at + 1 + level < source.size() && source[at + 1 + level] == '=')
			level++;
		if (at + 1 + level < source.size() && source[at + 1 + level] == '[')
			return level;
		return std::nullopt;
	}

	// Lua source with its comments removed and its string literals emptied (the quotes stay, as token boundaries), so
	// that only code can satisfy the coverage check: a name in a comment, an assert message or a log line is not a use.
	std::string LuaCodeOnly(const std::string& source)
	{
		std::string code;
		code.reserve(source.size());
		auto afterLongBracket = [&](size_t open, size_t level) {
			const std::string close = "]" + std::string(level, '=') + "]";
			const size_t end = source.find(close, open + level + 2);
			return end == std::string::npos ? source.size() : end + close.size();
		};
		size_t i = 0;
		while (i < source.size())
		{
			const char c = source[i];
			if (c == '-' && i + 1 < source.size() && source[i + 1] == '-')
			{
				if (std::optional<size_t> level = LongBracketLevel(source, i + 2))
				{
					i = afterLongBracket(i + 2, *level);
				}
				else
				{
					const size_t end = source.find('\n', i);
					i = end == std::string::npos ? source.size() : end; // The newline stays.
				}
				code += ' ';
			}
			else if (c == '"' || c == '\'')
			{
				// As Lua's lexer reads a short string: an unescaped line break ends it (unfinished), a backslash before
				// a line break (\n, \r, \r\n or \n\r) continues it on the next line, and \z skips all whitespace after
				// it, line breaks included.
				auto isLineBreak = [](char value) { return value == '\n' || value == '\r'; };
				size_t j = i + 1;
				while (j < source.size() && source[j] != c && !isLineBreak(source[j]))
				{
					if (source[j++] != '\\' || j >= source.size())
						continue;
					if (source[j] == 'z')
					{
						j++;
						while (j < source.size() && std::isspace(static_cast<unsigned char>(source[j])))
							j++;
					}
					else if (isLineBreak(source[j]))
					{
						const char first = source[j++];
						if (j < source.size() && isLineBreak(source[j]) && source[j] != first)
							j++;
					}
					else
					{
						j++;
					}
				}
				code += c;
				code += c;
				i = j < source.size() && source[j] == c ? j + 1 : j; // An unfinished string leaves its line break.
			}
			else if (std::optional<size_t> level = LongBracketLevel(source, i))
			{
				code += "\"\"";
				i = afterLongBracket(i, *level);
			}
			else
			{
				code += c;
				i++;
			}
		}
		return code;
	}

	// The code of a Markdown document, block by block: each Lua or untagged fenced block, and each `inline span`, passed
	// through LuaCodeOnly so its comments and strings do not count either. Prose does not document a name (a reader
	// looks for the call spelled out), and fences in other languages (shell, C++, JSON) are not script code.
	std::vector<std::string> MarkdownCodeBlocks(const std::string& markdown)
	{
		std::vector<std::string> blocks;
		std::string fence; // The open fence's marker run ("```", "~~~~"), or empty outside a fence.
		bool keepFence = false;
		std::string fenced;
		std::istringstream lines(markdown);
		std::string line;
		while (std::getline(lines, line))
		{
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			// Fences may be indented (inside list items), so leading whitespace is skipped.
			const size_t indent = std::min(line.find_first_not_of(" \t"), line.size());
			const std::string_view text = std::string_view(line).substr(indent);
			size_t run = 0;
			if (!text.empty() && (text[0] == '`' || text[0] == '~'))
			{
				while (run < text.size() && text[run] == text[0])
					run++;
			}
			const std::string_view rest = text.substr(run);
			if (!fence.empty())
			{
				// A closing fence uses the opening character, at least as many times, and nothing after it.
				if (run >= fence.size() && text[0] == fence[0] && rest.find_first_not_of(" \t") == std::string_view::npos)
				{
					if (keepFence)
						blocks.push_back(LuaCodeOnly(fenced));
					fence.clear();
				}
				else
				{
					fenced += line + '\n';
				}
				continue;
			}
			// An opening fence; a backtick fence's info string cannot contain a backtick (it would be an inline span).
			if (run >= 3 && !(text[0] == '`' && rest.find('`') != std::string_view::npos))
			{
				const size_t start = std::min(rest.find_first_not_of(" \t"), rest.size());
				std::string language(rest.substr(start, rest.find_first_of(" \t{", start) - start));
				std::transform(language.begin(), language.end(), language.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
				fence = std::string(text.substr(0, run));
				keepFence = language.empty() || language == "lua";
				fenced.clear();
				continue;
			}
			// Inline spans: a run of backticks closes at the next run of the same length.
			size_t i = 0;
			while (i < line.size())
			{
				if (line[i] != '`')
				{
					i++;
					continue;
				}
				const size_t open = i;
				while (i < line.size() && line[i] == '`')
					i++;
				const size_t length = i - open;
				size_t close = i;
				while ((close = line.find('`', close)) != std::string::npos)
				{
					size_t end = close;
					while (end < line.size() && line[end] == '`')
						end++;
					if (end - close == length)
						break;
					close = end;
				}
				if (close == std::string::npos)
					continue; // Unmatched backticks are literal text.
				blocks.push_back(LuaCodeOnly(line.substr(i, close - i)));
				i = close + length;
			}
		}
		if (!fence.empty() && keepFence)
			blocks.push_back(LuaCodeOnly(fenced)); // An unclosed fence runs to the end of the document.
		return blocks;
	}

	// All of a document's code (see MarkdownCodeBlocks), one block per line.
	std::string MarkdownCodeOnly(const std::string& markdown)
	{
		std::string code;
		for (const std::string& block : MarkdownCodeBlocks(markdown))
			code += block + '\n';
		return code;
	}

	// The engine methods a block of documented code calls with ":Method(": not the block's own functions (function
	// Player:Helper() ... self:Helper()), and not C++ scope resolution ("Reflection::MakeField(").
	std::set<std::string> MethodCalls(const std::string& block)
	{
		std::set<std::string> defined;
		const std::regex definition("function\\s+[A-Za-z_][A-Za-z0-9_.]*:([A-Za-z_][A-Za-z0-9_]*)");
		for (std::sregex_iterator match(block.begin(), block.end(), definition), end; match != end; ++match)
			defined.insert((*match)[1]);
		std::set<std::string> calls;
		const std::regex call("(::?)([A-Za-z_][A-Za-z0-9_]*)\\s*\\(");
		for (std::sregex_iterator match(block.begin(), block.end(), call), end; match != end; ++match)
		{
			const std::string member = (*match)[2];
			if ((*match)[1].length() == 1 && !defined.contains(member))
				calls.insert(member);
		}
		return calls;
	}

	// A qualified name starts after a character that is neither part of an identifier nor a member access, or after the
	// concatenation operator ('a'..Scene.Load(x)).
	const std::string s_NameStart = "(^|[^A-Za-z0-9_.:]|\\.\\.)";

	// Whether code uses an API name in its qualified spelling. A function must be called: "Table.Function(" (or passed
	// to pcall, the Lua way to test an error), ":Method(" or "Type.Static(" for userdata. A field must be read as
	// "Table.field" or ".property". The whole name has to match: "probe:Destroy()" does not cover Scene.Destroy, and
	// "Scene.Destroy(e)" does not cover Entity:Destroy.
	bool UsesName(const std::string& code, const std::string& name, bool function)
	{
		const std::string notIdentifier = "(?![A-Za-z0-9_])";
		const size_t separator = name.find(':');
		std::string pattern;
		if (separator != std::string::npos)
		{
			// Limitation: a static check cannot tell a value's type, so ":Normalized(" on any receiver covers vec2, vec3,
			// vec4 and quat alike, and metamethods (operators) are not checked here at all. ScriptSystemTests pins the
			// vector types' shared methods and operators instead; counting calls per type at run time is the real fix.
			const std::string type = name.substr(0, separator);
			const std::string member = name.substr(separator + 1);
			pattern = function ? "(:|" + s_NameStart + type + "\\.)" + member + "\\s*\\(" : "\\." + member + notIdentifier;
		}
		else
		{
			const std::string qualified = std::regex_replace(name, std::regex("\\."), "\\.");
			pattern = function ? s_NameStart + qualified + "\\s*\\(|pcall\\(\\s*" + qualified + "\\s*[,)]" : s_NameStart + qualified + notIdentifier;
		}
		return std::regex_search(code, std::regex(pattern));
	}

	// The live scripting API, enumerated from the bindings by the script system itself, with a running scene to ask
	// whether each name is a function (to be called) or a field (to be read).
	class ScriptApi
	{
	public:
		ScriptApi()
		{
			m_Scripts = static_cast<ScriptSystem*>(m_Scene.AddSystem(CreateScope<ScriptSystem>(m_Assets)));
			m_Scene.OnRuntimeStart();
			Names = m_Scripts->GetApiNames();
		}

		~ScriptApi() { m_Scene.OnRuntimeStop(); }

		bool IsFunction(const std::string& name)
		{
			// A userdata member is looked up on an instance ("vec3():Length" is a function, "vec3().x" a number).
			static const std::map<std::string, std::string> s_Samples = {{"vec2", "vec2()"}, {"vec3", "vec3()"}, {"vec4", "vec4()"}, {"quat", "quat()"}, {"Entity", "Scene.CreateEntity()"}, {"Component", "Scene.CreateEntity():GetComponent('Transform')"}};
			std::string expression = name;
			if (const size_t separator = name.find(':'); separator != std::string::npos)
			{
				const auto sample = s_Samples.find(name.substr(0, separator));
				REQUIRE_MESSAGE(sample != s_Samples.end(), "Add a sample value for the userdata type of " << name << " to this test");
				expression = sample->second + "." + name.substr(separator + 1);
			}
			return TypeOf(expression) == "function";
		}

		// Whether `name` is a string method ("s:find(...)" in the docs is Lua's, not the engine's).
		bool IsStringMethod(const std::string& name) { return TypeOf("string['" + name + "']") == "function"; }

		std::vector<std::string> Names;

	private:
		std::string TypeOf(const std::string& expression)
		{
			std::string error;
			const std::optional<std::string> type = m_Scripts->Evaluate("return type(" + expression + ")", error);
			REQUIRE_MESSAGE(type, error);
			return *type;
		}

		AssetManager m_Assets;
		Scene m_Scene{"ApiCoverage"};
		ScriptSystem* m_Scripts = nullptr;
	};

}

TEST_CASE("FeatureTest: the scene contains every registered component")
{
	Ref<Scene> scene = SceneSerializer::LoadFromFile(FeatureTestDirectory() / "Assets" / "Scenes" / "FeatureTest.pxscene");
	REQUIRE(scene);
	std::set<std::string> present;
	scene->ForEachEntity([&](Entity entity) {
		for (const ComponentInfo& info : ComponentRegistry::GetAll())
		{
			if (info.Has(scene->GetRegistry(), entity.GetHandle()))
				present.insert(info.Name);
		}
	});
	for (const ComponentInfo& info : ComponentRegistry::GetAll())
	{
		CAPTURE(info.Name);
		CHECK_MESSAGE(present.contains(info.Name),
					  "Add the component to the feature test scene (tools/featuretest/build_feature_test.py) and to the list in FeatureTest.lua");
	}
}

TEST_CASE("FeatureTest: the coverage check counts only qualified uses in code")
{
	// Lua: comments and strings are not code.
	CHECK(UsesName(LuaCodeOnly("Physics.SetGravity(vec3(0, -9.81, 0))"), "Physics.SetGravity", true));
	CHECK(UsesName(LuaCodeOnly("assert(not pcall(Scene.Load, 'Scenes/Missing.pxscene'))"), "Scene.Load", true));
	CHECK_FALSE(UsesName(LuaCodeOnly("-- Physics.SetGravity(vec3(0, -9.81, 0))"), "Physics.SetGravity", true));
	CHECK_FALSE(UsesName(LuaCodeOnly("--[[ Physics.SetGravity(g)\n]] local x = 1"), "Physics.SetGravity", true));
	CHECK_FALSE(UsesName(LuaCodeOnly("Log.Info(\"Physics.SetGravity(g) was called\")"), "Physics.SetGravity", true));
	CHECK_FALSE(UsesName(LuaCodeOnly("local s = [[Physics.SetGravity(g)]]"), "Physics.SetGravity", true));
	CHECK_FALSE(UsesName(LuaCodeOnly("local s = 'it\\'s' .. \"Scene.Load(x)\""), "Scene.Load", true));
	CHECK(UsesName(LuaCodeOnly("local s = 'it\\'s' .. Scene.Load(x)"), "Scene.Load", true));
	CHECK(UsesName(LuaCodeOnly("local s = 'a'..Scene.Load(x)"), "Scene.Load", true)); // Right after concatenation.
	CHECK(UsesName(LuaCodeOnly("local s = 'a'..Time.frame"), "Time.frame", false));
	// Escapes that continue a string over a line break, with Windows line endings too: \z skips all whitespace, and a
	// backslash before \r\n is one escape.
	CHECK_FALSE(UsesName(LuaCodeOnly("local s = 'a\\z\r\n   Physics.SetGravity(g)'"), "Physics.SetGravity", true));
	CHECK_FALSE(UsesName(LuaCodeOnly("local s = 'a\\\r\nPhysics.SetGravity(g)'"), "Physics.SetGravity", true));
	CHECK_FALSE(UsesName(LuaCodeOnly("local s = 'a\\\n\rPhysics.SetGravity(g)'"), "Physics.SetGravity", true));
	CHECK(UsesName(LuaCodeOnly("local s = 'a\\z\r\n  b' Physics.SetGravity(g)"), "Physics.SetGravity", true));
	CHECK(UsesName(LuaCodeOnly("local s = 'unfinished\r\nPhysics.SetGravity(g)"), "Physics.SetGravity", true)); // A raw line break ends a string.
	// A function has to be called, and the whole qualified name has to match.
	CHECK_FALSE(UsesName(LuaCodeOnly("local f = Scene.Load"), "Scene.Load", true));
	CHECK_FALSE(UsesName(LuaCodeOnly("MyScene.Load(1)"), "Scene.Load", true));
	CHECK_FALSE(UsesName(LuaCodeOnly("Scene.Loader(1)"), "Scene.Load", true));
	CHECK_FALSE(UsesName(LuaCodeOnly("game.Scene.Load(1)"), "Scene.Load", true));
	CHECK_FALSE(UsesName(LuaCodeOnly("probe:Destroy()"), "Scene.Destroy", true));
	CHECK_FALSE(UsesName(LuaCodeOnly("Scene.Destroy(probe)"), "Entity:Destroy", true));
	CHECK(UsesName(LuaCodeOnly("probe:Destroy()"), "Entity:Destroy", true));
	CHECK(UsesName(LuaCodeOnly("quat.FromEuler(vec3(0, 1, 0))"), "quat:FromEuler", true));
	CHECK(UsesName(LuaCodeOnly("q:Normalized()"), "quat:Normalized", true));
	CHECK_FALSE(UsesName(LuaCodeOnly("Normalized(q)"), "quat:Normalized", true));
	// Fields and properties are read in their qualified spelling.
	CHECK(UsesName(LuaCodeOnly("if Time.frame == 1 then end"), "Time.frame", false));
	CHECK_FALSE(UsesName(LuaCodeOnly("-- not one spawned this frame"), "Time.frame", false));
	CHECK_FALSE(UsesName(LuaCodeOnly("local frame = Time.frames"), "Time.frame", false));
	CHECK(UsesName(LuaCodeOnly("local height = v.x"), "vec3:x", false));
	CHECK_FALSE(UsesName(LuaCodeOnly("local x = 1; return x"), "vec3:x", false));
	CHECK_FALSE(UsesName(LuaCodeOnly("local v = { xy = 1 }; return v.xy"), "vec3:x", false));
	// Markdown: only code spans and fenced blocks count, never prose.
	CHECK_FALSE(UsesName(MarkdownCodeOnly("`Set` works on dynamic bodies only; `Get` works on any body."), "Game.Set", true));
	CHECK_FALSE(UsesName(MarkdownCodeOnly("Game.Set(key, value) stores a value."), "Game.Set", true));
	CHECK(UsesName(MarkdownCodeOnly("| `Game.Set(key, value)` | Stores a value. |"), "Game.Set", true));
	CHECK(UsesName(MarkdownCodeOnly("```lua\nGame.Set(\"Score\", 1)\n```\n"), "Game.Set", true));
	CHECK(UsesName(MarkdownCodeOnly("`Time.delta`, `Time.elapsed` and `Time.frame`"), "Time.elapsed", false));
	CHECK_FALSE(UsesName(MarkdownCodeOnly("Time.elapsed is in seconds"), "Time.elapsed", false));
	CHECK(UsesName(MarkdownCodeOnly("Use ``Game.Set(key, value)`` to store."), "Game.Set", true));         // Double-backtick span.
	CHECK_FALSE(UsesName(MarkdownCodeOnly("`Log.Info(\"Game.Set(k, v) is called\")`"), "Game.Set", true)); // Strings are emptied.
	CHECK_FALSE(UsesName(MarkdownCodeOnly("`x = 1 -- Game.Set(k, v)`"), "Game.Set", true));                // So are comments.
	// Only Lua and untagged fences are script code; ~~~ fences and fences indented in list items count too.
	CHECK(UsesName(MarkdownCodeOnly("```\nGame.Set('a', 1)\n```\n"), "Game.Set", true));
	CHECK(UsesName(MarkdownCodeOnly("```Lua\nGame.Set('a', 1)\n```\n"), "Game.Set", true));
	CHECK_FALSE(UsesName(MarkdownCodeOnly("```bash\nGame.Set('a', 1)\n```\n"), "Game.Set", true));
	CHECK_FALSE(UsesName(MarkdownCodeOnly("```cpp\nGame.Set('a', 1);\n```\n"), "Game.Set", true));
	CHECK(UsesName(MarkdownCodeOnly("- Example:\n  ```lua\n  Game.Set('a', 1)\n  ```\n"), "Game.Set", true));
	CHECK(UsesName(MarkdownCodeOnly("~~~lua\nGame.Set('a', 1)\n~~~\n"), "Game.Set", true));
	CHECK_FALSE(UsesName(MarkdownCodeOnly("~~~json\n```\n~~~\nGame.Set(k, v) in prose\n"), "Game.Set", true)); // ``` does not close ~~~.
	CHECK_FALSE(UsesName(MarkdownCodeOnly("```json\n``` still open\n````\nGame.Set(k, v) in prose\n"), "Game.Set", true));
	CHECK(UsesName(MarkdownCodeOnly("```lua\r\nGame.Set('a', 1)\r\n```\r\n"), "Game.Set", true));
	// Methods called in the docs: not a block's own functions, and not C++ scope resolution.
	CHECK(MethodCalls(LuaCodeOnly("function P:Helper() end self:Helper() self.entity:GetComponent('T') s:find('x')")) ==
		  std::set<std::string>{"GetComponent", "find"});
	CHECK(MethodCalls("Reflection::MakeField(x) e:IsValid()") == std::set<std::string>{"IsValid"});
	ScriptApi api;
	CHECK(api.IsStringMethod("find"));
	CHECK_FALSE(api.IsStringMethod("GetComponent"));
}

TEST_CASE("FeatureTest: the feature script and the documentation cover the whole scripting API")
{
	const std::string script = LuaCodeOnly(Paralax::Testing::ReadFileText(FeatureTestDirectory() / "Assets" / "Scripts" / "FeatureTest.lua"));
	const std::string documentation = MarkdownCodeOnly(Paralax::Testing::ReadFileText(RepositoryRoot() / "docs" / "Scripting.md"));
	REQUIRE_FALSE(script.empty());
	REQUIRE_FALSE(documentation.empty());

	ScriptApi api;
	CHECK(api.Names.size() > 60); // Sanity: the enumeration found the bindings.
	for (const std::string& name : api.Names)
	{
		CAPTURE(name);
		const bool function = api.IsFunction(name);
		// std::string: doctest prints a bare const char* as a pointer.
		CHECK_MESSAGE(UsesName(script, name, function),
					  std::string(function ? "Call it in Paralax-Tests/FeatureTest/Assets/Scripts/FeatureTest.lua, as Table.Function(...), value:Method(...) or through pcall"
										   : "Read it in Paralax-Tests/FeatureTest/Assets/Scripts/FeatureTest.lua, as Table.field or value.property"));
		CHECK_MESSAGE(UsesName(documentation, name, function),
					  std::string(function ? "Document it in docs/Scripting.md, in code, as `Table.Function(...)` or `value:Method(...)`"
										   : "Document it in docs/Scripting.md, in code, as `Table.field` or `value.property`"));
	}
}

TEST_CASE("FeatureTest: everything the documentation names in code exists in the scripting API")
{
	// The reverse direction: a script written from the docs must not meet "attempt to call a nil value".
	const std::vector<std::string> blocks = MarkdownCodeBlocks(Paralax::Testing::ReadFileText(RepositoryRoot() / "docs" / "Scripting.md"));
	REQUIRE_FALSE(blocks.empty());
	std::string documentation;
	for (const std::string& block : blocks)
		documentation += block + '\n';
	ScriptApi api;
	const std::set<std::string> names(api.Names.begin(), api.Names.end());
	std::set<std::string> tables, types, methods;
	for (const std::string& name : api.Names)
	{
		const size_t separator = name.find_first_of(".:");
		if (separator == std::string::npos)
			continue;
		(name[separator] == '.' ? tables : types).insert(name.substr(0, separator));
		if (name[separator] == ':')
			methods.insert(name.substr(separator + 1));
	}
	tables.erase("math"); // A Lua library the engine extends (random, randomseed); the rest (math.pi) is standard Lua.

	// Table.member for the API tables, and Type.Static (quat.FromEuler) for the userdata types.
	size_t checked = 0;
	const std::regex qualified(s_NameStart + "([A-Za-z_][A-Za-z0-9_]*)\\.([A-Za-z_][A-Za-z0-9_]*)");
	for (std::sregex_iterator match(documentation.begin(), documentation.end(), qualified), end; match != end; ++match)
	{
		const std::string prefix = (*match)[2];
		const bool table = tables.contains(prefix);
		if (!table && !types.contains(prefix))
			continue;
		const std::string name = prefix + (table ? "." : ":") + (*match)[3].str();
		CAPTURE(name);
		CHECK_MESSAGE(names.contains(name), "docs/Scripting.md names it, but no binding does: bind it or correct the docs");
		checked++;
	}
	// :Method( calls, which the docs write on values of any type. Lua's own string methods (s:find) are not the engine's.
	for (const std::string& block : blocks)
	{
		for (const std::string& member : MethodCalls(block))
		{
			if (api.IsStringMethod(member))
				continue;
			CAPTURE(member);
			CHECK_MESSAGE(methods.contains(member), "docs/Scripting.md calls this method, but no userdata type binds it");
			checked++;
		}
	}
	CHECK(checked > 60); // Sanity: the documentation's code was found and parsed.
}
