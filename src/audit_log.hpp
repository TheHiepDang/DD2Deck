#pragma once

#include "audit.hpp"

#include <cstdio>
#include <string>

namespace dd2::graphics {

inline const char* pipeline_gate_name(Diagnostic gate) {
    switch (gate) {
    case Diagnostic::eligible: return "eligible";
    case Diagnostic::draw_pso_depth: return "depth";
    case Diagnostic::draw_pso_layout: return "topology_shader_or_samples";
    case Diagnostic::draw_pso_targets: return "rtv_count_or_format";
    case Diagnostic::draw_pso_blend: return "blend_or_write_mask";
    case Diagnostic::draw_pso_shader_outputs: return "noncolor_or_unverified_pixel_outputs";
    case Diagnostic::draw_pso_format_mismatch: return "bound_format_mismatch";
    case Diagnostic::draw_stream: return "unsupported_stream_or_compute";
    default: return "untracked";
    }
}

inline const char* rate_name(unsigned rate) {
    switch (rate) {
    case D3D12_SHADING_RATE_1X1: return "1x1";
    case D3D12_SHADING_RATE_1X2: return "1x2";
    case D3D12_SHADING_RATE_2X1: return "2x1";
    case D3D12_SHADING_RATE_2X2: return "2x2";
    case D3D12_SHADING_RATE_2X4: return "2x4";
    case D3D12_SHADING_RATE_4X2: return "4x2";
    case D3D12_SHADING_RATE_4X4: return "4x4";
    default: return "unknown";
    }
}

inline const char* combiner_name(unsigned combiner) {
    constexpr const char* names[]{"PASSTHROUGH", "OVERRIDE", "MIN", "MAX", "SUM"};
    return combiner < std::size(names) ? names[combiner] : "unknown";
}

inline std::string argument_types(const SignatureInfo& sig) {
    std::string types;
    for (size_t i = 0; i < std::min<size_t>(sig.argument_count, sig.argument_types.size()); ++i) {
        if (i) types += ',';
        types += std::to_string(sig.argument_types[i]);
    }
    if (sig.argument_count > sig.argument_types.size()) types += ",...";
    return types.empty() ? "-" : types;
}

inline std::string view_text(const AuditView& view) {
    if (!view.tracked) return "untracked";
    return std::to_string(view.extent.width) + "x" + std::to_string(view.extent.height) + "@" +
        std::to_string(unsigned(view.format));
}

using AuditLogFn = void (*)(const char*, ...);

inline void write_audit(AuditReport report, AuditLogFn log, bool force_active) {
    const auto u = [](uint64_t n) { return static_cast<unsigned long long>(n); };
    std::string signature_tags;
    for (size_t i = 0; i < report.tagged_signatures.size(); ++i) {
        if (!report.tagged_signatures[i]) continue;
        signature_tags += ' ';
        signature_tags += indirect_names[i];
        signature_tags += '=';
        signature_tags += std::to_string(report.tagged_signatures[i]);
    }
    const bool has_calls = std::any_of(report.paths.begin(), report.paths.end(),
        [](const auto& path) { return path.calls != 0; });
    log("[DD2 VRS] Audit 0.3.0 | force=%u signature_hook=%u compute_pso_hook=%u signature_tag_failures=%llu"
        " nested_callbacks_excluded=%llu tagged:%s",
        unsigned(force_active), unsigned(report.signature_hook), unsigned(report.compute_pipeline_hook),
        u(report.signature_tag_failures), u(report.nested_callbacks), signature_tags.empty() ? " none" : signature_tags.c_str());
    if (!has_calls) return;

    for (size_t i = 0; i < report.paths.size(); ++i) {
        const auto& path = report.paths[i];
        if (!path.calls) continue;
        std::string gates;
        for (size_t j = 0; j < path.gates.size(); ++j) {
            if (!path.gates[j]) continue;
            gates += ' ';
            gates += gate_names[j];
            gates += '=';
            gates += std::to_string(path.gates[j]);
        }
        log("[DD2 VRS] Audit path | %s%s calls=%llu plugin_image_bound=%llu max_commands_sum=%llu count_buffer_calls=%llu"
            " lists[direct=%llu compute=%llu other=%llu] pso[graphics=%llu compute=%llu unknown=%llu]"
            " engine_vrs_with_eligible_pso=%llu | first_gate:%s",
            i >= 2 ? "indirect_" : "", path_name(i), u(path.calls), u(path.image_bound_calls),
            u(path.max_commands_sum), u(path.count_buffer_calls),
            u(path.direct_lists), u(path.compute_lists), u(path.other_lists),
            u(path.pipelines[size_t(PipelineKind::graphics)]), u(path.pipelines[size_t(PipelineKind::compute)]),
            u(path.pipelines[size_t(PipelineKind::unknown)]), u(path.vrs_psos_eligible),
            gates.empty() ? " not_evaluated" : gates.c_str());
    }

    std::sort(report.signature_usage.begin(), report.signature_usage.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.calls > rhs.calls;
    });
    constexpr size_t signature_log_limit = 12;
    uint64_t other_signature_calls = report.signature_usage_overflow;
    for (size_t i = 0; i < report.signature_usage.size(); ++i) {
        const auto& used = report.signature_usage[i];
        if (i >= signature_log_limit) {
            other_signature_calls += used.calls;
            continue;
        }
        const auto& sig = used.info;
        log("[DD2 VRS] Audit signature | id=%u action=%s calls=%llu stride=%u args=%u types=%s",
            sig.id, indirect_names[size_t(sig.kind)], u(used.calls), sig.stride,
            sig.argument_count, argument_types(sig).c_str());
    }
    if (other_signature_calls)
        log("[DD2 VRS] Audit signature | other_tagged_calls=%llu", u(other_signature_calls));

