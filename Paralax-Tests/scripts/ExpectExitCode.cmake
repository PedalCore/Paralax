# Script mode (cmake -P): runs a command and passes only if it exits with exactly EXPECTED_EXIT_CODE and, when
# EXPECTED_OUTPUT (a regular expression) is set, its combined stdout and stderr match it. CTest's WILL_FAIL passes on
# any failure, so it cannot tell the failure a test expects from a crash or an unrelated error.
#
#   cmake -DEXPECTED_EXIT_CODE=<code> [-DEXPECTED_OUTPUT=<regex>] [-DTIMEOUT_SECONDS=<n>] [-DWORKING_DIRECTORY=<dir>]
#         -P ExpectExitCode.cmake -- <command> [<arg>...]
#
# The command is forwarded as a CMake list, so an empty argument is dropped and one containing ';' is split.
# TIMEOUT_SECONDS (default 240) stops the command first, below CTest's own timeout: CTest killing this script would
# leave the command running. WORKING_DIRECTORY, when set, is created if missing and the command runs there (CTest's
# own WORKING_DIRECTORY must already exist, which a directory made only at configure time may no longer).

cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED EXPECTED_EXIT_CODE OR NOT EXPECTED_EXIT_CODE MATCHES "^[0-9]+$")
	message(FATAL_ERROR "ExpectExitCode.cmake: set EXPECTED_EXIT_CODE to a non-negative integer")
endif()
if(NOT DEFINED TIMEOUT_SECONDS)
	set(TIMEOUT_SECONDS 240)
elseif(NOT TIMEOUT_SECONDS MATCHES "^[1-9][0-9]*$")
	message(FATAL_ERROR "ExpectExitCode.cmake: TIMEOUT_SECONDS must be a positive integer")
endif()

# Everything after "--" is the command; CMake passes it through unparsed.
set(command "")
set(separator_seen OFF)
math(EXPR last_argument "${CMAKE_ARGC} - 1")
foreach(index RANGE ${last_argument})
	if(separator_seen)
		list(APPEND command "${CMAKE_ARGV${index}}")
	elseif(CMAKE_ARGV${index} STREQUAL "--")
		set(separator_seen ON)
	endif()
endforeach()
if(NOT command)
	message(FATAL_ERROR "ExpectExitCode.cmake: no command given after --")
endif()

set(working_directory_option "")
if(DEFINED WORKING_DIRECTORY AND NOT WORKING_DIRECTORY STREQUAL "")
	file(MAKE_DIRECTORY "${WORKING_DIRECTORY}")
	set(working_directory_option WORKING_DIRECTORY "${WORKING_DIRECTORY}")
endif()

execute_process(COMMAND ${command} ${working_directory_option} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output
	TIMEOUT ${TIMEOUT_SECONDS})
# Shown in the test log either way, so a failure can be diagnosed from it.
message("${output}")

# A signal, a timeout or a failure to start makes the result a message rather than a number, which never matches.
if(NOT result STREQUAL EXPECTED_EXIT_CODE)
	message(FATAL_ERROR "Expected exit code ${EXPECTED_EXIT_CODE}, but the command exited with: ${result}")
endif()
if(DEFINED EXPECTED_OUTPUT AND NOT output MATCHES "${EXPECTED_OUTPUT}")
	message(FATAL_ERROR "The command exited with ${result} as expected, but its output does not match '${EXPECTED_OUTPUT}'")
endif()
