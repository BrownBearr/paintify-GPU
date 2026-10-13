// OpenGL-on-Metal for the subset in gl_metal.h. See that header for the
// contract; the notes here are about how it is met.
//
// One command buffer is open at a time. Work is encoded into it lazily: a
// compute encoder for dispatches, a render encoder per framebuffer for draws,
// a blit encoder for copies and uploads, and switching kind ends the current
// encoder. Every resource uses Metal's default hazard tracking, and compute
// encoders are serial, so a dispatch always sees everything encoded before
// it -- which is what all of Paintify's glMemoryBarrier calls ask for.
//
// The buffer is committed only when the CPU has to see a result (a readback,
// glFinish, a query) or a frame is presented, so a paint is one submission.

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Foundation/Foundation.h>

#include "gl_metal.h"
#include "shader_translate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void* pool);

namespace {

// ── half floats ─────────────────────────────────────────────────────────────
uint16_t floatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int32_t e = int32_t((x >> 23) & 0xffu) - 127 + 15;
    uint32_t m = x & 0x7fffffu;
    if (((x >> 23) & 0xffu) == 0xffu) return uint16_t(sign | 0x7c00u | (m ? 0x200u : 0u));
    if (e >= 31) return uint16_t(sign | 0x7c00u);
    if (e <= 0) {
        if (e < -10) return uint16_t(sign);
        m |= 0x800000u;
        const uint32_t shift = uint32_t(14 - e);
        uint32_t h = m >> shift;
        if ((m >> (shift - 1)) & 1u) ++h;   // round half up
        return uint16_t(sign | h);
    }
    uint32_t h = sign | (uint32_t(e) << 10) | (m >> 13);
    if (m & 0x1000u) ++h;                   // round to nearest
    return uint16_t(h);
}

float halfToFloat(uint16_t h) {
    const uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 0x1fu, m = h & 0x3ffu;
    float v;
    if (e == 0u) v = std::ldexp(float(m), -24);
    else if (e == 31u) v = m ? NAN : INFINITY;
    else v = std::ldexp(float(m + 1024u), int(e) - 25);
    return s ? -v : v;
}

// ── formats ─────────────────────────────────────────────────────────────────
struct FormatInfo {
    MTLPixelFormat mtl;
    int channels;
    int bytesPerChannel;   // 1 = unorm8, 2 = half
};

FormatInfo formatInfo(GLenum ifmt) {
    switch (ifmt) {
        case GL_RGBA8: return {MTLPixelFormatRGBA8Unorm, 4, 1};
        case GL_RGBA16F: return {MTLPixelFormatRGBA16Float, 4, 2};
        case GL_RG16F: return {MTLPixelFormatRG16Float, 2, 2};
        case GL_R16F: return {MTLPixelFormatR16Float, 1, 2};
        default:
            fprintf(stderr, "[metal] unsupported texture format 0x%x\n", ifmt);
            return {MTLPixelFormatRGBA8Unorm, 4, 1};
    }
}

int channelsOf(GLenum format) {
    switch (format) {
        case GL_RED: case GL_RED_INTEGER: return 1;
        case GL_RG: return 2;
        case GL_RGB: return 3;
        default: return 4;
    }
}

int bytesOfType(GLenum type) {
    switch (type) {
        case GL_UNSIGNED_BYTE: return 1;
        case GL_HALF_FLOAT: return 2;
        default: return 4;
    }
}

// Client pixel (format, type) -> RGBA float, GL's defaults for missing ones.
void unpackClient(const uint8_t* p, GLenum format, GLenum type, float out[4]) {
    const int n = channelsOf(format);
    out[0] = out[1] = out[2] = 0.f;
    out[3] = 1.f;
    for (int c = 0; c < n; ++c) {
        if (type == GL_UNSIGNED_BYTE) out[c] = float(p[c]) / 255.f;
        else if (type == GL_HALF_FLOAT) { uint16_t h; std::memcpy(&h, p + c * 2, 2); out[c] = halfToFloat(h); }
        else std::memcpy(&out[c], p + c * 4, 4);
    }
}

void packClient(const float in[4], GLenum format, GLenum type, uint8_t* p) {
    const int n = channelsOf(format);
    for (int c = 0; c < n; ++c) {
        if (type == GL_UNSIGNED_BYTE) {
            const float v = std::min(std::max(in[c], 0.f), 1.f);
            p[c] = uint8_t(std::lround(v * 255.f));
        } else if (type == GL_HALF_FLOAT) {
            const uint16_t h = floatToHalf(in[c]);
            std::memcpy(p + c * 2, &h, 2);
        } else {
            std::memcpy(p + c * 4, &in[c], 4);
        }
    }
}

void unpackTexel(const uint8_t* p, const FormatInfo& f, float out[4]) {
    out[0] = out[1] = out[2] = 0.f;
    out[3] = 1.f;
    for (int c = 0; c < f.channels; ++c) {
        if (f.bytesPerChannel == 1) out[c] = float(p[c]) / 255.f;
        else { uint16_t h; std::memcpy(&h, p + c * 2, 2); out[c] = halfToFloat(h); }
    }
}

void packTexel(const float in[4], const FormatInfo& f, uint8_t* p) {
    for (int c = 0; c < f.channels; ++c) {
        if (f.bytesPerChannel == 1) {
            const float v = std::min(std::max(in[c], 0.f), 1.f);
            p[c] = uint8_t(std::lround(v * 255.f));
        } else {
            const uint16_t h = floatToHalf(in[c]);
            std::memcpy(p + c * 2, &h, 2);
        }
    }
}

// ── objects ─────────────────────────────────────────────────────────────────
struct Texture {
    id<MTLTexture> tex = nil;
    GLenum target = GL_TEXTURE_2D;
    GLenum ifmt = 0;
    int w = 0, h = 0, layers = 1, levels = 1;
    GLenum minF = GL_NEAREST_MIPMAP_LINEAR, magF = GL_LINEAR;
    GLenum wrapS = GL_REPEAT, wrapT = GL_REPEAT;
};

struct Buffer {
    id<MTLBuffer> buf = nil;
    size_t size = 0;
    // Uniform blocks live on the CPU and are copied into each dispatch, so a
    // glNamedBufferSubData between two dispatches behaves as it does in GL.
    std::vector<uint8_t> shadow;
    bool uniform = false;
};

struct Framebuffer {
    GLuint color[2] = {0, 0};
};

struct Shader {
    GLenum type = 0;
    std::string source;
    bool ok = false;
    std::string log;
    mslx::Result xlat;
    id<MTLFunction> fn = nil;
};

struct StageInfo {
    std::vector<mslx::Resource> resources;
    bool bufferSizes = false;     // the shader calls .length() on an SSBO
    std::vector<uint8_t> block;   // the loose-uniform block's bytes
};

struct Program {
    bool ok = false;
    bool compute = false;
    std::string log;
    std::vector<GLuint> attached;
    id<MTLComputePipelineState> cps = nil;
    id<MTLFunction> vs = nil, fs = nil;
    StageInfo stage[2];               // compute uses [0]; raster: [0] vertex, [1] fragment
    MTLSize local = MTLSizeMake(1, 1, 1);
    // name -> location; location -> where it lives in each stage's block
    std::map<std::string, GLint> locations;
    struct Slot { int stage; uint32_t offset, size; };
    std::vector<std::vector<Slot>> slots;
    std::map<uint64_t, id<MTLRenderPipelineState>> rps;
};

struct Query {
    id<MTLCommandBuffer> startCb = nil;   // TIME_ELAPSED: first buffer of the range
    id<MTLCommandBuffer> endCb = nil;     // last buffer (TIMESTAMP: the only one)
    bool elapsed = false;
    double hostTime = 0.0;                // TIMESTAMP with no GPU work before it
};

template <class T>
struct Table {
    std::unordered_map<GLuint, std::unique_ptr<T>> items;
    GLuint next = 1;
    GLuint create() { const GLuint name = next++; items[name] = std::make_unique<T>(); return name; }
    T* get(GLuint name) {
        auto it = items.find(name);
        return it == items.end() ? nullptr : it->second.get();
    }
    void erase(GLuint name) { items.erase(name); }
};

