#pragma once

#include "engine_settings.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <reframework/API.hpp>

namespace dd2::engine {

using Object = reframework::API::ManagedObject;
// Comparison token only. Callers always reacquire the live SDK object.
inline uintptr_t object_key(const void* object) {
    return reinterpret_cast<uintptr_t>(object) * uintptr_t{0x9e3779b97f4a7c15ull};
}
enum class WorldReadiness { waiting, settling, ready };
struct CombatReading { bool active{}; const char* source{}; };
Object* gameplay_player(const reframework::API::TDB*);
std::optional<CombatReading> read_combat(const reframework::API::TDB*, Object* player);
struct Status {
    int active_tweaks{}, pending_snapshot{};
    bool editing{}, night{}, clock_valid{}, camera_tweaks_active{};
    float hour{};
    WorldReadiness world{WorldReadiness::waiting};
    std::array<const char*, tweak_count> properties{};
    std::array<const char*, tweak_count> tweak_status{};
    struct Crowd {
        float fps{}, average{}, minimum{}, maximum{};
        int npc{}, pawn{}, vanilla_npc{}, vanilla_pawn{}, adjustments{};
        CrowdMode mode{CrowdMode::warming};
        bool caps_ready{}, cap_write_failed{}, rule_hook{}, paused{}, pause_available{}, restoring{}, owned{};
    } crowd;
};
using Baselines = std::array<std::optional<double>, tweak_count>;

void update(const EngineSettings&, Object* player, Object* camera_object, Object* (*find_camera_object)(),
            bool motion, bool combat, bool blocked, double now);
void suspend_world();
void edit(bool enabled);
void refresh();
void reseed_crowd_limits();
void invalidate();
Status status();
Baselines baselines();
void restore_snapshot(const Baselines&);
void cancel_snapshot_restore();

} // namespace dd2::engine
