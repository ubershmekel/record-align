cmake_minimum_required(VERSION 3.24)
if(CMAKE_HOST_APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()
project(record-align VERSION 0.2.0 LANGUAGES C CXX)

if(APPLE)
  enable_language(OBJCXX)
  set(CMAKE_OBJCXX_STANDARD 17)
  set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# libsamplerate 0.2.2 predates CMake 4; its build works with the 3.5 policies.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

if(NOT WIN32 AND NOT APPLE)
  message(FATAL_ERROR "Supported platforms are Windows and macOS.")
endif()

option(RA_ASAN "Build with AddressSanitizer" OFF)
option(RA_APP "Build the desktop UI" ON)

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
  if(RA_ASAN)
    add_compile_options(/fsanitize=address /Zi)
    add_link_options(/INCREMENTAL:NO)
  endif()
elseif(RA_ASAN)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()

add_library(pffft_lib STATIC
  ${pffft_SOURCE_DIR}/src/pffft.c
  ${pffft_SOURCE_DIR}/src/pffft_common.c)
target_include_directories(pffft_lib PUBLIC ${pffft_SOURCE_DIR}/include PRIVATE ${pffft_SOURCE_DIR}/include/pffft)
target_compile_definitions(pffft_lib PUBLIC PFFFT_STATIC_DEFINE PRIVATE _USE_MATH_DEFINES)

if(WIN32)
  set(RA_CAPTURE_SOURCE src/audio/capture_wasapi.cpp)
else()
  set(RA_CAPTURE_SOURCE src/audio/capture_coreaudio.mm)
  set_source_files_properties(src/audio/capture_coreaudio.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()

add_library(ra_core STATIC
  ${RA_CAPTURE_SOURCE}
  src/platform/process.cpp
  src/audio/edits.cpp
  src/audio/import.cpp
  src/audio/recorder.cpp
  src/audio/session.cpp
  src/audio/wav.cpp
  src/sync/export.cpp
  src/sync/sync.cpp)
target_include_directories(ra_core PUBLIC src)
target_link_libraries(ra_core PRIVATE pffft_lib samplerate)
if(WIN32)
  target_compile_definitions(ra_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(ra_core PUBLIC ole32 avrt)
else()
  target_link_libraries(ra_core PUBLIC "-framework CoreAudio" "-framework AudioUnit"
    "-framework AVFoundation" "-framework Foundation" "-framework CoreFoundation" objc)
endif()

add_executable(record-align-cli src/cli/main.cpp)
target_link_libraries(record-align-cli PRIVATE ra_core)

execute_process(
  COMMAND git rev-parse --short HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE RA_GIT_HASH
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT RA_GIT_HASH)
  set(RA_GIT_HASH "unknown")
endif()

if(RA_APP)
  if(WIN32)
    configure_file(src/app/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/app.rc @ONLY)
    set(RA_APP_RESOURCES ${CMAKE_CURRENT_BINARY_DIR}/app.rc)
  else()
    set(RA_APP_RESOURCES assets/logo.icns)
    set_source_files_properties(assets/logo.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  endif()
  configure_file(src/app/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/version.h @ONLY)
  add_executable(record-align WIN32 MACOSX_BUNDLE
    src/app/main.cpp
    src/app/playback.cpp
    ${RA_APP_RESOURCES}
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
  target_include_directories(record-align PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${CMAKE_CURRENT_BINARY_DIR})
  target_link_libraries(record-align PRIVATE ra_core SDL3::SDL3-static)
  if(APPLE)
    set_target_properties(record-align PROPERTIES
      MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/src/app/Info.plist.in
      MACOSX_BUNDLE_GUI_IDENTIFIER com.ubershmekel.record-align
      MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
      MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
  endif()
endif()

enable_testing()
add_executable(record-align-tests
  tests/test_edits.cpp
  tests/test_import.cpp
  tests/test_main.cpp
  tests/test_recorder.cpp
  tests/test_soak.cpp
  tests/test_sync.cpp
  tests/test_wav.cpp)
target_include_directories(record-align-tests PRIVATE ${doctest_SOURCE_DIR})
target_link_libraries(record-align-tests PRIVATE ra_core)
add_test(NAME record-align-tests COMMAND record-align-tests)

if(APPLE)
  # Standalone CLI also needs a microphone purpose string for macOS privacy checks.
  configure_file(src/app/cli-Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/cli-Info.plist @ONLY)
  target_link_options(record-align-cli PRIVATE
    "LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_BINARY_DIR}/cli-Info.plist")
endif()
