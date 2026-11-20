#pragma once

#include <algorithm>
#include <array>
#include "crowd_controller.hpp"

namespace dd2 {

enum EngineTweak { speedtree, motion_quality, contact_shadows, foliage, effects,
                   shadow_quality, shadow_resolution, ssao_half, ssr_half, tweak_count };
inline constexpr std::array engine_labels{
    "SpeedTree LOD Bias", "Motion Quality", "Contact Shadows", "Distant Foliage",
    "Particle LOD", "Shadow Quality", "Shadow Resolution", "SSAO Half Resolution",
    "SSR Half Resolution"
};
inline constexpr std::array engine_keys{
    "speedtree_lod", "motion_quality", "contact_shadows", "far_foliage_cull", "effect_lod",
    "shadow_quality", "shadow_resolution", "ssao_half_resolution", "ssr_half_resolution"
};

struct EngineSettings {
    bool enabled{}, always{}, force_night{}, heavy{}, npc{};
    bool trigger_motion{true}, trigger_combat{true}, trigger_night{true};
    bool effects_combat_only{true};
    std::array<bool, tweak_count> tweaks{true, true, true, true, true};
    float speedtree_bias{3}, motion_target{}, foliage_retention{70};
    float shadow_target{1}, shadow_pixels{512};
    float night_start{20}, night_end{5};
    float npc_dot{-0.25f}, npc_distance{8};
    CrowdSettings crowd{};
    bool operator==(const EngineSettings&) const = default;
};

inline void sanitize(EngineSettings& e) {
    e.speedtree_bias = std::clamp(e.speedtree_bias, 0.0f, 5.0f);
    e.motion_target = std::clamp(e.motion_target, 0.0f, 1.0f);
    e.foliage_retention = std::clamp(e.foliage_retention, 50.0f, 95.0f);
    e.shadow_target = std::clamp(e.shadow_target, 0.0f, 4.0f);
    e.shadow_pixels = std::clamp(e.shadow_pixels, 256.0f, 4096.0f);
    e.night_start = std::clamp(e.night_start, 0.0f, 23.0f);
    e.night_end = std::clamp(e.night_end, 0.0f, 23.0f);
    e.npc_dot = std::clamp(e.npc_dot, -0.9f, -0.15f);
    e.npc_distance = std::clamp(e.npc_distance, 8.0f, 50.0f);
    sanitize(e.crowd);
}

inline bool is_night(float hour, float start, float end) {
    return start > end ? hour >= start || hour < end : hour >= start && hour < end;
}

} // namespace dd2
