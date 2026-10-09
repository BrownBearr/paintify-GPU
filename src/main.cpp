#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h>   // ClosePopupsExceptModals: Esc closes menus

#include "pipeline.h"
#include "params.h"
#include "brush_atlas.h"
#include "media.h"
#include "jobs.h"
#include "filedialog.h"
#include "paramfile.h"
#include "styles.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "app_resources.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace {

// ── Radius sets ───────────────────────────────────────────────────────────
// These choices only set the coarse-to-fine radii. All other controls stay
// editable and keep their current values.
struct Preset { const char* name; const char* radii; };

const Preset kPresets[] = {
    {"Balanced", "8,4,2"},
    {"Broad", "12,6,3"},
    {"Fine", "6,3,1.5"},
    {"Large wash", "20,10"},
    {"Tiny marks", "4,2"},
};
constexpr int kPresetCount = int(sizeof(kPresets) / sizeof(kPresets[0]));

// parseRadii / radiiToString live in paramfile.cpp: the GUI text box and a
// saved file have to agree on what "8, 4, 2" means, so there is one copy.
using paramfile::parseRadii;
using paramfile::radiiToString;

void applyPreset(const Preset& pr, TuningParams&, RenderConfig& cfg, std::string* radiiText) {
    cfg.radii = parseRadii(pr.radii);
    if (radiiText) *radiiText = radiiToString(cfg.radii);
}

struct Options {
    bool headless = false;
    std::string in;
    std::string out = "out.png";
    int dump = 0;
    bool debugCells = false;
    int passes = 0;
    std::string framesDir;
    std::string batchDir;
    std::string videoIn;
    std::string outDir = "frames_out";
    double fps = 0.0;          // 0 = take it from the input
    int crf = 21;
    std::string vcodec = "libx264";
    std::string vpreset = "slow";
    bool noAudio = false;
    std::string batchSuffix = "_painted";
    std::string batchFormat = "png";
    std::string preset;
    std::string paramsFile;
    std::string saveParamsFile;
    std::string radii;
    bool haveUnderpaint = false;
    Underpaint underpaint = Underpaint::Blur;
    // Overrides; NaN means "leave the preset's value alone".
    float threshold = NAN, curvature = NAN, opacity = NAN, gridFactor = NAN;
    float maxLen = NAN, minLen = NAN, tensorSigma = NAN;
    float brushTexture = NAN, bristleDensity = NAN, textureTaper = NAN;
    float impasto = NAN, impastoLight = NAN, lightAngle = NAN, dryBrush = NAN;
    float sizeJitter = NAN, angleJitter = NAN, opacityJitter = NAN;
    float temporalDiff = NAN;
    int relax = -1, relaxSubs = 0;
    int etf = -1;
    int flow = -1, flowIters = 0;
    float freshPaint = NAN;
    float etfRadius = NAN;
    float relaxArea = NAN, relaxMove = NAN, relaxCands = NAN, relaxRemove = NAN;
    bool relaxLog = false;
    bool etfLog = false;
    bool flowLog = false;
    bool jitterPerFrame = false;
    // brushkit
    std::string style;              // empty / "none" = classic tile brush
    float styleScale = NAN;         // NaN = width multiplier 1
    bool listStyles = false;
    bool play = false;              // interactive: start playing a --in video
};

void usage() {
    printf(
        "Brushkit -- GPU painting with editable historical brush textures\n\n"
        " Brush textures\n"
        "  --style <name>          choose an era brush texture (--list-styles);\n"
        "                          'none' selects the classic tile brush\n"
        "  --style-scale <f>       procedural brush width multiplier; default 1\n"

        "  --list-styles           list the brush textures and exit\n"
        "  --play                  with --in <video>: paint it in realtime in the\n"
        "                          window (also the Play button)\n"
        "  --in <path>            source image (omitted: synthetic subject)\n"
        "  --out <path>            output PNG for --headless and the S key\n"
        "  --headless              render and exit, no window\n"
        "  --preset <name>         radius sets: Balanced | Broad | Fine |\n"
        "                          Large wash | Tiny marks (legacy names accepted)\n"

        "  --save-params <file>    write the fully resolved parameters and carry\n"
        "                          on, so a preset plus a few flags becomes an\n"
        "                          editable file to tweak by hand\n"
        "  --params <file>         load a parameter file saved from the GUI.\n"
        "                          Applied after --preset, before the individual\n"
        "                          overrides below, so a flag still wins.\n"
        "  --radii \"8,4,2\"         brush radii, coarse to fine (max 8)\n"
        "  --threshold <f>         Lab error a cell must exceed to earn a stroke\n"
        "  --curvature <0..1>      Hertzmann's fc filter\n"
        "  --opacity <0..1>        per-stroke alpha\n"
        "  --grid-factor <f>       seed grid pitch, in radii\n"
        "  --max-len / --min-len   stroke length bounds, in steps\n"
        "  --tensor-sigma <f>      > 0 swaps in the structure-tensor flow field\n"
        "  --etf <n>               Edge Tangent Flow iterations over the flow\n"
        "                          field (Kang 2007). Edge-aware smoothing: weak\n"
        "                          pixels defer to strong neighbours and nothing\n"
        "                          is smoothed across a perpendicular flow, which\n"
        "                          a Gaussian cannot express. 0 (default) leaves\n"
        "                          the field exactly as the web version has it;\n"
        "                          2-3 converges.\n"
        "  --etf-radius <f>        ETF neighbourhood radius in px (default 5)\n"
        "  --brush-texture <0..1>  bristle texture strength\n"
        "  --bristle-density <f>   bristles across the stroke (default 10)\n"
        "  --texture-taper <0..1>  how much the stroke tips narrow\n"
        "  --impasto <f>           paint height accumulated per stroke\n"
        "  --impasto-light <f>     relief lighting strength\n"
        "  --light-angle <deg>     relief light direction\n"
        "  --dry-brush <0..1>      opacity falloff along the stroke\n"
        "  --size/--angle/--opacity-jitter <f>   per-stroke variation\n"
        "  --underpaint <mode>     blur | none | average\n"
        "\n Video and batch\n"
        "  --video <in> --out <out.mp4>    paint a video; frames stream through\n"
        "                          ffmpeg as raw RGBA, nothing hits the disk.\n"
        "                          Audio is copied from the input when present.\n"
        "  --batch <dir> --outdir <dir>    paint every image in a directory\n"
        "  --frames <dir> --outdir <dir>   image sequence, temporal coherence on\n"
        "  --fps <f>               output frame rate (default: the input's)\n"
        "  --crf <n>               x264 quality, lower is better (default 21)\n"
        "  --vcodec / --vpreset    encoder and speed preset (libx264, slow)\n"
        "  --no-audio              drop the input's audio track\n"
        "  --suffix <s>            batch output suffix (default _painted)\n"
        "  --format <ext>          batch output format: png | jpg | bmp | tga\n"
        "  --temporal-diff <f>     per-cell source change needed to repaint\n"
        "                          (0..255). 0 paints every frame from scratch,\n"
        "                          which makes strokes shimmer; 12 is a good\n"
        "                          start for video.\n"
        "  --flow <n>              pyramidal Lucas-Kanade levels for temporal\n"
        "                          advection. Carries the painting along the\n"
        "                          motion instead of leaving it pinned to the\n"
        "                          pixel grid, which is what --temporal-diff\n"
        "                          alone cannot fix on a moving camera. 0 (the\n"
        "                          default) is the web version's behaviour; 4\n"
        "                          tracks roughly 32 px/frame. Needs\n"
        "                          --temporal-diff > 0 to do anything.\n"
        "  --flow-iters <n>        LK refinements per level (default 3)\n"
        "  --fresh-paint <f>       0..1 anti-smear: repaint moving areas each\n"
        "                          frame instead of warping old paint (default 0)\n"
        "  --passes <n>            painting passes per layer (default 8). The web\n"
        "                          version paints one stroke at a time and each\n"
        "                          stroke sees the last one's paint, which is what\n"
        "                          sets stroke length; more passes approximate it\n"
        "                          more closely. 8 lands within 1%%.\n"
        "  --debug-cells           print each layer's error distribution\n"
        "  --dump <n>              print the first n traced polylines\n"
        "\n"
        " Relaxation (Hertzmann 2001, \"Paint By Relaxation\"). After the greedy\n"
        " pass places the strokes, iteratively improve them against\n"
        "   E = sum|Lab(canvas)-Lab(source)| + area-weight * sum(stroke area)\n"
        "  --relax <n>             iterations; 0 (default) = plain Hertzmann 1998\n"
        "  --relax-area <f>        w_area: higher = fewer, larger, bolder strokes\n"
        "  --relax-move <f>        perturbation amplitude, in stroke radii\n"
        "  --relax-candidates <n>  trial moves per stroke per iteration\n"
        "  --relax-remove <f>      > 0 deletes strokes that do not pay for their\n"
        "                          area; 1.0 is break-even\n"
        "  --relax-subpasses <n>   hash-scattered subsets per iteration\n"
        "  --relax-log             print the global energy after each iteration\n");
}

Options parseArgs(int argc, char** argv) {
    Options o;
    auto next = [&](int& i) -> const char* {
        if (i + 1 >= argc || argv[i + 1][0] == '\0' ||
            (argv[i + 1][0] == '-' && argv[i + 1][1] == '-')) {
            fprintf(stderr, "missing value for %s\n", argv[i]);
            std::exit(2);
        }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if      (a == "--headless")        o.headless = true;
        else if (a == "--in")              o.in = next(i);
        else if (a == "--out")             o.out = next(i);
        else if (a == "--dump")            o.dump = atoi(next(i));
        else if (a == "--debug-cells")     o.debugCells = true;
        else if (a == "--passes")          o.passes = atoi(next(i));
        else if (a == "--relax")           o.relax = atoi(next(i));
        else if (a == "--etf")             o.etf = atoi(next(i));
        else if (a == "--flow")            o.flow = atoi(next(i));
        else if (a == "--flow-iters")      o.flowIters = atoi(next(i));
        else if (a == "--etf-radius")      o.etfRadius = std::strtof(next(i), nullptr);
        else if (a == "--relax-subpasses") o.relaxSubs = atoi(next(i));
        else if (a == "--relax-area")      o.relaxArea = std::strtof(next(i), nullptr);
        else if (a == "--relax-move")      o.relaxMove = std::strtof(next(i), nullptr);
        else if (a == "--relax-candidates") o.relaxCands = std::strtof(next(i), nullptr);
        else if (a == "--relax-remove")    o.relaxRemove = std::strtof(next(i), nullptr);
        else if (a == "--relax-log")       o.relaxLog = true;
        else if (a == "--etf-log")         o.etfLog = true;
        else if (a == "--flow-log")        o.flowLog = true;
        else if (a == "--jitter-per-frame") o.jitterPerFrame = true;
        else if (a == "--style")           o.style = next(i);
        else if (a == "--style-scale")     o.styleScale = std::strtof(next(i), nullptr);
        else if (a == "--list-styles")     o.listStyles = true;
        else if (a == "--play")            o.play = true;
        else if (a == "--frames")        { o.framesDir = next(i); o.headless = true; }
        else if (a == "--video")         { o.videoIn = next(i); o.headless = true; }
        else if (a == "--batch")         { o.batchDir = next(i); o.headless = true; }
        else if (a == "--fps")             o.fps = atof(next(i));
        else if (a == "--crf")             o.crf = atoi(next(i));
        else if (a == "--vcodec")          o.vcodec = next(i);
        else if (a == "--vpreset")         o.vpreset = next(i);
        else if (a == "--no-audio")        o.noAudio = true;
        else if (a == "--suffix")          o.batchSuffix = next(i);
        else if (a == "--format")          o.batchFormat = next(i);
        else if (a == "--outdir")          o.outDir = next(i);
        else if (a == "--preset")          o.preset = next(i);
        else if (a == "--params")          o.paramsFile = next(i);
        else if (a == "--save-params")     o.saveParamsFile = next(i);
        else if (a == "--radii")           o.radii = next(i);
        else if (a == "--threshold")       o.threshold = std::strtof(next(i), nullptr);
        else if (a == "--curvature")       o.curvature = std::strtof(next(i), nullptr);
        else if (a == "--opacity")         o.opacity = std::strtof(next(i), nullptr);
        else if (a == "--grid-factor")     o.gridFactor = std::strtof(next(i), nullptr);
        else if (a == "--max-len")         o.maxLen = std::strtof(next(i), nullptr);
        else if (a == "--min-len")         o.minLen = std::strtof(next(i), nullptr);
        else if (a == "--tensor-sigma")    o.tensorSigma = std::strtof(next(i), nullptr);
        else if (a == "--brush-texture")   o.brushTexture = std::strtof(next(i), nullptr);
        else if (a == "--bristle-density") o.bristleDensity = std::strtof(next(i), nullptr);
        else if (a == "--texture-taper")   o.textureTaper = std::strtof(next(i), nullptr);
        else if (a == "--impasto")         o.impasto = std::strtof(next(i), nullptr);
        else if (a == "--impasto-light")   o.impastoLight = std::strtof(next(i), nullptr);
        else if (a == "--light-angle")     o.lightAngle = std::strtof(next(i), nullptr);
        else if (a == "--dry-brush")       o.dryBrush = std::strtof(next(i), nullptr);
        else if (a == "--size-jitter")     o.sizeJitter = std::strtof(next(i), nullptr);
        else if (a == "--angle-jitter")    o.angleJitter = std::strtof(next(i), nullptr);
        else if (a == "--opacity-jitter")  o.opacityJitter = std::strtof(next(i), nullptr);
        else if (a == "--temporal-diff")   o.temporalDiff = std::strtof(next(i), nullptr);
        else if (a == "--fresh-paint")     o.freshPaint = std::strtof(next(i), nullptr);
        else if (a == "--underpaint") {
            const std::string m = next(i);
            o.haveUnderpaint = true;
            o.underpaint = (m == "none") ? Underpaint::None
                         : (m == "average") ? Underpaint::Average : Underpaint::Blur;
        }
        else if (a == "--no-underpaint") { o.haveUnderpaint = true; o.underpaint = Underpaint::None; }
        else if (a == "--help" || a == "-h") { usage(); exit(0); }
        else {
            fprintf(stderr, "unknown option %s (try --help)\n", a.c_str());
            std::exit(2);
        }
    }
    return o;
}

void applyOverrides(const Options& o, TuningParams& p, RenderConfig& cfg,
                    std::string* radiiText) {
    auto set = [](float& dst, float v) { if (!std::isnan(v)) dst = v; };
    if (!o.radii.empty()) {
        cfg.radii = parseRadii(o.radii);
        if (radiiText) *radiiText = radiiToString(cfg.radii);
    }
    if (o.haveUnderpaint) cfg.underpaint = o.underpaint;
    if (o.passes > 0) cfg.passesPerLayer = o.passes;
    set(p.threshold, o.threshold);
    set(p.curvature, o.curvature);
    set(p.opacity, o.opacity);
    set(p.gridFactor, o.gridFactor);
    set(p.maxStrokeLength, o.maxLen);
    set(p.minStrokeLength, o.minLen);
    set(p.tensorSigma, o.tensorSigma);
    set(p.texStrength, o.brushTexture);
    set(cfg.bristleDensity, o.bristleDensity);
    set(p.texTaper, o.textureTaper);
    set(p.impastoStrength, o.impasto);
    set(p.impastoLight, o.impastoLight);
    set(p.lightAngle, o.lightAngle);
    set(p.dryBrush, o.dryBrush);
    set(p.sizeJitter, o.sizeJitter);
    set(p.angleJitter, o.angleJitter);
    set(p.opacityJitter, o.opacityJitter);
    set(p.frameDiffThreshold, o.temporalDiff);
    if (o.etf >= 0) cfg.etfIterations = o.etf;
    if (o.flow >= 0) cfg.flowLevels = o.flow;
    if (o.flowIters > 0) cfg.flowIterations = o.flowIters;
    if (!std::isnan(o.freshPaint)) cfg.freshPaint = std::clamp(o.freshPaint, 0.f, 1.f);
    set(p.etfRadius, o.etfRadius);
    if (o.relax >= 0) cfg.relaxIterations = o.relax;
    if (o.relaxSubs > 0) cfg.relaxSubPasses = o.relaxSubs;
    set(p.relaxAreaWeight, o.relaxArea);
    set(p.relaxMoveScale, o.relaxMove);
    set(p.relaxCandidates, o.relaxCands);
    set(p.relaxRemove, o.relaxRemove);
    p.jitterPerFrame = o.jitterPerFrame ? 1.f : 0.f;
}

// Fallback subject when no image is supplied: smooth colour ramps plus a few
// hard edges, which exercises both the flow field and the stroke termination.
std::vector<unsigned char> syntheticImage(int w, int h) {
    std::vector<unsigned char> px(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float u = float(x) / w, v = float(y) / h;
            const float swirl = std::sin((u * 6.0f + v * 3.0f) * 3.14159f)
                              * std::cos((v * 5.0f - u * 2.0f) * 3.14159f);
            float r = 0.5f + 0.45f * swirl;
            float g = 0.45f + 0.4f * std::sin(u * 9.0f + swirl * 2.0f);
            float b = 0.55f + 0.4f * std::cos(v * 7.0f - swirl * 2.0f);

            const float dx = u - 0.35f, dy = v - 0.45f;
            if (dx * dx + dy * dy < 0.02f) { r = 0.95f; g = 0.35f; b = 0.2f; }
            if (std::fabs(u - 0.72f) < 0.06f && v > 0.25f && v < 0.8f) {
                r = 0.1f; g = 0.15f; b = 0.3f;
            }

            const size_t i = (size_t(y) * w + x) * 4;
            px[i + 0] = (unsigned char)(r * 255.f);
            px[i + 1] = (unsigned char)(g * 255.f);
            px[i + 2] = (unsigned char)(b * 255.f);
            px[i + 3] = 255;
        }
    }
    return px;
}

