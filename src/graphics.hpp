#pragma once

#include <windows.h>
#include <d3d12.h>
#include <type_traits>

#include "platform.h"
#include "vrs_core.hpp"

namespace dd2 {

template<class T> bool guarded(T&& fn) {
    return dd2_guarded_call([](void* context) -> int {
        return (*static_cast<std::remove_reference_t<T>*>(context))() ? 1 : 0;
    }, &fn) != 0;
}

namespace graphics {

enum class Diagnostic : size_t {
    rtv_created, dsv_created, pso_created,
    om_calls, om_untracked_list, om_color_count, om_no_depth,
    om_missing_rtv, om_missing_dsv, om_view_layout, om_depth_format,
    om_extent_mismatch, om_extent_outside_reference, om_accepted,
    om_mrt_accepted, om_image_bound, indexed_calls, draw_untracked_list, draw_render_pass, draw_targets,
    draw_other_scene_extent,
    draw_untracked_pso, draw_stream,
    draw_pso_depth, draw_pso_layout, draw_pso_targets, draw_pso_blend, draw_pso_shader_outputs,
    draw_indirect_signature,
    draw_pso_format_mismatch, draw_preset_off, draw_image_unready, eligible, shaded,
    nonindexed_calls, nonindexed_image_bound, indirect_calls, indirect_image_bound,
    rate_combiner_preserved, engine_image_deferred, render_pass_begins, bundle_calls,
    dispatch_calls, unbind_om, unbind_pso, unbind_dispatch, unbind_indirect, indirect_signature_skipped,
    om_output_excluded, om_transparent_image_bound, om_viewport_fallback, count
};

struct BindingSample {
    Diagnostic reason{Diagnostic::count};
    Extent color{}, depth{};
    DXGI_FORMAT color_format{}, depth_format{};
    bool color_tracked{}, depth_tracked{};
};

struct Diagnostics {
    static constexpr size_t binding_sample_count =
        size_t(Diagnostic::om_extent_outside_reference) - size_t(Diagnostic::om_missing_rtv) + 1;
    std::array<uint64_t, size_t(Diagnostic::count)> counts{};
    // One representative binding per rejection reason and reporting interval.
    std::array<BindingSample, binding_sample_count> bindings{};
    uint64_t operator[](Diagnostic key) const { return counts[size_t(key)]; }
};

struct Status {
    unsigned tier{};
    unsigned tile{};
    bool additional_rates{};
    int preset{};
    const char* message{};
    Extent image{};
    Extent scene{};
    Extent output{};
    uint64_t candidates{};
    uint64_t shaded{};
    uint64_t unknown_targets{};
    uint64_t indirect_with_image{};
    bool failed{};
    bool mask_pending{};
    bool hot_hooks_active{}, hot_hooks_out_of_sync{};
};

struct AuditReport;
bool start(ID3D12Device* device, ID3D12CommandQueue* queue, const Settings& settings);
void frame(ID3D12Device* device, ID3D12CommandQueue* queue, const Settings& settings, int preset, Extent output);
void set_preset(int preset);
void set_optimizer_enabled(bool enabled);
void set_transparent_passes(bool enabled);
void set_swapchain_backbuffers(ID3D12Resource* const* buffers, size_t count);
Status status();
Diagnostics take_diagnostics();
AuditReport take_audit();
} // namespace graphics
} // namespace dd2