enum class EncKind { None, Compute, Render, Blit };

struct Context {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    bool unified = true;
    std::string name;

    Table<Texture> textures;
    Table<Buffer> buffers;
    Table<Framebuffer> fbos;
    Table<Shader> shaders;
    Table<Program> programs;
    Table<Query> queries;
    GLuint nextVao = 1;

    // binding state
    GLuint program = 0;
    GLuint texUnits[16] = {};
    GLuint imageUnits[8] = {};
    GLuint ssbo[16] = {};
    GLuint ubo[4] = {};
    std::map<GLenum, GLuint> bufferTarget;
    GLuint boundTex2D = 0;
    GLuint drawFbo = 0;
    GLint viewport[4] = {0, 0, 0, 0};
    GLint scissor[4] = {0, 0, 0, 0};
    bool scissorOn = false, blendOn = false;
    GLenum blendSrc[2] = {GL_ONE, GL_ONE}, blendDst[2] = {GL_ZERO, GL_ZERO};
    float clearColor[4] = {0.f, 0.f, 0.f, 0.f};
    GLuint activeElapsed = 0;

    // encoding state
    id<MTLCommandBuffer> cb = nil;
    bool cbHasWork = false;            // an encoder was opened on `cb`
    id<MTLCommandBuffer> lastCb = nil; // most recently committed buffer with work
    EncKind kind = EncKind::None;
    id<MTLComputeCommandEncoder> compute = nil;
    id<MTLRenderCommandEncoder> render = nil;
    GLuint renderFbo = 0;             // the fbo `render` targets; 0 = screen
    id<MTLBlitCommandEncoder> blit = nil;
    id<MTLBuffer> dummy = nil;        // bound to unbound buffer slots
    // Bound to sampler / image slots the current pass does not use. GL reads
    // zero from an unbound unit; Metal leaves an empty slot undefined.
    id<MTLTexture> dummy2D = nil, dummyArray = nil;
    id<MTLSamplerState> dummySampler = nil;

    std::map<uint32_t, id<MTLSamplerState>> samplers;

    // screen
    CAMetalLayer* layer = nil;
    id<CAMetalDrawable> drawable = nil;
    MTLRenderPassDescriptor* screenDesc = nil;
    int fbW = 0, fbH = 0;
    bool screenCleared = false;
    id<MTLRenderPipelineState> quadPso = nil;
    id<MTLSamplerState> quadNearest = nil, quadLinear = nil;
};

Context* g = nullptr;

void warnOnce(const char* what) {
    static std::map<std::string, bool> seen;
    if (seen[what]) return;
    seen[what] = true;
    fprintf(stderr, "[metal] %s\n", what);
}

// ── command stream ──────────────────────────────────────────────────────────
id<MTLCommandBuffer> cmd() {
    if (!g->cb) {
        @autoreleasepool {
            g->cb = [g->queue commandBuffer];
        }
    }
    return g->cb;
}

void endEncoder() {
    switch (g->kind) {
        case EncKind::Compute: [g->compute endEncoding]; g->compute = nil; break;
        case EncKind::Render: [g->render endEncoding]; g->render = nil; break;
        case EncKind::Blit: [g->blit endEncoding]; g->blit = nil; break;
        default: break;
    }
    g->kind = EncKind::None;
}

id<MTLComputeCommandEncoder> computeEnc() {
    if (g->kind != EncKind::Compute) {
        endEncoder();
        @autoreleasepool {
            g->compute = [cmd() computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial];
        }
        g->cbHasWork = true;
        g->kind = EncKind::Compute;
    }
    return g->compute;
}

id<MTLBlitCommandEncoder> blitEnc() {
    if (g->kind != EncKind::Blit) {
        endEncoder();
        @autoreleasepool {
            g->blit = [cmd() blitCommandEncoder];
        }
        g->cbHasWork = true;
        g->kind = EncKind::Blit;
    }
    return g->blit;
}

void reportErrors(id<MTLCommandBuffer> cb) {
    if (cb.status == MTLCommandBufferStatusError) {
        fprintf(stderr, "[metal] command buffer failed: %s\n",
                cb.error.localizedDescription.UTF8String);
    }
}

// Submits what is encoded. `wait` blocks until the GPU has finished it, which
// (the queue being in order) means everything submitted before it as well.
void submit(bool wait) {
    endEncoder();
    if (!g->cb) return;
    id<MTLCommandBuffer> cb = g->cb;
    g->cb = nil;
    if (g->cbHasWork) g->lastCb = cb;
    g->cbHasWork = false;
    [cb commit];
    if (wait) {
        [cb waitUntilCompleted];
        reportErrors(cb);
    }
}

void finishAll() {
    if (g->cb) { submit(true); return; }
    // Nothing open: an empty buffer still waits for everything before it.
    @autoreleasepool {
        id<MTLCommandBuffer> cb = [g->queue commandBuffer];
        [cb commit];
        [cb waitUntilCompleted];
    }
}

// ── helpers ─────────────────────────────────────────────────────────────────
id<MTLBuffer> newBuffer(size_t size, bool cpuVisible) {
    const MTLResourceOptions opts = (cpuVisible || g->unified) ? MTLResourceStorageModeShared
                                                              : MTLResourceStorageModePrivate;
    return [g->device newBufferWithLength:std::max<size_t>(size, 16) options:opts];
}

id<MTLBuffer> staging(const void* data, size_t size) {
    return [g->device newBufferWithBytes:data length:std::max<size_t>(size, 4)
                                 options:MTLResourceStorageModeShared];
}

MTLSamplerMinMagFilter minMag(GLenum f) {
    return (f == GL_NEAREST || f == GL_NEAREST_MIPMAP_NEAREST || f == GL_NEAREST_MIPMAP_LINEAR)
               ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
}

id<MTLSamplerState> samplerFor(const Texture& t) {
    const uint32_t key = (uint32_t(t.minF & 0xff) << 0) | (uint32_t(t.magF & 0xff) << 8) |
                         (uint32_t(t.wrapS == GL_REPEAT) << 16) | (uint32_t(t.wrapT == GL_REPEAT) << 17) |
                         (uint32_t(t.levels > 1) << 18);
    auto it = g->samplers.find(key);
    if (it != g->samplers.end()) return it->second;
    MTLSamplerDescriptor* d = [MTLSamplerDescriptor new];
    d.minFilter = minMag(t.minF);
    d.magFilter = minMag(t.magF);
    switch (t.minF) {
        case GL_NEAREST_MIPMAP_NEAREST: case GL_LINEAR_MIPMAP_NEAREST:
            d.mipFilter = MTLSamplerMipFilterNearest; break;
        case GL_NEAREST_MIPMAP_LINEAR: case GL_LINEAR_MIPMAP_LINEAR:
            d.mipFilter = MTLSamplerMipFilterLinear; break;
        default: d.mipFilter = MTLSamplerMipFilterNotMipmapped; break;
    }
    d.sAddressMode = t.wrapS == GL_REPEAT ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
    d.tAddressMode = t.wrapT == GL_REPEAT ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
    d.rAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> s = [g->device newSamplerStateWithDescriptor:d];
    g->samplers[key] = s;
    return s;
}

void allocTexture(Texture& t, GLenum ifmt, int levels, int w, int h, int layers) {
    const FormatInfo f = formatInfo(ifmt);
    MTLTextureDescriptor* d = [MTLTextureDescriptor new];
    d.textureType = t.target == GL_TEXTURE_2D_ARRAY ? MTLTextureType2DArray : MTLTextureType2D;
    d.pixelFormat = f.mtl;
    d.width = NSUInteger(std::max(1, w));
    d.height = NSUInteger(std::max(1, h));
    d.arrayLength = NSUInteger(t.target == GL_TEXTURE_2D_ARRAY ? std::max(1, layers) : 1);
    d.mipmapLevelCount = NSUInteger(std::max(1, levels));
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
    t.tex = [g->device newTextureWithDescriptor:d];
    t.ifmt = ifmt;
    t.w = w; t.h = h; t.layers = layers; t.levels = std::max(1, levels);
}