bool loadSource(Pipeline& pipe, const std::string& path, std::string* note) {
    if (!path.empty()) {
        int w = 0, h = 0, n = 0;
        unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 4);
        if (px) {
            const bool ok = pipe.setSource(px, w, h);
            stbi_image_free(px);
            *note = path + "  " + std::to_string(w) + "x" + std::to_string(h);
            return ok;
        }
        fprintf(stderr, "could not read %s (%s)\n",
                path.c_str(), stbi_failure_reason());
        return false;
    }
    const int w = 1920, h = 1080;
    const std::vector<unsigned char> px = syntheticImage(w, h);
    *note = "synthetic 1920x1080";
    return pipe.setSource(px.data(), w, h);
}

bool writePng(const std::string& path, const std::vector<unsigned char>& px, int w, int h) {
    // Canvas texel row 0 is image row 0 (stroke.vert does not flip y), and
    // readCanvas hands back texel rows in order, so the buffer is already
    // top-row-first and must not be flipped again.
    if (!stbi_write_png(path.c_str(), w, h, 4, px.data(), w * 4)) {
        fprintf(stderr, "failed to write %s\n", path.c_str());
        return false;
    }
    printf("wrote %s (%dx%d)\n", path.c_str(), w, h);
    return true;
}

// ── GUI state ──────────────────────────────────────────────────────────────

enum class InputKind { None, Image, Video, Images };

// Everything the window needs to know about what is loaded and where it is
// going. Kept in one struct so the drop handler, the picker buttons and the
// export button all agree.
struct Gui {
    InputKind kind = InputKind::None;
    std::string inputPath;                 // image or video
    std::vector<std::string> queue;        // Images: the files to paint
    media::VideoInfo video;                // valid when kind == Video
    double previewAt = 0.0;                // preview position, seconds
    double previewRequested = -1.0;        // set on slider release, -1 = idle
    int previewDelay = 0;                  // frames since the release

    std::string outDir;
    std::string videoOut;
    std::string format, suffix;
    double fpsOverride = 0.0;
    int crf = 21;
    bool keepAudio = true;

    // Export in flight
    bool startExport = false;
    bool exporting = false;
    bool cancel = false;
    jobs::Progress progress;
    std::string status;
    bool statusIsError = false;
};

std::string fileName(const std::string& p) {
    return std::filesystem::path(p).filename().string();
}

// Loads one frame of a video as the preview, so the sliders act on real
// footage rather than on a still the user has to imagine.
bool loadVideoPreview(Pipeline& pipe, Gui& gui, std::string* note) {
    std::vector<unsigned char> frame;
    if (!media::readFrameAt(gui.inputPath, gui.previewAt,
                            gui.video.width, gui.video.height, frame))
        return false;
    pipe.setSource(frame.data(), gui.video.width, gui.video.height);
    pipe.resetTemporal();

    char buf[512];
    snprintf(buf, sizeof(buf), "%s  %dx%d  %.2f fps%s",
             fileName(gui.inputPath).c_str(), gui.video.width, gui.video.height,
             gui.video.fps, gui.video.hasAudio ? "  +audio" : "");
    *note = buf;
    return true;
}

// One dropped or picked path, or several. A folder means batch, a video means
// video, anything else is tried as a still.
void acceptPaths(const std::vector<std::string>& paths, Pipeline& pipe,
                 Gui& gui, std::string& note) {
    if (paths.empty()) return;
    namespace fs = std::filesystem;
    std::error_code ec;

    // Several files at once, or a folder, is a batch.
    if (paths.size() > 1 || fs::is_directory(paths[0], ec)) {
        std::vector<std::string> files;
        for (const std::string& p : paths) {
            if (fs::is_directory(p, ec)) {
                const std::vector<std::string> inDir = media::listImages(p);
                files.insert(files.end(), inDir.begin(), inDir.end());
                if (gui.outDir.empty() || gui.outDir == "frames_out")
                    gui.outDir = (fs::path(p) / "painted").string();
            } else if (media::isImageExtension(p)) {
                files.push_back(p);
            }
        }
        if (files.empty()) {
            gui.status = "No images found in " + fileName(paths[0]);
            gui.statusIsError = true;
            return;
        }
        // Validate the preview before replacing the current input.
        if (!loadSource(pipe, files.front(), &note)) {
            gui.status = "Could not open " + files.front();
            gui.statusIsError = true;
            return;
        }
        gui.kind = InputKind::Images;
        gui.queue = files;
        gui.inputPath.clear();
        pipe.resetTemporal();
        gui.status = std::to_string(files.size()) + " images queued for export";
        gui.statusIsError = false;
        return;
    }

    const std::string& p = paths[0];

    if (media::isVideoExtension(p)) {
        std::string err;
        media::VideoInfo info;
        // A missing ffprobe used to surface as "could not read <file>".
        std::string missing;
        if (!media::haveFfmpeg(&missing)) {
            gui.status = "Video needs ffmpeg and ffprobe on PATH (" + missing + " not found)";
            gui.statusIsError = true;
            return;
        }
        if (!media::probe(p, &info, &err)) {
            gui.status = err;
            gui.statusIsError = true;
            return;
        }
        gui.kind = InputKind::Video;
        gui.inputPath = p;
        gui.video = info;
        gui.previewAt = 0.0;
        gui.queue.clear();
        if (gui.videoOut.empty() || gui.videoOut == "painted.mp4") {
            gui.videoOut = (fs::path(p).parent_path() /
                            (fs::path(p).stem().string() + "_painted.mp4")).string();
        }
        if (!loadVideoPreview(pipe, gui, &note)) {
            gui.status = "ffmpeg could not decode a frame from " + fileName(p);
            gui.statusIsError = true;
            gui.kind = InputKind::None;
            return;
        }
        {
            char b[160];
            const double dur = (info.fps > 0.0 && info.frames > 0)
                             ? double(info.frames) / info.fps : 0.0;
            snprintf(b, sizeof(b), "Video loaded: %lld frames, %.1f s", (long long)info.frames, dur);
            gui.status = b;
        }
        gui.statusIsError = false;
        return;
    }

    if (!loadSource(pipe, p, &note)) {
        gui.status = "Could not open " + p;
        gui.statusIsError = true;
        return;
    }
    gui.kind = InputKind::Image;
    gui.inputPath = p;
    gui.queue.clear();
    pipe.resetTemporal();
    if (gui.outDir.empty() || gui.outDir == "frames_out")
        gui.outDir = fs::path(p).parent_path().string();
    gui.status.clear();
    gui.statusIsError = false;
}

// ── Framing ────────────────────────────────────────────────────────────────

// The interactive part of ViewXform: what is being dragged, and by how much.
struct View {
    ViewXform xf;
    bool panning = false;
    bool dragDivider = false;
};

// Wheel zooms about the cursor, left-drag pans, and in split view a drag near
// the divider moves it instead.
//
// This runs *inside* the ImGui frame, after NewFrame, for two reasons:
// io.WantCaptureMouse is then this frame's answer rather than the previous
// frame's, and io.MouseWheel has been resolved from the event queue. The blit
// therefore has to happen after the panel is built -- see the main loop.
void updateView(View& v, int fbH, int vx, int vy, int vw, int vh,
                int imgW, int imgH, ViewMode mode) {
    ImGuiIO& io = ImGui::GetIO();
    if (imgW <= 0 || imgH <= 0 || vw <= 0 || vh <= 0) return;

    // io.MousePos is in logical window units with a top-left origin; the blit
    // works in framebuffer pixels with a bottom-left one.
    const float sx = io.DisplayFramebufferScale.x;
    const float sy = io.DisplayFramebufferScale.y;
    const float mx = io.MousePos.x * sx;
    const float my = float(fbH) - io.MousePos.y * sy;
    const bool over = !io.WantCaptureMouse && mx >= float(vx) &&
                      mx < float(vx + vw) && my >= float(vy) && my < float(vy + vh);

    const float fit = std::min(float(vw) / float(imgW), float(vh) / float(imgH));
    const float scale = fit * std::max(v.xf.zoom, 0.01f);
    const float fx = v.xf.focusX < 0.f ? float(imgW) * 0.5f : v.xf.focusX;
    const float fy = v.xf.focusY < 0.f ? float(imgH) * 0.5f : v.xf.focusY;
    const float cx = float(vx) + float(vw) * 0.5f;
    const float cy = float(vy) + float(vh) * 0.5f;

    // The divider gets first refusal on a press: once the image fills the
    // viewport a pan would otherwise make it impossible to grab.
    const float cut = float(vx) + float(vw) * v.xf.wipe;
    const bool onDivider = mode == ViewMode::Split && std::fabs(mx - cut) < 8.f * sx;
    if (over && onDivider) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && over) {
        if (onDivider) v.dragDivider = true;
        else           v.panning = true;
    }
    // Double-click fits, like the F key.
    if (over && !onDivider && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        v.xf.zoom = 1.f;
        v.xf.focusX = v.xf.focusY = -1.f;
    }
    if (v.panning && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.f))
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        v.panning = false;
        v.dragDivider = false;
    }

    if (v.dragDivider) {
        v.xf.wipe = std::clamp((mx - float(vx)) / float(vw), 0.f, 1.f);
    } else if (v.panning) {
        // One screen pixel of drag moves the image one screen pixel, whatever
        // the zoom -- hence dividing the delta by the scale to get image px.
        v.xf.focusX = fx - io.MouseDelta.x * sx / scale;
        v.xf.focusY = fy - io.MouseDelta.y * sy / scale;
    }

    if (over && io.MouseWheel != 0.f) {
        // Pin the image point under the cursor, so the wheel magnifies what is
        // being looked at rather than the middle of the window.
        const float ix = fx + (mx - cx) / scale;
        const float iy = fy - (my - cy) / scale;
        const float z = std::clamp(v.xf.zoom * std::pow(1.15f, io.MouseWheel),
                                   0.25f, 64.f);
        const float s2 = fit * z;
        v.xf.zoom = z;
        v.xf.focusX = ix - (mx - cx) / s2;
        v.xf.focusY = iy + (my - cy) / s2;
    }

    if (!io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
            v.xf.zoom = 1.f;
            v.xf.focusX = v.xf.focusY = -1.f;   // back to the image centre
        }
        // 1:1 means one canvas texel per screen pixel, which is the only
        // framing at which brush texture can be judged at all.
        if (ImGui::IsKeyPressed(ImGuiKey_1, false) && fit > 0.f) v.xf.zoom = 1.f / fit;
    }

    // Never let the image be panned entirely out of sight.
    v.xf.focusX = std::clamp(v.xf.focusX < 0.f ? float(imgW) * 0.5f : v.xf.focusX,
                             0.f, float(imgW));
    v.xf.focusY = std::clamp(v.xf.focusY < 0.f ? float(imgH) * 0.5f : v.xf.focusY,
                             0.f, float(imgH));
}

void printTimings(const Pipeline& pipe, float relaxAreaWeight) {
    // Seeds vs drawn: a stroke that traced to a single point draws nothing, and
    // the gradient-magnitude floor stops the walk at its first step across flat
    // regions. `drawn` is the figure comparable to worker.js's
    // renderStrokeSolid calls -- seeds outnumber it heavily.
    printf("seeds: %u  drawn: %u  mean points/stroke: %.2f\n",
           pipe.lastStrokeCount(), pipe.lastDrawnCount(), pipe.lastMeanPoints());
    printf("reference %.2f | error %.2f | seeds %.2f | trace %.2f | raster %.2f"
           " | relax %.2f | impasto %.2f | total %.2f ms\n",
           pipe.msReference(), pipe.msError(), pipe.msSeeds(), pipe.msTrace(),
           pipe.msRaster(), pipe.msRelax(), pipe.msImpasto(), pipe.msTotal());
    if (pipe.lastPoolCount() > 0) {
        printf("relaxation: pool %u  moves kept %u  removed %u\n",
               pipe.lastPoolCount(), pipe.lastRelaxAccepted(),
               pipe.lastRelaxRemoved());
        const double coverage =
            pipe.relaxTotalArea() / (double(pipe.width()) * pipe.height());
        if (pipe.relaxLiveStrokes() > 0)
            printf("  geometry: %u live strokes, %.2f points, mean radius %.2f px,"
                   " coverage %.2fx\n",
                   pipe.relaxLiveStrokes(), pipe.relaxMeanPoints(),
                   pipe.relaxMeanRadius(), coverage);

        const std::vector<double>& log = pipe.relaxEnergyLog();
        if (!log.empty()) {
            // E_app alone falls monotonically only at w_area = 0. With a
            // positive area weight relaxation is deliberately spending
            // appearance to buy economy, so E_app can rise while the real
            // objective E_app + w_area * sum(area) still falls -- so print
            // both, and the objective's area term alongside.
            printf("  E_app (mean Lab error/px):");
            for (double e : log) printf(" %.3f", e);
            printf("\n  E_area term: w %.2f x coverage %.2f = %.3f"
                   "   ->  objective %.3f\n",
                   relaxAreaWeight, coverage, relaxAreaWeight * coverage,
                   log.back() + relaxAreaWeight * coverage);
        }
    }
    // Coherence, per layer, before and after ETF. Printed separately from the
    // stroke counts because it is a property of the *field*, not of the
    // painting -- and the field is what ETF changes. A picture that merely
    // looks different is not evidence that the filter works.
    const std::vector<std::pair<double, double>>& elog = pipe.etfLog();
    if (!elog.empty()) {
        printf("flow coherence (mean |t.t| over 8 neighbours; 0.64 = random):\n");
        for (size_t i = 0; i < elog.size(); ++i) {
            if (elog[i].second < 0.0)
                printf("  layer %zu: %.4f  (ETF off)\n", i, elog[i].first);
            else
                printf("  layer %zu: %.4f -> %.4f  (%+.1f%%)\n", i, elog[i].first,
                       elog[i].second,
                       100.0 * (elog[i].second - elog[i].first) /
                           std::max(elog[i].first, 1e-9));
        }
    }
    for (const LayerStats& ls : pipe.layerStats())
        printf("  r %.1f px  grid %.0f  cells %u  passes %u  seeds %u  drawn %u\n",
               ls.radius, ls.grid, ls.cells, ls.chunks, ls.strokes, ls.drawn);
}

} // namespace