    std::vector<size_t> vrs_tuples;
    const auto total = [&](size_t i) {
        return report.vrs_tuples[0][i] + report.vrs_tuples[1][i] + report.vrs_tuples[2][i];
    };
    for (size_t i = 0; i < vrs_tuple_count; ++i) if (total(i)) vrs_tuples.push_back(i);
    std::sort(vrs_tuples.begin(), vrs_tuples.end(), [&](size_t lhs, size_t rhs) { return total(lhs) > total(rhs); });
    uint64_t other_tuples = 0;
    for (size_t j = 0; j < vrs_tuples.size(); ++j) {
        const size_t i = vrs_tuples[j];
        if (j >= 12 || i == vrs_tuple_count - 1) {
            other_tuples += total(i);
            continue;
        }
        const auto v = vrs_tuple_at(i);
        log("[DD2 VRS] Audit VRS state | rate=%s(%u) primitive=%s(%u) image_combiner=%s(%u)"
            " image_bound=%u direct_indexed=%llu direct_draw=%llu indirect=%llu",
            rate_name(v.rate), v.rate, combiner_name(v.primitive), v.primitive,
            combiner_name(v.image_combiner), v.image_combiner, unsigned(v.image_bound),
            u(report.vrs_tuples[0][i]), u(report.vrs_tuples[1][i]), u(report.vrs_tuples[2][i]));
    }
    if (other_tuples) log("[DD2 VRS] Audit VRS state | other_calls=%llu", u(other_tuples));

    std::stable_sort(report.samples.begin(), report.samples.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.plugin_image_bound > rhs.plugin_image_bound;
    });
    constexpr size_t sample_limit = 24;
    for (size_t i = 0; i < std::min(sample_limit, report.samples.size()); ++i) {
        const auto& s = report.samples[i];
        std::string rtvs;
        for (size_t j = 0; j < std::min<size_t>(s.bindings.color_count, s.bindings.colors.size()); ++j) {
            if (j) rtvs += ',';
            rtvs += view_text(s.bindings.colors[j]);
        }
        const auto& p = s.pipeline;
        std::string pso_rtvs;
        for (size_t j = 0; j < p.color_count; ++j) {
            if (j) pso_rtvs += ',';
            pso_rtvs += std::to_string(unsigned(p.colors[j]));
        }
        log("[DD2 VRS] Audit draw sample %u | %s%s first_gate=%s list_type=%d signature_id=%u"
            " max_commands=%u count_buffer=%u targets=%u RTV=%s DSV=%s viewport=%.1fx%.1f origin=(%.1f,%.1f)"
            " preset=%d matching_image=%u plugin_image_bound=%u",
            unsigned(i + 1), s.path >= 2 ? "indirect_" : "", path_name(s.path), gate_names[size_t(s.gate)],
            int(s.list_type), s.signature.id, s.max_commands, unsigned(s.count_buffer), s.bindings.color_count,
            rtvs.empty() ? "-" : rtvs.c_str(), s.bindings.has_depth ? view_text(s.bindings.depth).c_str() : "none",
            s.viewport.Width, s.viewport.Height, s.viewport.TopLeftX, s.viewport.TopLeftY,
            s.preset, unsigned(s.plugin_image_ready), unsigned(s.plugin_image_bound));
        log("[DD2 VRS] Audit state sample %u | pso=%s source=%u pso_diagnostic=%s RTV=%s DSV=%u"
            " depth_enabled=%u depth_write=%u depth_func=%u topology=%u samples=%u"
            " blend_mask=0x%x alpha_to_coverage=%u logic_op=%u color0_write_mask=0x%x VS=%016llx PS=%016llx"
            " | rate=%s(%u) primitive=%s(%u) image_combiner=%s(%u) image_bound=%u",
            unsigned(i + 1), pipeline_names[size_t(p.kind)], unsigned(p.source), pipeline_gate_name(s.pipeline_gate),
            pso_rtvs.empty() ? "-" : pso_rtvs.c_str(), unsigned(p.depth_format),
            unsigned(p.depth_enabled), unsigned(p.depth_write), unsigned(p.depth_func), unsigned(p.topology),
            p.samples, unsigned(p.blend_mask), unsigned(p.alpha_to_coverage), unsigned(p.logic_op),
            unsigned(p.color_write_mask), u(p.vs_id), u(p.ps_id), rate_name(s.vrs.rate), s.vrs.rate,
            combiner_name(s.vrs.primitive), s.vrs.primitive, combiner_name(s.vrs.image_combiner), s.vrs.image_combiner,
            unsigned(s.vrs.image_bound));
    }
    if (report.samples.size() > sample_limit)
        log("[DD2 VRS] Audit draw sample | omitted=%llu", u(report.samples.size() - sample_limit));
}

} // namespace dd2::graphics
