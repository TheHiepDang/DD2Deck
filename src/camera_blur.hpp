#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace dd2 {

struct BorderBlurSettings {
    bool enabled{true};
    float multiplier{2.0f};
    float threshold{0.15f};
    float fade_ms{120.0f};
};

inline void sanitize(BorderBlurSettings& settings) {
    settings.multiplier = std::clamp(settings.multiplier, 1.0f, 3.0f);
    settings.threshold = std::clamp(settings.threshold, 0.05f, 1.0f);
    settings.fade_ms = std::clamp(settings.fade_ms, 50.0f, 250.0f);
}

struct CameraPan {
    float yaw{}, pitch{}, roll{};
    float speed() const { return std::hypot(yaw, pitch, roll); }
};

// The REFramework world rotation is an XYZW quaternion. Project the next
// world forward axis into the previous camera's basis to measure yaw/pitch.
// The up axis measures roll independently of translation.
inline CameraPan camera_pan(const std::array<float, 4>& before,
                            const std::array<float, 4>& after, float dt) {
    const auto [x, y, z, w] = after;
    const std::array forward{2.0f * (x * z + w * y), 2.0f * (y * z - w * x),
                             1.0f - 2.0f * (x * x + y * y)};
    const std::array current_up{2.0f * (x * y - w * z), 1.0f - 2.0f * (x * x + z * z),
                               2.0f * (y * z + w * x)};
    const auto [px, py, pz, pw] = before;
    const std::array right{1.0f - 2.0f * (py * py + pz * pz), 2.0f * (px * py + pw * pz),
                           2.0f * (px * pz - pw * py)};
    const std::array up{2.0f * (px * py - pw * pz), 1.0f - 2.0f * (px * px + pz * pz),
                        2.0f * (py * pz + pw * px)};
    const std::array previous_forward{2.0f * (px * pz + pw * py), 2.0f * (py * pz - pw * px),
                                     1.0f - 2.0f * (px * px + py * py)};
    const auto project = [](const std::array<float, 3>& v, const std::array<float, 3>& axis) {
        return v[0] * axis[0] + v[1] * axis[1] + v[2] * axis[2];
    };
    const float horizontal = project(forward, right), vertical = project(forward, up);
    const float depth = project(forward, previous_forward);
    return {std::atan2(horizontal, depth) / dt,
            std::atan2(vertical, std::hypot(horizontal, depth)) / dt,
            std::atan2(-project(current_up, right), project(current_up, up)) / dt};
}

struct CameraBlurEnvelope {
    CameraPan direction{};
    float speed{}, alpha{}, seconds_under_threshold{};

    void update(CameraPan pan, float dt, const BorderBlurSettings& settings) {
        if (!std::isfinite(dt) || dt <= 0.0f || !std::isfinite(pan.speed())) {
            *this = {};
            return;
        }
        const float raw = pan.speed();
        const float fade = settings.fade_ms * 0.001f;
        const bool panning = raw > settings.threshold;
        seconds_under_threshold = panning ? 0.0f : seconds_under_threshold + dt;
        if (seconds_under_threshold >= fade) {
            *this = {};
            return;
        }
        // Roughly 99% of the exponential tail elapses by the configured fade.
        const auto blend = [&](float time_constant) { return -std::expm1(-dt / time_constant); };
        const float attack = blend(0.020f), release = blend(fade / 4.60517f);
        const float filter = panning ? attack : release;
        const float target_speed = panning ? raw : 0.0f;
        speed = std::lerp(speed, target_speed, filter);
        direction.yaw = std::lerp(direction.yaw, panning ? pan.yaw : 0.0f, filter);
        direction.pitch = std::lerp(direction.pitch, panning ? pan.pitch : 0.0f, filter);
        direction.roll = std::lerp(direction.roll, panning ? pan.roll : 0.0f, filter);
        constexpr float ramp = 3.0f; // rad/s above the threshold for full strength
        const float target = panning ? std::clamp((speed - settings.threshold) / ramp, 0.0f, 1.0f) : 0.0f;
        alpha = std::lerp(alpha, target, target > alpha ? attack : release);
        if (alpha <= 0.001f) alpha = 0.0f;
    }
};

} // namespace dd2
