cmake_minimum_required(VERSION 3.21)
project(brushkit LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(glfw3 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
if(WIN32)
    find_package(Spout2 CONFIG REQUIRED)
endif()
find_path(STB_INCLUDE_DIRS "stb_image.h")

add_executable(brushkit
    src/main.cpp
    src/pipeline.cpp
    src/gl_util.cpp
    src/brush_atlas.cpp
    src/media.cpp
    src/jobs.cpp
    src/filedialog.cpp
    src/paramfile.cpp
    src/styles.cpp
    src/live_spout.cpp
)

target_include_directories(brushkit PRIVATE src ${STB_INCLUDE_DIRS})
target_link_libraries(brushkit PRIVATE glfw glad::glad glm::glm imgui::imgui)
if(WIN32)
    target_link_libraries(brushkit PRIVATE Spout2::Spout)

    # Round-trips a test pattern through --live-spout, the way TouchDesigner does.
    add_executable(brushkit-spout-smoke tests/spout_smoke.cpp)
    target_link_libraries(brushkit-spout-smoke PRIVATE glfw glad::glad Spout2::Spout)
    target_compile_definitions(brushkit-spout-smoke PRIVATE NOMINMAX)
endif()

if(MSVC)
    target_compile_options(brushkit PRIVATE /W3 /permissive- /utf-8)
    target_compile_definitions(brushkit PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

# Shaders are loaded at runtime; point the exe at the source tree so hot-reload
# edits take effect without a rebuild.
target_compile_definitions(brushkit PRIVATE SBR_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
