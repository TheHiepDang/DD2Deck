#include "graphics.hpp"

#include <dxgi1_2.h>
#include <wrl/client.h>
#include <string>

#include <reframework/API.hpp>
#include <nlohmann/json.hpp>
#include "audit_log.hpp"
#include "engine_optimizer.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <type_traits>

namespace {
using namespace dd2;
using reframework::API;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

const REFrameworkPluginInitializeParam* host{};
std::filesystem::path config_path;
std::mutex frame_mutex;
Settings settings;
std::atomic<Activity> active_reason{Activity::idle};
std::atomic<bool> g_in_combat{false};
float camera_velocity{};
bool camera_found{};
CameraBlurEnvelope camera_blur{};
GpuGate gpu_gate{};
float gpu_time_ms{}, gpu_load_weight{};
Clock::time_point last_gpu_sample{};
std::atomic<bool> renderer_reset{};
bool perf_lua_present{}, timing_callbacks{}, timing_valid{};
Extent previous_output{};
unsigned blur_probe_frames{};
const char* save_status{};
const char* snapshot_status{};
bool confirm_snapshot_restore{};
bool renderer_initialized{};
const char* last_renderer_error{};

struct CameraSample {
    API::ManagedObject* camera{};
    API::ManagedObject* game_object{};
    API::ManagedObject* transform{};
    Quaternion rotation{};
};
CameraSample previous{};
Clock::time_point previous_time{};
Clock::time_point next_diagnostics{};
Clock::time_point next_combat_poll{};
Motion motion{};
Combat combat{};

struct BattleBindings {
    API::TypeDefinition* type{};
    API::Method* is_battle{};
    API::Field* is_battle_field{};
    API::Method* battle_state{};
    size_t state_bytes{};
    bool resolved{};
} battle;

const char* battle_source{"Waiting for BattleManager"};

struct ObjectMethod {
    API::TypeDefinition* type{};
    API::Method* method{};

    API::Method* get(API::ManagedObject* object, const char* name) {
        if (!object) return nullptr;
        auto* current = object->get_type_definition();
        if (!current) return nullptr;
        if (current != type || !method) {
            type = current;
            method = type->find_method(name);
        }
        return method;
    }
};

struct CameraBindings {
    const API::TDB* tdb{};
    API::TypeDefinition* manager{};
    API::TypeDefinition* quaternion{};
    API::Method* main_view{};
    API::Method* primary_camera{};
    ObjectMethod get_object, get_transform, get_rotation;
} camera_bindings;

struct ImVec2 { float x, y; };
struct Ui {
    bool (*header)(const char*, int){};
    void (*text)(const char*, const char*){};
    bool (*checkbox)(const char*, bool*){};
    bool (*radio)(const char*, int*, int){};
    bool (*slider)(const char*, float*, float, float, const char*, int){};
    bool (*button)(const char*, ImVec2){};

