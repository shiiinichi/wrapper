cmake_minimum_required(VERSION 3.25)
project(wrapper-native VERSION 0.1.0 LANGUAGES NONE)

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" OR
   NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    message(FATAL_ERROR "The native macOS build requires Apple Silicon running an arm64 shell.")
endif()

# Use the same native build/package entrypoint as the desktop Run action.
add_custom_target(wrapper-native ALL
    COMMAND "${CMAKE_SOURCE_DIR}/script/build_and_run.sh" --app-build
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    USES_TERMINAL VERBATIM)
add_custom_target(native-verify
    COMMAND "${CMAKE_SOURCE_DIR}/script/build_and_run.sh" --verify
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    USES_TERMINAL VERBATIM)