void uploadRegion(Texture& t, int level, int x, int y, int z, int w, int h, int d,
                  GLenum format, GLenum type, const void* pixels) {
    if (!t.tex || !pixels || w <= 0 || h <= 0 || d <= 0) return;
    const FormatInfo f = formatInfo(t.ifmt);
    const size_t texelBytes = size_t(f.channels * f.bytesPerChannel);
    const size_t rowBytes = size_t(w) * texelBytes;
    const size_t sliceBytes = rowBytes * size_t(h);
    std::vector<uint8_t> conv;
    const uint8_t* src = static_cast<const uint8_t*>(pixels);
    const bool direct = (f.bytesPerChannel == 1 && type == GL_UNSIGNED_BYTE &&
                         channelsOf(format) == f.channels) ||
                        (f.bytesPerChannel == 2 && type == GL_HALF_FLOAT &&
                         channelsOf(format) == f.channels);
    if (!direct) {
        conv.resize(sliceBytes * size_t(d));
        const size_t clientTexel = size_t(channelsOf(format) * bytesOfType(type));
        const size_t n = size_t(w) * size_t(h) * size_t(d);
        for (size_t i = 0; i < n; ++i) {
            float px[4];
            unpackClient(src + i * clientTexel, format, type, px);
            packTexel(px, f, conv.data() + i * texelBytes);
        }
        src = conv.data();
    }
    @autoreleasepool {
        id<MTLBuffer> stage = staging(src, sliceBytes * size_t(d));
        id<MTLBlitCommandEncoder> b = blitEnc();
        for (int s = 0; s < d; ++s) {
            [b copyFromBuffer:stage
                 sourceOffset:NSUInteger(sliceBytes * size_t(s))
            sourceBytesPerRow:rowBytes
          sourceBytesPerImage:sliceBytes
                   sourceSize:MTLSizeMake(NSUInteger(w), NSUInteger(h), 1)
                    toTexture:t.tex
             destinationSlice:NSUInteger(z + s)
             destinationLevel:NSUInteger(level)
            destinationOrigin:MTLOriginMake(NSUInteger(x), NSUInteger(y), 0)];
        }
    }
}

// Reads `size` bytes of a buffer back to the CPU, waiting for the GPU.
void readBuffer(Buffer& b, size_t offset, size_t size, void* out) {
    if (b.uniform) {
        std::memcpy(out, b.shadow.data() + offset, std::min(size, b.shadow.size() - offset));
        return;
    }
    if (b.buf.storageMode == MTLStorageModeShared) {
        finishAll();
        std::memcpy(out, static_cast<const uint8_t*>(b.buf.contents) + offset, size);
        return;
    }
    @autoreleasepool {
        id<MTLBuffer> st = newBuffer(size, true);
        [blitEnc() copyFromBuffer:b.buf sourceOffset:offset toBuffer:st destinationOffset:0 size:size];
        submit(true);
        std::memcpy(out, st.contents, size);
    }
}

// ── the screen quad: glClear and glBlitNamedFramebuffer onto the drawable ──
const char* kQuadSource = R"(
#include <metal_stdlib>
using namespace metal;
struct QuadArgs { float4 rect; float4 uv; float4 color; float4 mode; };
struct VOut { float4 pos [[position]]; float2 uv; };
vertex VOut quad_vs(uint vid [[vertex_id]], constant QuadArgs& a [[buffer(0)]]) {
    float2 c = float2(float(vid & 1u), float(vid >> 1));
    VOut o;
    o.pos = float4(mix(a.rect.xy, a.rect.zw, c), 0.0, 1.0);
    o.uv = mix(a.uv.xy, a.uv.zw, c);
    return o;
}
fragment float4 quad_fs(VOut in [[stage_in]], constant QuadArgs& a [[buffer(0)]],
                        texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
    if (a.mode.x > 0.5) return float4(t.sample(s, in.uv).rgb, 1.0);
    return a.color;
}
)";

struct QuadArgs { float rect[4]; float uv[4]; float color[4]; float mode[4]; };

bool buildQuadPipeline() {
    NSError* err = nil;
    id<MTLLibrary> lib = [g->device newLibraryWithSource:@(kQuadSource) options:nil error:&err];
    if (!lib) {
        fprintf(stderr, "[metal] quad shader: %s\n", err.localizedDescription.UTF8String);
        return false;
    }
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"quad_vs"];
    d.fragmentFunction = [lib newFunctionWithName:@"quad_fs"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    g->quadPso = [g->device newRenderPipelineStateWithDescriptor:d error:&err];
    if (!g->quadPso) {
        fprintf(stderr, "[metal] quad pipeline: %s\n", err.localizedDescription.UTF8String);
        return false;
    }
    MTLSamplerDescriptor* s = [MTLSamplerDescriptor new];
    s.sAddressMode = s.tAddressMode = MTLSamplerAddressModeClampToEdge;
    s.minFilter = s.magFilter = MTLSamplerMinMagFilterNearest;
    g->quadNearest = [g->device newSamplerStateWithDescriptor:s];
    s.minFilter = s.magFilter = MTLSamplerMinMagFilterLinear;
    g->quadLinear = [g->device newSamplerStateWithDescriptor:s];
    return true;
}

id<MTLRenderCommandEncoder> screenEnc() {
    if (!g->drawable) return nil;
    if (g->kind == EncKind::Render && g->renderFbo == 0) return g->render;
    endEncoder();
    MTLRenderPassDescriptor* d = g->screenDesc;
    // The first pass of a frame clears (to the GL clear colour); a pass that
    // resumes after compute work loads what is already there.
    d.colorAttachments[0].loadAction = g->screenCleared ? MTLLoadActionLoad : MTLLoadActionClear;
    d.colorAttachments[0].clearColor =
        MTLClearColorMake(g->clearColor[0], g->clearColor[1], g->clearColor[2], 1.0);
    g->screenCleared = true;
    @autoreleasepool {
        g->render = [cmd() renderCommandEncoderWithDescriptor:d];
    }
    g->cbHasWork = true;
    g->kind = EncKind::Render;
    g->renderFbo = 0;
    return g->render;
}

// GL window rect (bottom-left origin) -> Metal scissor; false when empty.
bool screenScissor(const GLint r[4], MTLScissorRect* out) {
    int x0 = std::max(0, r[0]), x1 = std::min(g->fbW, r[0] + r[2]);
    int yTop = std::max(0, g->fbH - (r[1] + r[3])), yBot = std::min(g->fbH, g->fbH - r[1]);
    if (x1 <= x0 || yBot <= yTop) return false;
    *out = MTLScissorRect{NSUInteger(x0), NSUInteger(yTop), NSUInteger(x1 - x0), NSUInteger(yBot - yTop)};
    return true;
}

void drawScreenQuad(const QuadArgs& a, id<MTLTexture> tex, id<MTLSamplerState> smp) {
    id<MTLRenderCommandEncoder> e = screenEnc();
    if (!e || !g->quadPso) return;
    MTLScissorRect sc{0, 0, NSUInteger(g->fbW), NSUInteger(g->fbH)};
    if (g->scissorOn && !screenScissor(g->scissor, &sc)) return;
    [e setViewport:MTLViewport{0.0, 0.0, double(g->fbW), double(g->fbH), 0.0, 1.0}];
    [e setScissorRect:sc];
    [e setRenderPipelineState:g->quadPso];
    [e setVertexBytes:&a length:sizeof(a) atIndex:0];
    [e setFragmentBytes:&a length:sizeof(a) atIndex:0];
    if (tex) {
        [e setFragmentTexture:tex atIndex:0];
        [e setFragmentSamplerState:smp atIndex:0];
    }
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
}

