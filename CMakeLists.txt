cmake_minimum_required(VERSION 3.21)
project(gpu-sbr LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(glfw3 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_path(STB_INCLUDE_DIRS "stb_image.h")

add_executable(gpu-sbr
    src/main.cpp
    src/pipeline.cpp
    src/gl_util.cpp
    src/brush_atlas.cpp
    src/media.cpp
    src/jobs.cpp
    src/filedialog.cpp
    src/paramfile.cpp
)

target_include_directories(gpu-sbr PRIVATE src ${STB_INCLUDE_DIRS})
target_link_libraries(gpu-sbr PRIVATE glfw glad::glad glm::glm imgui::imgui)

if(MSVC)
    target_compile_options(gpu-sbr PRIVATE /W3 /permissive- /utf-8)
    target_compile_definitions(gpu-sbr PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

# Shaders and brushes are loaded at runtime; point the exe at the source tree so
# hot-reload edits take effect without a rebuild.
target_compile_definitions(gpu-sbr PRIVATE SBR_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
