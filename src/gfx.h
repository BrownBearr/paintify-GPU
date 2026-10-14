#pragma once
// Window-side graphics: context creation, the ImGui renderer, presenting.
//
// The painting pipeline talks GL everywhere (natively on Windows, through the
// Metal layer in mac/gl_metal.h on macOS); what differs between the two is
// only how a window gets a surface and how the UI is drawn onto it. main.cpp
// calls these instead of glfw/glad/imgui_impl_* directly.
#include "gl_util.h"

#include <imgui.h>

struct GLFWwindow;

namespace gfx {

// GLFW hints for the window about to be created.
void windowHints(bool visible);
// A window is required for GL; Metal paints offscreen without one, so
// command-line jobs on macOS never put a window (or a Dock icon) on screen.
bool needsWindowForOffscreen();
// Makes the device current. `win` may be null where needsWindowForOffscreen() is false.
bool initContext(GLFWwindow* win);
// One line for the log: API and device.
const char* describe();

void imguiInit(GLFWwindow* win);
void imguiShutdown();
// After ui::applyTheme rebuilt the atlas.
void rebuildFontTexture();

// Brackets one displayed frame. beginFrame must come before imguiNewFrame;
// between the two calls GL's default framebuffer is the window.
bool beginFrame(GLFWwindow* win);
void imguiNewFrame();
void imguiRender();
void present(GLFWwindow* win);

ImTextureID textureId(GLuint tex);

} // namespace gfx