// ── resource binding for a dispatch or draw ─────────────────────────────────
template <class SetBuffer, class SetBytes, class SetTexture, class SetSampler>
void bindStage(StageInfo& st, SetBuffer setBuffer, SetBytes setBytes, SetTexture setTexture,
               SetSampler setSampler) {
    uint32_t sizes[mslx::kBufferSizeSlot] = {};
    for (const mslx::Resource& r : st.resources) {
        switch (r.kind) {
            case mslx::ResKind::Storage: {
                Buffer* b = r.glBinding < 16 ? g->buffers.get(g->ssbo[r.glBinding]) : nullptr;
                if (b && b->buf) {
                    setBuffer(b->buf, r.slot);
                    if (r.slot < mslx::kBufferSizeSlot) sizes[r.slot] = uint32_t(b->size);
                } else {
                    // Legal in GL when the pass never touches it.
                    setBuffer(g->dummy, r.slot);
                }
                break;
            }
            case mslx::ResKind::Uniform: {
                Buffer* b = r.glBinding < 4 ? g->buffers.get(g->ubo[r.glBinding]) : nullptr;
                if (b && b->uniform) setBytes(b->shadow.data(), b->shadow.size(), r.slot);
                else if (b && b->buf) setBuffer(b->buf, r.slot);
                else { warnOnce("dispatch with an unbound uniform buffer"); setBuffer(g->dummy, r.slot); }
                break;
            }
            case mslx::ResKind::Sampler: {
                Texture* t = r.glBinding < 16 ? g->textures.get(g->texUnits[r.glBinding]) : nullptr;
                if (t && t->tex) {
                    setTexture(t->tex, r.slot);
                    setSampler(samplerFor(*t), r.glBinding);
                } else {
                    setTexture(r.array ? g->dummyArray : g->dummy2D, r.slot);
                    setSampler(g->dummySampler, r.glBinding);
                }
                break;
            }
            case mslx::ResKind::Image: {
                Texture* t = r.glBinding < 8 ? g->textures.get(g->imageUnits[r.glBinding]) : nullptr;
                // A write-only dummy would still need the right format; an
                // unbound image is only ever declared, never stored to, so
                // any texture keeps the slot defined.
                setTexture(t && t->tex ? t->tex : g->dummy2D, r.slot);
                break;
            }
        }
    }
    if (!st.block.empty()) setBytes(st.block.data(), st.block.size(), mslx::kDefaultBlockSlot);
    if (st.bufferSizes) setBytes(sizes, sizeof(sizes), mslx::kBufferSizeSlot);
}

MTLBlendFactor blendFactor(GLenum f) {
    switch (f) {
        case GL_ZERO: return MTLBlendFactorZero;
        case GL_SRC_ALPHA: return MTLBlendFactorSourceAlpha;
        case GL_ONE_MINUS_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
        default: return MTLBlendFactorOne;
    }
}

id<MTLRenderPipelineState> renderPipeline(Program& p, const Framebuffer& fb) {
    uint64_t key = g->blendOn ? 1u : 0u;
    for (int i = 0; i < 2; ++i) {
        key = key * 4099u + g->blendSrc[i];
        key = key * 4099u + g->blendDst[i];
        const Texture* t = g->textures.get(fb.color[i]);
        key = key * 131u + (t ? uint64_t(formatInfo(t->ifmt).mtl) : 0u);
    }
    auto it = p.rps.find(key);
    if (it != p.rps.end()) return it->second;

    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = p.vs;
    d.fragmentFunction = p.fs;
    for (int i = 0; i < 2; ++i) {
        const Texture* t = g->textures.get(fb.color[i]);
        if (!t) continue;
        MTLRenderPipelineColorAttachmentDescriptor* a = d.colorAttachments[i];
        a.pixelFormat = formatInfo(t->ifmt).mtl;
        a.blendingEnabled = g->blendOn;
        a.sourceRGBBlendFactor = a.sourceAlphaBlendFactor = blendFactor(g->blendSrc[i]);
        a.destinationRGBBlendFactor = a.destinationAlphaBlendFactor = blendFactor(g->blendDst[i]);
        a.rgbBlendOperation = a.alphaBlendOperation = MTLBlendOperationAdd;
    }
    NSError* err = nil;
    id<MTLRenderPipelineState> pso = [g->device newRenderPipelineStateWithDescriptor:d error:&err];
    if (!pso) fprintf(stderr, "[metal] render pipeline: %s\n", err.localizedDescription.UTF8String);
    p.rps[key] = pso;
    return pso;
}

id<MTLRenderCommandEncoder> renderEnc(GLuint fboId) {
    if (g->kind == EncKind::Render && g->renderFbo == fboId) return g->render;
    endEncoder();
    Framebuffer* fb = g->fbos.get(fboId);
    if (!fb) return nil;
    MTLRenderPassDescriptor* d = [MTLRenderPassDescriptor renderPassDescriptor];
    for (int i = 0; i < 2; ++i) {
        Texture* t = g->textures.get(fb->color[i]);
        if (!t) continue;
        d.colorAttachments[i].texture = t->tex;
        d.colorAttachments[i].loadAction = MTLLoadActionLoad;
        d.colorAttachments[i].storeAction = MTLStoreActionStore;
    }
    @autoreleasepool {
        g->render = [cmd() renderCommandEncoderWithDescriptor:d];
    }
    g->cbHasWork = true;
    g->kind = EncKind::Render;
    g->renderFbo = fboId;
    return g->render;
}

Texture* tex(GLuint name) {
    Texture* t = g->textures.get(name);
    if (!t) warnOnce("operation on an unknown texture name");
    return t;
}

Buffer* buf(GLuint name) {
    Buffer* b = g->buffers.get(name);
    if (!b) warnOnce("operation on an unknown buffer name");
    return b;
}

void writeUniform(GLint loc, const void* data, uint32_t size) {
    Program* p = g->programs.get(g->program);
    if (!p || loc < 0 || size_t(loc) >= p->slots.size()) return;
    for (const Program::Slot& s : p->slots[size_t(loc)]) {
        std::vector<uint8_t>& block = p->stage[s.stage].block;
        if (s.offset + std::min(size, s.size) <= block.size())
            std::memcpy(block.data() + s.offset, data, std::min(size, s.size));
    }
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// mgl
// ═══════════════════════════════════════════════════════════════════════════
namespace mgl {

bool init() {
    if (g) return true;
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) return false;
        g = new Context();
        g->device = dev;
        g->queue = [dev newCommandQueue];
        g->unified = dev.hasUnifiedMemory;
        g->name = dev.name.UTF8String;
        g->dummy = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
        std::memset(g->dummy.contents, 0, 256);
        MTLTextureDescriptor* td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                               width:1 height:1 mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        td.storageMode = MTLStorageModeShared;
        g->dummy2D = [dev newTextureWithDescriptor:td];
        td.textureType = MTLTextureType2DArray;
        g->dummyArray = [dev newTextureWithDescriptor:td];
        const uint16_t zero[4] = {0, 0, 0, 0};
        [g->dummy2D replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:zero bytesPerRow:8];
        [g->dummyArray replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:0
                           withBytes:zero bytesPerRow:8 bytesPerImage:8];
        g->dummySampler = [dev newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
        buildQuadPipeline();
    }
    return true;
}

const char* deviceName() { return g ? g->name.c_str() : "none"; }
void* device() { return g ? (__bridge void*)g->device : nullptr; }

void attachLayer(void* layer) {
    g->layer = (__bridge CAMetalLayer*)layer;
    g->layer.device = g->device;
    g->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    g->layer.framebufferOnly = YES;
    // The GL build writes sRGB-encoded values to a plain 8-bit framebuffer;
    // tagging the layer sRGB displays those same values the same way.
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    g->layer.colorspace = cs;
    CGColorSpaceRelease(cs);
    g->screenDesc = [MTLRenderPassDescriptor renderPassDescriptor];
    g->screenDesc.colorAttachments[0].loadAction = MTLLoadActionClear;
    g->screenDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
}

bool beginFrame(int fbWidth, int fbHeight) {
    if (!g || !g->layer || fbWidth <= 0 || fbHeight <= 0) return false;
    const CGSize want = CGSizeMake(fbWidth, fbHeight);
    if (!CGSizeEqualToSize(g->layer.drawableSize, want)) g->layer.drawableSize = want;
    @autoreleasepool {
        g->drawable = [g->layer nextDrawable];
    }
    if (!g->drawable) return false;
    g->fbW = fbWidth;
    g->fbH = fbHeight;
    g->screenCleared = false;
    g->screenDesc.colorAttachments[0].texture = g->drawable.texture;
    return true;
}

void* screenPassDescriptor() { return (__bridge void*)g->screenDesc; }
void* screenEncoder() { return (__bridge void*)screenEnc(); }
void* commandBuffer() { return (__bridge void*)cmd(); }

void endFrame() {
    if (!g || !g->drawable) return;
    screenEnc();   // even an empty frame must present something cleared
    endEncoder();
    [cmd() presentDrawable:g->drawable];
    submit(false);
    g->drawable = nil;
    g->screenDesc.colorAttachments[0].texture = nil;
}

void* textureObject(GLuint t) {
    Texture* x = g ? g->textures.get(t) : nullptr;
    return x ? (__bridge void*)x->tex : nullptr;
}

void* poolPush() { return objc_autoreleasePoolPush(); }
void poolPop(void* pool) { objc_autoreleasePoolPop(pool); }

} // namespace mgl

