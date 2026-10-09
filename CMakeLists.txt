cmake_minimum_required(VERSION 3.21)

project(GTASADE.PCFix VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
  message(FATAL_ERROR "GTASADE.PCFix is a Windows x64 ASI project")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "GTASADE.PCFix must be built as x64")
endif()

if(NOT MSVC)
  message(FATAL_ERROR "GTASADE.PCFix currently targets MSVC x64")
endif()

add_library(sade_common STATIC
  src/common/build_fingerprint.cpp
  src/common/config.cpp
  src/common/csv_trace.cpp
  src/common/file_util.cpp
  src/common/logger.cpp
  src/common/pe_fingerprint.cpp
  src/input/raw_input_observer.cpp
  src/input/raw_input_queue.cpp
)

target_include_directories(sade_common PUBLIC src)
target_compile_definitions(sade_common PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(sade_common PUBLIC advapi32 version)
target_compile_options(sade_common PRIVATE /W4 /permissive- /EHsc)

add_library(GTASADE.PCFix MODULE
  src/asi/dllmain.cpp
  src/bootstrap/observe_only_runtime.cpp
  src/devtools/value_scanner.cpp
  src/game/fps_lock.cpp
  src/hooks/iat_hook.cpp
)

target_link_libraries(GTASADE.PCFix PRIVATE sade_common user32)
target_compile_definitions(GTASADE.PCFix PRIVATE SADE_CORE_VERSION="${PROJECT_VERSION}")
target_compile_options(GTASADE.PCFix PRIVATE /W4 /permissive- /EHsc)
set_target_properties(GTASADE.PCFix PROPERTIES
  PREFIX ""
  OUTPUT_NAME "GTASADE.PCFix"
  SUFFIX ".asi"
)

# ReShade add-on front-end. Loads the core .asi and shows its settings in the ReShade overlay.
add_library(GTASADE.PCFix.Addon MODULE
  src/addon/reshade_addon.cpp
)
target_include_directories(GTASADE.PCFix.Addon PRIVATE
  src
  third_party/reshade/include
  third_party/imgui
)
target_compile_definitions(GTASADE.PCFix.Addon PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(GTASADE.PCFix.Addon PRIVATE /W4 /permissive- /EHsc)
set_target_properties(GTASADE.PCFix.Addon PROPERTIES
  PREFIX ""
  OUTPUT_NAME "GTASADE.PCFix"
  SUFFIX ".addon64"
)

add_executable(GTASADE.PCFix.RawInputHelper WIN32
  src/companion/raw_input_companion.cpp
)
target_include_directories(GTASADE.PCFix.RawInputHelper PRIVATE src)
target_compile_definitions(GTASADE.PCFix.RawInputHelper PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(GTASADE.PCFix.RawInputHelper PRIVATE user32 shell32 hid)
target_compile_options(GTASADE.PCFix.RawInputHelper PRIVATE /W4 /permissive- /EHsc)

add_executable(sade_tests
  tests/main.cpp
  tests/raw_input_queue_tests.cpp
  tests/statistics_tests.cpp
)

target_link_libraries(sade_tests PRIVATE sade_common)
target_compile_options(sade_tests PRIVATE /W4 /permissive- /EHsc)

enable_testing()
add_test(NAME sade_tests COMMAND sade_tests)
