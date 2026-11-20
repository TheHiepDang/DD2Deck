#pragma once

#include "graphics.hpp"
#include "pipeline.hpp"

#include <atomic>
#include <mutex>
#include <vector>

namespace dd2::graphics {

enum class IndirectKind : uint8_t { untracked, draw, indexed, dispatch, mesh, rays, unsupported, count };
enum class Gate : uint8_t {
    engine_vrs, eligible, pipeline, untracked_list, render_pass,
    no_binding, no_color, multiple_colors, invalid_color_binding, no_depth, missing_rtv, missing_dsv, layout,
    depth_format, extent_mismatch, outside_reference, other_scene_extent, indirect_signature, signature_only, count
};

inline constexpr const char* indirect_names[]{"untracked", "draw", "draw_indexed", "dispatch",
    "dispatch_mesh", "dispatch_rays", "unsupported"};
inline constexpr const char* gate_names[]{"engine_vrs", "eligible", "pipeline", "untracked_list",
    "render_pass", "no_observed_binding", "no_color", "multiple_colors", "invalid_color_binding", "no_depth", "missing_rtv", "missing_dsv",
    "view_layout", "depth_format", "extent_mismatch", "below_scene_minimum", "other_scene_extent", "indirect_signature", "signature_only"};
inline constexpr const char* pipeline_names[]{"unknown", "graphics", "compute"};
static_assert(std::size(indirect_names) == size_t(IndirectKind::count));
static_assert(std::size(gate_names) == size_t(Gate::count));
static_assert(std::size(pipeline_names) == size_t(PipelineKind::count));

struct SignatureInfo {
    uint32_t id{}, stride{}, argument_count{};
    IndirectKind kind{};
    std::array<uint8_t, 12> argument_types{};
};

inline SignatureInfo describe_signature(const D3D12_COMMAND_SIGNATURE_DESC& desc) {
    SignatureInfo info{};
    info.stride = desc.ByteStride;
    info.argument_count = desc.NumArgumentDescs;
    info.kind = IndirectKind::unsupported;
    if (!desc.pArgumentDescs || !desc.NumArgumentDescs || desc.NumArgumentDescs > 128) return info;
    for (size_t i = 0; i < std::min<size_t>(info.argument_types.size(), desc.NumArgumentDescs); ++i)
        info.argument_types[i] = static_cast<uint8_t>(desc.pArgumentDescs[i].Type);
    switch (desc.pArgumentDescs[desc.NumArgumentDescs - 1].Type) {
    case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: info.kind = IndirectKind::draw; break;
    case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: info.kind = IndirectKind::indexed; break;
    case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: info.kind = IndirectKind::dispatch; break;
    case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH: info.kind = IndirectKind::mesh; break;
    case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS: info.kind = IndirectKind::rays; break;
    default: break;
    }
    return info;
}

struct AuditView {
    Extent extent{};
    DXGI_FORMAT format{};
    bool tracked{};
};

struct AuditBinding {
    std::array<AuditView, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> colors{};
    AuditView depth{};
    UINT color_count{};
    bool has_depth{};
    Diagnostic reason{Diagnostic::count};
};

struct AuditVrs {
    unsigned rate{}, primitive{}, image_combiner{};
    bool image_bound{};
};

inline constexpr size_t vrs_tuple_count = 11 * 5 * 5 * 2 + 1;
inline size_t vrs_tuple_index(AuditVrs v) {
    if (v.rate >= 11 || v.primitive >= 5 || v.image_combiner >= 5) return vrs_tuple_count - 1;
    return ((v.rate * 5 + v.primitive) * 5 + v.image_combiner) * 2 + v.image_bound;
}
inline AuditVrs vrs_tuple_at(size_t index) {
    return {unsigned(index / 50), unsigned(index / 10 % 5), unsigned(index / 2 % 5), index % 2 != 0};
}

inline Gate binding_gate(const AuditBinding& binding) {
    switch (binding.reason) {
    case Diagnostic::om_color_count:
        if (binding.color_count == 0) return Gate::no_color;
        return binding.color_count > 1 ? Gate::multiple_colors : Gate::invalid_color_binding;
    case Diagnostic::om_no_depth: return Gate::no_depth;
    case Diagnostic::om_missing_rtv: return Gate::missing_rtv;
    case Diagnostic::om_missing_dsv: return Gate::missing_dsv;
    case Diagnostic::om_view_layout: return Gate::layout;
    case Diagnostic::om_depth_format: return Gate::depth_format;
    case Diagnostic::om_extent_mismatch: return Gate::extent_mismatch;
    case Diagnostic::om_extent_outside_reference: return Gate::outside_reference;
    default: return Gate::no_binding;
    }
}

struct AuditSample {
    size_t path{};
    Gate gate{};
    Diagnostic pipeline_gate{Diagnostic::draw_untracked_pso};
    SignatureInfo signature{};
    UINT max_commands{};
    bool count_buffer{};
    int list_type{-1};
    AuditBinding bindings{};
    D3D12_VIEWPORT viewport{};
    PipelineInfo pipeline{};
    AuditVrs vrs{};
    int preset{};
    bool plugin_image_ready{};
    bool plugin_image_bound{};
};

// Paths 0 and 1 are direct indexed and direct nonindexed. Subsequent paths
// correspond to indirect_names in order.
inline constexpr size_t audit_path_count = 2 + size_t(IndirectKind::count);
inline constexpr size_t audit_gate_count = size_t(Gate::count);
inline const char* path_name(size_t path) {
    if (path < 2) return path ? "direct_draw" : "direct_indexed";
    return indirect_names[path - 2];
}

struct AuditPathReport {
    uint64_t calls{}, max_commands_sum{}, count_buffer_calls{}, image_bound_calls{};
    std::array<uint64_t, size_t(PipelineKind::count)> pipelines{};
    uint64_t compute_lists{}, direct_lists{}, other_lists{};
    uint64_t vrs_psos_eligible{};
    std::array<uint64_t, audit_gate_count> gates{};
};

struct SignatureUsage {
    SignatureInfo info{};
    uint64_t calls{};
};

struct AuditReport {
    std::array<AuditPathReport, audit_path_count> paths{};
    std::array<uint64_t, size_t(IndirectKind::count)> tagged_signatures{};
    uint64_t signature_tag_failures{};
    bool signature_hook{}, compute_pipeline_hook{};
    uint64_t signature_usage_overflow{};
    uint64_t nested_callbacks{};
    std::vector<SignatureUsage> signature_usage;
    // Rows distinguish indexed, nonindexed, and indirect calls.
    std::array<std::array<uint64_t, vrs_tuple_count>, 3> vrs_tuples{};
    std::vector<AuditSample> samples;
};

class Audit {
    using Counter = std::atomic<uint64_t>;
    struct PathCounters {
        Counter calls{}, max_commands_sum{}, count_buffer_calls{}, image_bound_calls{};
        std::array<Counter, size_t(PipelineKind::count)> pipelines{};
        Counter compute_lists{}, direct_lists{}, other_lists{};
        Counter vrs_psos_eligible{};
        std::array<Counter, audit_gate_count> gates{};
    };
    struct SampleSlot {
        std::atomic<bool> wanted{true};
        bool filled{};
        AuditSample value{};
    };
    std::array<PathCounters, audit_path_count> paths_{};
    std::array<Counter, size_t(IndirectKind::count)> signatures_{};
    Counter signature_failures_{};
    Counter nested_callbacks_{};
    static constexpr size_t signature_usage_limit = 512;
    std::array<Counter, signature_usage_limit> signature_calls_{};
    std::array<SignatureInfo, signature_usage_limit> signature_descriptions_{};
    Counter signature_usage_overflow_{};
    std::mutex signature_mutex_;
    std::array<std::array<Counter, vrs_tuple_count>, 3> vrs_tuples_{};
    std::array<std::array<SampleSlot, audit_gate_count>, audit_path_count> samples_{};
    std::mutex samples_mutex_;

