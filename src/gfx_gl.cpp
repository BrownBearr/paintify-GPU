#include "gfx.h"

#include <GLFW/glfw3.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <cstdio>
#include <string>

namespace gfx {

void windowHints(bool visible) {
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
    if (!visible) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
}

bool needsWindowForOffscreen() { return true; }

bool initContext(GLFWwindow* win) {
    if (!win) {
        fprintf(stderr, "need an OpenGL 4.6 core context\n");
        return false;
    }
    glfwMakeContextCurrent(win);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        fprintf(stderr, "glad failed\n");
        return false;
    }
    // The UI and canvas are presented together. Uncapped swaps tear through
    // both layers and can look like flicker even when the document is idle.
    glfwSwapInterval(1);
    return true;
}

const char* describe() {
    static std::string s;
    s = std::string("GL ") + (const char*)glGetString(GL_VERSION) + " | " +
        (const char*)glGetString(GL_RENDERER);
    return s.c_str();
}

void imguiInit(GLFWwindow* win) {
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 460");
}

void imguiShutdown() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
}

void rebuildFontTexture() {
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    ImGui_ImplOpenGL3_CreateFontsTexture();
}

bool beginFrame(GLFWwindow*) {
    // The paint pipeline uses offscreen FBOs and changes raster state. Start
    // the window frame from a known target/state before clearing and drawing
    // ImGui, including on frames where renderIfChanged skips painting.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    return true;
}

void imguiNewFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
}

void imguiRender() { ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); }

void present(GLFWwindow* win) { glfwSwapBuffers(win); }

ImTextureID textureId(GLuint tex) { return ImTextureID(intptr_t(tex)); }

} // namespace gfx
