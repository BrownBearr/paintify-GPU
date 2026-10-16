// Persistent Pipeline integration checks: no visible window or UI automation.
#include "pipeline.h"
#include "paramfile.h"
#include <GLFW/glfw3.h>
#include <cmath>
#include <cstdio>
#include <vector>

int main() {
    if (!glfwInit()) return 2;
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    auto* window = glfwCreateWindow(32, 32, "Direction integration audit", nullptr, nullptr);
    if (!window) return 2;
    glfwMakeContextCurrent(window);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return 2;
    Pipeline pipeline;
    if (!pipeline.init()) return 2;
    constexpr int size = 160;
    std::vector<unsigned char> image(size * size * 4, 255);
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x)
        for (int k = 0; k < 3; ++k) image[(y * size + x) * 4 + k] = 64 + x * 51 / (size-1);
    if (!pipeline.setSource(image.data(), size, size)) return 2;
    TuningParams params;
    params.minStrokeLength = params.maxStrokeLength = 8;
    params.gridFactor = 2.5f; params.opacity = 1;
    params.jitterHue = params.jitterSat = params.jitterVal = params.texStrength = 0;
    RenderConfig cfg;
    cfg.radii = {3}; cfg.underpaint = Underpaint::None; cfg.passesPerLayer = 1;
    StyleParams style;
    style.enabled = 1; style.bristles = 0;
    style.edgeRough = 0; style.impasto = 0;
    int failures = 0, assertions = 0;
    auto check = [&](bool ok, const char* label) {
        ++assertions; if (!ok) ++failures;
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    };
    for (int brush = 0; brush <= 1; ++brush) {
        style.brushModel = float(brush);
        for (int smoothing = 0; smoothing < 4; ++smoothing) {
            params.tensorSigma = (smoothing & 1) ? 3.f : 0.f;
            cfg.etfIterations = (smoothing & 2) ? 2 : 0;
            style.fieldMode = 0; style.fieldMix = 1;
            pipeline.setStyle(style);
            pipeline.render(params, cfg);
            const auto sourceRender = pipeline.readCanvas();
            for (int mode = 1; mode <= 5; ++mode) {
                style.fieldMode = float(mode); style.fieldMix = 1;
                pipeline.setStyle(style);
                check(pipeline.renderIfChanged(params, cfg), "switching mode triggers repaint");
                check(!pipeline.renderIfChanged(params, cfg), "unchanged mode stays cached");
                style.fieldMix = 0;
                pipeline.setStyle(style);
                check(pipeline.renderIfChanged(params, cfg), "strength change triggers repaint");
                check(pipeline.readCanvas() == sourceRender, "switch to strength zero restores source paths");
                check(!pipeline.renderIfChanged(params, cfg), "strength zero stays cached");
                style.fieldMode = 0;
                pipeline.setStyle(style);
                check(pipeline.renderIfChanged(params, cfg), "return to Follow image triggers repaint");
                check(pipeline.readCanvas() == sourceRender, "Follow image restored after mode switch");
            }
        }
    }
    // The same loader and saver serve the GUI and CLI. Verify the settings
    // actually survive reloading and a subsequent render in this live pipeline.
    style.fieldMode = 5; style.fieldMix = .5f; style.fallbackAngle = 37;
    style.layerB[0][2] = .3f; params.tensorSigma = 2.5f; cfg.etfIterations = 2;
    cfg.layerSpecs.resize(1); cfg.layerSpecs[0].curvature = .7f;
    std::string err;
    check(paramfile::save("audit/direction-deep/persistent.sbr", params, cfg, &err, &style, "pop"),
          "save persistent look");
    TuningParams loadedParams;
    RenderConfig loadedCfg;
    StyleParams loadedStyle;
    std::string key;
    bool haveStyle = false;
    check(paramfile::load("audit/direction-deep/persistent.sbr", &loadedParams, &loadedCfg,
                         &err, &loadedStyle, &key, &haveStyle), "load persistent look");
    check(haveStyle && loadedStyle.fieldMode == 5 && loadedStyle.fieldMix == .5f &&
          loadedStyle.fallbackAngle == 37 && loadedStyle.layerB[0][2] == .3f &&
          loadedParams.tensorSigma == 2.5f && loadedCfg.etfIterations == 2 &&
          loadedCfg.layerSpecs.size() == 1 && loadedCfg.layerSpecs[0].curvature == .7f,
          "direction smoothing and per-layer controls survive save/load");
    pipeline.setStyle(style); pipeline.render(params, cfg);
    const auto beforeSave = pipeline.readCanvas();
    pipeline.setStyle(loadedStyle); pipeline.render(loadedParams, loadedCfg);
    check(pipeline.readCanvas() == beforeSave, "loaded look reproduces pixels in live pipeline");
    check(glGetError() == GL_NO_ERROR, "no OpenGL errors");
    std::printf("Assertions: %d. Failures: %d\n", assertions, failures);
    pipeline.shutdown(); glfwDestroyWindow(window); glfwTerminate();
    return failures ? 1 : 0;
}