    static void inc(Counter& value, uint64_t amount = 1) { value.fetch_add(amount, std::memory_order_relaxed); }
    static uint64_t drain(Counter& value) { return value.exchange(0, std::memory_order_relaxed); }
    template<size_t N> static void drain(std::array<uint64_t, N>& to, std::array<Counter, N>& from) {
        for (size_t i = 0; i < N; ++i) to[i] = drain(from[i]);
    }

public:
    std::atomic<bool> enabled{};
    bool signature_hook{}, compute_pipeline_hook{};
    void tagged(SignatureInfo info) {
        inc(signatures_[size_t(info.kind)]);
        if (!info.id || info.id > signature_usage_limit) return;
        std::lock_guard lock{signature_mutex_};
        signature_descriptions_[info.id - 1] = info;
    }
    void tag_failed() { inc(signature_failures_); }
    void nested_callback() { inc(nested_callbacks_); }
    void used(uint32_t id) {
        if (!id) return;
        if (id <= signature_usage_limit) inc(signature_calls_[id - 1]);
        else inc(signature_usage_overflow_);
    }

    void call(size_t path, PipelineKind pipeline, int type, UINT max_commands, bool has_count, bool image_bound) {
        auto& counters = paths_[path];
        inc(counters.calls);
        if (image_bound) inc(counters.image_bound_calls);
        inc(counters.pipelines[size_t(pipeline)]);
        if (type == D3D12_COMMAND_LIST_TYPE_DIRECT) inc(counters.direct_lists);
        else if (type == D3D12_COMMAND_LIST_TYPE_COMPUTE) inc(counters.compute_lists);
        else inc(counters.other_lists);
        if (path >= 2) {
            inc(counters.max_commands_sum, max_commands);
            if (has_count) inc(counters.count_buffer_calls);
        }
    }

