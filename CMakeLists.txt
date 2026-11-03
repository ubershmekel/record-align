cmake_minimum_required(VERSION 3.24)
project(mydaw LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

if(NOT WIN32)
  message(FATAL_ERROR "This prototype only has a WASAPI capture backend so far. "
                      "macOS needs src/audio/capture_coreaudio.cpp and POSIX file mapping in wav.cpp.")
endif()

option(MYDAW_ASAN "Build with AddressSanitizer" OFF)
option(MYDAW_APP "Build the desktop UI" ON)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

set(SDL_SHARED OFF)
set(SDL_STATIC ON)
set(SDL_TEST_LIBRARY OFF)
FetchContent_Declare(SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG release-3.4.18
  GIT_SHALLOW TRUE)

set(LIBSAMPLERATE_EXAMPLES OFF)
set(LIBSAMPLERATE_INSTALL OFF)
set(BUILD_TESTING OFF)
FetchContent_Declare(samplerate
  GIT_REPOSITORY https://github.com/libsndfile/libsamplerate.git
  GIT_TAG 0.2.2
  GIT_SHALLOW TRUE)

# These three are compiled directly, so point SOURCE_SUBDIR somewhere without a CMakeLists.
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.93.0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR no-cmake)
FetchContent_Declare(pffft
  GIT_REPOSITORY https://github.com/marton78/pffft.git
  GIT_TAG b3344191adc040a0f4c8553a168ab38b3ef04367
  SOURCE_SUBDIR no-cmake)
FetchContent_Declare(doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.5.3
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR no-cmake)

FetchContent_MakeAvailable(SDL3 samplerate imgui pffft doctest)

if(MSVC)
  add_compile_options(/utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
  if(MYDAW_ASAN)
    add_compile_options(/fsanitize=address /Zi)
    add_link_options(/INCREMENTAL:NO)
  endif()
endif()

add_library(pffft_lib STATIC
  ${pffft_SOURCE_DIR}/src/pffft.c
  ${pffft_SOURCE_DIR}/src/pffft_common.c)
target_include_directories(pffft_lib PUBLIC ${pffft_SOURCE_DIR}/include PRIVATE ${pffft_SOURCE_DIR}/include/pffft)
target_compile_definitions(pffft_lib PUBLIC PFFFT_STATIC_DEFINE PRIVATE _USE_MATH_DEFINES)

add_library(mydaw_core STATIC
  src/audio/capture_wasapi.cpp
  src/audio/edits.cpp
  src/audio/import.cpp
  src/audio/recorder.cpp
  src/audio/session.cpp
  src/audio/wav.cpp
  src/sync/export.cpp
  src/sync/sync.cpp)
target_include_directories(mydaw_core PUBLIC src)
target_compile_definitions(mydaw_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(mydaw_core PUBLIC ole32 avrt PRIVATE pffft_lib samplerate)

add_executable(mydaw-cli src/cli/main.cpp)
target_link_libraries(mydaw-cli PRIVATE mydaw_core)

if(MYDAW_APP)
  add_executable(mydaw WIN32
    src/app/main.cpp
    src/app/playback.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
  target_include_directories(mydaw PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(mydaw PRIVATE mydaw_core SDL3::SDL3-static)
endif()

enable_testing()
add_executable(mydaw-tests
  tests/test_edits.cpp
  tests/test_import.cpp
  tests/test_main.cpp
  tests/test_recorder.cpp
  tests/test_soak.cpp
  tests/test_sync.cpp
  tests/test_wav.cpp)
target_include_directories(mydaw-tests PRIVATE ${doctest_SOURCE_DIR})
target_link_libraries(mydaw-tests PRIVATE mydaw_core)
add_test(NAME mydaw-tests COMMAND mydaw-tests)