// ═══════════════════════════════════════════════════════════════════════════
// GL
// ═══════════════════════════════════════════════════════════════════════════

const GLubyte* glGetString(GLenum name) {
    static std::string renderer, version = "Metal (Paintify GL 4.6 subset)";
    switch (name) {
        case GL_RENDERER: renderer = mgl::deviceName(); return (const GLubyte*)renderer.c_str();
        case GL_VENDOR: return (const GLubyte*)"Apple";
        default: return (const GLubyte*)version.c_str();
    }
}

void glEnable(GLenum cap) {
    if (cap == GL_SCISSOR_TEST) g->scissorOn = true;
    else if (cap == GL_BLEND) g->blendOn = true;
}
void glDisable(GLenum cap) {
    if (cap == GL_SCISSOR_TEST) g->scissorOn = false;
    else if (cap == GL_BLEND) g->blendOn = false;
}
GLboolean glIsEnabled(GLenum cap) {
    if (cap == GL_SCISSOR_TEST) return g->scissorOn;
    if (cap == GL_BLEND) return g->blendOn;
    return GL_FALSE;
}
void glGetIntegerv(GLenum pname, GLint* data) {
    if (pname == GL_SCISSOR_BOX) std::memcpy(data, g->scissor, sizeof(g->scissor));
    else if (pname == GL_VIEWPORT) std::memcpy(data, g->viewport, sizeof(g->viewport));
    else *data = 0;
}
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    g->viewport[0] = x; g->viewport[1] = y; g->viewport[2] = w; g->viewport[3] = h;
}
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    g->scissor[0] = x; g->scissor[1] = y; g->scissor[2] = w; g->scissor[3] = h;
}
void glClearColor(GLfloat r, GLfloat gg, GLfloat b, GLfloat a) {
    g->clearColor[0] = r; g->clearColor[1] = gg; g->clearColor[2] = b; g->clearColor[3] = a;
}

void glClear(GLbitfield mask) {
    if (!(mask & GL_COLOR_BUFFER_BIT)) return;
    if (g->drawFbo != 0) { warnOnce("glClear on an offscreen framebuffer"); return; }
    if (!g->drawable) return;
    const bool whole = !g->scissorOn || (g->scissor[0] <= 0 && g->scissor[1] <= 0 &&
                                         g->scissor[2] >= g->fbW && g->scissor[3] >= g->fbH);
    if (whole && !(g->kind == EncKind::Render && g->renderFbo == 0) && !g->screenCleared) {
        screenEnc();   // the pass' own clear does it
        return;
    }
    QuadArgs a{};
    a.rect[0] = -1.f; a.rect[1] = -1.f; a.rect[2] = 1.f; a.rect[3] = 1.f;
    std::memcpy(a.color, g->clearColor, sizeof(a.color));
    a.color[3] = 1.f;
    drawScreenQuad(a, nil, nil);
}

void glFinish() {
    if (g) finishAll();
}
void glMemoryBarrier(GLbitfield) {}
void glTextureBarrier() {}
void glBlendFunci(GLuint b, GLenum src, GLenum dst) {
    if (b < 2) { g->blendSrc[b] = src; g->blendDst[b] = dst; }
}

// ── textures ────────────────────────────────────────────────────────────────
void glCreateTextures(GLenum target, GLsizei n, GLuint* out) {
    for (GLsizei i = 0; i < n; ++i) {
        out[i] = g->textures.create();
        g->textures.get(out[i])->target = target;
    }
}
void glGenTextures(GLsizei n, GLuint* out) { glCreateTextures(GL_TEXTURE_2D, n, out); }
void glDeleteTextures(GLsizei n, const GLuint* ids) {
    for (GLsizei i = 0; i < n; ++i) {
        if (!ids[i]) continue;
        for (GLuint& u : g->texUnits) if (u == ids[i]) u = 0;
        for (GLuint& u : g->imageUnits) if (u == ids[i]) u = 0;
        g->textures.erase(ids[i]);   // in-flight work keeps its own reference
    }
}
void glBindTexture(GLenum, GLuint t) { g->boundTex2D = t; }

void glTextureStorage2D(GLuint t, GLsizei levels, GLenum ifmt, GLsizei w, GLsizei h) {
    if (Texture* x = tex(t)) @autoreleasepool { allocTexture(*x, ifmt, levels, w, h, 1); }
}
void glTextureStorage3D(GLuint t, GLsizei levels, GLenum ifmt, GLsizei w, GLsizei h, GLsizei d) {
    if (Texture* x = tex(t)) @autoreleasepool { allocTexture(*x, ifmt, levels, w, h, d); }
}
void glTexImage2D(GLenum, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint, GLenum format,
                  GLenum type, const void* pixels) {
    Texture* x = tex(g->boundTex2D);
    if (!x || level != 0) return;
    // Allocated with a full chain so a following glGenerateMipmap has room.
    int levels = 1;
    for (int m = std::max(w, h); m > 1; m >>= 1) ++levels;
    @autoreleasepool {
        allocTexture(*x, GLenum(ifmt), levels, w, h, 1);
        uploadRegion(*x, 0, 0, 0, 0, w, h, 1, format, type, pixels);
    }
}
void glTextureSubImage2D(GLuint t, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                         GLenum format, GLenum type, const void* pixels) {
    if (Texture* tx = tex(t)) uploadRegion(*tx, level, x, y, 0, w, h, 1, format, type, pixels);
}
void glTextureSubImage3D(GLuint t, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h,
                         GLsizei d, GLenum format, GLenum type, const void* pixels) {
    if (Texture* tx = tex(t)) uploadRegion(*tx, level, x, y, z, w, h, d, format, type, pixels);
}
void glTextureParameteri(GLuint t, GLenum pname, GLint param) {
    Texture* x = tex(t);
    if (!x) return;
    switch (pname) {
        case GL_TEXTURE_MIN_FILTER: x->minF = GLenum(param); break;
        case GL_TEXTURE_MAG_FILTER: x->magF = GLenum(param); break;
        case GL_TEXTURE_WRAP_S: x->wrapS = GLenum(param); break;
        case GL_TEXTURE_WRAP_T: x->wrapT = GLenum(param); break;
        default: break;
    }
}
void glTexParameteri(GLenum, GLenum pname, GLint param) {
    glTextureParameteri(g->boundTex2D, pname, param);
}
void glGenerateTextureMipmap(GLuint t) {
    Texture* x = tex(t);
    if (!x || !x->tex || x->levels < 2) return;
    [blitEnc() generateMipmapsForTexture:x->tex];
}
void glGenerateMipmap(GLenum) { glGenerateTextureMipmap(g->boundTex2D); }
void glBindTextureUnit(GLuint unit, GLuint t) { if (unit < 16) g->texUnits[unit] = t; }
void glBindImageTexture(GLuint unit, GLuint t, GLint level, GLboolean, GLint, GLenum, GLenum) {
    if (level != 0) warnOnce("image binding of a non-zero level is not supported");
    if (unit < 8) g->imageUnits[unit] = t;
}

void glClearTexImage(GLuint t, GLint level, GLenum format, GLenum type, const void* data) {
    Texture* x = tex(t);
    if (!x || !x->tex) return;
    float c[4] = {0.f, 0.f, 0.f, 0.f};
    if (data) unpackClient(static_cast<const uint8_t*>(data), format, type, c);
    endEncoder();
    @autoreleasepool {
        MTLRenderPassDescriptor* d = [MTLRenderPassDescriptor renderPassDescriptor];
        d.colorAttachments[0].texture = x->tex;
        d.colorAttachments[0].level = NSUInteger(level);
        d.colorAttachments[0].loadAction = MTLLoadActionClear;
        d.colorAttachments[0].storeAction = MTLStoreActionStore;
        d.colorAttachments[0].clearColor = MTLClearColorMake(c[0], c[1], c[2], c[3]);
        [[cmd() renderCommandEncoderWithDescriptor:d] endEncoding];
    }
    g->cbHasWork = true;
}

