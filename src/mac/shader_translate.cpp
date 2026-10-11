#include "shader_translate.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <spirv_cross/spirv_msl.hpp>

#include <algorithm>
#include <mutex>

namespace mslx {
namespace {

// Descriptor sets are rewritten per resource type before the MSL pass, so
// that GL's overlapping binding numbers (sampler 0, image 0, SSBO 0) become
// distinct keys for SPIRV-Cross's binding table.
enum : uint32_t { SET_STORAGE = 0, SET_UNIFORM = 1, SET_SAMPLER = 2, SET_IMAGE = 3,
                  SET_DEFAULT_BLOCK = 4 };

const char* kDefaultBlock = "gl_DefaultUniformBlock";

void initOnce() {
    static std::once_flag once;
    std::call_once(once, [] { glslang::InitializeProcess(); });
}

EShLanguage language(Stage s) {
    switch (s) {
        case Stage::Vertex: return EShLangVertex;
        case Stage::Fragment: return EShLangFragment;
        default: return EShLangCompute;
    }
}

spv::ExecutionModel model(Stage s) {
    switch (s) {
        case Stage::Vertex: return spv::ExecutionModelVertex;
        case Stage::Fragment: return spv::ExecutionModelFragment;
        default: return spv::ExecutionModelGLCompute;
    }
}

bool toSpirv(Stage stage, const std::string& glsl, std::vector<uint32_t>* spirv,
             std::string* log) {
    initOnce();
    const EShLanguage lang = language(stage);
    glslang::TShader shader(lang);
    const char* src = glsl.c_str();
    shader.setStrings(&src, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    // OpenGL-flavoured GLSL: loose uniforms go into one block, and
    // gl_VertexID / gl_InstanceID keep their GL meaning.
    shader.setEnvInputVulkanRulesRelaxed();
    shader.setGlobalUniformBlockName(kDefaultBlock);
    shader.setGlobalUniformSet(SET_DEFAULT_BLOCK);
    shader.setGlobalUniformBinding(0);
    // The GL sources declare varyings without locations; vertex and fragment
    // stages list them in the same order, so auto-assignment pairs them up.
    shader.setAutoMapLocations(true);

    const EShMessages msgs = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 460, ECoreProfile, false, false, msgs)) {
        *log = shader.getInfoLog();
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(msgs)) {
        *log = program.getInfoLog();
        return false;
    }
    glslang::SpvOptions opt;
    opt.generateDebugInfo = false;
    opt.disableOptimizer = true;
    glslang::GlslangToSpv(*program.getIntermediate(lang), *spirv, &opt);
    return true;
}

} // namespace

Result translate(Stage stage, const std::string& glsl) {
    Result r;
    std::vector<uint32_t> spirv;
    if (!toSpirv(stage, glsl, &spirv, &r.log)) return r;

    try {
        spirv_cross::CompilerMSL msl(std::move(spirv));
        spirv_cross::CompilerMSL::Options mo;
        mo.platform = spirv_cross::CompilerMSL::Options::macOS;
        mo.set_msl_version(2, 3);
        mo.enable_decoration_binding = false;
        msl.set_msl_options(mo);

        spirv_cross::CompilerGLSL::Options co = msl.get_common_options();
        // GL puts window row 0 at the bottom, and the canvas' texel row 0 is
        // painted there; Metal's NDC +y is up but its framebuffer origin is
        // the top-left. Flipping clip-space y keeps "texel row k is painted at
        // window row k" true on both APIs, gl_FragCoord included.
        co.vertex.flip_vert_y = true;
        msl.set_common_options(co);

        const spv::ExecutionModel em = model(stage);
        const spirv_cross::ShaderResources res = msl.get_shader_resources();

        auto pin = [&](uint32_t id, uint32_t set, uint32_t glBinding, uint32_t buffer,
                       uint32_t texture, uint32_t sampler) {
            msl.set_decoration(id, spv::DecorationDescriptorSet, set);
            spirv_cross::MSLResourceBinding b;
            b.stage = em;
            b.desc_set = set;
            b.binding = glBinding;
            b.count = 1;
            b.msl_buffer = buffer;
            b.msl_texture = texture;
            b.msl_sampler = sampler;
            msl.add_msl_resource_binding(b);
        };

        for (const auto& b : res.storage_buffers) {
            const uint32_t g = msl.get_decoration(b.id, spv::DecorationBinding);
            pin(b.id, SET_STORAGE, g, g, 0, 0);
            r.resources.push_back({ResKind::Storage, g, g, b.name});
        }
        for (const auto& b : res.uniform_buffers) {
            const uint32_t g = msl.get_decoration(b.id, spv::DecorationBinding);
            const std::string blockName = msl.get_name(b.base_type_id);
            const bool loose = b.name == kDefaultBlock || blockName == kDefaultBlock;
            if (loose) {
                pin(b.id, SET_DEFAULT_BLOCK, g, kDefaultBlockSlot, 0, 0);
                const spirv_cross::SPIRType& t = msl.get_type(b.base_type_id);
                r.uniformBlockSize = uint32_t(msl.get_declared_struct_size(t));
                for (uint32_t i = 0; i < uint32_t(t.member_types.size()); ++i) {
                    UniformMember m;
                    m.name = msl.get_member_name(b.base_type_id, i);
                    m.offset = msl.type_struct_member_offset(t, i);
                    m.size = uint32_t(msl.get_declared_struct_member_size(t, i));
                    r.uniforms.push_back(m);
                }
            } else {
                pin(b.id, SET_UNIFORM, g, kUboBase + g, 0, 0);
                r.resources.push_back({ResKind::Uniform, g, kUboBase + g, b.name});
            }
        }
        for (const auto& b : res.sampled_images) {
            const uint32_t g = msl.get_decoration(b.id, spv::DecorationBinding);
            pin(b.id, SET_SAMPLER, g, 0, g, g);
            r.resources.push_back({ResKind::Sampler, g, g, b.name});
        }
        for (const auto& b : res.storage_images) {
            const uint32_t g = msl.get_decoration(b.id, spv::DecorationBinding);
            pin(b.id, SET_IMAGE, g, 0, kImageBase + g, 0);
            r.resources.push_back({ResKind::Image, g, kImageBase + g, b.name});
        }
        if (!res.separate_images.empty() || !res.separate_samplers.empty() ||
            !res.atomic_counters.empty()) {
            r.log = "unsupported resource type (separate image/sampler or atomic counter)";
            return r;
        }

        if (stage == Stage::Compute) {
            for (int i = 0; i < 3; ++i)
                r.localSize[i] = std::max<uint32_t>(
                    1u, msl.get_execution_mode_argument(spv::ExecutionModeLocalSize, uint32_t(i)));
        }

        r.msl = msl.compile();
        const auto entries = msl.get_entry_points_and_stages();
        r.entry = entries.empty() ? std::string("main0")
                                  : msl.get_cleansed_entry_point_name(entries[0].name,
                                                                      entries[0].execution_model);
        r.ok = true;
    } catch (const std::exception& e) {
        r.log = std::string("SPIRV-Cross: ") + e.what();
        r.ok = false;
    }
    return r;
}

} // namespace mslx