    void verdict(size_t path, Gate gate) { inc(paths_[path].gates[size_t(gate)]); }
    void vrs(size_t path, AuditVrs state, bool pipeline_eligible) {
        inc(vrs_tuples_[std::min<size_t>(path, 2)][vrs_tuple_index(state)]);
        if (pipeline_eligible) inc(paths_[path].vrs_psos_eligible);
    }

    template<class F> void sample(size_t path, Gate gate, F&& make_sample) {
        auto& entry = samples_[path][size_t(gate)];
        if (!entry.wanted.load(std::memory_order_relaxed) ||
            !entry.wanted.exchange(false, std::memory_order_relaxed)) return;
        auto value = make_sample();
        std::lock_guard lock{samples_mutex_};
        entry.value = value;
        entry.filled = true;
    }

    AuditReport take() {
        AuditReport report;
        report.signature_hook = signature_hook;
        report.compute_pipeline_hook = compute_pipeline_hook;
        drain(report.tagged_signatures, signatures_);
        report.signature_tag_failures = drain(signature_failures_);
        report.nested_callbacks = drain(nested_callbacks_);
        report.signature_usage_overflow = drain(signature_usage_overflow_);
        {
            std::lock_guard lock{signature_mutex_};
            for (size_t i = 0; i < signature_calls_.size(); ++i) {
                const auto calls = drain(signature_calls_[i]);
                if (calls) report.signature_usage.push_back({signature_descriptions_[i], calls});
            }
        }
        for (size_t i = 0; i < report.vrs_tuples.size(); ++i)
            drain(report.vrs_tuples[i], vrs_tuples_[i]);
        for (size_t i = 0; i < paths_.size(); ++i) {
            auto& from = paths_[i];
            auto& to = report.paths[i];
            to.calls = drain(from.calls);
            to.max_commands_sum = drain(from.max_commands_sum);
            to.count_buffer_calls = drain(from.count_buffer_calls);
            to.image_bound_calls = drain(from.image_bound_calls);
            drain(to.pipelines, from.pipelines);
            to.compute_lists = drain(from.compute_lists);
            to.direct_lists = drain(from.direct_lists);
            to.other_lists = drain(from.other_lists);
            to.vrs_psos_eligible = drain(from.vrs_psos_eligible);
            drain(to.gates, from.gates);
        }
        report.samples.reserve(audit_gate_count * audit_path_count);
        std::lock_guard lock{samples_mutex_};
        for (auto& path : samples_) for (auto& entry : path) {
            if (entry.filled) report.samples.push_back(entry.value);
            entry.filled = false;
            entry.wanted.store(true, std::memory_order_relaxed);
        }
        return report;
    }
};

} // namespace dd2::graphics