void glCopyImageSubData(GLuint src, GLenum, GLint srcLevel, GLint sx, GLint sy, GLint sz,
                        GLuint dst, GLenum, GLint dstLevel, GLint dx, GLint dy, GLint dz,
                        GLsizei w, GLsizei h, GLsizei d) {
    Texture* a = tex(src);
    Texture* b = tex(dst);
    if (!a || !b || !a->tex || !b->tex) return;
    id<MTLBlitCommandEncoder> e = blitEnc();
    for (GLsizei s = 0; s < std::max(1, d); ++s) {
        [e copyFromTexture:a->tex
               sourceSlice:NSUInteger(sz + s)
               sourceLevel:NSUInteger(srcLevel)
              sourceOrigin:MTLOriginMake(NSUInteger(sx), NSUInteger(sy), 0)
                sourceSize:MTLSizeMake(NSUInteger(w), NSUInteger(h), 1)
                 toTexture:b->tex
          destinationSlice:NSUInteger(dz + s)
          destinationLevel:NSUInteger(dstLevel)
         destinationOrigin:MTLOriginMake(NSUInteger(dx), NSUInteger(dy), 0)];
    }
}

void glGetTextureImage(GLuint t, GLint level, GLenum format, GLenum type, GLsizei bufSize,
                       void* pixels) {
    Texture* x = tex(t);
    if (!x || !x->tex) return;
    const int w = std::max(1, x->w >> level), h = std::max(1, x->h >> level);
    const FormatInfo f = formatInfo(x->ifmt);
    const size_t texel = size_t(f.channels * f.bytesPerChannel);
    const size_t rowBytes = size_t(w) * texel;
    const size_t clientTexel = size_t(channelsOf(format) * bytesOfType(type));
    if (size_t(bufSize) < clientTexel * size_t(w) * size_t(h)) {
        warnOnce("glGetTextureImage: buffer too small");
        return;
    }
    @autoreleasepool {
        id<MTLBuffer> st = [g->device newBufferWithLength:rowBytes * size_t(h)
                                                  options:MTLResourceStorageModeShared];
        [blitEnc() copyFromTexture:x->tex
                       sourceSlice:0
                       sourceLevel:NSUInteger(level)
                      sourceOrigin:MTLOriginMake(0, 0, 0)
                        sourceSize:MTLSizeMake(NSUInteger(w), NSUInteger(h), 1)
                          toBuffer:st
                 destinationOffset:0
            destinationBytesPerRow:rowBytes
          destinationBytesPerImage:rowBytes * size_t(h)];
        submit(true);
        const uint8_t* src = static_cast<const uint8_t*>(st.contents);
        uint8_t* out = static_cast<uint8_t*>(pixels);
        const size_t n = size_t(w) * size_t(h);
        for (size_t i = 0; i < n; ++i) {
            float px[4];
            unpackTexel(src + i * texel, f, px);
            packClient(px, format, type, out + i * clientTexel);
        }
    }
}

// ── buffers ─────────────────────────────────────────────────────────────────
void glGenBuffers(GLsizei n, GLuint* out) {
    for (GLsizei i = 0; i < n; ++i) out[i] = g->buffers.create();
}
void glDeleteBuffers(GLsizei n, const GLuint* ids) {
    for (GLsizei i = 0; i < n; ++i) {
        if (!ids[i]) continue;
        for (GLuint& u : g->ssbo) if (u == ids[i]) u = 0;
        for (GLuint& u : g->ubo) if (u == ids[i]) u = 0;
        g->buffers.erase(ids[i]);
    }
}
void glBindBuffer(GLenum target, GLuint b) { g->bufferTarget[target] = b; }
void glBindBufferBase(GLenum target, GLuint index, GLuint b) {
    if (target == GL_SHADER_STORAGE_BUFFER && index < 16) g->ssbo[index] = b;
    else if (target == GL_UNIFORM_BUFFER && index < 4) g->ubo[index] = b;
    g->bufferTarget[target] = b;
}
void glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum) {
    Buffer* b = buf(g->bufferTarget[target]);
    if (!b) return;
    b->size = size_t(size);
    b->uniform = target == GL_UNIFORM_BUFFER;
    if (b->uniform) {
        b->shadow.assign(size_t(size), 0);
        if (data) std::memcpy(b->shadow.data(), data, size_t(size));
        return;
    }
    @autoreleasepool {
        b->buf = newBuffer(size_t(size), target == GL_COPY_WRITE_BUFFER);
        // GL leaves new storage undefined; zero is the safe reading of it.
        if (data) {
            id<MTLBuffer> st = staging(data, size_t(size));
            [blitEnc() copyFromBuffer:st sourceOffset:0 toBuffer:b->buf destinationOffset:0
                                 size:size_t(size)];
        } else {
            [blitEnc() fillBuffer:b->buf range:NSMakeRange(0, b->buf.length) value:0];
        }
    }
}
void glNamedBufferSubData(GLuint name, GLintptr offset, GLsizeiptr size, const void* data) {
    Buffer* b = buf(name);
    if (!b || !data) return;
    if (b->uniform) {
        if (size_t(offset + size) <= b->shadow.size())
            std::memcpy(b->shadow.data() + offset, data, size_t(size));
        return;
    }
    @autoreleasepool {
        id<MTLBuffer> st = staging(data, size_t(size));
        [blitEnc() copyFromBuffer:st sourceOffset:0 toBuffer:b->buf
                destinationOffset:NSUInteger(offset) size:size_t(size)];
    }
}
void glGetNamedBufferSubData(GLuint name, GLintptr offset, GLsizeiptr size, void* data) {
    Buffer* b = buf(name);
    if (!b || size <= 0) return;
    readBuffer(*b, size_t(offset), size_t(size), data);
}
void glClearNamedBufferSubData(GLuint name, GLenum, GLintptr offset, GLsizeiptr size, GLenum,
                               GLenum, const void* data) {
    Buffer* b = buf(name);
    if (!b || size <= 0) return;
    uint32_t pattern = 0;
    if (data) std::memcpy(&pattern, data, 4);
    if (b->uniform) {
        for (GLsizeiptr i = 0; i + 4 <= size; i += 4)
            std::memcpy(b->shadow.data() + offset + i, &pattern, 4);
        return;
    }
    if (pattern == 0) {
        [blitEnc() fillBuffer:b->buf range:NSMakeRange(NSUInteger(offset), NSUInteger(size)) value:0];
        return;
    }
    std::vector<uint32_t> fill(size_t(size + 3) / 4, pattern);
    glNamedBufferSubData(name, offset, size, fill.data());
}
void glClearNamedBufferData(GLuint name, GLenum ifmt, GLenum format, GLenum type, const void* data) {
    Buffer* b = buf(name);
    if (b) glClearNamedBufferSubData(name, ifmt, 0, GLsizeiptr(b->size), format, type, data);
}
void glCopyNamedBufferSubData(GLuint src, GLuint dst, GLintptr so, GLintptr dO, GLsizeiptr size) {
    Buffer* a = buf(src);
    Buffer* b = buf(dst);
    if (!a || !b || !a->buf || !b->buf || size <= 0) return;
    [blitEnc() copyFromBuffer:a->buf sourceOffset:NSUInteger(so) toBuffer:b->buf
            destinationOffset:NSUInteger(dO) size:NSUInteger(size)];
}

