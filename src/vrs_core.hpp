#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "engine_settings.hpp"

namespace dd2 {

struct Extent {
    uint32_t width{};
    uint32_t height{};
    bool operator==(const Extent&) const = default;
};

enum ActivationMode { adaptive, always_on, motion_only };
enum class Activity { idle, combat, motion, forced };

struct CenterSize {
    float width{100.0f};
    float height{100.0f};
    bool locked{true};
    // Defaults and v0.2.2/v0.2.3 configurations are relative to the profile.
    bool profile_relative{true};
    bool operator==(const CenterSize&) const = default;
};

inline void sanitize(CenterSize& size) {
    const float minimum = size.profile_relative ? 25.0f : 0.0f;
    const float maximum = size.profile_relative ? 1000.0f : 100.0f;
    size.width = std::clamp(size.width, minimum, maximum);
    size.height = std::clamp(size.height, minimum, maximum);
    if (!size.profile_relative && size.locked)
        size.width = size.height = std::max(size.width, size.height);
}

inline bool full_sharp_center(const CenterSize& size) {
    return !size.profile_relative && size.width >= 100.0f && size.height >= 100.0f;
}

struct Settings {
    float motion_threshold{2.2f};
    float hold{0.35f};
    float combat_exit_delay{2.0f};
    int activation_mode{adaptive};
    int profile_mode{};
    bool optimizer_enabled{true};
    bool enabled{true};
    bool verbose_logging{};
    bool shade_transparent_passes{true};
    bool allow_native_vrs{true};
    EngineSettings engine{};
    CenterSize center{};
    Extent reference{1280, 800};
};

inline constexpr Settings recommended_steam_deck_settings() {
    Settings s;
    s.motion_threshold = 1.8f;
    s.hold = 1.2f;
    s.combat_exit_delay = 2.5f;
    s.activation_mode = always_on;
    s.center = {85.0f, 85.0f, true, false};
    s.engine.enabled = s.engine.always = s.engine.heavy = true;
    s.engine.tweaks.fill(true);
    s.engine.motion_target = 1.0f;
    s.engine.shadow_pixels = 384.0f;
    auto& crowd = s.engine.crowd;
    crowd.enabled = crowd.npc = crowd.pawn = true;
    crowd.npc_baseline = 20;
    crowd.manual_npc_limit = 0;
    crowd.pawn_min = 2;
    crowd.pawn_baseline = 2;
    return s;
}

inline void sanitize(Settings& s) {
    sanitize(s.engine);
    s.motion_threshold = std::clamp(s.motion_threshold, 0.1f, 20.0f);
    s.hold = std::clamp(s.hold, 0.0f, 3.0f);
    s.combat_exit_delay = std::clamp(s.combat_exit_delay, 0.0f, 30.0f);
    s.activation_mode = std::clamp(s.activation_mode, int(adaptive), int(motion_only));
    s.profile_mode = std::clamp(s.profile_mode, 0, 1);
    sanitize(s.center);
    s.reference.width = std::clamp(s.reference.width, 320u, 7680u);
    s.reference.height = std::clamp(s.reference.height, 200u, 4320u);
}

using Quaternion = std::array<float, 4>;

inline bool normalize(Quaternion& q) {
    float norm = 0;
    for (float v : q) norm += v * v;
    if (!std::isfinite(norm) || norm < 1e-8f) return false;
    const float scale = 1.0f / std::sqrt(norm);
    for (float& v : q) v *= scale;
    return true;
}

inline float velocity(const Quaternion& previous, Quaternion current, float dt) {
    float dot = 0;
    for (size_t i = 0; i < current.size(); ++i) dot += previous[i] * current[i];
    if (dot < 0) for (float& v : current) v = -v;

    // Equivalent to 2*acos(abs(dot)) for normalized inputs, with better precision
    // for small deltas. q and -q represent the same rotation.
    float diff2 = 0;
    for (size_t i = 0; i < current.size(); ++i) {
        const float diff = previous[i] - current[i];
        diff2 += diff * diff;
    }
    return 4.0f * std::asin(std::clamp(0.5f * std::sqrt(diff2), 0.0f, 1.0f)) / dt;
}

struct Motion {
    bool active{};
    float remaining{};

