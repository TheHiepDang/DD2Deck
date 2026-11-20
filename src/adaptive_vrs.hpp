#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace dd2 {

struct GpuGateSettings {
    bool enabled{true};
    float target_ms{24.0f};
    float band_ms{3.0f};
};

struct GpuGate {
    float filtered_ms{};
    bool engaged{};

    void observe(float ms, const GpuGateSettings& settings) {
        if (!std::isfinite(ms) || ms <= 0.0f) return;
        filtered_ms = filtered_ms > 0.0f ? std::lerp(filtered_ms, ms, 0.18f) : ms;
        const float low = settings.target_ms - settings.band_ms;
        if (filtered_ms >= settings.target_ms) engaged = true;
        else if (filtered_ms <= low) engaged = false;
    }

    float weight(const GpuGateSettings& settings) const {
        const float low = settings.target_ms - settings.band_ms;
        return engaged ? std::clamp((filtered_ms - low) / settings.band_ms, 0.0f, 1.0f) : 0.0f;
    }
};

enum class MaskDirection : unsigned { uniform, left, right, top, bottom };
inline constexpr unsigned mask_levels = 3;
inline constexpr unsigned mask_variants = 1 + 5 * mask_levels;

inline unsigned adaptive_mask(float load, float alpha, float yaw, float pitch) {
    if (load <= 0.001f) return 0;
    MaskDirection direction{};
    const float moving = load * alpha;
    float strength = load;
    if (alpha > 0.08f && std::hypot(yaw, pitch) > 0.05f && moving > 0.01f) {
        if (std::abs(yaw) >= std::abs(pitch))
            direction = yaw > 0.0f ? MaskDirection::left : MaskDirection::right;
        else direction = pitch > 0.0f ? MaskDirection::bottom : MaskDirection::top;
        strength = moving;
    }
    const unsigned level = std::min(mask_levels - 1, static_cast<unsigned>(strength * mask_levels));
    return 1 + static_cast<unsigned>(direction) * mask_levels + level;
}

} // namespace dd2