// ── framebuffers ────────────────────────────────────────────────────────────
void glCreateFramebuffers(GLsizei n, GLuint* out) {
    for (GLsizei i = 0; i < n; ++i) out[i] = g->fbos.create();
}
void glDeleteFramebuffers(GLsizei n, const GLuint* ids) {
    for (GLsizei i = 0; i < n; ++i) {
        if (g->kind == EncKind::Render && g->renderFbo == ids[i]) endEncoder();
        g->fbos.erase(ids[i]);
    }
}
void glBindFramebuffer(GLenum, GLuint fbo) { g->drawFbo = fbo; }
void glNamedFramebufferTexture(GLuint fbo, GLenum attachment, GLuint t, GLint) {
    Framebuffer* f = g->fbos.get(fbo);
    const int i = int(attachment - GL_COLOR_ATTACHMENT0);
    if (f && i >= 0 && i < 2) f->color[i] = t;
}
void glNamedFramebufferDrawBuffers(GLuint, GLsizei, const GLenum*) {}

void glBlitNamedFramebuffer(GLuint readFbo, GLuint drawFbo, GLint sx0, GLint sy0, GLint sx1,
                            GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield,
                            GLenum filter) {
    if (drawFbo != 0) { warnOnce("blit to an offscreen framebuffer is not supported"); return; }
    if (!g->drawable) return;
    Framebuffer* f = g->fbos.get(readFbo);
    Texture* t = f ? g->textures.get(f->color[0]) : nullptr;
    if (!t || !t->tex || g->fbW <= 0 || g->fbH <= 0) return;
    // GL window coordinates and Metal NDC both put +y at the top, so the
    // destination maps straight across; the source rect's texel rows map to v.
    QuadArgs a{};
    a.rect[0] = float(dx0) / float(g->fbW) * 2.f - 1.f;
    a.rect[1] = float(dy0) / float(g->fbH) * 2.f - 1.f;
    a.rect[2] = float(dx1) / float(g->fbW) * 2.f - 1.f;
    a.rect[3] = float(dy1) / float(g->fbH) * 2.f - 1.f;
    a.uv[0] = float(sx0) / float(t->w);
    a.uv[1] = float(sy0) / float(t->h);
    a.uv[2] = float(sx1) / float(t->w);
    a.uv[3] = float(sy1) / float(t->h);
    a.mode[0] = 1.f;
    drawScreenQuad(a, t->tex, filter == GL_NEAREST ? g->quadNearest : g->quadLinear);
}

void glGenVertexArrays(GLsizei n, GLuint* out) { for (GLsizei i = 0; i < n; ++i) out[i] = g->nextVao++; }
void glDeleteVertexArrays(GLsizei, const GLuint*) {}
void glBindVertexArray(GLuint) {}

// ── shaders and programs ────────────────────────────────────────────────────
GLuint glCreateShader(GLenum type) {
    const GLuint name = g->shaders.create();
    g->shaders.get(name)->type = type;
    return name;
}
void glShaderSource(GLuint s, GLsizei count, const GLchar* const* strings, const GLint* lengths) {
    Shader* sh = g->shaders.get(s);
    if (!sh) return;
    sh->source.clear();
    for (GLsizei i = 0; i < count; ++i)
        sh->source += lengths && lengths[i] >= 0 ? std::string(strings[i], size_t(lengths[i]))
                                                 : std::string(strings[i]);
}
void glCompileShader(GLuint s) {
    Shader* sh = g->shaders.get(s);
    if (!sh) return;
    const mslx::Stage stage = sh->type == GL_VERTEX_SHADER ? mslx::Stage::Vertex
                            : sh->type == GL_FRAGMENT_SHADER ? mslx::Stage::Fragment
                                                             : mslx::Stage::Compute;
    sh->xlat = mslx::translate(stage, sh->source);
    sh->ok = sh->xlat.ok;
    sh->log = sh->xlat.log;
    if (!sh->ok) return;
    @autoreleasepool {
        NSError* err = nil;
        MTLCompileOptions* opt = [MTLCompileOptions new];
        opt.languageVersion = MTLLanguageVersion2_3;
        id<MTLLibrary> lib = [g->device newLibraryWithSource:@(sh->xlat.msl.c_str())
                                                     options:opt
                                                       error:&err];
        if (!lib) {
            sh->ok = false;
            sh->log = std::string("Metal compiler: ") + err.localizedDescription.UTF8String;
            if (std::getenv("PAINTIFY_DUMP_MSL")) sh->log += "\n--- generated MSL ---\n" + sh->xlat.msl;
            return;
        }
        sh->fn = [lib newFunctionWithName:@(sh->xlat.entry.c_str())];
        if (!sh->fn) { sh->ok = false; sh->log = "entry point " + sh->xlat.entry + " not found"; }
    }
}
void glGetShaderiv(GLuint s, GLenum pname, GLint* params) {
    Shader* sh = g->shaders.get(s);
    if (!sh) { *params = 0; return; }
    if (pname == GL_COMPILE_STATUS) *params = sh->ok ? GL_TRUE : GL_FALSE;
    else if (pname == GL_INFO_LOG_LENGTH) *params = GLint(sh->log.size() + 1);
}
void glGetShaderInfoLog(GLuint s, GLsizei maxLength, GLsizei* length, GLchar* log) {
    Shader* sh = g->shaders.get(s);
    const std::string& l = sh ? sh->log : std::string();
    const size_t n = std::min(l.size(), size_t(std::max(0, maxLength - 1)));
    if (maxLength > 0) { std::memcpy(log, l.data(), n); log[n] = 0; }
    if (length) *length = GLsizei(n);
}
void glDeleteShader(GLuint s) { g->shaders.erase(s); }
GLuint glCreateProgram() { return g->programs.create(); }
void glAttachShader(GLuint p, GLuint s) {
    if (Program* pr = g->programs.get(p)) pr->attached.push_back(s);
}
void glDetachShader(GLuint, GLuint) {}

void glLinkProgram(GLuint pid) {
    Program* p = g->programs.get(pid);
    if (!p) return;
    p->ok = false;
    std::vector<Shader*> sh;
    for (GLuint s : p->attached)
        if (Shader* x = g->shaders.get(s)) sh.push_back(x);
    p->attached.clear();
    for (Shader* s : sh)
        if (!s->ok) { p->log = "a shader did not compile"; return; }

    auto addStage = [&](int index, const mslx::Result& r) {
        p->stage[index].resources = r.resources;
        p->stage[index].bufferSizes = r.needsBufferSizes;
        // Rounded to 16: MSL's float3 is 16-byte aligned, so a block ending
        // in a vec3 is 4 bytes larger in Metal than its declared size.
        p->stage[index].block.assign((r.uniformBlockSize + 15u) & ~15u, 0);
        for (const mslx::UniformMember& m : r.uniforms) {
            auto it = p->locations.find(m.name);
            GLint loc;
            if (it == p->locations.end()) {
                loc = GLint(p->slots.size());
                p->locations[m.name] = loc;
                p->slots.emplace_back();
            } else {
                loc = it->second;
            }
            p->slots[size_t(loc)].push_back({index, m.offset, m.size});
        }
    };

    @autoreleasepool {
        if (sh.size() == 1 && sh[0]->type == GL_COMPUTE_SHADER) {
            p->compute = true;
            NSError* err = nil;
            p->cps = [g->device newComputePipelineStateWithFunction:sh[0]->fn error:&err];
            if (!p->cps) {
                p->log = std::string("Metal pipeline: ") + err.localizedDescription.UTF8String;
                return;
            }
            const uint32_t* ls = sh[0]->xlat.localSize;
            p->local = MTLSizeMake(ls[0], ls[1], ls[2]);
            if (p->local.width * p->local.height * p->local.depth > p->cps.maxTotalThreadsPerThreadgroup) {
                p->log = "workgroup is larger than this GPU allows";
                return;
            }
            addStage(0, sh[0]->xlat);
        } else {
            Shader* vs = nullptr;
            Shader* fs = nullptr;
            for (Shader* s : sh) (s->type == GL_VERTEX_SHADER ? vs : fs) = s;
            if (!vs || !fs) { p->log = "a raster program needs a vertex and a fragment shader"; return; }
            p->compute = false;
            p->vs = vs->fn;
            p->fs = fs->fn;
            addStage(0, vs->xlat);
            addStage(1, fs->xlat);
        }
    }
    p->ok = true;
}
void glGetProgramiv(GLuint pid, GLenum pname, GLint* params) {
    Program* p = g->programs.get(pid);
    if (!p) { *params = 0; return; }
    if (pname == GL_LINK_STATUS) *params = p->ok ? GL_TRUE : GL_FALSE;
    else if (pname == GL_INFO_LOG_LENGTH) *params = GLint(p->log.size() + 1);
}
void glGetProgramInfoLog(GLuint pid, GLsizei maxLength, GLsizei* length, GLchar* log) {
    Program* p = g->programs.get(pid);
    const std::string& l = p ? p->log : std::string();
    const size_t n = std::min(l.size(), size_t(std::max(0, maxLength - 1)));
    if (maxLength > 0) { std::memcpy(log, l.data(), n); log[n] = 0; }
    if (length) *length = GLsizei(n);
}
void glDeleteProgram(GLuint pid) {
    if (g->program == pid) g->program = 0;
    g->programs.erase(pid);
}
void glUseProgram(GLuint pid) { g->program = pid; }
GLint glGetUniformLocation(GLuint pid, const GLchar* name) {
    Program* p = g->programs.get(pid);
    if (!p) return -1;
    auto it = p->locations.find(name);
    return it == p->locations.end() ? -1 : it->second;
}
void glUniform1i(GLint loc, GLint v) { writeUniform(loc, &v, 4); }
void glUniform2i(GLint loc, GLint a, GLint b) { const GLint v[2] = {a, b}; writeUniform(loc, v, 8); }
void glUniform1ui(GLint loc, GLuint v) { writeUniform(loc, &v, 4); }
void glUniform1f(GLint loc, GLfloat v) { writeUniform(loc, &v, 4); }
void glUniform3f(GLint loc, GLfloat a, GLfloat b, GLfloat c) {
    const GLfloat v[3] = {a, b, c};
    writeUniform(loc, v, 12);
}