    bool bind(HMODULE module) {
        header = reinterpret_cast<decltype(header)>(GetProcAddress(module, "igCollapsingHeader_TreeNodeFlags"));
        text = reinterpret_cast<decltype(text)>(GetProcAddress(module, "igTextUnformatted"));
        checkbox = reinterpret_cast<decltype(checkbox)>(GetProcAddress(module, "igCheckbox"));
        radio = reinterpret_cast<decltype(radio)>(GetProcAddress(module, "igRadioButton_IntPtr"));
        slider = reinterpret_cast<decltype(slider)>(GetProcAddress(module, "igSliderFloat"));
        button = reinterpret_cast<decltype(button)>(GetProcAddress(module, "igButton"));
        return header && text && checkbox && radio && slider && button;
    }
} ui;
bool ui_ready{};

constexpr std::array crowd_flags{
    std::pair{"cpu_opt_enabled", &CrowdSettings::enabled}, std::pair{"npc_enabled", &CrowdSettings::npc},
    std::pair{"pawn_enabled", &CrowdSettings::pawn}, std::pair{"lod_enabled", &CrowdSettings::lod},
    std::pair{"chain_enabled", &CrowdSettings::chain}
};
constexpr std::array crowd_numbers{
    std::pair{"fps_target", &CrowdSettings::fps_target}, std::pair{"fps_critical", &CrowdSettings::fps_critical},
    std::pair{"fps_headroom", &CrowdSettings::fps_headroom}, std::pair{"npc_min", &CrowdSettings::npc_min},
    std::pair{"npc_baseline", &CrowdSettings::npc_baseline}, std::pair{"npc_max", &CrowdSettings::npc_max},
    std::pair{"npc_step", &CrowdSettings::npc_step}, std::pair{"npc_emergency_step", &CrowdSettings::npc_emergency_step},
    std::pair{"manual_npc_limit", &CrowdSettings::manual_npc_limit},
    std::pair{"pawn_min", &CrowdSettings::pawn_min}, std::pair{"pawn_max", &CrowdSettings::pawn_max},
    std::pair{"lod_interval", &CrowdSettings::lod_interval}, std::pair{"adjust_interval", &CrowdSettings::adjust_interval},
    std::pair{"sample_window", &CrowdSettings::sample_window}, std::pair{"hysteresis", &CrowdSettings::hysteresis}
};
constexpr std::array crowd_distances{
    std::pair{"lod_facial_dist", &CrowdSettings::facial_distance}, std::pair{"lod_joint_dist", &CrowdSettings::joint_distance},
    std::pair{"lod_upper_dist", &CrowdSettings::upper_distance}, std::pair{"chain_freeze_near", &CrowdSettings::freeze_near},
    std::pair{"chain_freeze_far", &CrowdSettings::freeze_far}
};

bool config_flag(const Json& j, const char* key, bool fallback) {
    const auto it = j.find(key);
    return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

float config_number(const Json& j, const char* key, float fallback) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return fallback;
    const float value = it->get<float>();
    return std::isfinite(value) ? value : fallback;
}

void read_crowd_config(const Json& j, CrowdSettings& c) {
    for (const auto& [key, member] : crowd_flags) c.*member = config_flag(j, key, c.*member);
    for (const auto& [key, member] : crowd_numbers)
        c.*member = int(std::round(std::clamp(config_number(j, key, float(c.*member)), 0.0f, 1200.0f)));
    for (const auto& [key, member] : crowd_distances) {
        const float m = c.*member;
        c.*member = std::sqrt(std::max(0.0f, config_number(j, key, m * m)));
    }
    c.recovery_seconds = config_number(j, "crowd_recovery_seconds", c.recovery_seconds);
}

void read_engine_tweaks(const Json& j, EngineSettings& e) {
    for (size_t i = 0; i < e.tweaks.size(); ++i) e.tweaks[i] = config_flag(j, engine_keys[i], e.tweaks[i]);
    e.speedtree_bias = config_number(j, "speedtree_lod_bias", e.speedtree_bias);
    e.motion_target = config_number(j, "motion_quality_target", e.motion_target);
    e.foliage_retention = config_number(j, "far_foliage_retention_pct", e.foliage_retention);
    e.shadow_target = config_number(j, "shadow_quality_target", e.shadow_target);
    e.shadow_pixels = config_number(j, "shadow_resolution_target", e.shadow_pixels);
    e.night_start = config_number(j, "night_start_hour", e.night_start);
    e.night_end = config_number(j, "night_end_hour", e.night_end);
}

void import_lua_settings(Settings& s) {
    if (config_path.empty()) return;
    std::ifstream file{config_path.parent_path() / "AdaptivePerformanceOptimizer.json"};
    if (!file) return;
    const auto j = Json::parse(file, nullptr, false);
    if (!j.is_object() || !j.contains("cpu_opt_enabled")) return;
    auto& e = s.engine;
    const auto mode = j.find("mode");
    const auto mode_name = mode != j.end() && mode->is_string() ? mode->get<std::string>() : "always";
    e.enabled = mode_name != "off";
    e.always = mode_name == "always";
    e.force_night = mode_name == "night";
    e.trigger_motion = config_flag(j, "trigger_motion", e.trigger_motion);
    e.trigger_combat = config_flag(j, "trigger_combat", e.trigger_combat);
    e.trigger_night = config_flag(j, "trigger_night", e.trigger_night);
    e.effects_combat_only = false;
    read_engine_tweaks(j, e);
    e.heavy = std::any_of(e.tweaks.begin() + shadow_quality, e.tweaks.end(), [](bool enabled) { return enabled; });
    s.motion_threshold = config_number(j, "motion_threshold", 1.8f);
    s.hold = config_number(j, "motion_hold_seconds", 1.2f);
    s.combat_exit_delay = config_number(j, "combat_hold_seconds", 2.5f);
    read_crowd_config(j, e.crowd);
}

Settings read_config() {
    Settings s;
    try {
        import_lua_settings(s);
        sanitize(s);
        std::ifstream file{config_path};
        if (!file) return s;
        const auto j = Json::parse(file);
        if (!j.is_object()) return s;
        const auto number = [&](const char* key, float fallback) { return config_number(j, key, fallback); };
        const auto flag = [&](const char* key, bool fallback) { return config_flag(j, key, fallback); };
        s.motion_threshold = number("motion_threshold", number("fast_threshold", s.motion_threshold));
        s.hold = number("hold_seconds", s.hold);
        s.combat_exit_delay = number("combat_exit_delay", s.combat_exit_delay);
        const auto mode = [&](const char* key, int fallback, int last) {
            const auto it = j.find(key);
            if (it == j.end() || !it->is_number_integer()) return fallback;
            const double value = it->get<double>();
            return value >= 0 && value <= last ? static_cast<int>(value) : fallback;
        };
        s.activation_mode = mode("activation_mode", flag("force_always_on", false) ? always_on : adaptive, motion_only);
        s.profile_mode = mode("profile_mode", 0, int(profiles.size()) - 1);
        s.enabled = flag("enabled", s.enabled);
        s.verbose_logging = flag("verbose_logging", s.verbose_logging);
        s.shade_transparent_passes = flag("shade_transparent_passes", s.shade_transparent_passes);
        s.border_blur.enabled = flag("border_motion_blur", s.border_blur.enabled);
        s.border_blur.multiplier = number("border_blur_multiplier", s.border_blur.multiplier);
        s.border_blur.threshold = number("border_blur_camera_threshold", s.border_blur.threshold);
        s.border_blur.fade_ms = number("border_blur_fade_ms", s.border_blur.fade_ms);
        s.gpu_gate.enabled = flag("adaptive_gpu_load_trigger", s.gpu_gate.enabled);
        s.gpu_gate.target_ms = number("target_gpu_frame_time_ms", s.gpu_gate.target_ms);
        s.gpu_gate.band_ms = number("gpu_hysteresis_band_ms", s.gpu_gate.band_ms);
        auto& e = s.engine;
        e.enabled = flag("native_engine_optimizer", e.enabled);
        e.always = flag("engine_force_on", e.always);
        e.force_night = flag("engine_force_night", e.force_night);
        e.heavy = flag("heavy_render_target_tweaks", e.heavy);
        e.npc = flag("behind_camera_npc_throttling", e.npc);
        e.trigger_motion = flag("engine_trigger_motion", e.trigger_motion);
        e.trigger_combat = flag("engine_trigger_combat", e.trigger_combat);
        e.trigger_night = flag("engine_trigger_night", e.trigger_night);
        e.effects_combat_only = flag("effect_lod_combat_only", e.effects_combat_only);
        read_engine_tweaks(j, e);
        read_crowd_config(j, e.crowd);
        e.npc_dot = number("npc_behind_dot", e.npc_dot);
        e.npc_distance = number("npc_min_distance", e.npc_distance);
        s.center.width = number("center_width_percent", s.center.width);
        s.center.height = number("center_height_percent", s.center.height);
        s.center.locked = flag("lock_center_aspect_ratio", s.center.locked);
        s.center.profile_relative = !flag("center_size_uses_full_range", false);
        s.reference.width = uint32_t(std::clamp(number("render_width", float(s.reference.width)), 320.0f, 7680.0f));
        s.reference.height = uint32_t(std::clamp(number("render_height", float(s.reference.height)), 200.0f, 4320.0f));
    } catch (...) {
        guarded([] { API::get()->log_warn("[DD2 VRS] Could not parse config. Using defaults."); return true; });
    }
    sanitize(s);
    return s;
}

bool save_config(const Settings& s) {
    if (config_path.empty()) return false;
    Json j{
        {"enabled", s.enabled}, {"activation_mode", s.activation_mode}, {"profile_mode", s.profile_mode},
        {"verbose_logging", s.verbose_logging}, {"motion_threshold", s.motion_threshold},
        {"shade_transparent_passes", s.shade_transparent_passes},
        {"border_motion_blur", s.border_blur.enabled},
        {"border_blur_multiplier", s.border_blur.multiplier},
        {"border_blur_camera_threshold", s.border_blur.threshold},
        {"border_blur_fade_ms", s.border_blur.fade_ms},
        {"adaptive_gpu_load_trigger", s.gpu_gate.enabled},
        {"target_gpu_frame_time_ms", s.gpu_gate.target_ms},
        {"gpu_hysteresis_band_ms", s.gpu_gate.band_ms},
        {"center_width_percent", s.center.width}, {"center_height_percent", s.center.height},
        {"lock_center_aspect_ratio", s.center.locked},
        {"center_size_uses_full_range", !s.center.profile_relative},
        {"hold_seconds", s.hold}, {"combat_exit_delay", s.combat_exit_delay},
        {"render_width", s.reference.width}, {"render_height", s.reference.height}
    };
    const auto& e = s.engine;
    j.update(Json{{"native_engine_optimizer", e.enabled}, {"engine_force_on", e.always}, {"engine_force_night", e.force_night},
        {"heavy_render_target_tweaks", e.heavy}, {"behind_camera_npc_throttling", e.npc},
        {"engine_trigger_motion", e.trigger_motion}, {"engine_trigger_combat", e.trigger_combat},
        {"engine_trigger_night", e.trigger_night}, {"effect_lod_combat_only", e.effects_combat_only},
        {"speedtree_lod_bias", e.speedtree_bias},
        {"motion_quality_target", e.motion_target}, {"far_foliage_retention_pct", e.foliage_retention},
        {"shadow_quality_target", e.shadow_target}, {"shadow_resolution_target", e.shadow_pixels},
        {"night_start_hour", e.night_start}, {"night_end_hour", e.night_end},
        {"npc_behind_dot", e.npc_dot}, {"npc_min_distance", e.npc_distance}});
    for (size_t i = 0; i < e.tweaks.size(); ++i) j[engine_keys[i]] = e.tweaks[i];
    const auto& c = e.crowd;
    for (const auto& [key, member] : crowd_flags) j[key] = c.*member;
    for (const auto& [key, member] : crowd_numbers) j[key] = c.*member;
    for (const auto& [key, member] : crowd_distances) j[key] = (c.*member) * (c.*member);
    j["crowd_recovery_seconds"] = c.recovery_seconds;
    std::filesystem::create_directories(config_path.parent_path());
    auto temp = config_path;
    temp += L".tmp";
    std::ofstream file{temp, std::ios::binary | std::ios::trunc};
    file << j.dump(2) << '\n';
    file.close();
    if (!file || !MoveFileExW(temp.c_str(), config_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code error;
        std::filesystem::remove(temp, error);
        return false;
    }
    return true;
}

bool save_baseline(const engine::Baselines& values) {
    if (config_path.empty()) return false;
    constexpr const char* ids[]{"speedtree", "motion_quality", "contact_shadows", "far_foliage", "effect_lod",
        "shadow_quality", "shadow_resolution", "ssao_half", "ssr_half"};
    Json snapshot = Json::object();
    for (size_t i = 0; i < values.size(); ++i) if (values[i]) {
        if (i >= ssao_half) snapshot[ids[i]] = {{"kind", "boolean"}, {"value", *values[i] != 0}};
        else snapshot[ids[i]] = {{"kind", "number"}, {"value", *values[i]}};
    }
    if (snapshot.empty()) return false;
    const auto path = config_path.parent_path() / "AdaptivePerformanceOptimizer_baseline.json";
    auto temporary = path;
    temporary += L".tmp";
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{temporary, std::ios::binary | std::ios::trunc};
    file << Json({{"format", 1}, {"source", "DD2 VRS native"}, {"values", snapshot}}).dump(2) << '\n';
    file.close();
    return file && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

std::optional<engine::Baselines> load_baseline() {
    if (config_path.empty()) return {};
    std::ifstream file{config_path.parent_path() / "AdaptivePerformanceOptimizer_baseline.json"};
    if (!file) return {};
    const auto snapshot = Json::parse(file);
    if (!snapshot.is_object() || snapshot.value("format", 0) != 1 ||
        !snapshot.contains("values") || !snapshot["values"].is_object()) return {};
    constexpr const char* ids[]{"speedtree", "motion_quality", "contact_shadows", "far_foliage", "effect_lod",
        "shadow_quality", "shadow_resolution", "ssao_half", "ssr_half"};
    engine::Baselines values{};
    for (size_t i = 0; i < values.size(); ++i) {
        const auto it = snapshot["values"].find(ids[i]);
        if (it == snapshot["values"].end() || !it->is_object() || !it->contains("value")) continue;
        const auto& value = (*it)["value"];
        if (i >= ssao_half && value.is_boolean()) values[i] = value.get<bool>();
        if (i < ssao_half && value.is_number()) {
            const auto v = value.get<double>();
            if (std::isfinite(v)) values[i] = v;
        }
    }
    if (std::none_of(values.begin(), values.end(), [](const auto& v) { return v.has_value(); })) return {};
    return values;
}

bool invoke(API::Method* method, void* instance, reframework::InvokeRet& out) {
    if (!method || !instance) return false;
    return host->sdk->method->invoke(*method, instance, nullptr, 0, &out, sizeof(out)) ==
        REFRAMEWORK_ERROR_NONE && !out.exception_thrown;
}

void resolve_battle(const API::TDB* tdb) {
    battle.resolved = true;
    battle.type = tdb->find_type("app.BattleManager");
    auto* boolean = tdb->find_type("System.Boolean");
    if (!battle.type) return;
    const auto getter = [](const char* name) -> API::Method* {
        auto* method = battle.type->find_method(name);
        return method && !method->is_static() && method->get_num_params() == 0 ? method : nullptr;
    };
    guarded([&] {
        auto* method = getter("get_IsBattle");
        if (method && boolean && method->get_return_type() == boolean) battle.is_battle = method;
        return true;
    });
    if (host->sdk->field) guarded([&] {
        auto* field = battle.type->find_field("_IsBattle");
        if (field && boolean && !field->is_static() && field->get_type() == boolean)
            battle.is_battle_field = field;
        return true;
    });
    guarded([&] {
        auto* method = getter("get_BattleState");
        auto* type = method ? method->get_return_type() : nullptr;
        if (!type) return false;
        if (!type->is_enum()) {
            const auto name = type->get_full_name();
            if (name != "System.Byte" && name != "System.SByte" && name != "System.Int16" &&
                name != "System.UInt16" && name != "System.Int32" && name != "System.UInt32" &&
                name != "System.Int64" && name != "System.UInt64") return false;
        }
        const auto size = type->get_valuetype_size();
        if (size != 1 && size != 2 && size != 4 && size != 8) return false;
        battle.battle_state = method;
        battle.state_bytes = size;
        return true;
    });
}

std::optional<bool> read_battle() {
    auto& api = API::get();
    if (!api->get_vm_context()) return {};
    if (!battle.is_battle && !battle.is_battle_field && !battle.battle_state) return {};
    auto* manager = api->get_managed_singleton("app.BattleManager");
    if (!manager || manager->get_type_definition() != battle.type) return {};

    bool active{};
    if (battle.is_battle && guarded([&] {
        reframework::InvokeRet out{};
        if (!invoke(battle.is_battle, manager, out)) return false;
        active = out.bytes[0] != 0;
        return true;
    })) {
        battle_source = "get_IsBattle";
        return active;
    }
    if (battle.is_battle_field && guarded([&] {
        const auto* value = static_cast<const uint8_t*>(battle.is_battle_field->get_data_raw(manager));
        if (!value) return false;
        active = *value != 0;
        return true;
    })) {
        battle_source = "_IsBattle";
        return active;
    }
    if (battle.battle_state && guarded([&] {
        reframework::InvokeRet out{};
        if (!invoke(battle.battle_state, manager, out)) return false;
        active = std::any_of(out.bytes.begin(), out.bytes.begin() + battle.state_bytes,
                             [](auto byte) { return byte != 0; });
        return true;
    })) {
        battle_source = "get_BattleState";
        return active;
    }
    return {};
}

void reset_combat() {
    combat = {};
    next_combat_poll = {};
    g_in_combat.store(false, std::memory_order_relaxed);
    battle_source = "Waiting for BattleManager";
}

void update_combat(Clock::time_point now, const Settings& current) {
    using namespace std::chrono_literals;
    const auto& e = current.engine;
    const bool engine_combat = e.enabled && !perf_lua_present &&
        ((e.tweaks[effects] && e.effects_combat_only) || (!e.always && !e.force_night && e.trigger_combat));
    if ((!current.enabled || current.activation_mode != adaptive || current.gpu_gate.enabled) && !engine_combat) return;
    const double seconds = std::chrono::duration<double>(now.time_since_epoch()).count();
    if (now >= next_combat_poll) {
        next_combat_poll = now + 1s;
        std::optional<bool> sample;
        guarded([&] {
            // Initialization normally resolves the TDB. If the host had no TDB
            // at load time, resolve it once as soon as it becomes available.
            if (!battle.resolved) if (const auto* tdb = API::get()->tdb()) resolve_battle(tdb);
            sample = read_battle();
            return true;
        });
        if (!sample) battle_source = "BattleManager unavailable";
        combat.observe(sample.value_or(false), seconds, current.combat_exit_delay);
    }
    g_in_combat.store(combat.active(seconds), std::memory_order_relaxed);
}

int select_preset(const Settings& current) {
    auto reason = activity(current, motion.active, g_in_combat.load(std::memory_order_relaxed));
    if (current.gpu_gate.enabled && current.activation_mode != always_on && current.enabled &&
        !full_sharp_center(current.center)) {
        reason = gpu_load_weight > 0.001f ? Activity::gpu_load : Activity::idle;
    }
    active_reason.store(reason, std::memory_order_relaxed);
    return reason == Activity::idle ? 0 : current.profile_mode + 1;
}

const char* reason_label(Activity reason) {
    switch (reason) {
    case Activity::combat: return "Combat";
    case Activity::motion: return "Motion";
    case Activity::forced: return "Forced";
    case Activity::gpu_load: return "GPU load";
    default: return "Idle";
    }
}

API::ManagedObject* object_result(API::Method* method, void* instance) {
    reframework::InvokeRet out{};
    return invoke(method, instance, out) ? static_cast<API::ManagedObject*>(out.ptr) : nullptr;
}

bool read_camera(CameraSample& sample, bool need_rotation) {
    auto& api = API::get();
    const auto* tdb = api->tdb();
    if (!tdb || !api->get_vm_context()) return false;
    auto& binding = camera_bindings;
    if (binding.tdb != tdb || !binding.primary_camera) {
        auto* manager = tdb->find_type("via.SceneManager");
        auto* view = tdb->find_type("via.SceneView");
        auto* quaternion = tdb->find_type("via.Quaternion");
        if (!manager || !view || !quaternion) return false;
        auto* main_view = manager->find_method("get_MainView");
        auto* primary = view->find_method("get_PrimaryCamera");
        if (!main_view || !primary) return false;
        binding = {tdb, manager, quaternion, main_view, primary, {}, {}, {}};
    }

    // Mirrors REFramework's Lua sdk.get_primary_camera implementation.
    auto* view = object_result(binding.main_view, binding.manager->get_instance());
    auto* camera = object_result(binding.primary_camera, view);
    if (!camera) return false;
    auto* object = object_result(binding.get_object.get(camera, "get_GameObject"), camera);
    sample.camera = camera;
    sample.game_object = object;
    if (!need_rotation) return object != nullptr;
    auto* transform = object_result(binding.get_transform.get(object, "get_Transform"), object);
    auto* rotation = binding.get_rotation.get(transform, "get_Rotation");
    if (!rotation || rotation->get_return_type() != binding.quaternion) return false;

    reframework::InvokeRet out{};
    if (!invoke(rotation, transform, out)) return false;
    std::memcpy(sample.rotation.data(), out.bytes.data(), sizeof(sample.rotation));
    sample.transform = transform;
    return normalize(sample.rotation);
}

void log_diagnostics(const Settings& settings_snapshot) {
    using graphics::Diagnostic;
    using namespace std::chrono_literals;
    if (!settings_snapshot.verbose_logging) {
        next_diagnostics = {};
        return;
    }
    const auto now = Clock::now();
    if (next_diagnostics == Clock::time_point{}) next_diagnostics = now + 5s;
    if (now < next_diagnostics) return;
    next_diagnostics = now + 5s;

    const auto snapshot = graphics::status();
    if (snapshot.tier < 2 || !snapshot.tile) return;
    const auto report = graphics::take_diagnostics();
    const auto n = [&](Diagnostic key) { return static_cast<unsigned long long>(report[key]); };
    auto& api = API::get();
    api->log_info("[DD2 VRS] Diagnostics | %s | output=%ux%u scene=%ux%u image=%ux%u tiles=%ux%u"
        " enabled=%u activation_mode=%d profile=%s preset=%d reason=%s camera=%s battle_source=%s shade_transparent=%u",
        snapshot.message, snapshot.output.width, snapshot.output.height,
        snapshot.scene.width, snapshot.scene.height, snapshot.image.width, snapshot.image.height,
        (snapshot.image.width + snapshot.tile - 1) / snapshot.tile,
        (snapshot.image.height + snapshot.tile - 1) / snapshot.tile, unsigned(settings_snapshot.enabled),
        settings_snapshot.activation_mode, profiles[settings_snapshot.profile_mode].name, snapshot.preset,
        reason_label(active_reason.load()), camera_found ? "available" : "missing", battle_source,
        unsigned(settings_snapshot.shade_transparent_passes));
    api->log_info("[DD2 VRS] Targets | created_rtv=%llu created_dsv=%llu OM=%llu accepted=%llu mrt_accepted=%llu"
        " image_bound_at_om=%llu transparent_image_bound=%llu output_excluded=%llu"
        " untracked_list=%llu color_count_outside_1_to_8=%llu no_depth=%llu"
        " missing_rtv=%llu missing_dsv=%llu view_layout=%llu depth_format=%llu extent_mismatch=%llu outside_scene_bounds=%llu",
        n(Diagnostic::rtv_created), n(Diagnostic::dsv_created), n(Diagnostic::om_calls),
        n(Diagnostic::om_accepted), n(Diagnostic::om_mrt_accepted), n(Diagnostic::om_image_bound),
        n(Diagnostic::om_transparent_image_bound), n(Diagnostic::om_output_excluded),
        n(Diagnostic::om_untracked_list), n(Diagnostic::om_color_count),
        n(Diagnostic::om_no_depth), n(Diagnostic::om_missing_rtv), n(Diagnostic::om_missing_dsv),
        n(Diagnostic::om_view_layout), n(Diagnostic::om_depth_format),
        n(Diagnostic::om_extent_mismatch), n(Diagnostic::om_extent_outside_reference));
    api->log_info("[DD2 VRS] Indexed | calls=%llu eligible=%llu shaded=%llu"
        " untracked_list=%llu render_pass=%llu targets=%llu other_scene_extent=%llu"
        " preset_off=%llu image_unready_or_wrong_extent=%llu",
        n(Diagnostic::indexed_calls), n(Diagnostic::eligible), n(Diagnostic::shaded), n(Diagnostic::draw_untracked_list),
        n(Diagnostic::draw_render_pass), n(Diagnostic::draw_targets), n(Diagnostic::draw_other_scene_extent),
        n(Diagnostic::draw_preset_off), n(Diagnostic::draw_image_unready));
    api->log_info("[DD2 VRS] Pass VRS | nonindexed=%llu nonindexed_with_image=%llu execute_indirect=%llu"
        " indirect_with_image=%llu engine_rate_max=%llu engine_image_deferred=%llu"
        " render_pass_begins=%llu execute_bundle=%llu tagged_psos=%llu",
        n(Diagnostic::nonindexed_calls), n(Diagnostic::nonindexed_image_bound), n(Diagnostic::indirect_calls),
        n(Diagnostic::indirect_image_bound), n(Diagnostic::rate_combiner_preserved), n(Diagnostic::engine_image_deferred),
        n(Diagnostic::render_pass_begins), n(Diagnostic::bundle_calls), n(Diagnostic::pso_created));
    api->log_info("[DD2 VRS] Safety | max_rate=2x2 pipeline_policy=compute_only dispatch=%llu unbind_om=%llu unbind_pso=%llu"
        " unbind_dispatch=%llu unbind_indirect=%llu indirect_signature_skipped=%llu"
        " indexed_compute_pso_blocked=%llu",
        n(Diagnostic::dispatch_calls), n(Diagnostic::unbind_om), n(Diagnostic::unbind_pso),
        n(Diagnostic::unbind_dispatch), n(Diagnostic::unbind_indirect), n(Diagnostic::indirect_signature_skipped),
        n(Diagnostic::draw_stream));

    constexpr const char* names[]{"missing_rtv", "missing_dsv", "view_layout", "depth_format",
        "extent_mismatch", "outside_scene_bounds"};
    static_assert(std::size(names) == graphics::Diagnostics::binding_sample_count);
    for (size_t i = 0; i < report.bindings.size(); ++i) {
        const auto& sample = report.bindings[i];
        if (sample.reason == Diagnostic::count) continue;
        api->log_info("[DD2 VRS] Target sample | %s RTV=%s %ux%u DXGI=%u DSV=%s %ux%u DXGI=%u",
            names[i], sample.color_tracked ? "tracked" : "untracked", sample.color.width, sample.color.height,
            unsigned(sample.color_format), sample.depth_tracked ? "tracked" : "untracked", sample.depth.width,
            sample.depth.height, unsigned(sample.depth_format));
    }
    graphics::write_audit(graphics::take_audit(), host->functions->log_info,
                          settings_snapshot.activation_mode == always_on);
    if (settings_snapshot.engine.crowd.enabled && !perf_lua_present) {
        const auto c = engine::status().crowd;
        api->log_info("[DD2 VRS] Crowd | fps=%.1f avg=%.1f caps_ready=%u cap_write_failed=%u rule_hook=%u paused=%u pause_binding=%u npc=%d vanilla_npc=%d pawn=%d vanilla_pawn=%d changes=%d lists=0x%x lod_bindings=0x%x entities=%d facial=%d joints=%d upper=%d chain_partial=%d chain_full=%d",
            c.fps, c.average, unsigned(c.caps_ready), unsigned(c.cap_write_failed), unsigned(c.rule_hook), unsigned(c.paused),
            unsigned(c.pause_available), c.npc, c.vanilla_npc, c.pawn, c.vanilla_pawn, c.adjustments, c.list_sources, c.bound_lods,
            c.processed, c.facial, c.joint, c.upper, c.partial, c.frozen);
    }
}

Extent swapchain_extent(void* swapchain) {
    if (!swapchain) return {};
    Microsoft::WRL::ComPtr<IDXGISwapChain1> chain;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    const bool found = guarded([&] {
        auto* base = static_cast<IDXGISwapChain*>(swapchain);
        return SUCCEEDED(base->QueryInterface(IID_PPV_ARGS(&chain))) &&
            SUCCEEDED(chain->GetDesc1(&desc)) && desc.Width && desc.Height;
    });
    return found ? Extent{desc.Width, desc.Height} : Extent{};
}

void update_frame(bool sample_camera) {
    const auto& current = settings;
    const auto* renderer = host->renderer_data;
    ID3D12Device* device{};
    ID3D12CommandQueue* queue{};
    void* swapchain{};
    if (renderer && renderer->renderer_type == REFRAMEWORK_RENDERER_D3D12) {
        device = static_cast<ID3D12Device*>(renderer->device);
        queue = static_cast<ID3D12CommandQueue*>(renderer->command_queue);
        swapchain = renderer->swapchain;
    }
    if (!renderer_initialized && device && queue) {
        renderer_initialized = graphics::start(device, queue, current);
        const auto status = graphics::status();
        if (!status.failed)
            API::get()->log_info("[DD2 VRS] 0.3.1 optimizer preview | %s | tier=%u tile=%u additional_rates=%u max_rate=2x2",
                                 status.message, status.tier, status.tile, unsigned(status.additional_rates));
    }
    const auto output = swapchain_extent(swapchain);
    if (renderer_reset.exchange(false) || output != previous_output) {
        previous_output = output;
        graphics::reset_gpu_frame();
        gpu_gate = {};
        gpu_time_ms = gpu_load_weight = 0;
        last_gpu_sample = {};
        timing_valid = false;
        camera_bindings = {};
        battle = {};
        previous = {};
        camera_blur = {};
        motion = {};
        reset_combat();
        engine::invalidate();
    }
    if (sample_camera) {
        const auto now = Clock::now();
        if (const auto sample = graphics::begin_gpu_frame(device, queue,
                renderer_initialized && timing_callbacks && current.enabled && current.gpu_gate.enabled)) {
            gpu_time_ms = *sample;
            gpu_gate.observe(*sample, current.gpu_gate);
            last_gpu_sample = now;
        }
        timing_valid = current.gpu_gate.enabled && last_gpu_sample != Clock::time_point{} &&
            now - last_gpu_sample < std::chrono::milliseconds(500);
        if (!timing_valid) gpu_gate = {};
        gpu_load_weight = timing_valid ? gpu_gate.weight(current.gpu_gate) : 0;
        CameraSample next{};
        const auto engine_state = engine::status();
        const auto& e = current.engine;
        const bool engine_allowed = !perf_lua_present && !engine_state.editing;
        const bool needs_engine = (!perf_lua_present && (e.enabled || e.npc || e.crowd.enabled)) ||
            engine_state.active_tweaks || engine_state.throttled_npcs || engine_state.pending_snapshot || engine_state.crowd.owned;
        const bool needs_motion = (current.enabled && (current.border_blur.enabled ||
            (current.activation_mode != always_on && (!current.gpu_gate.enabled || gpu_load_weight > 0.001f)))) ||
            (engine_allowed && e.enabled && !e.always && !e.force_night && e.trigger_motion);
        const bool needs_npc_position = engine_allowed && e.npc;
        const bool needs_camera = needs_motion || needs_npc_position || (engine_allowed && e.enabled && e.heavy &&
            (e.tweaks[ssao_half] || e.tweaks[ssr_half])) || engine_state.camera_tweaks_active || engine_state.pending_snapshot;
        const bool found = needs_camera &&
            guarded([&] { return read_camera(next, needs_motion || needs_npc_position); });
        const float dt = std::chrono::duration<float>(now - previous_time).count();
        float speed = 0;
        const bool contiguous = needs_motion && found && previous.camera == next.camera && dt >= 0.001f && dt <= 0.2f;
        if (contiguous) speed = velocity(previous.rotation, next.rotation, dt);
        if (contiguous && speed * dt < 0.8f) {
            motion.update(speed, dt, current);
            camera_blur.update(camera_pan(previous.rotation, next.rotation, dt), dt, current.border_blur);
        } else {
            motion = {};
            camera_blur = {}; // A missing camera or a cut cannot seed a blur tail.
        }
        previous = found ? next : CameraSample{};
        previous_time = now;
        camera_found = found;
        camera_velocity = speed;
        update_combat(now, current);
        if (needs_engine)
            guarded([&] {
                engine::Camera camera{};
                camera.game_object = next.game_object;
                if (found && needs_npc_position) {
                    const auto [x, y, z, w] = next.rotation;
                    camera.forward = {2.0f * (x * z + w * y), 2.0f * (y * z - w * x),
                                      1.0f - 2.0f * (x * x + y * y)};
                    camera.valid = engine::position(next.transform, camera.position);
                }
                engine::update(current.engine, camera, motion.active, g_in_combat.load(), perf_lua_present,
                               std::chrono::duration<double>(now.time_since_epoch()).count());
                return true;
            });
    }
    const auto preset = select_preset(current);
    if (!renderer_initialized) return;
    const unsigned variant = active_reason.load() == Activity::gpu_load
        ? adaptive_mask(gpu_load_weight, camera_blur.alpha, camera_blur.direction.yaw, camera_blur.direction.pitch) : 0;
    graphics::frame(device, queue, current, preset, output, variant);
    const auto status = graphics::status();
    if (status.failed && status.message != last_renderer_error) {
        last_renderer_error = status.message;
        API::get()->log_error("[DD2 VRS] %s (tier %u, tile %u)", status.message, status.tier, status.tile);
    }
    if (sample_camera) log_diagnostics(current);
    if (sample_camera && blur_probe_frames && --blur_probe_frames == 0) {
        const auto report = graphics::finish_blur_probe();
        auto& api = API::get();
        for (const auto& sample : report.samples) {
            if (!sample.calls) break;
            const auto& key = sample.key;
            constexpr const char* kinds[]{"dispatch", "draw", "draw_indexed", "indirect_dispatch"};
            api->log_info("[DD2 VRS Blur Probe] %s shader=%016llx list=%d call=%ux%ux%u rtv=%ux%u fmt=%u count=%llu",
                kinds[static_cast<unsigned>(key.kind)], static_cast<unsigned long long>(key.shader), key.list_type,
                key.x, key.y, key.z, key.width, key.height, key.format, static_cast<unsigned long long>(sample.calls));
        }
        api->log_info("[DD2 VRS Blur Probe] End dropped=%llu", static_cast<unsigned long long>(report.overflow));
    }
}

void on_begin_rendering() noexcept {
    try {
        std::lock_guard lock{frame_mutex};
        if (guarded([] { update_frame(true); return true; })) return;
    } catch (...) {}
    graphics::set_preset(0);
    active_reason.store(Activity::idle);
}

void on_present() noexcept {
    try { guarded([] { graphics::end_gpu_frame(); return true; }); } catch (...) {}
}

void on_renderer_reset() noexcept {
    renderer_reset.store(true, std::memory_order_release);
}

bool draw_center_size(Settings& current, const graphics::Status& status) {
    if (!ui.header("Center Size", 0)) return false;
    const Extent extent = status.scene.width ? status.scene : current.reference;
    const auto& profile = profiles[current.profile_mode];
    auto size = resolve_center(extent, status.tile, profile, current.center);
    bool changed = ui.checkbox("Lock Center Aspect Ratio", &size.locked);
    ui.text("0%: coarse everywhere. 100%: sharp everywhere.", nullptr);
    const auto draw_size = [&](const char* label, float& value) {
        float edited = value;
        if (!ui.slider(label, &edited, 0.0f, 100.0f, "%.0f%%", 0) || !std::isfinite(edited))
            return false;
        value = std::clamp(edited, 0.0f, 100.0f);
        return true;
    };
    if (size.locked) {
        float linked = std::max(size.width, size.height);
        changed |= draw_size("Sharp Center Size", linked);
        if (changed) size.width = size.height = linked;
    } else {
        changed |= draw_size("Center Width", size.width);
        changed |= draw_size("Center Height", size.height);
    }
    if (ui.button("Reset Center to Profile Default", {})) {
        current.center = {};
        current.center.locked = size.locked;
        return true;
    }
    if (changed) current.center = size;
    if (status.mask_pending) ui.text("Updating shading-rate image", nullptr);
    return changed;
}

void draw_ui() {
    if (!ui.header("DD2 Foveated VRS", 0)) return;
    graphics::Status d;
    Settings current;
    const char* combat_source{};
    Activity reason{};
    float speed{};
    bool found{};
    CameraBlurEnvelope blur{};
    engine::Status engine_status{};
    float gpu_ms{}, load{};
    bool gpu_valid{}, in_combat{};
    unsigned capture_frames{};
    {
        std::lock_guard lock{frame_mutex};
        d = graphics::status();
        current = settings;
        reason = active_reason.load(std::memory_order_relaxed);
        combat_source = battle_source;
        speed = camera_velocity;
        found = camera_found;
        blur = camera_blur;
        gpu_ms = gpu_time_ms;
        gpu_valid = timing_valid;
        load = gpu_load_weight;
        in_combat = g_in_combat.load();
        engine_status = engine::status();
        capture_frames = blur_probe_frames;
    }
    char text[256];
    if (full_sharp_center(current.center)) {
        std::snprintf(text, sizeof(text), "State: Full-screen 1x1");
    } else if (d.preset && d.scene.width && d.image == d.scene) {
        const auto& profile = profiles[d.preset - 1];
        std::snprintf(text, sizeof(text), "State: ACTIVE (%s) | Profile: %s", reason_label(reason), profile.name);
    } else {
        std::snprintf(text, sizeof(text), "State: IDLE (1x1)%s", d.preset ? " | Waiting for scene image" : "");
    }
    ui.text(text, nullptr);
    const float effective_load = d.preset && d.scene.width && d.image == d.scene ?
        (reason == Activity::gpu_load ? load : 1.0f) : 0.0f;
    if (gpu_valid)
        std::snprintf(text, sizeof(text), "GPU %.2f ms | VRS %.2f | Camera %.2f rad/s | Motion %.2f",
            gpu_ms, effective_load, speed, blur.alpha);
    else
        std::snprintf(text, sizeof(text), "GPU time unavailable | VRS %.2f | Camera %.2f rad/s | Motion %.2f",
            effective_load, speed, blur.alpha);
    ui.text(text, nullptr);
    std::snprintf(text, sizeof(text), "Combat: %s | Night: %s | NPCs throttled: %d",
        in_combat ? "Yes" : "No", engine_status.clock_valid ? (engine_status.night ? "Yes" : "No") : "Unknown",
        engine_status.throttled_npcs);
    ui.text(text, nullptr);
    if (d.failed) ui.text(d.message, nullptr);
    ui.text("Activation Mode", nullptr);
    bool changed = ui.radio("Adaptive (Motion + Combat)", &current.activation_mode, adaptive);
    changed |= ui.radio("Force Always On", &current.activation_mode, always_on);
    changed |= ui.radio("Motion Only", &current.activation_mode, motion_only);
    ui.text("VRS Profile", nullptr);
    changed |= ui.radio("Default (Balanced Soft)", &current.profile_mode, 0);
    changed |= ui.radio("Extreme (Max Performance)", &current.profile_mode, 1);
    changed |= ui.checkbox("Shade Alpha / Particle Passes", &current.shade_transparent_passes);
    changed |= draw_center_size(current, d);

    bool capture_blur = false, edit_engine = false, resume_engine = false, refresh_engine = false;
    bool write_snapshot = false, read_snapshot = false, cancel_snapshot = false;
    if (ui.header("Adaptive VRS & Border Motion Blur", 0)) {
        changed |= ui.checkbox("Enable Adaptive GPU Load Trigger", &current.gpu_gate.enabled);
        changed |= ui.slider("Target GPU Frame Time", &current.gpu_gate.target_ms, 16.6f, 33.3f, "%.1f ms", 0);
        changed |= ui.slider("Hysteresis Band", &current.gpu_gate.band_ms, 1.0f, 5.0f, "%.1f ms", 0);
        changed |= ui.checkbox("Enable Dynamic VRS Border Motion Blur", &current.border_blur.enabled);
        changed |= ui.slider("Border Blur Multiplier", &current.border_blur.multiplier, 1.0f, 3.0f, "%.2fx", 0);
        changed |= ui.slider("Camera Velocity Threshold", &current.border_blur.threshold, 0.05f, 1.0f, "%.2f rad/s", 0);
        changed |= ui.slider("Temporal Decay Time", &current.border_blur.fade_ms, 50.0f, 250.0f, "%.0f ms", 0);
        ui.text("Native motion blur needs a verified post-upscaler pass.", nullptr);
        if (current.border_blur.enabled) {
            std::snprintf(text, sizeof(text), "Yaw %+.2f | Pitch %+.2f | Roll %+.2f | Requested blur %.2f",
                blur.direction.yaw, blur.direction.pitch, blur.direction.roll, effective_load * blur.alpha);
            ui.text(text, nullptr);
        }
        if (capture_frames) ui.text("Capturing pipeline IDs", nullptr);
        else capture_blur = ui.button("Capture Motion Blur Pipelines", {});
    }
    auto& e = current.engine;
    if (ui.header("Engine & Foliage/Shadow Optimizer", 0)) {
        if (perf_lua_present) ui.text("Native engine writes paused because perf.lua is installed.", nullptr);
        changed |= ui.checkbox("Enable Native Engine Optimizer", &e.enabled);
        if (e.enabled) {
            int mode = e.always ? 1 : e.force_night ? 2 : 0;
            changed |= ui.radio("Adaptive Engine Tweaks", &mode, 0);
            changed |= ui.radio("Always Apply Engine Tweaks", &mode, 1);
            changed |= ui.radio("Force Night Profile", &mode, 2);
            e.always = mode == 1;
            e.force_night = mode == 2;
        }
        changed |= ui.checkbox("Trigger on Camera Motion", &e.trigger_motion);
        changed |= ui.checkbox("Trigger in Combat", &e.trigger_combat);
        changed |= ui.checkbox("Trigger at Night", &e.trigger_night);
        changed |= ui.slider("Night Start", &e.night_start, 0.0f, 23.0f, "%.0f:00", 0);
        changed |= ui.slider("Night End", &e.night_end, 0.0f, 23.0f, "%.0f:00", 0);
        changed |= ui.slider("Motion Trigger Threshold", &current.motion_threshold, 0.5f, 5.0f, "%.2f rad/s", 0);
        changed |= ui.slider("Motion Hold", &current.hold, 0.0f, 3.0f, "%.2f s", 0);
        changed |= ui.slider("Combat Hold", &current.combat_exit_delay, 0.0f, 6.0f, "%.2f s", 0);
        changed |= ui.slider("SpeedTree Target", &e.speedtree_bias, 0.0f, 5.0f, "%.0f", 0);
        changed |= ui.slider("Motion Quality Target", &e.motion_target, 0.0f, 1.0f, "%.0f", 0);
        changed |= ui.slider("Foliage Distance Retained", &e.foliage_retention, 50.0f, 95.0f, "%.0f%%", 0);
        changed |= ui.checkbox("Allow Heavy Render-Target Tweaks", &e.heavy);
        if (e.heavy) {
            changed |= ui.slider("Shadow Quality Target", &e.shadow_target, 0.0f, 4.0f, "%.0f", 0);
            changed |= ui.slider("Shadow Resolution Limit", &e.shadow_pixels, 384.0f, 4096.0f, "%.0f px", 0);
        }
        for (size_t i = 0; i < e.tweaks.size() && (i < shadow_quality || e.heavy); ++i) {
            changed |= ui.checkbox(engine_labels[i], &e.tweaks[i]);
            if (i == effects && e.tweaks[i]) changed |= ui.checkbox("Particle LOD Only In Combat", &e.effects_combat_only);
            if (e.tweaks[i] && engine_status.tweak_status[i]) {
                std::snprintf(text, sizeof(text), "%s%s%s", engine_status.tweak_status[i],
                    engine_status.properties[i] ? " | " : "", engine_status.properties[i] ? engine_status.properties[i] : "");
                ui.text(text, nullptr);
            }
        }
        std::snprintf(text, sizeof(text), "Active overrides: %d", engine_status.active_tweaks);
        ui.text(text, nullptr);
        if (engine_status.editing) {
            if (engine_status.pending_snapshot)
                cancel_snapshot = ui.button("Cancel Saved Baseline Restore", {});
            if (engine_status.active_tweaks || engine_status.throttled_npcs || engine_status.pending_snapshot || engine_status.crowd.owned)
                ui.text("Restoring baseline", nullptr);
            else {
                ui.text("Baseline restored. Edit in-game settings now.", nullptr);
                resume_engine = ui.button("Finish Editing", {});
            }
        } else edit_engine = ui.button("Restore Baseline / Edit Settings", {});
        refresh_engine = ui.button("Retry Engine Bindings", {});
        write_snapshot = ui.button("Save Baseline Snapshot", {});
        if (!perf_lua_present) {
            if (confirm_snapshot_restore) {
                read_snapshot = ui.button("Confirm Saved Baseline Restore", {});
                if (read_snapshot || ui.button("Cancel##snapshot_confirmation", {})) confirm_snapshot_restore = false;
            } else confirm_snapshot_restore = ui.button("Restore Saved Baseline", {});
        }
        if (snapshot_status) ui.text(snapshot_status, nullptr);
    }
    if (ui.header("CPU & Crowd Optimizer", 0)) {
        auto& c = e.crowd;
        const auto& live = engine_status.crowd;
        const auto int_slider = [&](const char* label, int& value, int min, int max) {
            float f = float(value);
            if (!ui.slider(label, &f, float(min), float(max), "%.0f", 0)) return false;
            value = int(std::round(f));
            return true;
        };
        constexpr const char* modes[]{"Warming up", "Normal", "Throttle", "Critical", "Quality"};
        changed |= ui.checkbox("Enable CPU & Crowd Optimizer", &c.enabled);
        if (perf_lua_present) ui.text("Native CPU writes paused because perf.lua is installed.", nullptr);
        else if (engine_status.editing || live.restoring) ui.text("Crowd overrides paused for baseline restoration.", nullptr);
        else if (c.enabled && live.paused) ui.text(live.pause_available ? "Paused in game menu" : "Waiting for GUI pause binding", nullptr);
        else if (c.enabled) {
            std::snprintf(text, sizeof(text), "%s | FPS %.1f (avg %.1f, min %.1f, max %.1f) | Adjustments %d",
                modes[static_cast<unsigned>(live.mode)], live.fps, live.average, live.minimum, live.maximum, live.adjustments);
            ui.text(text, nullptr);
        }
        changed |= int_slider("Crowd Target FPS", c.fps_target, 20, 60);
        changed |= int_slider("Crowd Critical FPS", c.fps_critical, 15, 45);
        changed |= int_slider("Crowd Headroom FPS", c.fps_headroom, 25, 65);
        changed |= ui.slider("Crowd Recovery Time", &c.recovery_seconds, 5.0f, 10.0f, "%.1f s", 0);
        changed |= ui.checkbox("Adaptive NPC Generation Limits", &c.npc);
        changed |= ui.checkbox("Adaptive Stray Pawn Limits", &c.pawn);
        if (live.caps_ready) {
            std::snprintf(text, sizeof(text), "NPC cap %d (game %d) | Stray pawn cap %d (game %d) | Rule changes %s",
                live.npc, live.vanilla_npc, live.pawn, live.vanilla_pawn, live.rule_hook ? "hooked" : "polled");
            ui.text(text, nullptr);
        } else if (c.enabled && (c.npc || c.pawn) && !perf_lua_present) ui.text("Waiting for all four GenerateManager cap fields", nullptr);
        if (live.cap_write_failed) ui.text("Crowd cap write/readback failed", nullptr);
        bool manual = c.manual_npc_limit > 0;
        if (ui.checkbox("Manual NPC Cap", &manual)) {
            c.manual_npc_limit = manual ? (live.npc > 0 ? live.npc : c.npc_baseline) : 0;
            if (manual) c.npc = true;
            changed = true;
        }
        if (manual) changed |= int_slider("NPC Cap", c.manual_npc_limit, c.npc_min, c.npc_max);
        changed |= int_slider("Minimum NPCs", c.npc_min, 6, 30);
        changed |= int_slider("Baseline NPCs", c.npc_baseline, 10, 45);
        changed |= int_slider("Maximum NPCs", c.npc_max, 20, 60);
        changed |= int_slider("Minimum Stray Pawns", c.pawn_min, 0, 20);
        changed |= int_slider("Maximum Stray Pawns", c.pawn_max, 0, 30);
        if (ui.button("Restore Game NPC / Pawn Caps", {})) {
            c.npc = c.pawn = false;
            c.manual_npc_limit = 0;
            changed = true;
        }
        changed |= ui.checkbox("Distance-Based Character LOD", &c.lod);
        changed |= ui.checkbox("Distance-Based Cloth / Hair Freezing", &c.chain);
        if (c.lod) {
            changed |= ui.slider("Facial Cutoff", &c.facial_distance, 10.0f, 60.0f, "%.0f m", 0);
            changed |= ui.slider("Joint Cutoff", &c.joint_distance, 15.0f, 80.0f, "%.0f m", 0);
            changed |= ui.slider("Upper Body Cutoff", &c.upper_distance, 20.0f, 100.0f, "%.0f m", 0);
        }
        if (c.chain) {
            changed |= ui.slider("Start Cloth / Hair Freeze", &c.freeze_near, 10.0f, 60.0f, "%.0f m", 0);
            changed |= ui.slider("Full Cloth / Hair Freeze", &c.freeze_far, 25.0f, 120.0f, "%.0f m", 0);
        }
        if (c.enabled && (c.lod || c.chain) && !perf_lua_present) {
            std::snprintf(text, sizeof(text), "Distance checks %d | Disabled facial %d, joint %d, upper body %d | Chain %d partial, %d frozen",
                live.processed, live.facial, live.joint, live.upper, live.partial, live.frozen);
            ui.text(text, nullptr);
            if (!live.list_sources) ui.text("Waiting for a compatible character list", nullptr);
            else {
                std::snprintf(text, sizeof(text), "Lists available | Enemies %s | NPCs %s | Stray pawns %s",
                    live.list_sources & 1 ? "Yes" : "No", live.list_sources & 2 ? "Yes" : "No",
                    live.list_sources & 4 ? "Yes" : "No");
                ui.text(text, nullptr);
                std::snprintf(text, sizeof(text), "Read/write bindings found | Facial %s | Joint %s | Upper body %s | Chain %s",
                    live.bound_lods & 1 ? "Yes" : "No", live.bound_lods & 2 ? "Yes" : "No",
                    live.bound_lods & 4 ? "Yes" : "No", live.bound_lods & 8 ? "Yes" : "No");
                ui.text(text, nullptr);
            }
        }
        if (ui.header("Crowd Timing & Steps", 0)) {
            changed |= int_slider("NPC Step", c.npc_step, 1, 12);
            changed |= int_slider("Critical NPC Step", c.npc_emergency_step, 1, 20);
            changed |= int_slider("FPS Sample Frames", c.sample_window, 20, 240);
            changed |= int_slider("Cap Adjustment Frames", c.adjust_interval, 1, 600);
            changed |= int_slider("Cap Direction Hysteresis Frames", c.hysteresis, 0, 1200);
            changed |= int_slider("Distance Check Interval Frames", c.lod_interval, 1, 600);
        }
    }
    if (ui.header("CPU Behind-Camera NPC Throttling", 0)) {
        changed |= ui.checkbox("Enable NPC Throttling", &e.npc);
        changed |= ui.slider("Behind-Camera Cone Dot", &e.npc_dot, -0.9f, -0.15f, "%.2f", 0);
        changed |= ui.slider("Minimum NPC Distance", &e.npc_distance, 8.0f, 50.0f, "%.1f m", 0);
        if (e.npc && !engine_status.npc_bindings) ui.text("Waiting for a compatible NPC list and motion binding.", nullptr);
    }

    const bool advanced = ui.header("Advanced / Debug", 0);
    if (advanced) {
        changed |= ui.checkbox("Enable foveated VRS", &current.enabled);
        changed |= ui.checkbox("Verbose Audit Logging", &current.verbose_logging);
        std::snprintf(text, sizeof(text), "VRS Tier %u | Tile %u x %u | Transition %s",
                      d.tier, d.tile, d.tile, d.additional_rates ? "2x1 / 1x2" : "2x2 fallback");
        ui.text(text, nullptr);
        std::snprintf(text, sizeof(text), "Scene %u x %u | Output %u x %u | Indirect calls with image %llu",
                      d.scene.width, d.scene.height, d.output.width, d.output.height,
                      static_cast<unsigned long long>(d.indirect_with_image));
        ui.text(text, nullptr);
        std::snprintf(text, sizeof(text), "Previous frame: %llu/%llu indexed draws shaded",
                      static_cast<unsigned long long>(d.shaded), static_cast<unsigned long long>(d.candidates));
        ui.text(text, nullptr);
        if (current.activation_mode == adaptive) {
            std::snprintf(text, sizeof(text), "Combat source: %s", combat_source);
            ui.text(text, nullptr);
        }
        if (found) {
            std::snprintf(text, sizeof(text), "Camera: %.2f rad/s", speed);
            ui.text(text, nullptr);
        }
    }
    if (changed || capture_blur || edit_engine || resume_engine || refresh_engine || cancel_snapshot) {
        std::lock_guard lock{frame_mutex};
        if (settings.activation_mode != current.activation_mode || settings.enabled != current.enabled)
            reset_combat();
        if (settings.verbose_logging != current.verbose_logging) next_diagnostics = {};
        sanitize(current.border_blur);
        sanitize(current.engine);
        current.gpu_gate.target_ms = std::clamp(current.gpu_gate.target_ms, 16.6f, 33.3f);
        current.gpu_gate.band_ms = std::clamp(current.gpu_gate.band_ms, 1.0f, 5.0f);
        if (settings.gpu_gate.enabled != current.gpu_gate.enabled ||
            settings.gpu_gate.target_ms != current.gpu_gate.target_ms || settings.gpu_gate.band_ms != current.gpu_gate.band_ms) {
            gpu_gate = {};
            gpu_load_weight = 0;
            last_gpu_sample = {};
            timing_valid = false;
        }
        if (!current.enabled) camera_blur = {};
        if (settings.engine != current.engine || refresh_engine) engine::refresh();
        settings = current;
        if (edit_engine) engine::edit(true);
        if (resume_engine) engine::edit(false);
        if (cancel_snapshot) {
            engine::cancel_snapshot_restore();
            snapshot_status = "Saved baseline restore stopped";
        }
        graphics::set_transparent_passes(current.shade_transparent_passes);
        const auto preset = select_preset(current);
        graphics::set_preset(preset, active_reason.load() == Activity::gpu_load
            ? adaptive_mask(gpu_load_weight, camera_blur.alpha, camera_blur.direction.yaw, camera_blur.direction.pitch) : 0);
        if (capture_blur && !blur_probe_frames) {
            API::get()->log_info("[DD2 VRS Blur Probe] Begin next four game frames");
            graphics::begin_blur_probe();
            blur_probe_frames = 5;
        }
    }
    if (write_snapshot) {
        engine::Baselines values;
        { std::lock_guard lock{frame_mutex}; values = engine::baselines(); }
        try { snapshot_status = save_baseline(values) ? "Baseline snapshot saved" : "No baseline snapshot saved"; }
        catch (...) { snapshot_status = "Could not save baseline snapshot"; }
    }
    if (read_snapshot) {
        try {
            if (const auto values = load_baseline()) {
                std::lock_guard lock{frame_mutex};
                engine::restore_snapshot(*values);
                snapshot_status = "Restoring saved baseline on the game thread";
            } else snapshot_status = "No usable saved baseline";
        } catch (...) { snapshot_status = "Could not load baseline snapshot"; }
    }
    if (advanced && ui.button("Save Config", {})) {
        try { save_status = save_config(current) ? "Config saved" : "Could not save config"; }
        catch (...) { save_status = "Could not save config"; }
    }
    if (advanced && save_status) ui.text(save_status, nullptr);
}

void on_imgui_draw_ui(REFImGuiFrameCbData* data) noexcept {
    if (!ui_ready || !data || !data->context) return;
    try {
        // These exports execute inside the host's ImGui context and allocator.
        guarded([] { draw_ui(); return true; });
    } catch (...) {}
}

bool initialize(const REFrameworkPluginInitializeParam* param) {
    if (!param || !param->functions || !param->sdk || !param->renderer_data ||
        !param->sdk->functions || !param->sdk->tdb || !param->sdk->type_definition || !param->sdk->method ||
        !param->sdk->managed_object || !param->sdk->method->invoke ||
        !param->functions->on_pre_application_entry || !param->functions->on_imgui_draw_ui) return false;

    HMODULE module{};
    const auto address = reinterpret_cast<LPCWSTR>(&initialize);
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            address, &module)) return false;
    wchar_t filename[32768];
    const DWORD length = GetModuleFileNameW(module, filename, static_cast<DWORD>(std::size(filename)));
    if (length && length < std::size(filename))
        config_path = std::filesystem::path{filename}.parent_path().parent_path() / L"data" / L"dd2_foveated_vrs.json";
    API::initialize(param);
    host = param;
    settings = read_config();
    std::error_code filesystem_error;
    perf_lua_present = std::filesystem::exists(config_path.parent_path().parent_path() /
        L"autorun" / L"perf.lua", filesystem_error);
    guarded([] {
        if (const auto* tdb = API::get()->tdb()) resolve_battle(tdb);
        return true;
    });
    ui_ready = param->reframework_module && ui.bind(static_cast<HMODULE>(param->reframework_module));
    if (!ui_ready)
        API::get()->log_warn("[DD2 VRS] REFramework cimgui exports unavailable. JSON settings remain available.");

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            address, &module)) return false;
    const auto* functions = param->functions;
    if (!functions->on_pre_application_entry("BeginRendering", on_begin_rendering)) return false;
    timing_callbacks = functions->on_present && functions->on_present(on_present);
    if (functions->on_device_reset) functions->on_device_reset(on_renderer_reset);
    if (ui_ready && !functions->on_imgui_draw_ui(on_imgui_draw_ui))
        API::get()->log_warn("[DD2 VRS] Could not register the REFramework ImGui callback.");
    {
        std::lock_guard lock{frame_mutex};
        guarded([] { update_frame(false); return true; });
    }
    return true;
}
} // namespace

extern "C" __declspec(dllexport) void reframework_plugin_required_version(REFrameworkPluginVersion* version) {
    if (!version) return;
    version->major = REFRAMEWORK_PLUGIN_VERSION_MAJOR;
    version->minor = REFRAMEWORK_PLUGIN_VERSION_MINOR;
    version->patch = REFRAMEWORK_PLUGIN_VERSION_PATCH;
    version->game_name = "DD2";
}

extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* param) {
    try {
        return guarded([&] { return initialize(param); });
    } catch (...) {
        return false;
    }
}