    void update(float speed, float dt, const Settings& s) {
        const bool moving = speed >= s.motion_threshold;
        remaining = moving ? s.hold : std::max(0.0f, remaining - dt);
        active = moving || remaining > 0.0f;
    }
};

struct Combat {
    bool battling{};
    double exit_until{};

    void observe(bool in_battle, double now, float delay) {
        if (battling && !in_battle) exit_until = now + delay;
        battling = in_battle;
    }

    bool active(double now) const { return battling || now < exit_until; }
};

inline Activity activity(const Settings& s, bool moving, bool in_combat) {
    if (!s.optimizer_enabled || !s.enabled || full_sharp_center(s.center)) return Activity::idle;
    if (s.activation_mode == always_on) return Activity::forced;
    if (s.activation_mode == adaptive && in_combat) return Activity::combat;
    return moving ? Activity::motion : Activity::idle;
}

struct Profile { const char* name; float inner, mid; };
inline constexpr std::array profiles{
    Profile{"Default", 0.45f, 0.70f}, Profile{"Extreme", 0.32f, 0.50f}
};

inline constexpr uint8_t rate_1x1 = 0x0, rate_1x2 = 0x1, rate_2x1 = 0x4, rate_2x2 = 0x5;
inline constexpr float center_x = 0.5f, center_y = 0.46f;

inline float fullscreen_radius(Extent extent) {
    return std::hypot(extent.width * center_x, extent.height * (1.0f - center_y));
}

inline CenterSize resolve_center(Extent extent, uint32_t tile, const Profile& profile, CenterSize size) {
    if (!size.profile_relative) return size;
    const float transition = profile.mid / profile.inner;
    // Translate older settings using their original bounds so existing
    // configurations and untouched profiles retain their starting mask.
    const float available_x = std::max(1.0f, extent.width * center_x - tile);
    const float available_y = std::max(1.0f, extent.height * center_y - tile);
    const float max_x = available_x / transition;
    const float max_y = available_y / transition;
    const float full_radius = std::min({extent.width * 0.5f,
        available_x / profiles.front().mid, available_y / profiles.front().mid});
    const float radius_per_percent = full_radius * profile.inner / 100.0f;
    float x = size.width * radius_per_percent;
    float y = size.height * radius_per_percent;
    if (size.locked) {
        const float factor = std::min({1.0f, max_x / x, max_y / y});
        x *= factor;
        y *= factor;
    } else {
        x = std::min(x, max_x);
        y = std::min(y, max_y);
    }
    const float scale = 100.0f / fullscreen_radius(extent);
    return {x * scale, y * scale, size.locked, false};
}

struct ShadingEllipse {
    float x_radius{}, y_radius{}, transition_ratio{};
    bool full_screen{};
    float transition_ratio_sq{transition_ratio * transition_ratio};
    bool operator==(const ShadingEllipse&) const = default;
};

inline ShadingEllipse shading_ellipse(Extent extent, uint32_t tile, const Profile& profile, CenterSize size = {}) {
    size = resolve_center(extent, tile, profile, size);
    const float radius_per_percent = fullscreen_radius(extent) / 100.0f;
    const float ratio = profile.mid / profile.inner;
    return {radius_per_percent * size.width, radius_per_percent * size.height,
            ratio, full_sharp_center(size), ratio * ratio};
}

inline uint8_t rate_at(Extent extent, uint32_t tile, uint32_t x, uint32_t y,
                       const ShadingEllipse& ellipse, bool additional_rates) {
    if (ellipse.full_screen) return rate_1x1;
    if (ellipse.x_radius <= 0.0f || ellipse.y_radius <= 0.0f) return rate_2x2;
    const float dx = (std::min((x + 0.5f) * tile, extent.width - 0.5f) - extent.width * center_x) / ellipse.x_radius;
    const float dy = (std::min((y + 0.5f) * tile, extent.height - 0.5f) - extent.height * center_y) / ellipse.y_radius;
    const float distance_squared = dx * dx + dy * dy;
    if (distance_squared <= 1.0f) return rate_1x1;
    if (!additional_rates || distance_squared > ellipse.transition_ratio_sq) return rate_2x2;
    return ((x + y) & 1) ? rate_2x1 : rate_1x2;
}

} // namespace dd2
