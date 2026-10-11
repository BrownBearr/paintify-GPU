cmake_minimum_required(VERSION 3.21)
# Read by project() on Apple platforms; ignored elsewhere. macOS 12 is the
# oldest with every API the app uses (UTType panels, MSL 2.3).
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
project(brushkit LANGUAGES CXX)
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 17)
set(BRUSHKIT_VERSION 0.2.0)
set(BRUSHKIT_BUNDLE_ID "com.brownbearr.brushkit")

find_package(glfw3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_path(STB_INCLUDE_DIRS "stb_image.h")
if(APPLE)
    # macOS has no compute shaders in OpenGL: the renderer runs on Metal, with
    # the GLSL sources translated at load time (src/mac/shader_translate.cpp).
    find_package(glslang CONFIG REQUIRED)
    find_package(spirv_cross_core CONFIG REQUIRED)
    find_package(spirv_cross_glsl CONFIG REQUIRED)
    find_package(spirv_cross_msl CONFIG REQUIRED)
else()
    find_package(glad CONFIG REQUIRED)
endif()

set(BRUSHKIT_SOURCES
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
    src/app_resources.cpp
)
if(APPLE)
    set(BRUSHKIT_MAC_SOURCES
        src/mac/gl_metal.mm
        src/mac/gfx_metal.mm
        src/mac/mac_app.mm
        src/mac/filedialog_mac.mm
    )
    list(APPEND BRUSHKIT_SOURCES ${BRUSHKIT_MAC_SOURCES} src/mac/shader_translate.cpp)
    set_source_files_properties(${BRUSHKIT_MAC_SOURCES} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set(BRUSHKIT_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/brushkit.icns")
    add_executable(brushkit MACOSX_BUNDLE ${BRUSHKIT_SOURCES} ${BRUSHKIT_ICNS})
    set_source_files_properties(${BRUSHKIT_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    configure_file(assets/mac/Info.plist.in "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)
    set_target_properties(brushkit PROPERTIES
        OUTPUT_NAME Brushkit
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist")
else()
    add_executable(brushkit ${BRUSHKIT_SOURCES} src/gfx_gl.cpp)
endif()
if(WIN32)
    target_sources(brushkit PRIVATE assets/icon/brushkit.rc)
    # rc.exe's inputs are not dependency-scanned; rebuild when the images change.
    set_source_files_properties(assets/icon/brushkit.rc PROPERTIES OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/brushkit.ico;${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/brushkit.png")
endif()

target_include_directories(brushkit PRIVATE src ${STB_INCLUDE_DIRS})
target_link_libraries(brushkit PRIVATE glfw glm::glm imgui::imgui)
if(APPLE)
    target_link_libraries(brushkit PRIVATE
        glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits
        spirv-cross-msl spirv-cross-glsl spirv-cross-core
        "-framework Cocoa" "-framework Metal" "-framework QuartzCore"
        "-framework UniformTypeIdentifiers")
    # GLFW must not pull in <OpenGL/gl.h>: src/mac/gl_metal.h declares GL itself.
    target_compile_definitions(brushkit PRIVATE GLFW_INCLUDE_NONE GL_SILENCE_DEPRECATION)
else()
    target_link_libraries(brushkit PRIVATE glad::glad)
endif()
if(MSVC)
    target_compile_options(brushkit PRIVATE /W3 /permissive- /utf-8)
    target_compile_definitions(brushkit PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

# Shaders are loaded at runtime; point the exe at the source tree so hot-reload
# edits take effect without a rebuild.
target_compile_definitions(brushkit PRIVATE SBR_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

if(APPLE)
    # Brushkit.app is self-contained: shaders, fonts and the logo live in
    # Contents/Resources. Run from the build tree it still reads the source
    # shaders, so F5 / Cmd+R hot reload works during development. The bundle is
    # sealed with an ad-hoc signature last, after every file is in place.
    set(BRUSHKIT_RES "$<TARGET_BUNDLE_CONTENT_DIR:brushkit>/Resources")
    add_custom_command(TARGET brushkit POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "${BRUSHKIT_RES}/shaders"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${BRUSHKIT_RES}/fonts"
        COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/SchibstedGrotesk-Regular.ttf"
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/SchibstedGrotesk-Medium.ttf"
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/SchibstedGrotesk-SemiBold.ttf"
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/GeistMono-Regular.ttf"
                "${BRUSHKIT_RES}/fonts"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${BRUSHKIT_RES}/licenses"
        COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/SchibstedGrotesk-OFL.txt"
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/GeistMono-OFL.txt"
                "${BRUSHKIT_RES}/licenses"
        COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/brushkit.png" "${BRUSHKIT_RES}/brushkit.png"
        COMMAND codesign --force --sign - --timestamp=none "$<TARGET_BUNDLE_DIR:brushkit>"
        VERBATIM)

    install(TARGETS brushkit BUNDLE DESTINATION .)
    install(FILES README.md DESTINATION . RENAME "Read Me.md")
    set(CPACK_GENERATOR DragNDrop)
    set(CPACK_DMG_VOLUME_NAME Brushkit)
    set(CPACK_DMG_FORMAT UDZO)
    set(CPACK_PACKAGE_NAME Brushkit)
    set(CPACK_PACKAGE_VERSION ${BRUSHKIT_VERSION})
    set(CPACK_PACKAGE_FILE_NAME "Brushkit-${BRUSHKIT_VERSION}-macOS-${CMAKE_SYSTEM_PROCESSOR}")
    include(CPack)
    return()
endif()

# The build tree is directly runnable; the install tree is a portable bundle.
# UI fonts (Schibsted Grotesk + Geist Mono, SIL OFL 1.1) travel the same way, so the interface looks
# the same from the build tree and from a portable bundle.
add_custom_command(TARGET brushkit POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "$<TARGET_FILE_DIR:brushkit>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts" "$<TARGET_FILE_DIR:brushkit>/fonts")
install(TARGETS brushkit RUNTIME DESTINATION .)
install(FILES $<TARGET_RUNTIME_DLLS:brushkit> DESTINATION . OPTIONAL)
# TARGET_RUNTIME_DLLS lists direct dependencies only, and freetype brings its
# own (zlib, libpng, bzip2, brotli). vcpkg's applocal step has already copied
# the whole closure next to the exe, so ship every DLL found there.
install(CODE "
    file(GLOB _brushkit_dlls \"$<TARGET_FILE_DIR:brushkit>/*.dll\")
    file(INSTALL \${_brushkit_dlls} DESTINATION \"\${CMAKE_INSTALL_PREFIX}\")
")
install(DIRECTORY shaders DESTINATION .)
install(DIRECTORY assets/fonts/ DESTINATION fonts PATTERN "*.txt" EXCLUDE)
install(FILES assets/fonts/SchibstedGrotesk-OFL.txt DESTINATION licenses)
install(FILES assets/fonts/GeistMono-OFL.txt DESTINATION licenses)
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
set(CPACK_PACKAGE_VERSION ${BRUSHKIT_VERSION})
include(CPack)
