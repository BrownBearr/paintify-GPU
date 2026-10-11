#pragma once
#include <cstdint>
#include <string>
#include <vector>

// GLSL 4.60 (as written for the OpenGL renderer) -> Metal Shading Language.
//
// macOS stops at OpenGL 4.1, which has no compute shaders, so the Metal
// backend runs the very same shader files through glslang (Vulkan rules,
// relaxed, so loose `uniform int uPass;` declarations are still legal) and
// SPIRV-Cross. Doing it at runtime rather than at build time keeps F5 hot
// reload working exactly as it does on Windows.
//
// GL keeps a separate binding namespace per resource type, while Metal has one
// table per type of *slot*. The translation pins every resource to a slot
// derived from its GL binding so the host can bind by GL unit number:
//
//   SSBO binding b            -> [[buffer(b)]]                     b < 12
//   UBO binding b             -> [[buffer(kUboBase + b)]]
//   loose uniforms (one block) -> [[buffer(kDefaultBlockSlot)]]
//   sampler binding b         -> [[texture(b)]], [[sampler(b)]]    b < 16
//   image binding b           -> [[texture(kImageBase + b)]]
//
// Nothing here depends on Metal itself, so it builds and can be tested on any
// platform.
namespace mslx {

enum class Stage { Vertex, Fragment, Compute };
enum class ResKind { Storage, Uniform, Sampler, Image };

constexpr uint32_t kUboBase = 12;
constexpr uint32_t kDefaultBlockSlot = 15;
constexpr uint32_t kImageBase = 16;
// Where SPIRV-Cross reads storage-buffer sizes for GLSL's .length(): a uint
// per [[buffer]] slot, in bytes.
constexpr uint32_t kBufferSizeSlot = 30;

struct Resource {
    ResKind kind;
    uint32_t glBinding;   // GL unit / binding point the host binds to
    uint32_t slot;        // Metal [[buffer]] / [[texture]] index
    std::string name;
    bool array = false;   // sampler2DArray
};

// One member of the loose-uniform block: what glUniform* writes into.
struct UniformMember {
    std::string name;
    uint32_t offset = 0;
    uint32_t size = 0;
};

struct Result {
    bool ok = false;
    std::string log;      // compiler diagnostics on failure
    std::string msl;
    std::string entry;    // Metal function name
    std::vector<Resource> resources;
    std::vector<UniformMember> uniforms;
    uint32_t uniformBlockSize = 0;
    uint32_t localSize[3] = {1, 1, 1};
    bool needsBufferSizes = false;   // bind sizes at kBufferSizeSlot
};

Result translate(Stage stage, const std::string& glsl);

} // namespace mslx
