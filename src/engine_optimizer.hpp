#pragma once

#include "engine_settings.hpp"
#include <string>
#include <optional>
#include <reframework/API.hpp>

namespace dd2::engine {

using Object = reframework::API::ManagedObject;
using V3 = std::array<float, 3>;
struct Camera {
    Object* game_object{};
    V3 position{}, forward{};
    bool valid{};
};
struct Status {
    int active_tweaks{}, throttled_npcs{}, pending_snapshot{};
    bool editing{}, night{}, clock_valid{}, npc_bindings{}, camera_tweaks_active{};
    float hour{};
    std::array<const char*, tweak_count> properties{};
    std::array<const char*, tweak_count> tweak_status{};
    struct Crowd {
        float fps{}, average{}, minimum{}, maximum{};
        int npc{}, pawn{}, vanilla_npc{}, vanilla_pawn{}, adjustments{};
        int processed{}, facial{}, joint{}, upper{}, partial{}, frozen{};
        unsigned bound_lods{}, list_sources{};
        CrowdMode mode{CrowdMode::warming};
        bool caps_ready{}, cap_write_failed{}, rule_hook{}, paused{}, pause_available{}, restoring{}, owned{};
    } crowd;
};
using Baselines = std::array<std::optional<double>, tweak_count>;

bool position(Object* transform, V3& value);
void update(const EngineSettings&, const Camera&, bool motion, bool combat, bool blocked, double now);
void edit(bool enabled);
void refresh();
void invalidate();
Status status();
Baselines baselines();
void restore_snapshot(const Baselines&);
void cancel_snapshot_restore();

} // namespace dd2::engine