int main(int argc, char** argv) {
    const Options opt = parseArgs(argc, argv);

    if (opt.listStyles) {
        for (const styles::Info& s : styles::list())
            printf("%-14s %-24s %-24s %s\n", s.key, s.name, s.era, s.years);
        return 0;
    }
    if (!glfwInit()) { fprintf(stderr, "glfwInit failed\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
    if (opt.headless) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow* win = glfwCreateWindow(1600, 900, "Brushkit", nullptr, nullptr);
    if (!win) {
        fprintf(stderr, "need an OpenGL 4.6 core context\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(win);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        fprintf(stderr, "glad failed\n");
        return 1;
    }
    glfwSwapInterval(0);   // vsync off: the overlay is meant to show real cost
    printf("GL %s | %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));

    Pipeline pipe;
    if (!pipe.init()) return 1;
    pipe.setDebugCells(opt.debugCells);
    pipe.setRelaxLogging(opt.relaxLog);
    pipe.setEtfLogging(opt.etfLog);
    pipe.setFlowLogging(opt.flowLog);

    std::string sourceNote;
    // Interactive video is decoded by acceptPaths below, not by stb_image.
    const bool guiVideoInput = !opt.headless && !opt.in.empty() &&
                               media::isVideoExtension(opt.in);
    if (!loadSource(pipe, guiVideoInput ? "" : opt.in, &sourceNote)) {
        fprintf(stderr, "no source image\n");
        return 1;
    }

    TuningParams params;
    RenderConfig cfg;
    std::string radiiText = radiiToString(cfg.radii);

    int presetIdx = 0;
    applyPreset(kPresets[0], params, cfg, &radiiText);
    if (!opt.preset.empty()) {
        bool found = false;
        for (int i = 0; i < kPresetCount; ++i) {
            if (opt.preset == kPresets[i].name ||
                (i == 0 && opt.preset == "impressionist") ||
                (i == 1 && opt.preset == "expressionist") ||
                (i == 2 && opt.preset == "detail") ||
                (i == 3 && opt.preset == "wash") ||
                (i == 4 && opt.preset == "pointillist")) {
                presetIdx = i;
                applyPreset(kPresets[i], params, cfg, &radiiText);
                found = true;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "unknown preset '%s'\n", opt.preset.c_str());
            return 2;
        }
    }
    // A saved file sits between the preset and the individual flags: it is a
    // whole look, so it should replace the preset, but an explicit --threshold
    // on the same command line is clearly meant to win over both.
    StyleParams styleP;
    std::string fileStyleKey;
    bool fileStyle = false;
    if (!opt.paramsFile.empty()) {
        std::string perr;
        if (!paramfile::load(opt.paramsFile, &params, &cfg, &perr, &styleP, &fileStyleKey, &fileStyle)) {
            fprintf(stderr, "%s\n", perr.c_str());
            return 1;
        } else {
            if (!perr.empty()) fprintf(stderr, "%s: %s\n", opt.paramsFile.c_str(),
                                       perr.c_str());
            radiiText = radiiToString(cfg.radii);
        }
    }

    // ── Brush texture ─────────────────────────────────────────────────────
    // A named era changes the stroke mark only. Layout and paint controls
    // retain their values; saved legacy looks keep their full settings.
    int styleIdx = 0;
    const bool keepFileStyle = fileStyle && opt.style.empty();
    if (!opt.style.empty()) {
        styleIdx = styles::indexOf(opt.style);
        if (styleIdx < 0) {
            fprintf(stderr, "unknown style '%s' (try --list-styles)\n", opt.style.c_str());
            return 2;
        }
    } else if (keepFileStyle) {
        styleIdx = std::max(0, styles::indexOf(fileStyleKey));
    }
    auto applyStyleFor = [&](TuningParams& tp, RenderConfig& rc) {
        if (keepFileStyle) {
            // as saved
        } else if (styleIdx > 0) {
            const float sc = std::isnan(opt.styleScale) ? 1.f : opt.styleScale;
            styles::applyBrushTexture(styleIdx, styleP);
            styleP.widthScale *= sc;
        } else {
            styles::applyBrushTexture(0, styleP);
        }
        applyOverrides(opt, tp, rc, &radiiText);
        pipe.setStyle(styleP);
    };
    // Radius sets remain independent from the historical brush texture.
    applyStyleFor(params, cfg);

    if (!opt.saveParamsFile.empty()) {
        std::string perr;
        if (!paramfile::save(opt.saveParamsFile, params, cfg, &perr, &styleP,
                             styles::list()[size_t(styleIdx)].key)) {
            fprintf(stderr, "%s\n", perr.c_str());
            return 1;
        } else {
            printf("wrote %s\n", opt.saveParamsFile.c_str());
        }
    }

    // ------------------------------------------------------------------
    // Video in, video out. Frames stream through ffmpeg as raw RGBA, so
    // nothing hits the disk in between and the GPU is the only real cost.
    // ------------------------------------------------------------------
    if (!opt.videoIn.empty()) {
        media::VideoInfo info;
        std::string err;
        if (!media::haveFfmpeg(&err)) {
            fprintf(stderr, "%s is not on PATH; --video needs ffmpeg and ffprobe.\n"
                            "Install ffmpeg, or decode to PNGs yourself and use"
                            " --frames.\n", err.c_str());
            return 1;
        }
        if (!media::probe(opt.videoIn, &info, &err)) {
            fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }

        jobs::VideoSpec spec;
        spec.input = opt.videoIn;
        spec.output = opt.out;
        // A default of out.png would silently produce an unplayable file.
        if (spec.output.size() > 4 &&
            spec.output.compare(spec.output.size() - 4, 4, ".png") == 0)
            spec.output = "out.mp4";
        spec.fps = opt.fps;
        spec.crf = opt.crf;
        spec.codec = opt.vcodec;
        spec.preset = opt.vpreset;
        spec.keepAudio = !opt.noAudio;

        printf("input : %s  %dx%d  %.3f fps  %s%s\n", opt.videoIn.c_str(),
               info.width, info.height, info.fps, info.codec.c_str(),
               info.hasAudio ? " +audio" : "");
        if (info.frames > 0) printf("        %lld frames\n", (long long)info.frames);
        printf("output: %s  %s crf %d%s\n", spec.output.c_str(),
               spec.codec.c_str(), spec.crf,
               (spec.keepAudio && info.hasAudio) ? "  (audio copied)" : "");
        if (params.frameDiffThreshold <= 0.f)
            printf("note  : --temporal-diff is 0, so every frame is painted"
                   " independently and strokes will shimmer. Try 12.\n");

        const jobs::Result res = jobs::runVideo(
            pipe, params, cfg, spec, [&](const jobs::Progress& p) {
                if (p.total > 0)
                    printf("\r  %lld/%lld  %.0f%%  %.1f ms/frame  drawn %u   ",
                           (long long)p.done, (long long)p.total,
                           100.0 * double(p.done) / double(p.total),
                           p.msPerFrame, p.drawn);
                else
                    printf("\r  %lld frames  %.1f ms/frame   ",
                           (long long)p.done, p.msPerFrame);
                fflush(stdout);
                return true;
            });

        printf("\n%lld frames in %.1f s wall (%.1f fps), GPU %.2f ms/frame\n",
               (long long)res.done, res.wallSeconds,
               res.done / std::max(res.wallSeconds, 1e-9), res.gpuMsMean);
        if (!res.ok) fprintf(stderr, "%s\n", res.error.c_str());
        else         printf("wrote %s\n", spec.output.c_str());

        pipe.shutdown();
        glfwDestroyWindow(win);
        glfwTerminate();
        return res.ok ? 0 : 1;
    }

    // ------------------------------------------------------------------
    // Batch images. Each file is independent -- no temporal carry-over,
    // and the size is allowed to change between files.
    // ------------------------------------------------------------------
    if (!opt.batchDir.empty()) {
        jobs::BatchSpec spec;
        spec.dir = opt.batchDir;
        spec.outDir = opt.outDir;
        spec.suffix = opt.batchSuffix;
        spec.format = opt.batchFormat;

        const jobs::Result res = jobs::runBatch(
            pipe, params, cfg, spec, [&](const jobs::Progress& p) {
                printf("  %lld/%lld  %dx%d  %.1f ms  drawn %u  -> %s\n",
                       (long long)p.done, (long long)p.total, p.width, p.height,
                       p.msPerFrame, p.drawn, p.item.c_str());
                return true;
            });

        if (!res.ok && !res.cancelled) {
            fprintf(stderr, "%s\n", res.error.c_str());
        } else {
            printf("%lld painted, %lld skipped, %.1f s wall, GPU %.2f ms/image\n",
                   (long long)res.done, (long long)res.skipped, res.wallSeconds,
                   res.gpuMsMean);
        }
        pipe.shutdown();
        glfwDestroyWindow(win);
        glfwTerminate();
        return res.ok ? 0 : 1;
    }

    // ------------------------------------------------------------------
    // Frame sequence: image directory in, image directory out. Kept for
    // when ffmpeg is unavailable or the frames are already extracted.
    // ------------------------------------------------------------------
    if (!opt.framesDir.empty()) {
        jobs::FramesSpec spec;
        spec.dir = opt.framesDir;
        spec.outDir = opt.outDir;

        if (std::isnan(opt.temporalDiff) && params.frameDiffThreshold <= 0.f)
            params.frameDiffThreshold = 12.f;
        const jobs::Result res = jobs::runFrames(
            pipe, params, cfg, spec, [&](const jobs::Progress& p) {
                printf("  %lld/%lld  drawn %u  gpu %.2f ms\n",
                       (long long)p.done, (long long)p.total, p.drawn, p.msPerFrame);
                return true;
            });

        if (!res.ok) fprintf(stderr, "%s\n", res.error.c_str());
        else printf("mean GPU cost %.2f ms/frame over %lld frames\n",
                    res.gpuMsMean, (long long)res.done);
        pipe.shutdown();
        glfwDestroyWindow(win);
        glfwTerminate();
        return res.ok ? 0 : 1;
    }

    // ------------------------------------------------------------------
    // Headless: render, read back, write, report. Three frames so the GPU
    // timers (which read one frame late) have real numbers to report.
    // ------------------------------------------------------------------
    if (opt.headless) {
        for (int i = 0; i < 3; ++i) {
            pipe.render(params, cfg);
            params.frame += 1.f;
        }
        glFinish();
        pipe.refreshStats();
        if (opt.dump > 0) pipe.dumpStrokes(opt.dump);
        const bool wrote = writePng(opt.out, pipe.readCanvas(), pipe.width(), pipe.height());
        printf("source: %s\n", sourceNote.c_str());
        printTimings(pipe, params.relaxAreaWeight);
        pipe.shutdown();
        glfwDestroyWindow(win);
        glfwTerminate();
        return wrote ? 0 : 1;
    }

    // ------------------------------------------------------------------
    // Interactive
    // ------------------------------------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;   // fixed layout: nothing to remember
    // Content scale (1.25 at 120 dpi...). Fonts are rasterised at this size
    // rather than scaled afterwards, which would blur them.
    float uiScale = 1.f;
    {
        float csx = 1.f, csy = 1.f;
        glfwGetWindowContentScale(win, &csx, &csy);
        uiScale = std::clamp(csx, 1.f, 4.f);
        // For checking layouts at other scales on a 96 dpi screen.
        if (const char* e = std::getenv("BRUSHKIT_UI_SCALE")) {
            const float f = std::strtof(e, nullptr);
            if (f >= 0.75f && f <= 4.f) uiScale = f;
        }
    }
    ui::applyTheme(uiScale);
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 460");
    // Below this the top bar's two clusters would collide.
    glfwSetWindowSizeLimits(win, int(1024 * uiScale), int(600 * uiScale), GLFW_DONT_CARE,
                            GLFW_DONT_CARE);
    printf("ui: %s at %.0f%%\n", ui::F.source.c_str(), uiScale * 100.f);

    // Top-bar logo: the paint drop compiled into the exe (assets/icon).
    GLuint logoTex = 0;
    if (std::vector<unsigned char> png; loadEmbeddedResource("BRUSHKIT_LOGO", png)) {
        int lw = 0, lh = 0, ln = 0;
        if (unsigned char* lp = stbi_load_from_memory(png.data(), int(png.size()), &lw, &lh, &ln, 4)) {
            glGenTextures(1, &logoTex);
            glBindTexture(GL_TEXTURE_2D, logoTex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, lw, lh, 0, GL_RGBA, GL_UNSIGNED_BYTE, lp);
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glBindTexture(GL_TEXTURE_2D, 0);
            stbi_image_free(lp);
        }
    }

    filedialog::init();

    // Drops arrive on the GLFW callback, which cannot touch local state, so
    // they queue here and the loop drains them.
    static std::vector<std::string> droppedPaths;
    glfwSetDropCallback(win, [](GLFWwindow*, int count, const char** paths) {
        for (int i = 0; i < count; ++i) droppedPaths.push_back(paths[i]);
    });
    // Moving to a monitor with another scale rebuilds fonts and style.
    static float pendingScale = 0.f;
    glfwSetWindowContentScaleCallback(win, [](GLFWwindow*, float sx, float) {
        pendingScale = sx;
    });

    bool temporal = params.frameDiffThreshold > 0.f;
    bool temporalChoiceMade = false;
    bool tensorOn = params.tensorSigma > 0.f;
    ViewMode view = ViewMode::Split;   // start split so the input is visible
    std::string shaderMsg = "Shaders compiled";
    View viewCtl;
    char radiiBuf[128];
    snprintf(radiiBuf, sizeof(radiiBuf), "%s", radiiText.c_str());
    double cpuMs = 0.0;

    // --- what is loaded, and where it is going -------------------------
    Gui gui;
    gui.outDir = opt.outDir;
    gui.format = opt.batchFormat;
    gui.suffix = opt.batchSuffix;
    gui.videoOut = "painted.mp4";
    gui.crf = opt.crf;
    gui.keepAudio = !opt.noAudio;
    gui.fpsOverride = opt.fps;
    if (!opt.in.empty()) { gui.kind = InputKind::Image; gui.inputPath = opt.in; }
    // A video on the command line opens like a dropped one (preview frame,
    // scrub, Play) rather than being tried as a still.
    if (!opt.in.empty() && media::isVideoExtension(opt.in))
        acceptPaths({opt.in}, pipe, gui, sourceNote);

    // --- brushkit style state ----------------------------------------------
    int guiStyle = styleIdx;
    bool customLookActive = keepFileStyle;
    const float styleScaleVal = std::isnan(opt.styleScale) ? 1.f : opt.styleScale;
    // Re-derives every parameter block from the chosen style, at the current
    // stroke size, and resyncs the widgets that mirror them.
    auto restyle = [&]() {
        customLookActive = false;
        const float sc = styleScaleVal;
        if (guiStyle > 0) {
            styles::applyBrushTexture(guiStyle, styleP);
            styleP.widthScale *= sc;
        } else styles::applyBrushTexture(0, styleP);
        pipe.setStyle(styleP);
        pipe.resetTemporal();
        radiiText = radiiToString(cfg.radii);
        snprintf(radiiBuf, sizeof(radiiBuf), "%s", radiiText.c_str());
        tensorOn = params.tensorSigma > 0.f;
    };

    // --- realtime playback of a loaded video --------------------------------
    // The painting runs in a few ms per frame, so the window can paint the
    // footage as it plays.
    bool playing = false;
    media::Reader player;
    std::vector<unsigned char> playFrame;
    double playT0 = 0.0, playFpsShown = 0.0, playFpsT = 0.0, playLogT = 0.0;
    int64_t playCount = 0, playFpsCount = 0;
    // Live playback wants temporal coherence: paint carried along the flow,
    // repainted where the subject changed. Switched on (visibly, in the panel
    // and the status bar) if the user has not set it up already.
    auto startPlay = [&]() -> bool {
        std::string perr;
        playing = player.open(gui.inputPath, gui.video.width, gui.video.height, &perr);
        playT0 = playFpsT = playLogT = glfwGetTime();
        playCount = playFpsCount = 0;
        pipe.resetTemporal();
        if (!playing) { gui.status = perr; gui.statusIsError = true; return false; }
        if (!temporalChoiceMade) {
            if (!temporal) {
                gui.status = "Steady strokes turned on for playback";
                gui.statusIsError = false;
            }
            temporal = true;
            if (params.frameDiffThreshold <= 0.f) params.frameDiffThreshold = 12.f;
            if (cfg.flowLevels <= 0) cfg.flowLevels = 4;
        }
        return true;
    };
    auto stopPlay = [&]() {
        playing = false;
        player.close();
    };
    if (opt.play && gui.kind == InputKind::Video && !startPlay())
        fprintf(stderr, "%s\n", gui.status.c_str());

    // --- interface state ----------------------------------------------------
    namespace fs = std::filesystem;
    static const std::vector<filedialog::Filter> kImageFilter = {
        {"Images", "*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif"}};
    static const std::vector<filedialog::Filter> kVideoFilter = {
        {"Video", "*.mp4;*.mov;*.mkv;*.avi;*.webm;*.m4v;*.wmv"}};
    static const std::vector<filedialog::Filter> kParamFilter = {
        {"Brushkit look", "*.sbr"}};

    int backdrop = 0;                       // ui::Backdrop
    int inspectorTab = 0;                   // 0 = Look, 1 = Export
    float inspectorW = ui::M.inspectorW;    // at 100% scale
    std::string lookName = opt.paramsFile.empty() ? std::string() : fileName(opt.paramsFile);
    bool lookDirty = false;
    std::vector<std::string> pendingOpen;   // opened next frame, after "Opening..." shows
    int pendingAge = 0;
    std::string lastExportTarget;           // for "Show in folder"
    bool lastExportOk = false;
    std::string successStatus;              // the status text that earned a green dot
    bool lastExportFromPanel = false;       // false for an S-key snapshot
    double exportT0 = 0.0, escArmedUntil = 0.0;
    bool snapshotRequested = false;
    std::string windowTitle;

    // A confirmation for the two actions that silently threw work away before.
    struct Confirm {
        bool request = false;
        std::string title, body, ok;
        std::function<void()> action;
    } confirm;
    auto askConfirm = [&](const char* title, const char* body, const char* ok,
                          std::function<void()> action) {
        confirm.request = true;
        confirm.title = title;
        confirm.body = body;
        confirm.ok = ok;
        confirm.action = std::move(action);
    };

    // Defaults for "Reset to default" and the modified dot. Brush-mark fields
    // reset to the chosen brush's own values instead.
    const TuningParams kDefP{};
    const RenderConfig kDefC{};
    const StyleParams kDefS{};
    StyleParams brushDef;
    int brushDefFor = -1;

    auto queueOpen = [&](std::vector<std::string> paths) {
        if (paths.empty()) return;
        pendingOpen = std::move(paths);
        pendingAge = 0;
        gui.status = "Opening " + (pendingOpen.size() > 1
                                       ? std::to_string(pendingOpen.size()) + " items"
                                       : fileName(pendingOpen.front())) + "...";
        gui.statusIsError = false;
    };
    auto openImageDialog = [&]() {
        const std::string p = filedialog::openFile("Open an image", kImageFilter);
        if (!p.empty()) queueOpen({p});
    };
    auto openVideoDialog = [&]() {
        const std::string p = filedialog::openFile("Open a video", kVideoFilter);
        if (!p.empty()) queueOpen({p});
    };
    auto openImagesDialog = [&]() {
        const std::vector<std::string> ps =
            filedialog::openFiles("Open images to paint as a batch", kImageFilter);
        if (!ps.empty()) queueOpen(ps);
    };
    auto openFolderDialog = [&]() {
        const std::string p = filedialog::pickFolder("Open a folder of images");
        if (!p.empty()) queueOpen({p});
    };
    auto saveLook = [&]() {
        const std::string def = lookName.empty() ? std::string("look.sbr") : lookName;
        const std::string p = filedialog::saveFile("Save look", kParamFilter, def.c_str(), "sbr");
        if (p.empty()) return;
        std::string perr;
        gui.statusIsError = !paramfile::save(p, params, cfg, &perr, &styleP,
                                             styles::list()[size_t(guiStyle)].key);
        gui.status = gui.statusIsError ? perr : ("Saved look " + fileName(p));
        if (!gui.statusIsError) { lookName = fileName(p); lookDirty = false; }
    };
    auto loadLook = [&]() {
        const std::string p = filedialog::openFile("Open look", kParamFilter);
        if (p.empty()) return;
        std::string perr, key;
        bool withStyle = false;
        TuningParams loadedParams = params;
        RenderConfig loadedCfg = cfg;
        StyleParams loaded = styleP;
        if (!paramfile::load(p, &loadedParams, &loadedCfg, &perr, &loaded, &key, &withStyle)) {
            gui.status = perr;
            gui.statusIsError = true;
            return;
        }
        // Commit the whole look only after the file is validated.
        params = loadedParams;
        cfg = loadedCfg;
        styleP = withStyle ? loaded : StyleParams{};
        if (!withStyle) cfg.layerSpecs.clear();
        guiStyle = withStyle ? std::max(0, styles::indexOf(key)) : 0;
        customLookActive = withStyle;
        pipe.setStyle(styleP);
        // Everything the panel mirrors in its own state has to be resynced,
        // or the widgets would keep showing the old look while the renderer
        // used the new one.
        radiiText = radiiToString(cfg.radii);
        snprintf(radiiBuf, sizeof(radiiBuf), "%s", radiiText.c_str());
        tensorOn = params.tensorSigma > 0.f;
        pipe.buildBrushTiles(cfg.radii, cfg.bristleDensity);
        gui.status = perr.empty() ? ("Opened look " + fileName(p))
                                  : (fileName(p) + ": " + perr);
        gui.statusIsError = false;
        lookName = fileName(p);
        lookDirty = false;
    };
    auto cycleView = [&]() {
        // Painted -> Split -> Source, the order of the segmented control.
        view = view == ViewMode::Painted ? ViewMode::Split
             : view == ViewMode::Split   ? ViewMode::Source : ViewMode::Painted;
    };
    auto reloadShaders = [&]() {
        std::string err;
        const bool ok = pipe.reloadShaders(&err);
        shaderMsg = ok ? "Shaders reloaded" : err;
        if (!ok) { gui.status = "Shader reload failed: " + err; gui.statusIsError = true; }
    };
    auto fitView = [&]() {
        viewCtl.xf.zoom = 1.f;
        viewCtl.xf.focusX = viewCtl.xf.focusY = -1.f;
    };

    // ── Layout: one source of truth for every region ─────────────────────
    // Logical units (ImGui) for the chrome, framebuffer pixels with a
    // bottom-left origin for the canvas blit -- both derived here.
    struct Layout {
        float W = 0.f, H = 0.f;
        float top = 0.f, status = 0.f, strip = 0.f, insp = 0.f;
        ImVec2 c0, c1;                  // canvas region, logical
        ImVec2 i0, i1;                  // image area inside the surround margin
        int cbX = 0, cbY = 0, cbW = 0, cbH = 0;   // canvas region, framebuffer px
        int fbX = 0, fbY = 0, fbW = 0, fbH = 0;   // image area, framebuffer px
        int fbTotalW = 0, fbTotalH = 0;
    };
    auto computeLayout = [&]() {
        Layout L;
        int ww = 0, wh = 0, fw = 0, fh = 0;
        glfwGetWindowSize(win, &ww, &wh);
        glfwGetFramebufferSize(win, &fw, &fh);
        L.W = float(ww);
        L.H = float(wh);
        L.fbTotalW = fw;
        L.fbTotalH = fh;
        L.top = ui::px(ui::M.topBarH);
        L.status = ui::px(ui::M.statusBarH);
        L.strip = (gui.kind == InputKind::Video || gui.kind == InputKind::Images)
                ? ui::px(ui::M.stripH) : 0.f;
        const float maxW = std::max(ui::M.inspectorMinW,
                                    std::min(ui::M.inspectorMaxW, L.W / ui::scale() * 0.5f));
        inspectorW = std::clamp(inspectorW, ui::M.inspectorMinW, maxW);
        L.insp = ui::px(inspectorW);
        L.c0 = ImVec2(0.f, L.top);
        L.c1 = ImVec2(std::max(0.f, L.W - L.insp), std::max(L.top, L.H - L.status - L.strip));
        // The painting sits inside a margin of backdrop, never touching the
        // chrome, so its edges read cleanly at fit.
        const float m = ui::px(ui::M.canvasMargin);
        L.i0 = ImVec2(L.c0.x + m, L.c0.y + m);
        L.i1 = ImVec2(std::max(L.i0.x, L.c1.x - m), std::max(L.i0.y, L.c1.y - m));
        const float sx = ww > 0 ? float(fw) / float(ww) : 1.f;
        const float sy = wh > 0 ? float(fh) / float(wh) : 1.f;
        auto toFb = [&](ImVec2 a, ImVec2 b, int& x, int& y, int& w, int& h) {
            x = int(std::lround(a.x * sx));
            w = int(std::lround((b.x - a.x) * sx));
            y = int(std::lround((L.H - b.y) * sy));
            h = int(std::lround((b.y - a.y) * sy));
        };
        toFb(L.c0, L.c1, L.cbX, L.cbY, L.cbW, L.cbH);
        toFb(L.i0, L.i1, L.fbX, L.fbY, L.fbW, L.fbH);
        return L;
    };

    const ImGuiWindowFlags kRegion = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoSavedSettings |
                                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                                     ImGuiWindowFlags_NoFocusOnAppearing;
    auto beginRegion = [&](const char* name, ImVec2 pos, ImVec2 size, ImVec2 pad,
                           ImGuiWindowFlags extra = 0) {
        ImGui::SetNextWindowPos(pos);
        ImGui::SetNextWindowSize(size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pad);
        ImGui::Begin(name, nullptr, kRegion | extra);
        ImGui::PopStyleVar();
    };

    bool settingsChanged = false;   // per frame; resets temporal state while playing
    bool lookChanged = false;       // per frame; a look control was edited

    // Scrub-field shorthands. Every one ORs into settingsChanged, as the old
    // slider helpers did.
    auto sf = [&](const char* label, float* v, float lo, float hi, const char* fmt,
                  const float* def, const char* help = nullptr, const char* unit = nullptr) {
        ui::ScrubOpts o;
        o.format = fmt;
        o.def = def;
        o.help = help;
        o.unit = unit;
        const bool c = ui::ScrubFloat(label, v, lo, hi, o);
        settingsChanged |= c;
        return c;
    };
    auto si = [&](const char* label, int* v, int lo, int hi, const int* def,
                  const char* help = nullptr, const char* unit = nullptr) {
        ui::ScrubOpts o;
        o.format = "%.0f";
        const float d = def ? float(*def) : 0.f;
        o.def = def ? &d : nullptr;
        o.help = help;
        o.unit = unit;
        const bool c = ui::ScrubInt(label, v, lo, hi, o);
        settingsChanged |= c;
        return c;
    };
    // Look controls also have to switch the style block on (see lookChanged).
    auto lf = [&](const char* label, float* v, float lo, float hi, const char* fmt,
                  const float* def, const char* help = nullptr, const char* unit = nullptr) {
        const bool c = sf(label, v, lo, hi, fmt, def, help, unit);
        lookChanged |= c;
        return c;
    };

    // ── Inspector: Look tab ──────────────────────────────────────────────
    auto drawLookTab = [&]() {
        const std::vector<styles::Info>& sl = styles::list();
        StyleParams& s = styleP;
        if (brushDefFor != guiStyle) {
            brushDef = StyleParams{};
            styles::applyBrushTexture(guiStyle, brushDef);
            brushDef.widthScale *= styleScaleVal;
            brushDefFor = guiStyle;
        }

        // ── A. Brush ────────────────────────────────────────────────
        if (ui::Section("Brush", true, true)) {
            if (ui::BeginSelect("Brush##picker", sl[size_t(guiStyle)].name, 0.f, ui::px(ui::M.pickerMaxH))) {
                for (int i = 0; i < int(sl.size()); ++i) {
                    char sub[160];
                    if (i == 0) snprintf(sub, sizeof(sub), "Original tile brush");
                    else snprintf(sub, sizeof(sub), "%s  \xC2\xB7  %s", sl[size_t(i)].era,
                                  sl[size_t(i)].years);
                    if (ui::SelectItem(sl[size_t(i)].name, i == guiStyle, sub)) {
                        auto choose = [&, i]() {
                            if (customLookActive) {
                                cfg.layerSpecs.clear();
                                styleP = StyleParams{};
                            }
                            guiStyle = i;
                            restyle();
                            lookDirty = true;
                        };
                        if (customLookActive)
                            askConfirm("Replace the loaded look?",
                                       "The loaded look carries its own colour, ground and "
                                       "finish effects and per-layer stroke paths. Choosing a "
                                       "brush clears them.",
                                       "Choose brush", choose);
                        else choose();
                    }
                }
                ui::EndSelect();
            }
            ui::Tooltip("The shape and texture of each mark. Changing it resets only the "
                        "Brush section.");
            if (guiStyle > 0)
                ui::Caption("Inspired by %s, %s. Sets the mark only; colour, strokes and "
                            "finish keep their values.",
                            sl[size_t(guiStyle)].era, sl[size_t(guiStyle)].artists);
            else
                ui::Caption("Original tile brush. Tune its texture below.");
            if (customLookActive)
                ui::Notice(ui::Tone::Accent, "A loaded look is active. Choosing a brush clears "
                                             "its effects.");

            // Stroke size: a preset, or "Custom" when the radii match none.
            {
                const std::string cur = radiiToString(cfg.radii);
                int match = -1;
                for (int i = 0; i < kPresetCount; ++i)
                    if (radiiToString(parseRadii(kPresets[i].radii)) == cur) {
                        match = i;
                        if (i == presetIdx) break;
                    }
                ui::Gap(ui::M.s1);
                if (ui::BeginSelect("Stroke size", match >= 0 ? kPresets[match].name : "Custom")) {
                    for (int i = 0; i < kPresetCount; ++i) {
                        char sub[64];
                        snprintf(sub, sizeof(sub), "%s px",
                                 radiiToString(parseRadii(kPresets[i].radii)).c_str());
                        if (ui::SelectItem(kPresets[i].name, i == match, sub)) {
                            auto apply = [&, i]() {
                                presetIdx = i;
                                applyPreset(kPresets[presetIdx], params, cfg, &radiiText);
                                cfg.layerSpecs.clear();
                                snprintf(radiiBuf, sizeof(radiiBuf), "%s", radiiText.c_str());
                                tensorOn = params.tensorSigma > 0.f;
                                lookDirty = true;
                            };
                            if (!cfg.layerSpecs.empty())
                                askConfirm("Clear per-layer stroke paths?",
                                           "This look sets stroke length, detail and "
                                           "curvature per layer. Choosing a stroke size "
                                           "clears those overrides.",
                                           "Clear and apply", apply);
                            else apply();
                        }
                    }
                    ui::EndSelect();
                }
                ui::Tooltip("Brush sizes from largest to smallest. Each size paints one layer.");
                if (ui::InputField("Custom sizes##radii", radiiBuf, sizeof(radiiBuf),
                                   ImGuiInputTextFlags_EnterReturnsTrue, "e.g. 12, 6, 3")) {
                    cfg.radii = parseRadii(radiiBuf);
                    radiiText = radiiToString(cfg.radii);
                    snprintf(radiiBuf, sizeof(radiiBuf), "%s", radiiText.c_str());
                    lookDirty = true;
                }
                ui::Caption("Largest to smallest, in pixels. Press Enter to apply.");
            }

            if (guiStyle > 0) {
                ui::Gap(ui::M.s2);
                sf("Width", &s.widthScale, 0.3f, 2.5f, "%.2f", &brushDef.widthScale,
                   "Stroke width relative to the brush size.", "\xC3\x97");
                sf("Bristle count", &s.bristles, 0.f, 60.f, "%.0f", &brushDef.bristles,
                   "0 = a smooth mark.");
                sf("Paint load", &s.load, 0.1f, 1.f, "%.2f", &brushDef.load,
                   "Less paint gives broken, dry strokes.");
                sf("Paint thickness", &s.impasto, 0.f, 2.f, "%.2f", &brushDef.impasto,
                   "Raised paint height, lit by Brush relief in Ground & finish.");
                if (ui::Disclosure("Advanced##brush")) {
                    ui::GroupLabel("Bristles");
                    sf("Bristle clumping", &s.bristleClump, 0.f, 1.f, "%.2f", &brushDef.bristleClump);
                    sf("Streak contrast", &s.bristleContrast, 0.f, 1.f, "%.2f", &brushDef.bristleContrast);
                    ui::GroupLabel("Dry brush");
                    sf("Runs dry", &s.dryout, 0.f, 1.f, "%.2f", &brushDef.dryout,
                       "Paint fades along the stroke.");
                    sf("Skips canvas texture", &s.dryTooth, 0.f, 1.f, "%.2f", &brushDef.dryTooth,
                       "Paint catches only the raised weave.");
                    ui::GroupLabel("Edges");
                    sf("Edge roughness", &s.edgeRough, 0.f, 0.6f, "%.2f", &brushDef.edgeRough);
                    sf("Ragged ends", &s.endJag, 0.f, 1.f, "%.2f", &brushDef.endJag);
                    sf("Edge ridges", &s.ridge, 0.f, 1.f, "%.2f", &brushDef.ridge);
                    sf("Bristle grooves", &s.groove, 0.f, 2.f, "%.2f", &brushDef.groove);
                    ui::GroupLabel("Wet paint");
                    sf("Picks up wet paint", &s.pickup, 0.f, 1.f, "%.2f", &brushDef.pickup);
                    sf("Drags paint underneath", &s.smear, 0.f, 1.f, "%.2f", &brushDef.smear,
                       "Smears the paint below along each stroke. Different from Wet smear, "
                       "which acts on the finished painting.");
                }
                ui::Gap(ui::M.s1);
                if (ui::Button("Reset brush")) { restyle(); lookDirty = true; }
                ui::Tooltip("Restore every brush-mark value to this brush's defaults.");
                pipe.setStyle(s);
            }

            if (styleP.brushModel <= 0.5f) {
                ui::GroupLabel("Classic tile brush");
                sf("Brush texture strength", &params.texStrength, 0.f, 1.f, "%.2f",
                   &kDefP.texStrength);
                ui::BeginDisabledWhy(params.texStrength <= 0.f,
                                     "Raise Brush texture strength to use these.");
                if (sf("Bristle density", &cfg.bristleDensity, 2.f, 30.f, "%.1f",
                       &kDefC.bristleDensity, "Bristles across the stroke width."))
                    pipe.buildBrushTiles(cfg.radii, cfg.bristleDensity);
                sf("Tip taper", &params.texTaper, 0.f, 1.f, "%.2f", &kDefP.texTaper);
                sf("Fades along stroke", &params.dryBrush, 0.f, 1.f, "%.2f", &kDefP.dryBrush);
                ui::EndDisabledWhy();

                // Moved here from the old Paint tab: these only ever act on
                // the classic brush, and only while no finish relights them.
                ui::GroupLabel("Relief");
                const bool finishLights = styleP.enabled > 0.5f && styleP.finish > 0.f;
                sf("Thickness", &params.impastoStrength, 0.f, 0.5f, "%.2f",
                   &kDefP.impastoStrength, "Paint height accumulated per stroke.");
                ui::BeginDisabledWhy(finishLights,
                                     "Finish effects light the relief instead (Ground & finish).");
                sf("Light strength", &params.impastoLight, 0.f, 0.5f, "%.2f",
                   &kDefP.impastoLight);
                ui::BeginDisabledWhy(params.impastoLight <= 0.f, "Raise Light strength first.");
                sf("Light direction", &params.lightAngle, 0.f, 360.f, "%.0f", &kDefP.lightAngle,
                   nullptr, "\xC2\xB0");
                ui::EndDisabledWhy();
                ui::EndDisabledWhy();
            }
        }

        // ── B. Strokes ──────────────────────────────────────────────
        if (ui::Section("Strokes", true)) {
            if (!cfg.layerSpecs.empty()) {
                ui::Notice(ui::Tone::Accent, "This look sets stroke paths per layer, which "
                                             "override length, detail and curvature below.");
                if (ui::Button("Clear per-layer overrides")) {
                    cfg.layerSpecs.clear();
                    settingsChanged = true;
                }
                ui::Gap(ui::M.s1);
            }
            sf("Detail", &params.threshold, 1.f, 150.f, "%.0f", &kDefP.threshold,
               "Lower adds more strokes where the painting differs from the photo.");
            sf("Spacing", &params.gridFactor, 0.25f, 2.5f, "%.2f", &kDefP.gridFactor,
               "Grid spacing in brush sizes. Higher = fewer strokes.", "\xC3\x97");
            {
                // Min and max as a pair; each field's range stops at the
                // other's value so min can no longer exceed max.
                const float half = (ImGui::GetContentRegionAvail().x - ui::px(ui::M.s2)) * 0.5f;
                ui::ScrubOpts o;
                o.format = "%.0f";
                o.width = half;
                o.def = &kDefP.minStrokeLength;
                o.help = "Shortest stroke, in steps of about one brush size.";
                settingsChanged |= ui::ScrubFloat("Min length", &params.minStrokeLength, 1.f,
                                                  std::max(1.f, params.maxStrokeLength), o);
                ImGui::SameLine(0.f, ui::px(ui::M.s2));
                o.def = &kDefP.maxStrokeLength;
                o.help = "Longest stroke, in steps of about one brush size.";
                settingsChanged |= ui::ScrubFloat("Max length", &params.maxStrokeLength,
                                                  std::min(32.f, params.minStrokeLength), 32.f, o);
            }
            sf("Curvature", &params.curvature, 0.f, 1.f, "%.2f", &kDefP.curvature,
               "0 = straight strokes, 1 = strokes follow the image's curves.");

            if (ui::Disclosure("Advanced##strokes")) {
                ui::GroupLabel("Variation");
                sf("Size variation", &params.sizeJitter, 0.f, 0.6f, "%.2f", &kDefP.sizeJitter);
                sf("Angle variation", &params.angleJitter, 0.f, 45.f, "%.0f", &kDefP.angleJitter,
                   nullptr, "\xC2\xB0");

                // Moved from the old Paint tab: direction is a path property.
                ui::GroupLabel("Direction");
                const char* kFields[] = {"Follow image", "Swirl", "Patches", "Curl", "Waves",
                                         "Fixed angle"};
                int fm = std::clamp(int(s.fieldMode + 0.5f), 0, 5);
                if (ui::BeginSelect("Direction source", kFields[fm])) {
                    for (int i = 0; i < 6; ++i)
                        if (ui::SelectItem(kFields[i], i == fm) && i != fm) {
                            s.fieldMode = float(i);
                            lookChanged = true;
                            settingsChanged = true;
                        }
                    ui::EndSelect();
                }
                fm = std::clamp(int(s.fieldMode + 0.5f), 0, 5);
                lf("Pattern strength", &s.fieldMix, 0.f, 1.f, "%.2f", &kDefS.fieldMix,
                   "0 = follow the image, 1 = follow the pattern.");
                lf("Wobble", &s.perturb, 0.f, 1.5f, "%.2f", &kDefS.perturb);
                ui::BeginDisabledWhy(fm != 1, "Only for the Swirl direction source.");
                bool av = s.autoVortex > 0.5f;
                if (ui::Toggle("Centre swirl on brightest area", &av)) {
                    s.autoVortex = av ? 1.f : 0.f;
                    lookChanged = true;
                    settingsChanged = true;
                }
                lf("Spiral twist", &s.vortexSpiral, -1.5f, 1.5f, "%.2f", &kDefS.vortexSpiral);
                lf("Swirl size", &s.vortexRadius, 0.05f, 1.f, "%.2f", &kDefS.vortexRadius);
                ui::EndDisabledWhy();

                ui::GroupLabel("Direction smoothing");
                ui::Toggle("Smooth directions", &tensorOn,
                           "Uses a smoother direction field (structure tensor). Good for "
                           "faces and soft gradients.");
                ui::BeginDisabledWhy(!tensorOn, "Turn on Smooth directions first.");
                sf("Amount##tensor", &params.tensorSigma, 0.5f, 8.f, "%.1f", nullptr);
                ui::EndDisabledWhy();
                // Edge Tangent Flow: a filter on the field, not a stroke setting.
                const int defEtf = kDefC.etfIterations;
                si("Edge-aware passes", &cfg.etfIterations, 0, 8, &defEtf,
                   "Edge-aware smoothing of stroke directions. 0 = off. Slower: cost grows "
                   "with radius\xC2\xB2 \xC3\x97 passes.");
                ui::BeginDisabledWhy(cfg.etfIterations <= 0, "Set Edge-aware passes above 0.");
                sf("Radius##etf", &params.etfRadius, 1.f, 12.f, "%.0f", &kDefP.etfRadius,
                   nullptr, "px");
                ui::EndDisabledWhy();

                // Moved from the old Paint tab: relaxation places strokes.
                ui::GroupLabel("Refine strokes");
                const int defRelax = kDefC.relaxIterations;
                si("Refinement passes", &cfg.relaxIterations, 0, 16, &defRelax,
                   "Iteratively moves and removes strokes to match the image with fewer "
                   "marks (Hertzmann 2001). 0 = off. Each pass costs about one extra render.");
                ui::BeginDisabledWhy(cfg.relaxIterations <= 0, "Set Refinement passes above 0.");
                sf("Economy", &params.relaxAreaWeight, 0.f, 0.4f, "%.2f", &kDefP.relaxAreaWeight,
                   "Higher = fewer, larger, bolder strokes.");
                sf("Move distance", &params.relaxMoveScale, 0.f, 2.f, "%.2f",
                   &kDefP.relaxMoveScale, "How far a stroke may move, in brush sizes.",
                   "\xC3\x97");
                sf("Tries per stroke", &params.relaxCandidates, 1.f, 16.f, "%.0f",
                   &kDefP.relaxCandidates);
                sf("Remove weak strokes", &params.relaxRemove, 0.f, 2.f, "%.2f",
                   &kDefP.relaxRemove,
                   "0 = never, 1 = remove strokes that don't improve the match.");
                ui::EndDisabledWhy();
                if (cfg.relaxIterations > 0 && pipe.lastPoolCount() > 0) {
                    char line[160];
                    snprintf(line, sizeof(line), "%u strokes  \xC2\xB7  %u moved  \xC2\xB7  %u removed",
                             pipe.lastPoolCount(), pipe.lastRelaxAccepted(),
                             pipe.lastRelaxRemoved());
                    ui::Text(ui::F.mono, ui::C.textTertiary, line);
                    if (pipe.lastPoolCount() >= SBR_POOL_MAX)
                        ui::Notice(ui::Tone::Accent,
                                   "Too many strokes to refine all of them (limit %u). "
                                   "Increase Spacing or lower Detail.", SBR_POOL_MAX);
                }
            }
        }

        // ── C. Colour ───────────────────────────────────────────────
        if (ui::Section("Colour", false)) {
            sf("Opacity", &params.opacity, 0.05f, 1.f, "%.2f", &kDefP.opacity);
            lf("Saturation", &s.saturation, 0.f, 2.f, "%.2f", &kDefS.saturation);
            lf("Warm / cool", &s.warmCool, -1.f, 1.f, "%.2f", &kDefS.warmCool,
               "Positive: warm lights and cool shadows. Negative: the reverse.");
            if (ui::Disclosure("Advanced##colour")) {
                ui::GroupLabel("Per-stroke variation");
                sf("Hue##jitter", &params.jitterHue, 0.f, 0.3f, "%.2f", &kDefP.jitterHue);
                sf("Saturation##jitter", &params.jitterSat, 0.f, 0.5f, "%.2f", &kDefP.jitterSat);
                sf("Brightness##jitter", &params.jitterVal, 0.f, 0.5f, "%.2f", &kDefP.jitterVal);
                sf("Opacity##jitter", &params.opacityJitter, 0.f, 0.6f, "%.2f",
                   &kDefP.opacityJitter);
                ui::GroupLabel("Broken colour");
                lf("Lightness##broken", &s.labJitterL, 0.f, 20.f, "%.1f", &kDefS.labJitterL);
                lf("Hue##broken", &s.labJitterAB, 0.f, 20.f, "%.1f", &kDefS.labJitterAB);
                if (s.paletteCount > 0.f) {
                    ui::GroupLabel("Palette");
                    lf("Palette pull", &s.palettePull, 0.f, 1.f, "%.2f", &kDefS.palettePull);
                    ui::Caption("From the loaded look's palette.");
                }
            }
        }

        // ── D. Ground & finish ──────────────────────────────────────
        if (ui::Section("Ground & finish", false)) {
            // Underpaint and Toned ground were two controls that fought: the
            // toned ground silently overrides the underpaint. One choice now.
            const char* kGround[] = {"Blurred photo", "Plain", "Average colour", "Toned ground"};
            const bool toned = s.groundMode > 0.5f;
            const int gi = toned ? 3 : std::clamp(int(cfg.underpaint), 0, 2);
            if (ui::BeginSelect("Ground", kGround[gi])) {
                for (int i = 0; i < 4; ++i) {
                    if (!ui::SelectItem(kGround[i], i == gi)) continue;
                    if (i == 3) {
                        if (!toned) { s.groundMode = 1.f; lookChanged = true; }
                    } else {
                        cfg.underpaint = Underpaint(i);
                        if (toned) { s.groundMode = 0.f; lookChanged = true; }
                    }
                    settingsChanged = true;
                }
                ui::EndSelect();
            }
            ui::Tooltip("What the strokes are painted onto.");
            ui::BeginDisabledWhy(s.groundMode <= 0.5f, "Only with Toned ground.");
            lf("Photo over ground", &s.underOpacity, 0.f, 1.f, "%.2f", &kDefS.underOpacity,
               "How strongly the blurred photo shows over the toned ground.");
            ui::EndDisabledWhy();

            ui::GroupLabel("Finish");
            bool finishEnabled = s.finish > 0.9f;
            if (ui::Toggle("Finish effects", &finishEnabled,
                           "Canvas weave, varnish, cracks, vignette and more.")) {
                s.finish = finishEnabled ? 1.f : (s.brushModel > 0.5f ? 0.5f : 0.f);
                lookChanged = true;
                settingsChanged = true;
            }
            ui::BeginDisabledWhy(s.finish <= 0.f, "Turn on Finish effects to light brush relief.");
            lf("Relief strength", &s.impastoLight, 0.f, 2.f, "%.2f", &kDefS.impastoLight);
            lf("Relief depth", &s.reliefScale, 0.f, 4.f, "%.2f", &kDefS.reliefScale);
            lf("Gloss", &s.specular, 0.f, 0.6f, "%.2f", &kDefS.specular);
            ui::EndDisabledWhy();
            if (s.finish > 0.f && !finishEnabled)
                ui::Caption("Era brushes always light their relief.");
            ui::BeginDisabledWhy(!finishEnabled, "Turn on Finish effects.");
            lf("Canvas weave", &s.weave, 0.f, 1.f, "%.2f", &kDefS.weave);
            lf("Varnish", &s.varnish, 0.f, 1.f, "%.2f", &kDefS.varnish);
            lf("Cracks", &s.crackle, 0.f, 1.f, "%.2f", &kDefS.crackle, "Craquelure.");
            lf("Vignette", &s.vignette, 0.f, 1.f, "%.2f", &kDefS.vignette);
            lf("Wet smear", &s.licStrength, 0.f, 1.f, "%.2f", &kDefS.licStrength,
               "Smears the finished painting along the stroke direction.");
            lf("Outline strokes", &s.contour, 0.f, 1.f, "%.2f", &kDefS.contour);
            lf("Rain", &s.rain, 0.f, 1.f, "%.2f", &kDefS.rain);
            ui::EndDisabledWhy();
        }

        // ── E. Video motion (video only, always last) ───────────────
        if (gui.kind == InputKind::Video && ui::Section("Video motion", false)) {
            ui::Caption("Previewed during playback and export.");
            if (ui::Toggle("Steady strokes", &temporal,
                           "Reuses paint where nothing moved, which stops flicker.")) {
                temporalChoiceMade = true;
                params.frameDiffThreshold = temporal ? 12.f : 0.f;
                pipe.resetTemporal();
            }
            ui::BeginDisabledWhy(!temporal, "Turn on Steady strokes first.");
            const float defDiff = 12.f;
            sf("Repaint sensitivity", &params.frameDiffThreshold, 1.f, 64.f, "%.0f", &defDiff,
               "Source change needed to repaint a cell. Lower = repaint more often.");
            sf("Fresh paint on movement", &cfg.freshPaint, 0.f, 1.f, "%.2f", &kDefC.freshPaint,
               "Repaints fast-moving areas each frame instead of dragging old paint along.");
            if (ui::Disclosure("Advanced##video")) {
                const int defLevels = kDefC.flowLevels, defIters = kDefC.flowIterations;
                si("Motion tracking range", &cfg.flowLevels, 0, 6, &defLevels,
                   "Pyramid levels for motion tracking. 0 = off; 4 tracks about 32 px per "
                   "frame.");
                ui::BeginDisabledWhy(cfg.flowLevels <= 0, "Set Motion tracking range above 0.");
                si("Tracking accuracy", &cfg.flowIterations, 1, 6, &defIters);
                ui::EndDisabledWhy();
                if (cfg.flowLevels > 0 && pipe.msFlow() > 0.0) {
                    char line[64];
                    snprintf(line, sizeof(line), "Motion tracking %.2f ms", pipe.msFlow());
                    ui::Text(ui::F.mono, ui::C.textTertiary, line);
                }
            }
            ui::EndDisabledWhy();
        }
    };

    // ── Export target preview ─────────────────────────────────────────────
    // Shows where the job will write, resolved exactly as jobs.cpp resolves
    // it (relative folders are relative to the working directory), and how
    // many of those files already exist.
    struct ExportPreview {
        std::string path;        // file (image, video) or folder (batch)
        std::string example;     // first file name for a batch
        int count = 0, existing = 0;
    };
    ExportPreview exportPreview;
    std::string exportPreviewKey;
    int exportPreviewAge = 0;
    auto updateExportPreview = [&]() {
        // While exporting, the files being written would read as "exists".
        if (gui.exporting) return;
        const std::string key = std::to_string(int(gui.kind)) + "|" + gui.outDir + "|" +
                                gui.suffix + "|" + gui.format + "|" + gui.videoOut + "|" +
                                gui.inputPath + "|" + std::to_string(gui.queue.size());
        // Re-stat at most twice a second; the file system can change under us.
        if (key == exportPreviewKey && ++exportPreviewAge < 30) return;
        exportPreviewKey = key;
        exportPreviewAge = 0;
        ExportPreview ep;
        std::error_code ec;
        auto absolute = [&](const fs::path& p) {
            const fs::path a = fs::absolute(p, ec);
            return (ec ? p : a).lexically_normal();
        };
        auto target = [&](const std::string& src) {
            return fs::path(gui.outDir) /
                   (fs::path(src).stem().string() + gui.suffix + "." + gui.format);
        };
        if (gui.kind == InputKind::Video) {
            const fs::path p = absolute(gui.videoOut);
            ep.path = p.string();
            ep.count = 1;
            ep.existing = fs::exists(p, ec) ? 1 : 0;
        } else if (gui.kind == InputKind::Image) {
            const fs::path p = absolute(target(gui.inputPath));
            ep.path = p.string();
            ep.count = 1;
            ep.existing = fs::exists(p, ec) ? 1 : 0;
        } else if (gui.kind == InputKind::Images) {
            ep.path = absolute(gui.outDir).string();
            ep.count = int(gui.queue.size());
            if (!gui.queue.empty()) ep.example = target(gui.queue.front()).filename().string();
            for (const std::string& f : gui.queue)
                if (fs::exists(target(f), ec)) ++ep.existing;
        }
        exportPreview = ep;
    };

    // ── Inspector: Export tab ────────────────────────────────────────────
    auto drawExportTab = [&]() {
        updateExportPreview();
        char buf[1024];
        if (ui::Section("Destination", true, true)) {
            if (gui.kind == InputKind::None) {
                ui::TextWrapped(ui::F.body, ui::C.textSecondary,
                                "Open an image, a video or a folder of images first. The demo "
                                "image can't be exported.");
                ui::Gap(ui::M.s2);
                if (ui::Button("Open image...", ui::ButtonKind::Secondary)) openImageDialog();
                ImGui::SameLine();
                if (ui::Button("Open video...")) openVideoDialog();
                ImGui::SameLine();
                if (ui::Button("Open folder...")) openFolderDialog();
            } else if (gui.kind == InputKind::Video) {
                const float bw = ui::ButtonWidth("Choose...");
                ui::Text(ui::F.body, ui::C.textSecondary, "Save to");
                snprintf(buf, sizeof(buf), "%s", gui.videoOut.c_str());
                if (ui::InputField("##videoOut", buf, sizeof(buf), 0, nullptr,
                                   ImGui::GetContentRegionAvail().x - bw - ui::px(ui::M.s2)))
                    gui.videoOut = buf;
                ImGui::SameLine(0.f, ui::px(ui::M.s2));
                if (ui::Button("Choose...##video")) {
                    const std::string p = filedialog::saveFile(
                        "Save painted video as", kVideoFilter,
                        fileName(gui.videoOut).c_str(), "mp4");
                    if (!p.empty()) gui.videoOut = p;
                }
                ui::Gap(ui::M.s2);
                // CRF runs the wrong way for a slider (lower = better), so the
                // field shows quality left-to-right and stores CRF.
                int q = 42 - gui.crf;
                char crfText[32];
                snprintf(crfText, sizeof(crfText), "CRF %d", gui.crf);
                ui::ScrubOpts o;
                o.valueText = crfText;
                const float defQ = float(42 - opt.crf);
                o.def = &defQ;
                o.help = "Right = better quality and a bigger file. Stored as x264 CRF "
                         "(lower is better).";
                if (ui::ScrubInt("Quality", &q, 12, 30, o)) gui.crf = 42 - q;
                ui::BeginDisabledWhy(!gui.video.hasAudio, "This clip has no audio track.");
                bool audio = gui.keepAudio && gui.video.hasAudio;
                if (ui::Toggle("Include audio", &audio)) gui.keepAudio = audio;
                ui::EndDisabledWhy();
                if (!temporal) {
                    ui::Gap(ui::M.s1);
                    ui::Notice(ui::Tone::Accent, "Steady strokes is off, so every frame is "
                                                 "painted from scratch and the video will "
                                                 "shimmer.");
                    if (ui::Button("Turn on steady strokes")) {
                        temporal = true;
                        temporalChoiceMade = true;
                        params.frameDiffThreshold = 12.f;
                        pipe.resetTemporal();
                    }
                }
                ui::Caption("Encoder %s, preset %s (set with --vcodec / --vpreset).",
                            opt.vcodec.c_str(), opt.vpreset.c_str());
            } else {
                const float bw = ui::ButtonWidth("Choose...");
                ui::Text(ui::F.body, ui::C.textSecondary, "Save to folder");
                snprintf(buf, sizeof(buf), "%s", gui.outDir.c_str());
                if (ui::InputField("##outDir", buf, sizeof(buf), 0, nullptr,
                                   ImGui::GetContentRegionAvail().x - bw - ui::px(ui::M.s2)))
                    gui.outDir = buf;
                ImGui::SameLine(0.f, ui::px(ui::M.s2));
                if (ui::Button("Choose...##folder")) {
                    const std::string p = filedialog::pickFolder("Choose where to save");
                    if (!p.empty()) gui.outDir = p;
                }
                ui::Gap(ui::M.s1);
                char sbuf[128];
                snprintf(sbuf, sizeof(sbuf), "%s", gui.suffix.c_str());
                if (ui::InputField("File name ending##suffix", sbuf, sizeof(sbuf)))
                    gui.suffix = sbuf;
                ui::Gap(ui::M.s1);
                ui::Text(ui::F.body, ui::C.textSecondary, "Format");
                const char* kFormats[] = {"png", "jpg", "bmp", "tga"};
                int fi = 0;
                for (int i = 0; i < 4; ++i) if (gui.format == kFormats[i]) fi = i;
                if (ui::Segmented("##format", kFormats, 4, &fi, -1.f)) gui.format = kFormats[fi];
            }
        }

        if (gui.kind != InputKind::None) {
            ui::GroupLabel("Output");
            if (gui.kind == InputKind::Images) {
                char line[256];
                snprintf(line, sizeof(line), "%d files, e.g. %s", exportPreview.count,
                         exportPreview.example.c_str());
                ui::TextWrapped(ui::F.body, ui::C.textPrimary, line);
            }
            ui::EllipsizedText(ui::F.mono, ui::C.textSecondary, exportPreview.path.c_str());
            if (exportPreview.existing > 0) {
                ui::Gap(ui::M.s1);
                if (gui.kind == InputKind::Images)
                    ui::Notice(ui::Tone::Danger, "%d of %d files already exist and will be "
                                                 "replaced.", exportPreview.existing,
                               exportPreview.count);
                else
                    ui::Notice(ui::Tone::Danger, "This file already exists and will be replaced.");
            }
        }

        ui::Gap(ui::M.s5);
        char label[64];
        if (exportPreview.existing > 0 && gui.kind != InputKind::None)
            snprintf(label, sizeof(label), "Export and replace");
        else if (gui.kind == InputKind::Video) snprintf(label, sizeof(label), "Export video");
        else if (gui.kind == InputKind::Images)
            snprintf(label, sizeof(label), "Export %zu images", gui.queue.size());
        else snprintf(label, sizeof(label), "Export image");
        ui::BeginDisabledWhy(gui.kind == InputKind::None,
                             "Open an image or video first. The demo can't be exported.");
        if (ui::Button(label, ui::ButtonKind::Primary, -1.f)) gui.startExport = true;
        ui::EndDisabledWhy();
        ui::Tooltip(nullptr, "Ctrl+E opens this panel.");

        if (!lastExportTarget.empty() && lastExportOk && lastExportFromPanel &&
            gui.kind != InputKind::None) {
            ui::Gap(ui::M.s3);
            if (ui::Button("Show in folder")) filedialog::reveal(lastExportTarget);
        }
    };

    // ── Regions ───────────────────────────────────────────────────────────
    auto sourceLabel = [&]() -> std::string {
        switch (gui.kind) {
            case InputKind::Image: return fileName(gui.inputPath);
            case InputKind::Video: return fileName(gui.inputPath);
            case InputKind::Images: return std::to_string(gui.queue.size()) + " images";
            default: return "Demo image";
        }
    };

    auto drawTopBar = [&](const Layout& L, bool exporting) {
        const float h = L.top;
        beginRegion("##topbar", ImVec2(0.f, 0.f), ImVec2(L.W, h),
                    ImVec2(ui::px(ui::M.barPadX), (h - ui::px(ui::M.controlH)) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::px(ui::M.s2), 0.f));
        if (logoTex) {
            const float slot = ui::px(ui::M.controlH), side = ui::px(ui::M.logoSize);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(side, slot));
            const ImVec2 a(p.x, std::floor(p.y + (slot - side) * 0.5f));
            ImGui::GetWindowDrawList()->AddImage(ImTextureID(intptr_t(logoTex)), a,
                                                 ImVec2(a.x + side, a.y + side));
            ImGui::SameLine(0.f, ui::px(ui::M.s2));
        }
        ui::BarText(ui::F.section, ui::C.textPrimary, "Brushkit");
        ImGui::SameLine(0.f, ui::px(ui::M.s5));

        ImGui::BeginDisabled(exporting);
        if (ui::BeginMenuButton("Open", ui::ButtonKind::Secondary)) {
            if (ui::MenuItem("Image...", "Ctrl+O")) openImageDialog();
            if (ui::MenuItem("Video...")) openVideoDialog();
            if (ui::MenuItem("Images as a batch...")) openImagesDialog();
            if (ui::MenuItem("Folder as a batch...")) openFolderDialog();
            ui::MenuSeparator();
            ImGui::Indent(ui::px(ui::M.menuPadX));
            ui::Text(ui::F.caption, ui::C.textTertiary, "Or drop files or a folder on the window");
            ImGui::Unindent(ui::px(ui::M.menuPadX));
            ui::Gap(ui::M.s2);
            ui::EndMenuButton();
        }
        ImGui::EndDisabled();

        // Source: name, then its size in mono.
        ImGui::SameLine(0.f, ui::px(ui::M.s4));
        std::string src = sourceLabel();
        if (src.size() > 42) src = src.substr(0, 26) + "..." + src.substr(src.size() - 13);
        ui::BarText(ui::F.body, gui.kind == InputKind::None ? ui::C.textSecondary
                                                            : ui::C.textPrimary, src.c_str());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ui::Tooltip(gui.kind == InputKind::None
                            ? "A synthetic demo image. Open or drop an image, a video or a "
                              "folder to start."
                            : sourceNote.c_str());
        char dims[96];
        if (gui.kind == InputKind::Video)
            snprintf(dims, sizeof(dims), "%d\xC3\x97%d  %.2f fps", gui.video.width,
                     gui.video.height, gui.video.fps);
        else
            snprintf(dims, sizeof(dims), "%d\xC3\x97%d", pipe.width(), pipe.height());
        ImGui::SameLine(0.f, ui::px(ui::M.s3));
        ui::BarText(ui::F.mono, ui::C.textTertiary, dims);

        // Look: name and a modified dot, with Save / Open in its menu.
        ImGui::SameLine(0.f, ui::px(ui::M.s5));
        std::string look = lookName.empty() ? "Untitled" : lookName;
        if (lookDirty) look += "  \xE2\x80\xA2";
        ImGui::BeginDisabled(exporting);
        if (ui::BeginMenuButton("Look", ui::ButtonKind::Ghost, look.c_str())) {
            if (ui::MenuItem("Save look...", "Ctrl+S")) saveLook();
            if (ui::MenuItem("Open look...", "Ctrl+Shift+O")) loadLook();
            ui::MenuSeparator();
            ImGui::Indent(ui::px(ui::M.menuPadX));
            ui::Text(ui::F.caption, ui::C.textTertiary, "A look stores every setting (.sbr)");
            ImGui::Unindent(ui::px(ui::M.menuPadX));
            ui::Gap(ui::M.s2);
            ui::EndMenuButton();
        }
        ImGui::EndDisabled();

        // Right cluster: view switch, then the one primary action.
        const char* kViews[] = {"Painted", "Split", "Source"};
        const char* kViewTips[] = {"The painting (V cycles views)",
                                   "Drag the divider to compare (V)",
                                   "The untouched source (hold B to peek)"};
        const float segW = ui::SegmentedWidth(kViews, 3);
        const float actionW = exporting ? ui::ButtonWidth("Cancel export")
                                        : ui::ButtonWidth("Export...", ui::ButtonKind::Primary);
        const float right = L.W - ui::px(ui::M.s4);
        const float segX = right - actionW - ui::px(ui::M.s4) - segW;
        ImGui::SameLine(segX);
        int vi = view == ViewMode::Painted ? 0 : view == ViewMode::Split ? 1 : 2;
        if (ui::Segmented("##view", kViews, 3, &vi, 0.f, 0.f, kViewTips))
            view = vi == 0 ? ViewMode::Painted : vi == 1 ? ViewMode::Split : ViewMode::Source;
        ImGui::SameLine(0.f, ui::px(ui::M.s4));
        if (exporting) {
            if (ui::Button("Cancel export")) gui.cancel = true;
        } else {
            const float y = ImGui::GetCursorPosY();
            ImGui::SetCursorPosY(y - (ui::px(ui::M.primaryH) - ui::px(ui::M.controlH)) * 0.5f);
            if (ui::Button("Export...", ui::ButtonKind::Primary)) inspectorTab = 1;
            ui::Tooltip("Choose where to save, then export.", "Ctrl+E");
        }
        ImGui::PopStyleVar();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(0.f, h - 1.f), ImVec2(L.W, h), ui::col(ui::C.borderSubtle));
        if (exporting) {
            const jobs::Progress& p = gui.progress;
            const float frac = p.total > 0 ? float(double(p.done) / double(p.total)) : 0.f;
            ui::DrawProgress(dl, ImVec2(0.f, h - ui::px(ui::M.progressH)), ImVec2(L.W, h), frac, p.total <= 0);
        }
        ImGui::End();
    };

    auto drawInspector = [&](const Layout& L, bool exporting) {
        const float x = L.W - L.insp;
        const float h = L.H - L.top - L.status;
        beginRegion("##inspector", ImVec2(x, L.top), ImVec2(L.insp, h), ImVec2(0.f, 0.f),
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::SetCursorPos(ImVec2(ui::px(ui::M.inspectorPadX), 0.f));
        const char* kTabs[] = {"Look", "Export"};
        ImGui::BeginDisabled(exporting);
        ui::TextTabs("##tabs", kTabs, 2, &inspectorTab);
        ImGui::EndDisabled();
        ImGui::SetCursorPos(ImVec2(0.f, ImGui::GetCursorPosY()));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(ui::px(ui::M.inspectorPadX), ui::px(ui::M.inspectorPadY)));
        ImGui::BeginChild("##scroll", ImVec2(L.insp, 0.f), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        // Reserve the scrollbar gutter even when nothing overflows, so Look and
        // Export lay out at the same width.
        if (ImGuiWindow* w = ImGui::GetCurrentWindow(); !w->ScrollbarY) {
            const float g = ImGui::GetStyle().ScrollbarSize;
            w->WorkRect.Max.x -= g;
            w->ContentRegionRect.Max.x -= g;
        }
        ImGui::BeginDisabled(exporting);
        if (inspectorTab == 0) drawLookTab();
        else drawExportTab();
        ImGui::EndDisabled();
        ui::Gap(ui::M.s5);
        ImGui::EndChild();
        ImGui::End();

        // Divider, with a hit zone for resizing the inspector.
        if (!exporting) {
            const float hit = ui::px(ui::M.splitterHit);
            ImGui::SetNextWindowPos(ImVec2(x - hit * 0.5f, L.top));
            ImGui::SetNextWindowSize(ImVec2(hit, h));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
            // Not kRegion: ImGui inserts NoBringToFrontOnFocus windows at the
            // *back* of the display order, which would bury this hit zone
            // under the inspector it overlaps.
            ImGui::Begin("##splitter", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                         ImGuiWindowFlags_NoFocusOnAppearing);
            ImGui::PopStyleVar();
            ImGui::InvisibleButton("##drag", ImVec2(hit, h));
            const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
            if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (ImGui::IsItemActive())
                inspectorW -= ImGui::GetIO().MouseDelta.x / ui::scale();
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                inspectorW = ui::M.inspectorW;
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(x - (hot ? 1.f : 0.f), L.top), ImVec2(x + 1.f, L.top + h),
                ui::col(hot ? ui::C.borderStrong : ui::C.borderSubtle));
            ImGui::End();
        }
    };

    auto drawStrip = [&](const Layout& L, bool exporting) {
        if (L.strip <= 0.f) return;
        beginRegion("##strip", ImVec2(0.f, L.c1.y), ImVec2(L.c1.x, L.strip),
                    ImVec2(ui::px(ui::M.s4), (L.strip - ui::px(ui::M.controlH)) * 0.5f));
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(0.f, L.c1.y),
                                                  ImVec2(L.c1.x, L.c1.y + 1.f),
                                                  ui::col(ui::C.borderSubtle));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::px(ui::M.s2), 0.f));
        ImGui::BeginDisabled(exporting);
        if (gui.kind == InputKind::Video) {
            if (ui::Button(playing ? "Pause" : "Play", ui::ButtonKind::Secondary, ui::px(ui::M.playButtonW))) {
                if (!playing) startPlay();
                else stopPlay();
            }
            ui::Tooltip(playing ? "Pause playback" : "Paint the clip in realtime", "Space");
            ImGui::SameLine();

            const double dur = (gui.video.fps > 0.0 && gui.video.frames > 0)
                             ? double(gui.video.frames) / gui.video.fps : 0.0;
            char info[160];
            if (playing)
                snprintf(info, sizeof(info), "%.1f / %.0f fps painted", playFpsShown,
                         gui.video.fps);
            else
                snprintf(info, sizeof(info), "%lld frames  \xC2\xB7  %s",
                         (long long)gui.video.frames, gui.video.hasAudio ? "audio" : "no audio");
            const float infoW = ui::TextWidth(ui::F.mono, info);
            // Scrub on release rather than on drag: each move would otherwise
            // spawn an ffmpeg seek per frame of UI.
            float at = float(gui.previewAt);
            char t[64];
            snprintf(t, sizeof(t), "%.2f / %.2f s", at, dur);
            ui::ScrubOpts o;
            o.format = "%.2f";
            o.valueText = t;
            o.width = std::max(ui::px(ui::M.scrubMinW), ImGui::GetContentRegionAvail().x - infoW - ui::px(ui::M.s4));
            bool released = false;
            o.released = &released;
            o.help = "Preview frame. The clip seeks when you let go.";
            if (ui::ScrubFloat("Preview at", &at, 0.f, float(std::max(dur, 0.1)), o)) {
                gui.previewAt = at;
                if (playing) pipe.resetTemporal();
            }
            if (released) {
                gui.previewRequested = gui.previewAt;
                gui.previewDelay = 0;
            }
            ImGui::SameLine(0.f, ui::px(ui::M.s4));
            ui::BarText(ui::F.mono, ui::C.textTertiary, info);
        } else if (gui.kind == InputKind::Images) {
            ui::BarText(ui::F.body, ui::C.textSecondary, "Batch");
            ImGui::SameLine(0.f, ui::px(ui::M.s3));
            char n[64];
            snprintf(n, sizeof(n), "%zu images", gui.queue.size());
            ui::BarText(ui::F.mono, ui::C.textPrimary, n);
            ImGui::SameLine(0.f, ui::px(ui::M.s3));
            if (!gui.queue.empty()) {
                const std::string prev = "Previewing " + fileName(gui.queue.front());
                ui::BarText(ui::F.caption, ui::C.textTertiary, prev.c_str());
                ImGui::SameLine(0.f, ui::px(ui::M.s4));
            }
            if (ui::BeginMenuButton("List", ui::ButtonKind::Ghost)) {
                for (size_t i = 0; i < gui.queue.size() && i < 200; ++i) {
                    ImGui::Indent(ui::px(ui::M.menuPadX));
                    ui::BarText(ui::F.body, i == 0 ? ui::C.textPrimary : ui::C.textSecondary,
                                fileName(gui.queue[i]).c_str());
                    ImGui::Unindent(ui::px(ui::M.menuPadX));
                }
                if (gui.queue.size() > 200) {
                    ImGui::Indent(ui::px(ui::M.menuPadX));
                    ui::Text(ui::F.caption, ui::C.textTertiary, "...and more");
                    ImGui::Unindent(ui::px(ui::M.menuPadX));
                }
                ui::EndMenuButton();
            }
            ImGui::SameLine();
            if (ui::Button("Clear queue", ui::ButtonKind::Ghost)) {
                // Back to the demo, so the canvas, the source label and the
                // Export panel all agree that nothing is loaded.
                gui.queue.clear();
                gui.kind = InputKind::None;
                loadSource(pipe, "", &sourceNote);
                pipe.resetTemporal();
                gui.status = "Queue cleared";
                gui.statusIsError = false;
            }
        }
        ImGui::EndDisabled();
        ImGui::PopStyleVar();
        ImGui::End();
    };

    auto drawDiagnostics = [&]() {
        auto row = [&](const char* k, const char* fmt, double v) {
            char b[64];
            snprintf(b, sizeof(b), fmt, v);
            ui::Text(ui::F.caption, ui::C.textSecondary, k);
            ImGui::SameLine(ui::px(ui::M.keyColumnW));
            ui::Text(ui::F.mono, ui::C.textPrimary, b);
        };
        ui::Text(ui::F.section, ui::C.textPrimary, "Diagnostics");
        ui::Gap(ui::M.s2);
        char b[128];
        snprintf(b, sizeof(b), "%u seeds  \xC2\xB7  %u drawn  \xC2\xB7  %.1f pts/stroke",
                 pipe.lastStrokeCount(), pipe.lastDrawnCount(), pipe.lastMeanPoints());
        ui::Text(ui::F.mono, ui::C.textPrimary, b);
        ui::Gap(ui::M.s1);
        row("GPU total", "%.2f ms", pipe.msTotal());
        row("Frame rate", "%.0f fps", pipe.msTotal() > 0.0 ? 1000.0 / pipe.msTotal() : 0.0);
        row("Reference", "%.2f ms", pipe.msReference());
        row("Error", "%.2f ms", pipe.msError());
        row("Seeds", "%.2f ms", pipe.msSeeds());
        row("Trace", "%.2f ms", pipe.msTrace());
        row("Raster", "%.2f ms", pipe.msRaster());
        row("Impasto", "%.2f ms", pipe.msImpasto());
        if (pipe.msRelax() > 0.0) row("Relax", "%.2f ms", pipe.msRelax());
        if (pipe.msStyle() > 0.0) row("Style", "%.2f ms", pipe.msStyle());
        row("Frame wall", "%.2f ms", cpuMs);
        if (ui::Disclosure("Layer details")) {
            const std::vector<LayerStats>& ls = pipe.layerStats();
            for (size_t i = 0; i < ls.size(); ++i) {
                snprintf(b, sizeof(b), "%d: r %.1f grid %.0f cells %u x%u seeds %u drawn %u",
                         int(i), ls[i].radius, ls[i].grid, ls[i].cells, ls[i].chunks,
                         ls[i].strokes, ls[i].drawn);
                ui::Text(ui::F.mono, ui::C.textSecondary, b);
            }
        }
        ui::Gap(ui::M.s2);
        ui::Text(ui::F.caption, ui::C.textTertiary, shaderMsg.c_str());
        ui::TextWrapped(ui::F.caption, ui::C.textTertiary, ("Fonts: " + ui::F.source).c_str());
        ui::Gap(ui::M.s2);
        if (ui::Button("Reload shaders")) reloadShaders();
        ui::Tooltip(nullptr, "F5");
    };

    auto drawShortcuts = [&]() {
        ui::Text(ui::F.section, ui::C.textPrimary, "Shortcuts");
        ui::Gap(ui::M.s2);
        const char* rows[][2] = {
            {"Ctrl+O", "Open image"},          {"Ctrl+S", "Save look"},
            {"Ctrl+Shift+O", "Open look"},     {"Ctrl+E", "Export panel"},
            {"V / Tab", "Cycle view"},         {"Hold B", "Show the source"},
            {"F / double-click", "Fit"},       {"1", "Actual pixels (1:1)"},
            {"Wheel", "Zoom at cursor"},       {"Drag", "Pan, or move the split"},
            {"Space", "Play / pause video"},   {"S", "Save a PNG snapshot (--out)"},
            {"F5", "Reload shaders"},          {"Ctrl+Q", "Quit"},
        };
        for (const auto& r : rows) {
            ui::Text(ui::F.mono, ui::C.textPrimary, r[0]);
            ImGui::SameLine(ui::px(ui::M.keyColumnW));
            ui::Text(ui::F.body, ui::C.textSecondary, r[1]);
        }
        ui::Gap(ui::M.s2);
        ui::Text(ui::F.caption, ui::C.textTertiary,
                 "Esc closes menus. During export, press it twice to cancel.");
    };

    auto drawStatusBar = [&](const Layout& L, bool exporting) {
        const float h = L.status;
        beginRegion("##status", ImVec2(0.f, L.H - h), ImVec2(L.W, h),
                    ImVec2(ui::px(ui::M.statusPadX), (h - ui::px(ui::M.controlH)) * 0.5f));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(0.f, L.H - h), ImVec2(L.W, L.H - h + 1.f),
                          ui::col(ui::C.borderSubtle));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::px(ui::M.s1), 0.f));

        // ── right cluster, measured first so the message can be clipped ──
        char zoomText[32], dimsText[48], perfText[64];
        const float fit = (pipe.width() > 0 && pipe.height() > 0)
            ? std::min(float(L.fbW) / float(pipe.width()), float(L.fbH) / float(pipe.height()))
            : 1.f;
        snprintf(zoomText, sizeof(zoomText), "%.0f%%", fit * viewCtl.xf.zoom * 100.f);
        snprintf(dimsText, sizeof(dimsText), "%d\xC3\x97%d", pipe.width(), pipe.height());
        const unsigned drawn = pipe.lastDrawnCount();
        if (drawn >= 10000)
            snprintf(perfText, sizeof(perfText), "%.1f ms  \xC2\xB7  %.0fk strokes",
                     pipe.msTotal(), drawn / 1000.0);
        else
            snprintf(perfText, sizeof(perfText), "%.1f ms  \xC2\xB7  %u strokes",
                     pipe.msTotal(), drawn);
        const char* kBackdrops[] = {"Light", "Gray", "White"};
        const char* kBackdropTips[] = {"Light surround, matches the interface",
                                       "18% grey surround, for judging values",
                                       "Paper-white surround"};
        const float bdW = ui::SegmentedWidth(kBackdrops, 3);
        const float linkPad = ui::px(ui::M.s3), gap = ui::px(ui::M.s1);
        const float rightW = ui::TextWidth(ui::F.mono, zoomText) + linkPad + gap +
                             ui::TextWidth(ui::F.caption, "Fit") + linkPad + gap +
                             ui::TextWidth(ui::F.caption, "1:1") + linkPad + ui::px(ui::M.s4) +
                             ui::TextWidth(ui::F.mono, dimsText) + ui::px(ui::M.s4) +
                             ui::TextWidth(ui::F.mono, perfText) + linkPad + ui::px(ui::M.s4) +
                             bdW + ui::px(ui::M.s3) + ui::TextWidth(ui::F.body, "?") + linkPad;
        const float rightX = L.W - ui::px(ui::M.s3) - rightW;

        // ── left: export progress, or the latest message ──
        ImGui::PushClipRect(ImVec2(0.f, L.H - h), ImVec2(std::max(0.f, rightX - ui::px(ui::M.s4)), L.H),
                            true);
        if (exporting) {
            const jobs::Progress& p = gui.progress;
            char m[256];
            const double el = glfwGetTime() - exportT0;
            if (p.total > 0) {
                const double eta = p.done > 0 ? el / double(p.done) * double(p.total - p.done) : 0.0;
                snprintf(m, sizeof(m), "Exporting  %lld / %lld  \xC2\xB7  %.0f%%  \xC2\xB7  %d:%02d left",
                         (long long)p.done, (long long)p.total,
                         100.0 * double(p.done) / double(p.total), int(eta) / 60, int(eta) % 60);
            } else {
                snprintf(m, sizeof(m), "Exporting  %lld frames  \xC2\xB7  %d:%02d elapsed",
                         (long long)p.done, int(el) / 60, int(el) % 60);
            }
            ui::BarText(ui::F.caption, ui::C.textPrimary, m);
        } else if (!gui.status.empty()) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const bool success = !gui.statusIsError && gui.status == successStatus;
            const ImVec4& dot = gui.statusIsError ? ui::C.danger
                              : success ? ui::C.success : ui::C.textTertiary;
            dl->AddCircleFilled(ImVec2(p.x + ui::px(ui::M.s1), p.y + ui::px(ui::M.controlH) * 0.5f),
                                ui::px(ui::M.dotR), ui::col(dot));
            ImGui::Dummy(ImVec2(ui::px(ui::M.s3), ui::px(ui::M.controlH)));
            ImGui::SameLine();
            // Long messages (paths) are shortened in the middle so the links
            // after them stay reachable; the tooltip has the whole text.
            const float links = ui::px(success ? 130.f : 30.f);
            const float avail = rightX - ui::px(ui::M.s6) - (ImGui::GetCursorScreenPos().x) - links;
            std::string msg = gui.status;
            if (ui::TextWidth(ui::F.caption, msg.c_str()) > avail && msg.size() > 8) {
                size_t keep = msg.size();
                std::string cut;
                do {
                    keep = keep * 9 / 10;
                    cut = msg.substr(0, keep / 2) + "..." + msg.substr(msg.size() - keep / 2);
                } while (keep > 8 && ui::TextWidth(ui::F.caption, cut.c_str()) > avail);
                msg = cut;
            }
            ui::BarText(ui::F.caption, gui.statusIsError ? ui::C.danger : ui::C.textSecondary,
                        msg.c_str());
            ui::Tooltip(gui.status.c_str());
            if (success && lastExportOk && !lastExportTarget.empty()) {
                ImGui::SameLine(0.f, ui::px(ui::M.s2));
                if (ui::BarLink(ui::F.caption, ui::C.textSecondary, "##reveal", "Show in folder"))
                    filedialog::reveal(lastExportTarget);
            }
            ImGui::SameLine(0.f, ui::px(ui::M.s1));
            if (ui::BarLink(ui::F.caption, ui::C.textTertiary, "##dismiss", "\xC3\x97"))
                gui.status.clear();
            ui::Tooltip("Dismiss");
        } else {
            ui::BarText(ui::F.caption, ui::C.textTertiary,
                        "Wheel to zoom  \xC2\xB7  drag to pan  \xC2\xB7  hold B for the source");
        }
        ImGui::PopClipRect();

        // ── right cluster ──
        ImGui::SameLine(rightX);
        if (ui::BarLink(ui::F.mono, ui::C.textSecondary, "##zoom", zoomText)) fitView();
        ui::Tooltip("Zoom. Click to fit.", "Wheel to zoom at the cursor");
        ImGui::SameLine(0.f, gap);
        if (ui::BarLink(ui::F.caption, ui::C.textTertiary, "##fit", "Fit")) fitView();
        ui::Tooltip("Fit the image", "F");
        ImGui::SameLine(0.f, gap);
        if (ui::BarLink(ui::F.caption, ui::C.textTertiary, "##1to1", "1:1") && fit > 0.f)
            viewCtl.xf.zoom = 1.f / fit;
        ui::Tooltip("Actual pixels: judge brush texture here", "1");
        ImGui::SameLine(0.f, ui::px(ui::M.s4));
        ui::BarText(ui::F.mono, ui::C.textTertiary, dimsText);
        ImGui::SameLine(0.f, ui::px(ui::M.s4));
        const bool slow = pipe.msTotal() > 33.0;
        if (ui::BarLink(ui::F.mono, slow ? ui::C.accent : ui::C.textTertiary, "##perf", perfText))
            ImGui::OpenPopup("##diag");
        ui::Tooltip(slow ? "GPU time per frame is above 33 ms. Refinement and edge-aware "
                           "smoothing are the usual cost."
                         : "GPU time per frame and strokes drawn. Click for diagnostics.");
        ImGui::SameLine(0.f, ui::px(ui::M.s4));
        const float compactInset = (ui::px(ui::M.controlH) - ui::px(ui::M.compactH)) * 0.5f;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + compactInset);
        ui::Segmented("##backdrop", kBackdrops, 3, &backdrop, 0.f, ui::px(ui::M.compactH),
                      kBackdropTips);
        ImGui::SameLine(0.f, ui::px(ui::M.s3));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - compactInset);
        if (ui::BarLink(ui::F.body, ui::C.textSecondary, "##help", "?"))
            ImGui::OpenPopup("##shortcuts");
        ui::Tooltip("Keyboard shortcuts");

        // Popovers open upwards from the status bar.
        auto popover = [&](const char* id, float width, const std::function<void()>& body) {
            ImGui::SetNextWindowPos(ImVec2(L.W - ui::px(ui::M.statusPadX),
                                           L.H - h - ui::px(ui::M.popoverGap)),
                                    ImGuiCond_Always, ImVec2(1.f, 1.f));
            ImGui::SetNextWindowSize(ImVec2(width, 0.f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                ImVec2(ui::px(ui::M.popoverPadX), ui::px(ui::M.popoverPadY)));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::px(ui::M.s2), ui::px(ui::M.s1)));
            if (ImGui::BeginPopup(id, ImGuiWindowFlags_NoMove)) {
                body();
                ImGui::EndPopup();
            }
            ImGui::PopStyleVar(2);
        };
        popover("##diag", ui::px(ui::M.diagnosticsW), drawDiagnostics);
        popover("##shortcuts", ui::px(ui::M.shortcutsW), drawShortcuts);

        ImGui::PopStyleVar();
        ImGui::End();
    };

    // Where the image actually is on screen (logical units), clipped to the
    // image area -- the same framing arithmetic as Pipeline::blitToScreen.
    auto imageRect = [&](const Layout& L, ImVec2& r0, ImVec2& r1) {
        const float iw = float(pipe.width()), ih = float(pipe.height());
        if (iw <= 0.f || ih <= 0.f || L.fbW <= 0 || L.fbH <= 0) { r0 = L.i0; r1 = L.i1; return; }
        const float fit = std::min(float(L.fbW) / iw, float(L.fbH) / ih);
        const float sc = fit * std::max(viewCtl.xf.zoom, 0.01f);
        const float fx = viewCtl.xf.focusX < 0.f ? iw * 0.5f : viewCtl.xf.focusX;
        const float fy = viewCtl.xf.focusY < 0.f ? ih * 0.5f : viewCtl.xf.focusY;
        const float cx = float(L.fbX) + float(L.fbW) * 0.5f;
        const float cy = float(L.fbY) + float(L.fbH) * 0.5f;
        const float sx = L.W > 0.f ? float(L.fbTotalW) / L.W : 1.f;
        const float sy = L.H > 0.f ? float(L.fbTotalH) / L.H : 1.f;
        const ImVec2 a((cx - fx * sc) / sx, (float(L.fbTotalH) - (cy + fy * sc)) / sy);
        const ImVec2 b((cx + (iw - fx) * sc) / sx, (float(L.fbTotalH) - (cy - (ih - fy) * sc)) / sy);
        r0 = ImVec2(std::max(a.x, L.i0.x), std::max(a.y, L.i0.y));
        r1 = ImVec2(std::min(b.x, L.i1.x), std::min(b.y, L.i1.y));
        if (r1.x < r0.x || r1.y < r0.y) { r0 = L.i0; r1 = L.i1; }
    };

    auto drawCanvasOverlays = [&](const Layout& L, bool exporting, ViewMode shown) {
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        const float pad = ui::px(ui::M.s3);
        const ImU32 pillBg = ui::col(ui::C.pillBg);
        const ImU32 pillFg = ui::col(ui::C.textSecondary);
        ImVec2 r0, r1;
        imageRect(L, r0, r1);
        bg->PushClipRect(L.c0, L.c1, true);
        // On the white surround a light painting needs a faint edge.
        if (ui::Backdrop(backdrop) == ui::Backdrop::White)
            bg->AddRect(ImVec2(r0.x - 1.f, r0.y - 1.f), ImVec2(r1.x + 1.f, r1.y + 1.f),
                        ui::col(ui::C.imageEdge));
        if (shown == ViewMode::Split) {
            // The wipe: a 1 px line with a grab handle, and corner labels.
            const float x = std::floor(L.i0.x + (L.i1.x - L.i0.x) * viewCtl.xf.wipe);
            const float cy = (r0.y + r1.y) * 0.5f;
            bg->AddRectFilled(ImVec2(x, r0.y), ImVec2(x + 1.f, r1.y),
                              ui::col(ui::C.wipeLine));
            const float r = ui::px(ui::M.handleR);
            bg->AddCircleFilled(ImVec2(x + 0.5f, cy), r, ui::col(ui::C.surface2), 32);
            bg->AddCircle(ImVec2(x + 0.5f, cy), r, ui::col(ui::C.borderStrong), 32, 1.f);
            const float a = ui::px(ui::M.s1);
            const ImU32 ac = ui::col(ui::C.textPrimary);
            const ImVec2 la[3] = {ImVec2(x - a * 0.5f, cy - a), ImVec2(x - a * 1.5f, cy),
                                  ImVec2(x - a * 0.5f, cy + a)};
            const ImVec2 ra[3] = {ImVec2(x + a * 1.5f, cy - a), ImVec2(x + a * 2.5f, cy),
                                  ImVec2(x + a * 1.5f, cy + a)};
            bg->AddPolyline(la, 3, ac, 0, ui::px(ui::M.iconStroke));
            bg->AddPolyline(ra, 3, ac, 0, ui::px(ui::M.iconStroke));
            ui::DrawPill(bg, ImVec2(r0.x + pad, r0.y + pad), ui::F.caption, "Source",
                         pillBg, pillFg);
            const float pw = ui::TextWidth(ui::F.caption, "Painted") + ui::px(ui::M.s4);
            ui::DrawPill(bg, ImVec2(r1.x - pad - pw, r0.y + pad), ui::F.caption, "Painted",
                         pillBg, pillFg);
        } else if (shown == ViewMode::Source) {
            ui::DrawPill(bg, ImVec2(r0.x + pad, r0.y + pad), ui::F.caption, "Source",
                         pillBg, pillFg);
        }
        if (gui.previewRequested >= 0.0)
            ui::DrawPill(bg, ImVec2(r0.x + pad, r1.y - pad - ui::px(ui::M.compactH)), ui::F.caption,
                         "Seeking...", pillBg, pillFg);
        bg->PopClipRect();

        // Cards: small windows, so they take the mouse only where they are.
        auto card = [&](const char* id, ImVec2 pos, ImVec2 pivot, float width,
                        const std::function<void()>& body) {
            ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
            ImGui::SetNextWindowSize(ImVec2(width, 0.f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                ImVec2(ui::px(ui::M.cardPadX), ui::px(ui::M.cardPadY)));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui::px(ui::M.radiusLg));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::C.surface2);
            ImGui::PushStyleColor(ImGuiCol_Border, ui::C.borderDefault);
            ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_AlwaysAutoResize |
                                          ImGuiWindowFlags_NoFocusOnAppearing);
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(3);
            body();
            ImGui::End();
        };
        const float cw = L.c1.x - L.c0.x;
        if (exporting) {
            card("##exportcard", ImVec2(L.c0.x + ui::px(ui::M.s5), L.c1.y - ui::px(ui::M.s5)),
                 ImVec2(0.f, 1.f), std::min(ui::px(ui::M.progressCardW), cw - ui::px(ui::M.s7)), [&]() {
                const jobs::Progress& p = gui.progress;
                const char* what = gui.kind == InputKind::Video ? "Exporting video"
                                 : gui.kind == InputKind::Images ? "Exporting images"
                                 : "Exporting image";
                ui::Text(ui::F.section, ui::C.textPrimary, what);
                if (!p.item.empty()) ui::Text(ui::F.caption, ui::C.textTertiary,
                                              fileName(p.item).c_str());
                ui::Gap(ui::M.s2);
                const ImVec2 a = ImGui::GetCursorScreenPos();
                const float w = ImGui::GetContentRegionAvail().x;
                const float frac = p.total > 0 ? float(double(p.done) / double(p.total)) : 0.f;
                ui::DrawProgress(ImGui::GetWindowDrawList(), a, ImVec2(a.x + w, a.y + ui::px(ui::M.s1)),
                                 frac, p.total <= 0);
                ImGui::Dummy(ImVec2(w, ui::px(ui::M.s1)));
                ui::Gap(ui::M.s1);
                char line[160];
                if (p.total > 0)
                    snprintf(line, sizeof(line), "%lld / %lld  \xC2\xB7  %.1f ms/frame  \xC2\xB7  %u strokes",
                             (long long)p.done, (long long)p.total, p.msPerFrame, p.drawn);
                else
                    snprintf(line, sizeof(line), "%lld frames  \xC2\xB7  %.1f ms/frame  \xC2\xB7  %u strokes",
                             (long long)p.done, p.msPerFrame, p.drawn);
                ui::Text(ui::F.mono, ui::C.textSecondary, line);
                ui::Gap(ui::M.s3);
                if (ui::Button("Cancel export")) gui.cancel = true;
                if (glfwGetTime() < escArmedUntil) {
                    ImGui::SameLine(0.f, ui::px(ui::M.s3));
                    ui::BarText(ui::F.caption, ui::C.textSecondary, "Press Esc again to cancel");
                }
            });
        } else if (gui.kind == InputKind::None && pendingOpen.empty()) {
            card("##democard", ImVec2((L.c0.x + L.c1.x) * 0.5f, L.c1.y - ui::px(ui::M.s6)),
                 ImVec2(0.5f, 1.f), std::min(ui::px(ui::M.demoCardW), cw - ui::px(ui::M.s7)), [&]() {
                ui::Text(ui::F.section, ui::C.textPrimary, "This is a demo image");
                ui::Gap(ui::M.s1);
                ui::TextWrapped(ui::F.body, ui::C.textSecondary,
                                "Open or drop an image, a video or a folder of images to paint "
                                "it. Every control works on the demo too.");
                ui::Gap(ui::M.s3);
                if (ui::Button("Open image...", ui::ButtonKind::Primary)) openImageDialog();
                ImGui::SameLine();
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                                     (ui::px(ui::M.primaryH) - ui::px(ui::M.controlH)) * 0.5f);
                if (ui::Button("Video...")) openVideoDialog();
                ImGui::SameLine();
                if (ui::Button("Folder...")) openFolderDialog();
            });
        }
    };

    auto drawConfirm = [&]() {
        if (confirm.request) {
            ImGui::OpenPopup("##confirm");
            confirm.request = false;
        }
        const ImVec2 c = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(c, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(ui::px(ui::M.dialogW), 0.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(ui::px(ui::M.dialogPadX), ui::px(ui::M.dialogPadY)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui::px(ui::M.radiusLg));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::C.surface2);
        if (ImGui::BeginPopupModal("##confirm", nullptr,
                                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings)) {
            ui::Text(ui::F.section, ui::C.textPrimary, confirm.title.c_str());
            ui::Gap(ui::M.s2);
            ui::TextWrapped(ui::F.body, ui::C.textSecondary, confirm.body.c_str());
            ui::Gap(ui::M.s5);
            const float okW = ui::ButtonWidth(confirm.ok.c_str(), ui::ButtonKind::Primary);
            const float cancelW = ui::ButtonWidth("Cancel");
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ui::px(ui::M.s5) - okW - cancelW - ui::px(ui::M.s2));
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                                 (ui::px(ui::M.primaryH) - ui::px(ui::M.controlH)) * 0.5f);
            if (ui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                ImGui::CloseCurrentPopup();
            ImGui::SameLine(0.f, ui::px(ui::M.s2));
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() -
                                 (ui::px(ui::M.primaryH) - ui::px(ui::M.controlH)) * 0.5f);
            if (ui::Button(confirm.ok.c_str(), ui::ButtonKind::Primary)) {
                if (confirm.action) confirm.action();
                confirm.action = nullptr;
                settingsChanged = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
    };

    // Every region, in one place, so the export pump draws exactly the same
    // chrome as the main loop (disabled) instead of a separate mini-UI.
    auto drawUi = [&](const Layout& L, bool exporting, ViewMode shown) {
        drawCanvasOverlays(L, exporting, shown);
        drawTopBar(L, exporting);
        drawStrip(L, exporting);
        drawInspector(L, exporting);
        drawStatusBar(L, exporting);
        if (!exporting) drawConfirm();
    };

    // Chrome colour everywhere, backdrop over the canvas region (the blit
    // letterboxes inside the image area with the same colour).
    auto clearFrame = [&](const Layout& FL) {
        const ImVec4 b = ui::backdropColor(ui::Backdrop(backdrop));
        pipe.setBackdrop(b.x, b.y, b.z);
        glViewport(0, 0, FL.fbTotalW, FL.fbTotalH);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(ui::C.bgApp.x, ui::C.bgApp.y, ui::C.bgApp.z, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (FL.cbW > 0 && FL.cbH > 0) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(FL.cbX, FL.cbY, FL.cbW, FL.cbH);
            glClearColor(b.x, b.y, b.z, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
        }
    };

    uint64_t shownFrames = 0;
    while (!glfwWindowShouldClose(win)) {
        ++shownFrames;
        glfwPollEvents();

        if (pendingScale > 0.f) {
            const float s = std::clamp(pendingScale, 1.f, 4.f);
            pendingScale = 0.f;
            if (std::fabs(s - uiScale) > 0.01f) {
                uiScale = s;
                ui::applyTheme(uiScale);
                ImGui_ImplOpenGL3_DestroyFontsTexture();
                ImGui_ImplOpenGL3_CreateFontsTexture();
            }
        }

        if (playing && gui.kind == InputKind::Video) {
            const double fps = gui.video.fps > 0.0 ? gui.video.fps : 30.0;
            const double now = glfwGetTime();
            // Paced to the clip's own rate; if painting falls behind, frames
            // are simply taken as fast as they can be painted.
            if (now >= playT0 + double(playCount) / fps) {
                if (!player.read(playFrame)) {          // end of clip: loop
                    std::string perr;
                    player.open(gui.inputPath, gui.video.width, gui.video.height, &perr);
                    pipe.resetTemporal();
                    playT0 = now;
                    playCount = 0;
                    if (!player.read(playFrame)) playing = false;
                }
                if (playing) {
                    pipe.setSource(playFrame.data(), gui.video.width, gui.video.height);
                    ++playCount;
                    ++playFpsCount;
                }
            }
            if (now - playFpsT > 0.5) {
                playFpsShown = double(playFpsCount) / (now - playFpsT);
                playFpsT = now;
                playFpsCount = 0;
            }
            if (now - playLogT > 2.0) {
                printf("playing: %.1f painted fps, GPU %.2f ms/frame, %u strokes\n",
                       playFpsShown, pipe.msTotal(), pipe.lastDrawnCount());
                fflush(stdout);
                playLogT = now;
            }
        } else if (playing) {
            stopPlay();
        }
        // A drop can be an image, a video, or a folder; each means something
        // different, so classify before deciding what to do with it. Drops
        // and picks open one frame late, so "Opening..." is on screen while
        // the (blocking) decode runs.
        if (!droppedPaths.empty()) {
            queueOpen(droppedPaths);
            droppedPaths.clear();
        }
        if (!pendingOpen.empty() && ++pendingAge > 2) {
            const std::vector<std::string> paths = std::move(pendingOpen);
            pendingOpen.clear();
            gui.status.clear();
            if (playing) stopPlay();
            // A new source: the last export's result no longer applies.
            lastExportOk = false;
            lastExportTarget.clear();
            successStatus.clear();
            acceptPaths(paths, pipe, gui, sourceNote);
            fitView();
        }

        const double t0 = glfwGetTime();
        params.tensorSigma = tensorOn ? std::max(0.1f, params.tensorSigma) : 0.f;
        // Paint only when an input actually changed: a still that nobody is
        // touching is painted once, not 60 times a second.
        pipe.renderIfChanged(params, cfg, playing && temporal);
        params.frame += 1.f;

        const Layout L = computeLayout();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // ── keys: never while a text field has focus ─────────────────────
        ImGuiIO& io = ImGui::GetIO();
        // Esc never quits (that used to lose unsaved work instantly). It
        // closes an open menu or popover; inside a text field ImGui uses it
        // to cancel the edit.
        if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::ClosePopupsExceptModals();
        // No shortcuts behind an open menu or the confirmation dialog.
        const bool popupOpen =
            ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        if (!io.WantTextInput && !popupOpen) {
            const bool ctrl = io.KeyCtrl, shift = io.KeyShift;
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Q, false))
                glfwSetWindowShouldClose(win, GLFW_TRUE);
            if (ctrl && !shift && ImGui::IsKeyPressed(ImGuiKey_O, false)) openImageDialog();
            if (ctrl && shift && ImGui::IsKeyPressed(ImGuiKey_O, false)) loadLook();
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) saveLook();
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_E, false)) inspectorTab = 1;
            if (!ctrl) {
                if (ImGui::IsKeyPressed(ImGuiKey_Tab, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_V, false))
                    cycleView();
                if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) reloadShaders();
                if (ImGui::IsKeyPressed(ImGuiKey_S, false)) snapshotRequested = true;
                if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && gui.kind == InputKind::Video) {
                    if (playing) stopPlay();
                    else startPlay();
                }
            }
        }

        // Hold B for the untouched source in place of the painting: a flicker
        // comparison catches things a side-by-side never does.
        const ViewMode shown = (ImGui::IsKeyDown(ImGuiKey_B) && !io.WantTextInput)
                             ? ViewMode::Source : view;
        settingsChanged = false;
        lookChanged = false;
        drawUi(L, false, shown);

        // Look controls only act once the style block is enabled.
        if (lookChanged) styleP.enabled = 1.f;
        pipe.setStyle(styleP);
        if (settingsChanged) lookDirty = true;
        if (settingsChanged && playing) pipe.resetTemporal();

        // Framing is resolved after the panel so WantCaptureMouse already
        // accounts for the widgets, then the canvas is blitted and ImGui's own
        // draw data goes on top of it.
        updateView(viewCtl, L.fbTotalH, L.fbX, L.fbY, L.fbW, L.fbH,
                   pipe.width(), pipe.height(), view);

        ImGui::Render();
        clearFrame(L);
        pipe.blitToScreen(L.fbX, L.fbY, L.fbW, L.fbH, L.fbTotalW, L.fbTotalH, shown, viewCtl.xf);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (snapshotRequested) {
            snapshotRequested = false;
            std::error_code ec;
            const bool existed = fs::exists(opt.out, ec);
            const fs::path abs = fs::absolute(opt.out, ec);
            const std::string shownPath = (ec ? fs::path(opt.out) : abs).string();
            const bool ok = writePng(opt.out, pipe.readCanvas(), pipe.width(), pipe.height());
            gui.status = ok ? ("Saved snapshot " + shownPath + (existed ? " (replaced)" : ""))
                            : ("Could not save " + shownPath);
            gui.statusIsError = !ok;
            lastExportOk = ok;
            lastExportFromPanel = false;
            lastExportTarget = ok ? shownPath : std::string();
            successStatus = ok ? gui.status : std::string();
        }

        glfwSwapBuffers(win);
        cpuMs = (glfwGetTime() - t0) * 1000.0;

        // Window title follows the source.
        {
            const std::string t = "Brushkit \xE2\x80\x94 " + sourceLabel();
            if (t != windowTitle) {
                windowTitle = t;
                glfwSetWindowTitle(win, windowTitle.c_str());
            }
        }

        // Re-seek the video preview once the scrub is released (one frame
        // late, so "Seeking..." is on screen during the blocking decode).
        if (gui.previewRequested >= 0.0 && ++gui.previewDelay > 1) {
            gui.previewAt = gui.previewRequested;
            gui.previewRequested = -1.0;
            gui.previewDelay = 0;
            if (!loadVideoPreview(pipe, gui, &sourceNote)) {
                gui.status = "Could not decode a frame at that time";
                gui.statusIsError = true;
            }
        }

        // --- export ----------------------------------------------------
        // Runs on this thread: the GL context is single-threaded, so the
        // progress callback pumps the window between frames instead. It draws
        // the same chrome as the main loop, disabled, plus a progress card.
        if (gui.startExport) {
            gui.startExport = false;
            if (playing) stopPlay();
            gui.exporting = true;
            gui.cancel = false;
            gui.progress = jobs::Progress{};
            exportT0 = glfwGetTime();
            escArmedUntil = 0.0;
            lastExportOk = false;

            auto pump = [&](const jobs::Progress& p) {
                gui.progress = p;
                glfwPollEvents();
                if (glfwWindowShouldClose(win)) return false;

                const Layout PL = computeLayout();
                clearFrame(PL);
                pipe.blitToScreen(PL.fbX, PL.fbY, PL.fbW, PL.fbH, PL.fbTotalW, PL.fbTotalH,
                                  view, viewCtl.xf);

                ImGui_ImplOpenGL3_NewFrame();
                ImGui_ImplGlfw_NewFrame();
                ImGui::NewFrame();
                // Esc twice cancels; once only arms it, so a stray key press
                // does not throw away a long render.
                if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                    if (glfwGetTime() < escArmedUntil) gui.cancel = true;
                    else escArmedUntil = glfwGetTime() + 3.0;
                }
                settingsChanged = false;
                lookChanged = false;
                drawUi(PL, true, view);
                ImGui::Render();
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                glfwSwapBuffers(win);
                return !gui.cancel;
            };

            jobs::Result res;
            std::string target;
            if (gui.kind == InputKind::Video) {
                jobs::VideoSpec vs;
                vs.input = gui.inputPath;
                vs.output = gui.videoOut;
                vs.fps = gui.fpsOverride;
                vs.crf = gui.crf;
                vs.codec = opt.vcodec;
                vs.preset = opt.vpreset;
                vs.keepAudio = gui.keepAudio;
                TuningParams exportParams = params;
                exportParams.frameDiffThreshold = temporal
                    ? std::max(1.f, params.frameDiffThreshold) : 0.f;
                res = jobs::runVideo(pipe, exportParams, cfg, vs, pump);
                target = gui.videoOut;
            } else if (gui.kind == InputKind::Images) {
                jobs::BatchSpec bs;
                bs.files = gui.queue;
                bs.outDir = gui.outDir;
                bs.suffix = gui.suffix;
                bs.format = gui.format;
                res = jobs::runBatch(pipe, params, cfg, bs, pump);
                target = gui.outDir;
            } else {
                jobs::BatchSpec bs;
                bs.files = {gui.inputPath};
                bs.outDir = gui.outDir;
                bs.suffix = gui.suffix;
                bs.format = gui.format;
                res = jobs::runBatch(pipe, params, cfg, bs, pump);
                target = (fs::path(gui.outDir) / (fs::path(gui.inputPath).stem().string() +
                                                  gui.suffix + "." + gui.format)).string();
            }

            gui.exporting = false;
            char msg[1024];
            if (res.cancelled)
                snprintf(msg, sizeof(msg), "Export cancelled after %lld", (long long)res.done);
            else if (!res.ok)
                snprintf(msg, sizeof(msg), "%s", res.error.c_str());
            else if (gui.kind == InputKind::Video)
                snprintf(msg, sizeof(msg), "Saved %s  (%lld frames, %.1f s)",
                         fileName(gui.videoOut).c_str(), (long long)res.done, res.wallSeconds);
            else if (gui.kind == InputKind::Images)
                snprintf(msg, sizeof(msg), "Saved %lld images to %s%s", (long long)res.done,
                         gui.outDir.c_str(), res.skipped ? "  (some skipped)" : "");
            else
                snprintf(msg, sizeof(msg), "Saved %s", fileName(target).c_str());
            gui.status = msg;
            gui.statusIsError = !res.ok && !res.cancelled;
            lastExportOk = res.ok && !res.cancelled;
            {
                std::error_code ec;
                const fs::path abs = fs::absolute(target, ec);
                lastExportTarget = lastExportOk ? (ec ? target : abs.string()) : std::string();
            }
            successStatus = lastExportOk ? gui.status : std::string();
            lastExportFromPanel = true;
            exportPreviewKey.clear();   // files now exist: refresh the preview

            // The export left the canvas holding its last frame; put the
            // preview back so the sliders keep acting on what is on screen.
            if (gui.kind == InputKind::Video) loadVideoPreview(pipe, gui, &sourceNote);
            else if (!gui.inputPath.empty()) loadSource(pipe, gui.inputPath, &sourceNote);
            else if (!gui.queue.empty()) loadSource(pipe, gui.queue.front(), &sourceNote);
            pipe.resetTemporal();
        }
    }

    printf("session: %llu frames shown, %llu canvas paints\n",
           (unsigned long long)shownFrames, (unsigned long long)pipe.renderCount());
    fflush(stdout);
    if (logoTex) glDeleteTextures(1, &logoTex);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    pipe.shutdown();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
