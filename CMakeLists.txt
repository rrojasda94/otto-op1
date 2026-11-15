cmake_minimum_required(VERSION 3.24)
project(otto_op1 C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
# SOURCE_SUBDIR none = download only, we compile the sources ourselves
FetchContent_Declare(rtaudio GIT_REPOSITORY https://github.com/thestk/rtaudio.git GIT_TAG 6.0.1 GIT_SHALLOW ON SOURCE_SUBDIR none)
FetchContent_Declare(rtmidi  GIT_REPOSITORY https://github.com/thestk/rtmidi.git  GIT_TAG 6.0.0 GIT_SHALLOW ON SOURCE_SUBDIR none)
FetchContent_Declare(imgui   GIT_REPOSITORY https://github.com/ocornut/imgui.git  GIT_TAG v1.91.5 GIT_SHALLOW ON SOURCE_SUBDIR none)
# ponytail: Gamma and dr_libs have no release tags; pinned to master. Pin a commit if upstream breaks.
FetchContent_Declare(gamma   GIT_REPOSITORY https://github.com/LancePutnam/Gamma.git GIT_TAG master GIT_SHALLOW ON SOURCE_SUBDIR none)
FetchContent_Declare(drlibs  GIT_REPOSITORY https://github.com/mackron/dr_libs.git  GIT_TAG master GIT_SHALLOW ON SOURCE_SUBDIR none)
FetchContent_Declare(json    URL https://github.com/nlohmann/json/releases/download/v3.11.3/include.zip SOURCE_SUBDIR none)
FetchContent_Declare(glfw    GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4 GIT_SHALLOW ON)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(rtaudio rtmidi imgui gamma drlibs json glfw)

# ---- Gamma (DSP library used by OTTO): only the parts without audio/file IO deps
file(GLOB GAMMA_SRC ${gamma_SOURCE_DIR}/src/*.cpp)
list(FILTER GAMMA_SRC EXCLUDE REGEX "(AudioIO|SoundFile|Recorder|Scheduler|AudioDevice)")
add_library(gamma STATIC ${GAMMA_SRC})
target_include_directories(gamma PUBLIC ${gamma_SOURCE_DIR})
target_compile_definitions(gamma PUBLIC _USE_MATH_DEFINES NOMINMAX)
if(MSVC)
  target_compile_options(gamma PRIVATE /W0)
endif()

# ---- core: engines, fx, sequencer, tape (no audio/midi/ui deps -> testable)
add_library(otto_core STATIC src/engines.cpp src/drums.cpp src/fx.cpp src/studio.cpp src/audio_file.cpp src/dr_wav.c)
target_include_directories(otto_core PUBLIC src ${drlibs_SOURCE_DIR} ${json_SOURCE_DIR}/include)
target_link_libraries(otto_core PUBLIC gamma)
if(MSVC)
  target_compile_options(otto_core PRIVATE /W3 /utf-8 /fp:fast)
endif()

# ---- app
add_executable(otto-op1
  src/main.cpp src/midi.cpp src/filedialog.cpp
  ${rtaudio_SOURCE_DIR}/RtAudio.cpp
  ${rtaudio_SOURCE_DIR}/include/asio.cpp
  ${rtaudio_SOURCE_DIR}/include/asiodrivers.cpp
  ${rtaudio_SOURCE_DIR}/include/asiolist.cpp
  ${rtaudio_SOURCE_DIR}/include/iasiothiscallresolver.cpp
  ${rtmidi_SOURCE_DIR}/RtMidi.cpp
  ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(otto-op1 PRIVATE ${rtaudio_SOURCE_DIR} ${rtaudio_SOURCE_DIR}/include ${rtmidi_SOURCE_DIR}
  ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(otto-op1 PRIVATE __WINDOWS_WASAPI__ __WINDOWS_DS__ __WINDOWS_ASIO__ __WINDOWS_MM__ NOMINMAX)
target_link_libraries(otto-op1 PRIVATE otto_core glfw opengl32 comdlg32 winmm ole32 ksuser mfplat mfuuid wmcodecdspuuid dsound)
if(MSVC)
  target_compile_options(otto-op1 PRIVATE /utf-8 /fp:fast)
endif()
add_custom_command(TARGET otto-op1 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/mapping.json $<TARGET_FILE_DIR:otto-op1>/mapping.json
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:otto-op1>/samples)

# ---- one runnable DSP check
add_executable(dsp_check tests/dsp_check.cpp)
target_link_libraries(dsp_check PRIVATE otto_core)
enable_testing()
add_test(NAME dsp_check COMMAND dsp_check)
