// Direct GPU readback regression for Follow image. Run from the project root.
// Builds against the same gl_util, GLFW and GLAD as the app; no image heuristics.
#include "gl_util.h"
#include "params.h"
#include "style.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

namespace {
constexpr int W = 64, H = 64;
constexpr double PI = 3.141592653589793;
int failures = 0;
void require(bool ok, const char* label) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
std::vector<float> read(GLuint tex) {
    std::vector<float> out(W * H * 4);
    glGetTextureImage(tex, 0, GL_RGBA, GL_FLOAT, GLsizei(out.size() * sizeof(float)), out.data());
    return out;
}
float maxDifference(const std::vector<float>& a, const std::vector<float>& b) {
    float d = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!std::isfinite(a[i]) || !std::isfinite(b[i])) return std::numeric_limits<float>::infinity();
        d = std::max(d, std::abs(a[i] - b[i]));
    }
    return d;
}
double angle(const std::vector<float>& v) {
    const int i = ((H / 2) * W + W / 2) * 4;
    return std::atan2(v[i], -v[i + 1]) * 180 / PI;
}
double axialDistance(double a, double b) {
    return std::abs(std::remainder(a - b, 180.0));
}
}

int main(int argc, char** argv) {
    const char* baselineDir = nullptr;
    if (argc == 3 && std::string(argv[1]) == "--baseline-dir") baselineDir = argv[2];
    else if (argc != 1) { std::fprintf(stderr, "usage: flow_regression [--baseline-dir <shaders>]\n"); return 2; }
    if (!glfwInit()) return 2;
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(32, 32, "flow regression", nullptr, nullptr);
    if (!window) return 2;
    glfwMakeContextCurrent(window);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return 2;
    std::printf("GPU: %s\n", glGetString(GL_RENDERER));
    glu::Program features, baseline, fixed, etf;
    if (!features.loadCompute("features.comp") ||
        !etf.loadCompute("etf.comp") ||
        !fixed.loadCompute("stylefield.comp")) return 2;
    if (baselineDir && !baseline.loadCompute(std::string("../") + baselineDir + "/stylefield.comp")) return 2;
    TuningParams params;
    params.imgW = W; params.imgH = H;
    params.texelW = 1.f / W; params.texelH = 1.f / H;
    const GLuint tuning = glu::createBuffer(GL_UNIFORM_BUFFER, sizeof(params), &params, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, tuning);
    StyleParams style;
    style.enabled = style.brushModel = 1.f;
    const GLuint styling = glu::createBuffer(GL_UNIFORM_BUFFER, sizeof(style), &style, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 1, styling);
    const unsigned zero = 0;
    const GLuint etfMax = glu::createBuffer(GL_SHADER_STORAGE_BUFFER, sizeof(zero), &zero, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 10, etfMax);
    const GLuint src = glu::createTexture2D(W, H, GL_RGBA32F);
    const GLuint grad = glu::createTexture2D(W, H, GL_RGBA16F);
    const GLuint oldField = glu::createTexture2D(W, H, GL_RGBA16F);
    const GLuint newField = glu::createTexture2D(W, H, GL_RGBA16F);
    auto dispatch = [&](glu::Program& program, GLuint input, GLuint output) {
        program.use();
        glBindTextureUnit(0, input);
        glBindImageTexture(0, output, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glDispatchCompute(W / 16, H / 16, 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
    };
    auto uploadStyle = [&]() { glNamedBufferSubData(styling, 0, sizeof(style), &style); };
    auto checkFilteredSource = [&](GLuint input, const char* label) {
        const auto original = read(input);
        style = StyleParams{}; style.enabled = style.brushModel = 1.f;
        uploadStyle(); dispatch(fixed, input, newField);
        require(maxDifference(original, read(newField)) == 0.f, label);
        for (int mode = 1; mode <= 5; ++mode) {
            style.fieldMode = float(mode); style.fieldMix = 0; uploadStyle();
            dispatch(fixed, input, newField);
            require(maxDifference(original, read(newField)) == 0.f, "filtered pattern strength zero exact source");
            style.fieldMix = .0001f; uploadStyle(); dispatch(fixed, input, newField);
            require(maxDifference(original, read(newField)) < .0006f, "filtered epsilon vector convergence");
        }
    };
    struct Case { const char* name; float dx, dy; double expected; };
    const Case cases[] = {
        {"ramp +x", .001f, 0, 90}, {"ramp -x", -.001f, 0, 90},
        {"ramp +y", 0, .001f, 0}, {"ramp -y", 0, -.001f, 0},
        {"ramp diagonal", .001f, .001f, -45}, {"flat", 0, 0, 0}
    };
    for (const auto& c : cases) {
        std::vector<float> pixels(W * H * 4, 1.f);
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            const float value = .5f + (x - W / 2) * c.dx + (y - H / 2) * c.dy;
            for (int k = 0; k < 3; ++k) pixels[(y * W + x) * 4 + k] = value;
        }
        glTextureSubImage2D(src, 0, 0, 0, W, H, GL_RGBA, GL_FLOAT, pixels.data());
        features.use(); glUniform1i(features.uniform("uPass"), 0);
        dispatch(features, src, grad);
        style = StyleParams{}; style.enabled = style.brushModel = 1.f; uploadStyle();
        if (baselineDir) dispatch(baseline, grad, oldField);
        dispatch(fixed, grad, newField);
        const auto raw = read(grad), after = read(newField);
        const bool flat = c.dx == 0 && c.dy == 0;
        std::printf("%s: fixed %.4f deg, source %.4f deg, field max delta %.8f",
                    c.name, angle(after), angle(raw), maxDifference(raw, after));
        if (baselineDir) std::printf(", baseline %.4f deg", angle(read(oldField)));
        std::printf("\n");
        require(maxDifference(raw, after) == 0.f, c.name);
        if (!flat) require(axialDistance(angle(after), c.expected) < .02, "analytic tangent");
        else require(*std::max_element(after.begin(), after.end()) == 0.f, "flat remains directionless");
        for (int mode = 1; mode <= 5; ++mode) {
            style.fieldMode = float(mode); style.fieldMix = 1.f; uploadStyle();
            if (baselineDir) dispatch(baseline, grad, oldField);
            dispatch(fixed, grad, newField);
            const auto full = read(newField);
            require(std::all_of(full.begin(), full.end(), [](float v) { return std::isfinite(v); }),
                    "full pattern finite");
            require(maxDifference(raw, full) > 0.f, "full pattern changes flow");
            if (mode == 5) {
                bool analyticFixed = true;
                for (size_t i = 0; i < full.size(); i += 4) {
                    const double pixelAngle = std::atan2(full[i], -full[i+1]) * 180 / PI;
                    analyticFixed &= axialDistance(pixelAngle, style.fallbackAngle) < .04 &&
                        std::abs(full[i+2] - std::max(raw[i+2], style.minFieldMag)) < .00002f;
                }
                require(analyticFixed, "fixed-angle analytic direction and magnitude at every pixel");
                if (baselineDir) {
                    const auto old = read(oldField);
                    bool sameAxis = true;
                    for (size_t i = 0; i < full.size(); i += 4) {
                        const float same = std::max(std::abs(old[i] - full[i]), std::abs(old[i+1] - full[i+1]));
                        const float flip = std::max(std::abs(old[i] + full[i]), std::abs(old[i+1] + full[i+1]));
                        sameAxis &= std::min(same, flip) == 0.f && old[i+2] == full[i+2];
                    }
                    require(sameAxis, "fixed-angle full strength axis and magnitude unchanged");
                }
            }
            style.fieldMix = 0.f; uploadStyle(); dispatch(fixed, grad, newField);
            require(maxDifference(raw, read(newField)) == 0.f, "pattern strength zero exactly source");
            style.fieldMix = .0001f; uploadStyle(); dispatch(fixed, grad, newField);
            const auto epsilon = read(newField);
            bool nearMagnitude = true;
            for (size_t i = 2; i < raw.size(); i += 4)
                nearMagnitude &= std::abs(epsilon[i] - raw[i]) < .00002f;
            require(nearMagnitude && maxDifference(raw, epsilon) < .00002f &&
                    (flat || axialDistance(angle(epsilon), angle(raw)) < .04),
                    "pattern strength epsilon converges to source");
        }
        glNamedBufferSubData(etfMax, 0, sizeof(zero), &zero);
        etf.use(); glUniform1i(etf.uniform("uPass"), 0); dispatch(etf, grad, oldField);
        etf.use(); glUniform1i(etf.uniform("uPass"), 1); dispatch(etf, grad, oldField);
        checkFilteredSource(oldField, "ETF Follow image exact source");
        if (!flat) {
            style = StyleParams{}; style.enabled = style.brushModel = 1.f;
            style.layerB[0][2] = float(PI / 6); uploadStyle();
            dispatch(fixed, grad, newField);
            const auto rotated = read(newField);
            require(axialDistance(angle(rotated), angle(raw) + 30) < .04, "explicit layer rotation");
            style.layerB[0][2] = 0; style.perturb = .3f; uploadStyle();
            dispatch(fixed, grad, newField);
            const auto wobble = read(newField);
            bool magUnchanged = true;
            for (size_t i = 2; i < raw.size(); i += 4) magUnchanged &= raw[i] == wobble[i];
            require(magUnchanged && maxDifference(raw, wobble) > 0.f, "explicit wobble preserves magnitude");
        }
        // Exercise the structure tensor path directly, without a blur so the
        // analytic ramp direction is exact. Tensor xy is unit length while z
        // remains the source confidence; Follow image must preserve both.
        features.use(); glUniform1i(features.uniform("uPass"), 1);
        dispatch(features, src, grad);
        glBindTextureUnit(1, grad);
        features.use(); glUniform1i(features.uniform("uPass"), 2);
        dispatch(features, src, oldField);
        const auto tensor = read(oldField);
        style = StyleParams{}; style.enabled = style.brushModel = 1.f;
        uploadStyle(); dispatch(fixed, oldField, newField);
        require(std::all_of(tensor.begin(), tensor.end(), [](float v) { return std::isfinite(v); }),
                "tensor finite");
        require(maxDifference(tensor, read(newField)) == 0.f, "tensor Follow image exact source");
        if (!flat) require(axialDistance(angle(tensor), c.expected) < .1, "tensor analytic tangent");
        checkFilteredSource(oldField, "tensor Follow image exact source and patterns");
    }
    require(glGetError() == GL_NO_ERROR, "no OpenGL errors");
    std::printf("Failures: %d\n", failures);
    glfwDestroyWindow(window); glfwTerminate();
    return failures ? 1 : 0;
}
