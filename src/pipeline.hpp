#pragma once

#include <cstddef>
#include <cstring>

#include "shader_outputs.hpp"

namespace dd2::graphics {

enum class PipelineKind : uint8_t { unknown, graphics, compute, count };
enum class PipelineSource : uint8_t { unknown, classic_graphics, classic_compute, stream };

struct PipelineInfo {
    DXGI_FORMAT color{};
    Diagnostic rejection{Diagnostic::draw_untracked_pso};
    PipelineKind kind{};
    PipelineSource source{};
    uint8_t color_count{};
    std::array<DXGI_FORMAT, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> colors{};
    DXGI_FORMAT depth_format{};
    UINT samples{};
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology{};
    bool depth_enabled{};
    D3D12_DEPTH_WRITE_MASK depth_write{};
    D3D12_COMPARISON_FUNC depth_func{};
    uint8_t blend_mask{};
    bool alpha_to_coverage{};
    bool logic_op{};
    uint8_t color_write_mask{};
    uint64_t vs_id{}, ps_id{}, cs_id{};
};

inline uint64_t shader_header_id(D3D12_SHADER_BYTECODE shader) {
    if (!shader.pShaderBytecode || !shader.BytecodeLength) return 0;
    const auto* bytes = static_cast<const uint8_t*>(shader.pShaderBytecode);
    uint64_t hash = 14695981039346656037ull ^ shader.BytecodeLength;
    // DXBC/DXIL containers carry a digest near the start. Bound the work for large shader caches.
    for (size_t i = 0; i < std::min<size_t>(shader.BytecodeLength, 24); ++i)
        hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}

inline PipelineInfo classify(const D3D12_BLEND_DESC& blend, BOOL depth, D3D12_DEPTH_WRITE_MASK write,
                             D3D12_COMPARISON_FUNC comparison, const D3D12_RT_FORMAT_ARRAY& rt,
                             DXGI_FORMAT dsv, DXGI_SAMPLE_DESC sample,
                             D3D12_PRIMITIVE_TOPOLOGY_TYPE topology, D3D12_SHADER_BYTECODE ps,
                             D3D12_SHADER_BYTECODE vs, PipelineSource source) {
    const auto& color = blend.RenderTarget[0];
    const bool scene_depth = depth && ((write == D3D12_DEPTH_WRITE_MASK_ALL &&
        (comparison == D3D12_COMPARISON_FUNC_LESS || comparison == D3D12_COMPARISON_FUNC_LESS_EQUAL ||
         comparison == D3D12_COMPARISON_FUNC_GREATER || comparison == D3D12_COMPARISON_FUNC_GREATER_EQUAL)) ||
         comparison == D3D12_COMPARISON_FUNC_EQUAL);
    PipelineInfo info{};
    info.color = rt.RTFormats[0];
    info.kind = PipelineKind::graphics;
    info.source = source;
    info.color_count = static_cast<uint8_t>(std::min<UINT>(rt.NumRenderTargets, UINT(info.colors.size())));
    std::copy_n(rt.RTFormats, info.color_count, info.colors.begin());
    info.depth_format = dsv;
    info.samples = sample.Count;
    info.topology = topology;
    info.depth_enabled = depth != FALSE;
    info.depth_write = write;
    info.depth_func = comparison;
    for (size_t i = 0; i < info.colors.size(); ++i)
        if (blend.RenderTarget[i].BlendEnable) info.blend_mask |= uint8_t(1u << i);
    info.alpha_to_coverage = blend.AlphaToCoverageEnable != FALSE;
    info.logic_op = color.LogicOpEnable != FALSE;
    info.color_write_mask = color.RenderTargetWriteMask;
    info.vs_id = shader_header_id(vs);
    info.ps_id = shader_header_id(ps);
    if (!scene_depth || dsv == DXGI_FORMAT_UNKNOWN) info.rejection = Diagnostic::draw_pso_depth;
    else if (!ps.BytecodeLength || !vs.BytecodeLength ||
             topology != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE || sample.Count != 1)
        info.rejection = Diagnostic::draw_pso_layout;
    else if (!rt.NumRenderTargets || rt.NumRenderTargets > info.colors.size())
        info.rejection = Diagnostic::draw_pso_targets;
    else if (!color_only_pixel_outputs(ps)) info.rejection = Diagnostic::draw_pso_shader_outputs;
    else if (info.alpha_to_coverage || info.blend_mask || info.logic_op || !info.color_write_mask)
        info.rejection = Diagnostic::draw_pso_blend;
    else info.rejection = Diagnostic::eligible;
    return info;
}

inline PipelineInfo classify(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) {
    D3D12_RT_FORMAT_ARRAY formats{};
    formats.NumRenderTargets = desc.NumRenderTargets;
    std::memcpy(formats.RTFormats, desc.RTVFormats, sizeof(formats.RTFormats));
    auto info = classify(desc.BlendState, desc.DepthStencilState.DepthEnable, desc.DepthStencilState.DepthWriteMask,
        desc.DepthStencilState.DepthFunc, formats, desc.DSVFormat, desc.SampleDesc,
        desc.PrimitiveTopologyType, desc.PS, desc.VS, PipelineSource::classic_graphics);
    if (desc.GS.BytecodeLength || desc.HS.BytecodeLength || desc.DS.BytecodeLength || desc.StreamOutput.NumEntries)
        info.rejection = Diagnostic::draw_stream;
    return info;
}

inline PipelineInfo classify(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc) {
    PipelineInfo info{};
    info.rejection = Diagnostic::draw_stream;
    info.kind = PipelineKind::compute;
    info.source = PipelineSource::classic_compute;
    info.cs_id = shader_header_id(desc.CS);
    return info;
}

template<class T> struct alignas(void*) StreamEntry {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    T value;
};

template<class T> bool read_entry(const std::byte*& data, size_t& remaining, T& value) {
    if (remaining < sizeof(StreamEntry<T>)) return false;
    std::memcpy(&value, data + offsetof(StreamEntry<T>, value), sizeof(T));
    data += sizeof(StreamEntry<T>);
    remaining -= sizeof(StreamEntry<T>);
    return true;
}

// Report incomplete stream metadata without guessing its shader identity.
// An observed compute shader is sufficient to classify the stream as compute.
inline PipelineInfo classify(const D3D12_PIPELINE_STATE_STREAM_DESC& desc) {
    PipelineInfo unsupported{};
    unsupported.rejection = Diagnostic::draw_stream;
    unsupported.source = PipelineSource::stream;
    auto* data = static_cast<const std::byte*>(desc.pPipelineStateSubobjectStream);
    size_t remaining = desc.SizeInBytes;
    if (!data || remaining > 65536) return unsupported;

    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    BOOL depth = TRUE;
    auto write = D3D12_DEPTH_WRITE_MASK_ALL;
    auto comparison = D3D12_COMPARISON_FUNC_LESS;
    auto topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
    D3D12_RT_FORMAT_ARRAY rt{};
    DXGI_FORMAT dsv{};
    DXGI_SAMPLE_DESC sample{1, 0};
    D3D12_SHADER_BYTECODE ps{}, vs{}, cs{}, gs{}, hs{}, ds{};
    D3D12_STREAM_OUTPUT_DESC stream_output{};

    while (remaining >= sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE)) {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
        std::memcpy(&type, data, sizeof(type));
#define DD2_READ(name, value) case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_##name: \
        if (!read_entry(data, remaining, value)) return unsupported; break
#define DD2_SKIP(name, T) case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_##name: { \
        T ignored{}; if (!read_entry(data, remaining, ignored)) return unsupported; break; }
        switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
            if (!read_entry(data, remaining, vs)) return unsupported;
            if (vs.BytecodeLength) unsupported.kind = PipelineKind::graphics;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
            if (!read_entry(data, remaining, ps)) return unsupported;
            if (ps.BytecodeLength) unsupported.kind = PipelineKind::graphics;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
            if (!read_entry(data, remaining, cs)) return unsupported;
            if (cs.BytecodeLength) {
                unsupported.kind = PipelineKind::compute;
                unsupported.cs_id = shader_header_id(cs);
                return unsupported;
            }
            break;
        DD2_READ(BLEND, blend);
        DD2_READ(PRIMITIVE_TOPOLOGY, topology);
        DD2_READ(RENDER_TARGET_FORMATS, rt);
        DD2_READ(DEPTH_STENCIL_FORMAT, dsv);
        DD2_READ(SAMPLE_DESC, sample);
        DD2_SKIP(ROOT_SIGNATURE, ID3D12RootSignature*);
        DD2_READ(DS, ds);
        DD2_READ(HS, hs);
        DD2_READ(GS, gs);
        DD2_READ(STREAM_OUTPUT, stream_output);
        DD2_SKIP(SAMPLE_MASK, UINT);
        DD2_SKIP(RASTERIZER, D3D12_RASTERIZER_DESC);
        DD2_SKIP(INPUT_LAYOUT, D3D12_INPUT_LAYOUT_DESC);
        DD2_SKIP(IB_STRIP_CUT_VALUE, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
        DD2_SKIP(NODE_MASK, UINT);
        DD2_SKIP(CACHED_PSO, D3D12_CACHED_PIPELINE_STATE);
        DD2_SKIP(FLAGS, D3D12_PIPELINE_STATE_FLAGS);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: {
            D3D12_DEPTH_STENCIL_DESC state{};
            if (!read_entry(data, remaining, state)) return unsupported;
            depth = state.DepthEnable;
            write = state.DepthWriteMask;
            comparison = state.DepthFunc;
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: {
            D3D12_DEPTH_STENCIL_DESC1 state{};
            if (!read_entry(data, remaining, state)) return unsupported;
            depth = state.DepthEnable;
            write = state.DepthWriteMask;
            comparison = state.DepthFunc;
            break;
        }
        default: return unsupported;
        }
#undef DD2_SKIP
#undef DD2_READ
    }
    if (remaining || gs.BytecodeLength || hs.BytecodeLength || ds.BytecodeLength ||
        stream_output.NumEntries) return unsupported;
    return classify(blend, depth, write, comparison, rt, dsv, sample, topology,
        ps, vs, PipelineSource::stream);
}

} // namespace dd2::graphics
