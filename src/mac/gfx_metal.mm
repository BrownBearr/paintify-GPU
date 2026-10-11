// gfx.h on macOS: a CAMetalLayer on the GLFW window, ImGui's Metal renderer,
// and the GL-on-Metal layer underneath the painting pipeline.

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <imgui_impl_glfw.h>
#include <imgui_impl_metal.h>

#include "gfx.h"

#include <cstdio>
#include <string>

namespace gfx {
namespace {
void* g_framePool = nullptr;   // autorelease pool spanning one displayed frame
}

void windowHints(bool visible) {
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    // Window size and content scale are in points; the drawable is pixels.
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_FALSE);
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_COCOA_GRAPHICS_SWITCHING, GLFW_TRUE);
    glfwWindowHintString(GLFW_COCOA_FRAME_NAME, "BrushkitMain");   // remembers its frame
    if (!visible) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
}

bool needsWindowForOffscreen() { return false; }

bool initContext(GLFWwindow* win) {
    if (!mgl::init()) {
        fprintf(stderr, "Brushkit needs a Metal-capable GPU.\n");
        return false;
    }
    if (win) {
        NSWindow* nsw = glfwGetCocoaWindow(win);
        CAMetalLayer* layer = [CAMetalLayer layer];
        layer.contentsScale = nsw.backingScaleFactor;
        // Keep the drawable count low: the painting is presented as it is
        // made, and a third buffer only adds a frame of latency to the sliders.
        layer.maximumDrawableCount = 2;
        nsw.contentView.layer = layer;
        nsw.contentView.wantsLayer = YES;
        mgl::attachLayer((__bridge void*)layer);
    }
    return true;
}

const char* describe() {
    static std::string s;
    s = std::string("Metal | ") + mgl::deviceName();
    return s.c_str();
}

void imguiInit(GLFWwindow* win) {
    ImGui_ImplGlfw_InitForOther(win, true);
    ImGui_ImplMetal_Init((__bridge id<MTLDevice>)mgl::device());
}

void imguiShutdown() {
    ImGui_ImplMetal_Shutdown();
    ImGui_ImplGlfw_Shutdown();
}

void rebuildFontTexture() {
    ImGui_ImplMetal_DestroyFontsTexture();
    ImGui_ImplMetal_CreateFontsTexture((__bridge id<MTLDevice>)mgl::device());
}

bool beginFrame(GLFWwindow* win) {
    if (!g_framePool) g_framePool = mgl::poolPush();
    NSWindow* nsw = glfwGetCocoaWindow(win);
    CAMetalLayer* layer = (CAMetalLayer*)nsw.contentView.layer;
    if (layer && layer.contentsScale != nsw.backingScaleFactor)
        layer.contentsScale = nsw.backingScaleFactor;
    int fw = 0, fh = 0;
    glfwGetFramebufferSize(win, &fw, &fh);
    return mgl::beginFrame(fw, fh);
}

void imguiNewFrame() {
    ImGui_ImplMetal_NewFrame((__bridge MTLRenderPassDescriptor*)mgl::screenPassDescriptor());
    ImGui_ImplGlfw_NewFrame();
}

void imguiRender() {
    id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)mgl::screenEncoder();
    if (!enc) return;
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(),
                                   (__bridge id<MTLCommandBuffer>)mgl::commandBuffer(), enc);
}

void present(GLFWwindow*) {
    mgl::endFrame();
    if (g_framePool) { mgl::poolPop(g_framePool); g_framePool = nullptr; }
}

ImTextureID textureId(GLuint tex) { return ImTextureID(mgl::textureObject(tex)); }

} // namespace gfx