// ── work ────────────────────────────────────────────────────────────────────
void glDispatchCompute(GLuint x, GLuint y, GLuint z) {
    Program* p = g->programs.get(g->program);
    if (!p || !p->ok || !p->compute || x == 0 || y == 0 || z == 0) return;
    id<MTLComputeCommandEncoder> e = computeEnc();
    [e setComputePipelineState:p->cps];
    bindStage(p->stage[0],
        [&](id<MTLBuffer> b, uint32_t i) { [e setBuffer:b offset:0 atIndex:i]; },
        [&](const void* d, size_t n, uint32_t i) { [e setBytes:d length:n atIndex:i]; },
        [&](id<MTLTexture> t, uint32_t i) { [e setTexture:t atIndex:i]; },
        [&](id<MTLSamplerState> s, uint32_t i) { [e setSamplerState:s atIndex:i]; });
    [e dispatchThreadgroups:MTLSizeMake(x, y, z) threadsPerThreadgroup:p->local];
}

void glDrawArraysIndirect(GLenum mode, const void* indirect) {
    Program* p = g->programs.get(g->program);
    Framebuffer* fb = g->fbos.get(g->drawFbo);
    Buffer* ind = g->buffers.get(g->bufferTarget[GL_DRAW_INDIRECT_BUFFER]);
    if (!p || !p->ok || p->compute || !fb || !ind || !ind->buf) return;
    id<MTLRenderPipelineState> pso = renderPipeline(*p, *fb);
    if (!pso) return;
    id<MTLRenderCommandEncoder> e = renderEnc(g->drawFbo);
    if (!e) return;
    const Texture* t0 = g->textures.get(fb->color[0]);
    const int fh = t0 ? t0->h : g->viewport[3];
    // GL's viewport origin is the bottom-left; Metal's the top-left.
    [e setViewport:MTLViewport{double(g->viewport[0]), double(fh - g->viewport[1] - g->viewport[3]),
                               double(g->viewport[2]), double(g->viewport[3]), 0.0, 1.0}];
    [e setRenderPipelineState:pso];
    [e setCullMode:MTLCullModeNone];
    bindStage(p->stage[0],
        [&](id<MTLBuffer> b, uint32_t i) { [e setVertexBuffer:b offset:0 atIndex:i]; },
        [&](const void* d, size_t n, uint32_t i) { [e setVertexBytes:d length:n atIndex:i]; },
        [&](id<MTLTexture> t, uint32_t i) { [e setVertexTexture:t atIndex:i]; },
        [&](id<MTLSamplerState> s, uint32_t i) { [e setVertexSamplerState:s atIndex:i]; });
    bindStage(p->stage[1],
        [&](id<MTLBuffer> b, uint32_t i) { [e setFragmentBuffer:b offset:0 atIndex:i]; },
        [&](const void* d, size_t n, uint32_t i) { [e setFragmentBytes:d length:n atIndex:i]; },
        [&](id<MTLTexture> t, uint32_t i) { [e setFragmentTexture:t atIndex:i]; },
        [&](id<MTLSamplerState> s, uint32_t i) { [e setFragmentSamplerState:s atIndex:i]; });
    const MTLPrimitiveType prim = mode == GL_TRIANGLE_STRIP ? MTLPrimitiveTypeTriangleStrip
                                                            : MTLPrimitiveTypeTriangle;
    [e drawPrimitives:prim indirectBuffer:ind->buf
        indirectBufferOffset:NSUInteger(reinterpret_cast<uintptr_t>(indirect))];
}

// ── queries ─────────────────────────────────────────────────────────────────
// Metal times whole command buffers, so a query splits the stream: the
// elapsed time of a range is from the GPU start of its first buffer to the
// GPU end of its last. The extra submissions cost a few microseconds each.
void glGenQueries(GLsizei n, GLuint* out) { for (GLsizei i = 0; i < n; ++i) out[i] = g->queries.create(); }
void glDeleteQueries(GLsizei n, const GLuint* ids) { for (GLsizei i = 0; i < n; ++i) g->queries.erase(ids[i]); }
void glBeginQuery(GLenum, GLuint name) {
    Query* q = g->queries.get(name);
    if (!q) return;
    submit(false);
    q->elapsed = true;
    q->startCb = cmd();
    q->endCb = nil;
    g->activeElapsed = name;
}
void glEndQuery(GLenum) {
    Query* q = g->queries.get(g->activeElapsed);
    g->activeElapsed = 0;
    if (!q) return;
    q->endCb = cmd();
    submit(false);
}
void glQueryCounter(GLuint name, GLenum) {
    Query* q = g->queries.get(name);
    if (!q) return;
    q->elapsed = false;
    q->startCb = nil;
    q->hostTime = 0.0;
    if (g->cb && g->cbHasWork) {
        q->endCb = g->cb;
        submit(false);
    } else if (g->lastCb) {
        // Everything before this point is already submitted: the timestamp
        // is when that work finishes.
        q->endCb = g->lastCb;
    } else {
        q->endCb = nil;
        q->hostTime = CACurrentMediaTime();
    }
}
static bool queryDone(const Query& q) {
    auto done = [](id<MTLCommandBuffer> cb) {
        return !cb || cb.status == MTLCommandBufferStatusCompleted ||
               cb.status == MTLCommandBufferStatusError;
    };
    return done(q.startCb) && done(q.endCb);
}
void glGetQueryObjectiv(GLuint name, GLenum pname, GLint* params) {
    Query* q = g->queries.get(name);
    if (pname == GL_QUERY_RESULT_AVAILABLE) *params = (!q || queryDone(*q)) ? GL_TRUE : GL_FALSE;
    else { GLuint64 v = 0; glGetQueryObjectui64v(name, pname, &v); *params = GLint(v); }
}
void glGetQueryObjectui64v(GLuint name, GLenum, GLuint64* params) {
    *params = 0;
    Query* q = g->queries.get(name);
    if (!q) return;
    if (!q->endCb) { *params = GLuint64(q->hostTime * 1e9); return; }
    if (q->endCb.status < MTLCommandBufferStatusCommitted) submit(false);
    [q->endCb waitUntilCompleted];
    if (q->elapsed) {
        if (!q->startCb) return;
        [q->startCb waitUntilCompleted];
        const double s = q->startCb.GPUStartTime, e = q->endCb.GPUEndTime;
        if (s > 0.0 && e > s) *params = GLuint64((e - s) * 1e9);
    } else {
        *params = GLuint64(q->endCb.GPUEndTime * 1e9);
    }
}
