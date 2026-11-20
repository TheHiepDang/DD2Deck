#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace dd2 {

struct CrowdSettings {
    bool enabled{}, npc{true}, pawn{true}, lod{true}, chain{true};
    int fps_target{29}, fps_critical{25}, fps_headroom{31};
    int npc_min{12}, npc_baseline{20}, npc_max{35}, npc_step{2}, npc_emergency_step{4};
    int manual_npc_limit{};
    int pawn_min{3}, pawn_max{10};
    float facial_distance{20}, joint_distance{25}, upper_distance{30};
    float freeze_near{25}, freeze_far{50};
    int lod_interval{45}, adjust_interval{90}, sample_window{60}, hysteresis{180};
    float recovery_seconds{5};
    bool operator==(const CrowdSettings&) const = default;
};

inline void sanitize(CrowdSettings& s) {
    s.fps_target = std::clamp(s.fps_target, 20, 60);
    s.fps_critical = std::clamp(s.fps_critical, 15, std::min(45, s.fps_target));
    s.fps_headroom = std::clamp(s.fps_headroom, std::max(25, s.fps_target), 65);
    s.npc_min = std::clamp(s.npc_min, 6, 30);
    s.npc_max = std::clamp(s.npc_max, std::max(s.npc_min, 20), 60);
    s.npc_baseline = std::clamp(s.npc_baseline, s.npc_min, s.npc_max);
    s.npc_step = std::clamp(s.npc_step, 1, 12);
    s.npc_emergency_step = std::clamp(s.npc_emergency_step, s.npc_step, 20);
    if (s.manual_npc_limit) s.manual_npc_limit = std::clamp(s.manual_npc_limit, s.npc_min, s.npc_max);
    s.pawn_min = std::clamp(s.pawn_min, 0, 20);
    s.pawn_max = std::clamp(s.pawn_max, s.pawn_min, 30);
    s.facial_distance = std::clamp(s.facial_distance, 10.0f, 60.0f);
    s.joint_distance = std::clamp(s.joint_distance, 15.0f, 80.0f);
    s.upper_distance = std::clamp(s.upper_distance, 20.0f, 100.0f);
    s.freeze_near = std::clamp(s.freeze_near, 10.0f, 60.0f);
    s.freeze_far = std::clamp(s.freeze_far, std::max(25.0f, s.freeze_near + 1.0f), 120.0f);
    s.lod_interval = std::clamp(s.lod_interval, 1, 600);
    s.adjust_interval = std::clamp(s.adjust_interval, 1, 600);
    s.sample_window = std::clamp(s.sample_window, 20, 240);
    s.hysteresis = std::clamp(s.hysteresis, 0, 1200);
    s.recovery_seconds = std::clamp(s.recovery_seconds, 5.0f, 10.0f);
}

enum class CrowdMode { warming, normal, throttle, critical, quality };

struct CrowdController {
    std::array<float, 240> samples{};
    float fps{}, average{}, minimum{}, maximum{};
    unsigned write_index{}, count{};
    int npc{}, pawn{}, direction{}, frames_since_turn{}, frames_since_adjust{}, adjustments{};
    CrowdMode mode{CrowdMode::warming};
    double headroom_seconds{};

    void set_limits(const CrowdSettings& s, int pawn_baseline) {
        npc = s.npc_baseline;
        pawn = std::clamp(pawn_baseline, s.pawn_min, s.pawn_max);
        frames_since_adjust = frames_since_turn = direction = 0;
        headroom_seconds = 0;
    }

    void observe(double dt, const CrowdSettings& s) {
        if (dt <= 0.0 || dt > 0.5 || !std::isfinite(dt)) { headroom_seconds = 0; return; }
        fps = float(1.0 / dt);
        samples[write_index] = fps;
        write_index = (write_index + 1) % samples.size();
        count = std::min(count + 1, unsigned(samples.size()));
        const unsigned n = std::min(count, unsigned(s.sample_window));
        float sum = 0;
        minimum = maximum = fps;
        for (unsigned i = 0; i < n; ++i) {
            const float v = samples[(write_index + samples.size() - 1 - i) % samples.size()];
            sum += v;
            minimum = std::min(minimum, v);
            maximum = std::max(maximum, v);
        }
        average = sum / float(n);
        if (n < 20) return;
        frames_since_turn = std::min(frames_since_turn + 1, 1200);
        ++frames_since_adjust;
        const auto previous_mode = mode;
        mode = average < s.fps_critical ? CrowdMode::critical : average < s.fps_target ? CrowdMode::throttle :
            average > s.fps_headroom ? CrowdMode::quality : CrowdMode::normal;
        if (mode == CrowdMode::quality) headroom_seconds = std::min(headroom_seconds + dt, 10.0);
        else headroom_seconds = 0;
        const int next = mode == CrowdMode::critical || mode == CrowdMode::throttle ? -1 :
            mode == CrowdMode::quality ? 1 : 0;
        const bool emergency = mode == CrowdMode::critical && previous_mode != CrowdMode::critical;
        if (!emergency && frames_since_adjust < s.adjust_interval) return;
        frames_since_adjust = 0;
        if (!next || (next > 0 && (headroom_seconds < s.recovery_seconds ||
                                  (direction < 0 && frames_since_turn < s.hysteresis)))) return;
        const int step = mode == CrowdMode::critical ? s.npc_emergency_step : s.npc_step;
        const int old_npc = npc, old_pawn = pawn;
        if (s.npc && !s.manual_npc_limit) npc = std::clamp(npc + next * step, s.npc_min, s.npc_max);
        if (s.pawn) pawn = std::clamp(pawn + next * std::max(1, step / 2), s.pawn_min, s.pawn_max);
        if (old_npc != npc || old_pawn != pawn) ++adjustments;
        if (direction != next) frames_since_turn = 0;
        direction = next;
    }
};

inline float chain_freeze(float distance_sq, const CrowdSettings& s) {
    const float near_sq = s.freeze_near * s.freeze_near;
    const float far_sq = s.freeze_far * s.freeze_far;
    return std::clamp((distance_sq - near_sq) / (far_sq - near_sq), 0.0f, 1.0f);
}

} // namespace dd2
