cmake_minimum_required(VERSION 3.21)
project(brushkit LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(glfw3 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
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
    src/ui_theme.cpp
    src/ui_widgets.cpp
)

target_include_directories(brushkit PRIVATE src ${STB_INCLUDE_DIRS})
target_link_libraries(brushkit PRIVATE glfw glad::glad glm::glm imgui::imgui)
if(MSVC)
    target_compile_options(brushkit PRIVATE /W3 /permissive- /utf-8)
    target_compile_definitions(brushkit PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

# Shaders are loaded at runtime; point the exe at the source tree so hot-reload
# edits take effect without a rebuild.
target_compile_definitions(brushkit PRIVATE SBR_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# The build tree is directly runnable; the install tree is a portable bundle.
# UI fonts (Geist, SIL OFL 1.1) travel the same way, so the interface looks
# the same from the build tree and from a portable bundle.
add_custom_command(TARGET brushkit POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "$<TARGET_FILE_DIR:brushkit>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts" "$<TARGET_FILE_DIR:brushkit>/fonts")
install(TARGETS brushkit RUNTIME DESTINATION .)
install(FILES $<TARGET_RUNTIME_DLLS:brushkit> DESTINATION . OPTIONAL)
install(DIRECTORY shaders DESTINATION .)
install(DIRECTORY assets/fonts/ DESTINATION fonts)
install(FILES assets/fonts/OFL.txt DESTINATION licenses RENAME Geist-OFL.txt)
install(FILES run.bat README.md DESTINATION .)
# Carry notices for the libraries linked into the portable release.
foreach(dependency IN ITEMS glfw3 glad glm imgui stb freetype brotli bzip2 libpng zlib)
    set(notice "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${dependency}/copyright")
    if(EXISTS "${notice}")
        install(FILES "${notice}" DESTINATION licenses RENAME "${dependency}.txt")
    endif()
endforeach()
set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME Brushkit)
set(CPACK_PACKAGE_VERSION 0.2.0)
include(CPack)
