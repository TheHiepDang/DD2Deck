#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include "crowd_controller.hpp"

namespace dd2 {

enum EngineTweak { speedtree, motion_quality, contact_shadows, shadow_quality, shadow_resolution,
                   ssao_half, ssr_half, shadow_filter, ssr_disable, ssr_low_resolve, dfao_disable,
                   oov_streaming_off, oov_texture_streaming_off, tweak_count };
inline constexpr std::array engine_labels{
    "SpeedTree LOD Bias", "Motion Quality", "Disable Contact Shadows", "Shadow Quality", "Shadow Resolution",
    "SSAO Half Resolution", "SSR Half Resolution", "Disable Shadow Filtering", "Disable SSR",
    "SSR 1-Point Resolve", "Disable DFAO", "Disable Out-of-View Streaming", "Out-of-View Texture Streaming"
};
inline constexpr std::array engine_keys{
    "speedtree_lod", "motion_quality", "contact_shadows", "shadow_quality", "shadow_resolution",
    "ssao_half_resolution", "ssr_half_resolution", "shadow_filter", "ssr_disable", "ssr_low_resolve",
    "dfao_disable", "oov_streaming_off"
};
inline constexpr std::array baseline_keys{
    "speedtree", "motion_quality", "contact_shadows", "shadow_quality", "shadow_resolution",
    "ssao_half", "ssr_half", "shadow_filter", "ssr_type", "ssr_resolve_point", "dfao",
    "out_of_view_streaming", "out_of_view_texture_streaming"
};
static_assert(engine_labels.size() == tweak_count && baseline_keys.size() == tweak_count);
static_assert(engine_keys.size() == oov_texture_streaming_off);
constexpr bool heavy_tweak(size_t i) { return i >= shadow_quality && i <= ssr_half; }
constexpr bool boolean_tweak(size_t i) {
    return i == ssao_half || i == ssr_half || i == dfao_disable || i == oov_streaming_off || i == oov_texture_streaming_off;
}

struct EngineSettings {
    bool enabled{}, always{}, force_night{}, heavy{};
    bool trigger_motion{true}, trigger_combat{true}, trigger_night{true};
    std::array<bool, engine_keys.size()> tweaks{true, true, true};
    bool tweak_enabled(size_t i) const { return tweaks[i == oov_texture_streaming_off ? oov_streaming_off : i]; }
    float speedtree_bias{3}, motion_target{};
    float shadow_target{1}, shadow_pixels{512};
    float night_start{20}, night_end{5};
    CrowdSettings crowd{};
    bool operator==(const EngineSettings&) const = default;
};

inline void sanitize(EngineSettings& e) {
    e.speedtree_bias = std::clamp(e.speedtree_bias, 0.0f, 5.0f);
    e.motion_target = std::clamp(e.motion_target, 0.0f, 1.0f);
    e.shadow_target = std::clamp(e.shadow_target, 0.0f, 4.0f);
    e.shadow_pixels = std::clamp(e.shadow_pixels, 256.0f, 4096.0f);
    e.night_start = std::clamp(e.night_start, 0.0f, 23.0f);
    e.night_end = std::clamp(e.night_end, 0.0f, 23.0f);
    sanitize(e.crowd);
}

inline bool is_night(float hour, float start, float end) {
    return start > end ? hour >= start || hour < end : hour >= start && hour < end;
}

} // namespace dd2
