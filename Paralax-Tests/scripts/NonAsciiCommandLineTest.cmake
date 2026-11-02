# Script mode (cmake -P): the editor and the runtime take paths with non-ASCII characters on their command lines.
#
#   -DMODE=editor    copies the feature test project to a non-ASCII directory, then has the editor create a project,
#                    open that one (with a scene, the agent server and a screenshot) and export it, all at non-ASCII
#                    paths
#   -DMODE=runtime   plays the copy and its export (both left by the editor mode) with the runtime, then removes WORK
#
# Other parameters: EDITOR and RUNTIME (the executables), EXECUTABLE_SUFFIX (".exe" on Windows), FEATURE_TEST (the
# feature test project) and WORK (a scratch directory, replaced by the editor mode).
#
# The characters (Latin-1 é, Japanese 日本, Greek Ω) share no ANSI code page, so on Windows argv could never hold
# them all: the programs only find these files if they read the Unicode command line. CMake runs the programs through
# CreateProcessW there, from its own UTF-8, and reads their output as UTF-8.

cmake_minimum_required(VERSION 3.25)

foreach(parameter IN ITEMS MODE EDITOR RUNTIME FEATURE_TEST WORK)
	if(NOT DEFINED ${parameter})
		message(FATAL_ERROR "${parameter} is not set")
	endif()
endforeach()

set(PROJECT_DIR "${WORK}/Projekt-é-日本-Ω")
set(SCENE "${PROJECT_DIR}/Assets/Scenes/FeatureTest.pxscene")
set(EXPORT_DIR "${WORK}/Export-é-日本-Ω")
set(EXPORTED_GAME "${EXPORT_DIR}/FeatureTest${EXECUTABLE_SUFFIX}")
# Project names are ASCII (they name the project file and the exported executable); the directories above need not be.
set(NEW_PROJECT_DIR "${WORK}/Neu-é-日本-Ω/Spiel")
set(AGENT_INFO "${WORK}/Agent-é-日本-Ω.json")
set(PLAY_ARGUMENTS --headless --frames 60 --fixed-timestep 0.0166667 --validation --fail-on-script-error)

# Runs a program in WORK and fails the test unless it exits with 0. Its output (stdout and stderr) is left in
# PX_OUTPUT.
function(px_run description)
	execute_process(COMMAND ${ARGN}
		WORKING_DIRECTORY "${WORK}"
		RESULT_VARIABLE result
		OUTPUT_VARIABLE output
		ERROR_VARIABLE output
		ENCODING UTF-8)
	if(NOT result STREQUAL "0")
		message(FATAL_ERROR "${description} failed (${result}):\n${output}")
	endif()
	message(STATUS "${description}: done")
	set(PX_OUTPUT "${output}" PARENT_SCOPE)
endfunction()

function(px_require_file description path)
	if(NOT EXISTS "${path}" OR IS_DIRECTORY "${path}")
		message(FATAL_ERROR "${description}: '${path}' was not written")
	endif()
endfunction()

function(px_require_output description output text)
	string(FIND "${output}" "${text}" found)
	if(found EQUAL -1)
		message(FATAL_ERROR "${description}: the output does not contain \"${text}\":\n${output}")
	endif()
endfunction()

function(px_require_output_match description output pattern)
	string(REGEX MATCH "${pattern}" found "${output}")
	if(NOT found)
		message(FATAL_ERROR "${description}: the output does not match \"${pattern}\":\n${output}")
	endif()
endfunction()

if(MODE STREQUAL "editor")
	file(REMOVE_RECURSE "${WORK}")
	file(MAKE_DIRECTORY "${WORK}")
	file(COPY "${FEATURE_TEST}/" DESTINATION "${PROJECT_DIR}" PATTERN ".paralax" EXCLUDE)

	# --new-project and --agent-info. The editor reports where it wrote the connection file, and removes it on exit.
	px_run("The editor creating a project" "${EDITOR}" --headless --frames 3 --new-project "${NEW_PROJECT_DIR}"
		--agent-port 0 --agent-info "${AGENT_INFO}")
	px_require_file("--new-project" "${NEW_PROJECT_DIR}/Spiel.pxproj")
	px_require_output("--agent-info" "${PX_OUTPUT}" "Agent connection details written to '${AGENT_INFO}'")

	# --project, --scene and --screenshot. Without --agent-info the connection file goes in the project directory,
	# which shows the project the editor opened. The editor logs that directory resolved (canonical), so only its name
	# is matched.
	px_run("The editor opening the project" "${EDITOR}" --headless --frames 5 --validation --project "${PROJECT_DIR}"
		--scene "${SCENE}" --agent-port 0 --screenshot "${WORK}/Editor-é-日本-Ω.png")
	px_require_output_match("--project" "${PX_OUTPUT}" "Agent connection details written to '[^']*/Projekt-é-日本-Ω/\\.paralax/AgentServer\\.json'")
	string(FIND "${PX_OUTPUT}" "Could not open scene" sceneError)
	if(NOT sceneError EQUAL -1)
		message(FATAL_ERROR "--scene: the editor could not open '${SCENE}':\n${PX_OUTPUT}")
	endif()
	px_require_file("--screenshot" "${WORK}/Editor-é-日本-Ω.png")

	# --export: the game folder holds the project's file and the runtime named after it.
	px_run("The editor exporting the project" "${EDITOR}" --headless --project "${PROJECT_DIR}" --export "${EXPORT_DIR}")
	px_require_file("--export" "${EXPORT_DIR}/FeatureTest.pxproj")
	px_require_file("--export" "${EXPORTED_GAME}")
elseif(MODE STREQUAL "runtime")
	# --project and --scene: a scene outside the project's assets would be refused, so playing it shows the project.
	px_run("The runtime playing the project" "${RUNTIME}" --project "${PROJECT_DIR}" --scene "${SCENE}" ${PLAY_ARGUMENTS}
		--screenshot "${WORK}/Runtime-é-日本-Ω.png")
	px_require_file("--screenshot" "${WORK}/Runtime-é-日本-Ω.png")

	# The exported game, from its non-ASCII folder: it finds the project beside its own executable.
	px_run("The exported game" "${EXPORTED_GAME}" ${PLAY_ARGUMENTS} --screenshot "${WORK}/Exported-é-日本-Ω.png")
	px_require_file("--screenshot" "${WORK}/Exported-é-日本-Ω.png")

	# The export holds a copy of the runtime and the engine resources. A failure above keeps it for inspection.
	file(REMOVE_RECURSE "${WORK}")
else()
	message(FATAL_ERROR "Unknown MODE '${MODE}' (expected editor or runtime)")
endif()
