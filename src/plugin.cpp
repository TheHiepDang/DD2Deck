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
#include <utility>

namespace {
using namespace dd2;
using reframework::API;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

const REFrameworkPluginInitializeParam* host{};
std::filesystem::path config_path;
std::mutex frame_mutex;
Settings settings = recommended_steam_deck_settings();
std::atomic<Activity> active_reason{Activity::idle};
std::atomic<bool> callbacks_ready{};
std::atomic<bool> renderer_reset{};
bool perf_lua_present{};
Extent previous_output{};
void* previous_swapchain{};
const char* save_status{};
const char* snapshot_status{};
bool confirm_snapshot_restore{};
bool renderer_initialized{};
const char* last_renderer_error{};

struct CameraSample {
    API::ManagedObject* camera{};
    API::ManagedObject* game_object{};
    Quaternion rotation{};
};
struct PreviousCameraSample {
    uintptr_t key{};
    Quaternion rotation{};
} previous;
Clock::time_point previous_time{};
Clock::time_point next_diagnostics{};
Clock::time_point next_combat_poll{};
Motion motion{};
Combat combat{};
constexpr const char* waiting_combat = "Waiting for combat binding";
const char* battle_source = waiting_combat;

// Accessed under frame_mutex. No managed handles cross the callback boundary.
struct GameplayFrame {
    engine::Status optimizer{};
    engine::Baselines baselines{};
    const char* battle_source{waiting_combat};
    float camera_speed{};
    bool camera_found{}, motion_active{}, in_combat{};
} published_gameplay;

struct GameplayRequests {
    bool reset_world{}, reset_combat{}, refresh{}, reseed_crowd{}, cancel_snapshot{};
    std::optional<bool> edit;
    std::optional<engine::Baselines> restore_snapshot;
} gameplay_requests;

struct ObjectMethod {
    API::TypeDefinition* type{};
    API::Method* method{};

    API::Method* get(API::ManagedObject* object, const char* name) {
        const auto* sdk = host->sdk->managed_object;
        if (!object || (sdk->is_managed_object && !sdk->is_managed_object(object))) return nullptr;
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
    API::ManagedObject* view_instance{};
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
    std::pair{"pawn_enabled", &CrowdSettings::pawn}
};
constexpr std::array crowd_numbers{
    std::pair{"fps_target", &CrowdSettings::fps_target}, std::pair{"fps_critical", &CrowdSettings::fps_critical},
    std::pair{"fps_headroom", &CrowdSettings::fps_headroom}, std::pair{"npc_min", &CrowdSettings::npc_min},
    std::pair{"npc_baseline", &CrowdSettings::npc_baseline}, std::pair{"npc_max", &CrowdSettings::npc_max},
    std::pair{"npc_step", &CrowdSettings::npc_step}, std::pair{"npc_emergency_step", &CrowdSettings::npc_emergency_step},
    std::pair{"manual_npc_limit", &CrowdSettings::manual_npc_limit},
    std::pair{"pawn_min", &CrowdSettings::pawn_min}, std::pair{"pawn_max", &CrowdSettings::pawn_max},
    std::pair{"adjust_interval", &CrowdSettings::adjust_interval},
    std::pair{"sample_window", &CrowdSettings::sample_window}, std::pair{"hysteresis", &CrowdSettings::hysteresis}
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
    c.manual_npc_limit = 0; // Legacy configurations without a manual cap were adaptive.
    for (const auto& [key, member] : crowd_numbers)
        c.*member = int(std::round(std::clamp(config_number(j, key, float(c.*member)), 0.0f, 1200.0f)));
    const float pawn_baseline = config_number(j, "pawn_baseline", -1.0f);
    c.pawn_baseline = pawn_baseline >= 0.0f
        ? std::optional{int(std::round(std::min(pawn_baseline, 30.0f)))} : std::nullopt;
    c.recovery_seconds = config_number(j, "crowd_recovery_seconds", c.recovery_seconds);
}

void read_engine_tweaks(const Json& j, EngineSettings& e) {
    for (size_t i = 0; i < e.tweaks.size(); ++i)
        e.tweaks[i] = config_flag(j, engine_keys[i], e.tweaks[i]);
    // Older configurations stored the screen-only choice separately from
    // their combined screen/water flag. Migrate the screen choice.
    e.tweaks[ssr_disable] = config_flag(j, "disable_screen_space_reflections", e.tweaks[ssr_disable]);
    e.speedtree_bias = config_number(j, "speedtree_lod_bias", e.speedtree_bias);
    e.motion_target = config_number(j, "motion_quality_target", e.motion_target);
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
    read_engine_tweaks(j, e);
    e.heavy = std::any_of(e.tweaks.begin() + shadow_quality, e.tweaks.begin() + ssr_half + 1,
        [](bool enabled) { return enabled; });
    s.motion_threshold = config_number(j, "motion_threshold", 1.8f);
    s.hold = config_number(j, "motion_hold_seconds", 1.2f);
    s.combat_exit_delay = config_number(j, "combat_hold_seconds", 2.5f);
    read_crowd_config(j, e.crowd);
}

Settings read_config() {
    Settings s = recommended_steam_deck_settings();
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
        const int default_activation = j.contains("force_always_on")
            ? (flag("force_always_on", false) ? always_on : adaptive) : s.activation_mode;
        s.activation_mode = mode("activation_mode", default_activation, motion_only);
        s.profile_mode = mode("profile_mode", 0, int(profiles.size()) - 1);
        s.optimizer_enabled = flag("optimizer_enabled", s.optimizer_enabled);
        s.enabled = flag("enabled", s.enabled);
        s.verbose_logging = flag("verbose_logging", s.verbose_logging);
        s.shade_transparent_passes = flag("shade_transparent_passes", s.shade_transparent_passes);
        s.allow_native_vrs = flag("allow_native_resolution_vrs", s.allow_native_vrs);
        auto& e = s.engine;
        e.enabled = flag("native_engine_optimizer", e.enabled);
        e.force_night = flag("engine_force_night", e.force_night);
        e.always = flag("engine_force_on", e.always && !e.force_night);
        e.heavy = flag("heavy_render_target_tweaks", e.heavy);
        e.trigger_motion = flag("engine_trigger_motion", e.trigger_motion);
        e.trigger_combat = flag("engine_trigger_combat", e.trigger_combat);
        e.trigger_night = flag("engine_trigger_night", e.trigger_night);
        read_engine_tweaks(j, e);
        read_crowd_config(j, e.crowd);
        const bool legacy_center = j.contains("center_width_percent") || j.contains("center_height_percent");
        s.center.profile_relative = !flag("center_size_uses_full_range", !legacy_center);
        s.center.width = number("center_width_percent", s.center.profile_relative ? 100.0f : s.center.width);
        s.center.height = number("center_height_percent", s.center.profile_relative ? 100.0f : s.center.height);
        s.center.locked = flag("lock_center_aspect_ratio", s.center.locked);
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
        {"optimizer_enabled", s.optimizer_enabled},
        {"enabled", s.enabled}, {"activation_mode", s.activation_mode}, {"profile_mode", s.profile_mode},
        {"verbose_logging", s.verbose_logging}, {"motion_threshold", s.motion_threshold},
        {"shade_transparent_passes", s.shade_transparent_passes},
        {"allow_native_resolution_vrs", s.allow_native_vrs},
        {"center_width_percent", s.center.width}, {"center_height_percent", s.center.height},
        {"lock_center_aspect_ratio", s.center.locked},
        {"center_size_uses_full_range", !s.center.profile_relative},
        {"hold_seconds", s.hold}, {"combat_exit_delay", s.combat_exit_delay},
        {"render_width", s.reference.width}, {"render_height", s.reference.height}
    };
    const auto& e = s.engine;
    j.update(Json{{"native_engine_optimizer", e.enabled}, {"engine_force_on", e.always}, {"engine_force_night", e.force_night},
        {"heavy_render_target_tweaks", e.heavy},
        {"engine_trigger_motion", e.trigger_motion}, {"engine_trigger_combat", e.trigger_combat},
        {"engine_trigger_night", e.trigger_night},
        {"speedtree_lod_bias", e.speedtree_bias},
        {"motion_quality_target", e.motion_target},
        {"shadow_quality_target", e.shadow_target}, {"shadow_resolution_target", e.shadow_pixels},
        {"night_start_hour", e.night_start}, {"night_end_hour", e.night_end}});
    for (size_t i = 0; i < e.tweaks.size(); ++i) j[engine_keys[i]] = e.tweaks[i];
    const auto& c = e.crowd;
    for (const auto& [key, member] : crowd_flags) j[key] = c.*member;
    for (const auto& [key, member] : crowd_numbers) j[key] = c.*member;
    if (c.pawn_baseline) j["pawn_baseline"] = *c.pawn_baseline;
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
    Json snapshot = Json::object();
    for (size_t i = 0; i < values.size(); ++i) if (values[i]) {
        if (boolean_tweak(i)) snapshot[baseline_keys[i]] = {{"kind", "boolean"}, {"value", *values[i] != 0}};
        else snapshot[baseline_keys[i]] = {{"kind", "number"}, {"value", *values[i]}};
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
    engine::Baselines values{};
    for (size_t i = 0; i < values.size(); ++i) {
        const auto it = snapshot["values"].find(baseline_keys[i]);
        if (it == snapshot["values"].end() || !it->is_object() || !it->contains("value")) continue;
        const auto& value = (*it)["value"];
        if (boolean_tweak(i) && value.is_boolean()) values[i] = value.get<bool>();
        if (!boolean_tweak(i) && value.is_number()) {
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

void reset_combat() {
    combat = {};
    next_combat_poll = {};
    battle_source = waiting_combat;
}

bool update_combat(Clock::time_point now, const Settings& current, engine::Object* player) {
    using namespace std::chrono_literals;
    if (!player) {
        if (next_combat_poll != Clock::time_point{}) reset_combat();
        return false;
    }
    const auto& e = current.engine;
    const bool engine_combat = e.enabled && !perf_lua_present &&
        !e.always && !e.force_night && e.trigger_combat;
    if ((!current.enabled || current.activation_mode != adaptive) && !engine_combat && !current.verbose_logging)
        return false;
    const double seconds = std::chrono::duration<double>(now.time_since_epoch()).count();
    if (now >= next_combat_poll) {
        next_combat_poll = now + 1s;
        std::optional<engine::CombatReading> sample;
        guarded([&] { sample = engine::read_combat(API::get()->tdb(), player); return true; });
        const char* source = sample ? sample->source : "Combat binding unavailable";
        if (std::string_view{battle_source} != source) {
            battle_source = source;
            API::get()->log_info("[DD2 VRS] Combat source %s", source);
        }
        combat.observe(sample && sample->active, seconds, current.combat_exit_delay);
    }
    return combat.active(seconds);
}

int select_preset(const Settings& current) {
    const auto reason = activity(current, published_gameplay.motion_active, published_gameplay.in_combat);
    active_reason.store(reason, std::memory_order_relaxed);
    return reason == Activity::idle ? 0 : current.profile_mode + 1;
}

const char* reason_label(Activity reason) {
    switch (reason) {
    case Activity::combat: return "Combat";
    case Activity::motion: return "Motion";
    case Activity::forced: return "Forced";
    default: return "Idle";
    }
}

const char* world_label(engine::WorldReadiness readiness) {
    switch (readiness) {
    case engine::WorldReadiness::settling: return "Waiting for the player to settle (2 seconds)";
    case engine::WorldReadiness::ready: return "Ready";
    default: return "Waiting for player";
    }
}

API::ManagedObject* object_result(API::Method* method, void* instance) {
    reframework::InvokeRet out{};
    return invoke(method, instance, out) ? static_cast<API::ManagedObject*>(out.ptr) : nullptr;
}

bool resolve_camera(CameraSample& sample) {
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
        binding = {tdb, manager, quaternion, main_view, primary, nullptr, {}, {}, {}};
    }

    // Mirrors REFramework's Lua sdk.get_primary_camera implementation.
    const auto* sdk = host->sdk->managed_object;
    if (!binding.view_instance || (sdk->is_managed_object && !sdk->is_managed_object(binding.view_instance)))
        binding.view_instance = object_result(binding.main_view, binding.manager->get_instance());
    auto* camera = object_result(binding.primary_camera, binding.view_instance);
    if (!camera) {
        binding.view_instance = nullptr;
        return false;
    }
    auto* object = object_result(binding.get_object.get(camera, "get_GameObject"), camera);
    if (!object || (sdk->is_managed_object && !sdk->is_managed_object(object))) return false;
    sample.camera = camera;
    sample.game_object = object;
    return true;
}

engine::Object* camera_render_owner() {
    CameraSample sample;
    return resolve_camera(sample) ? sample.game_object : nullptr;
}

bool read_camera(CameraSample& sample) {
    if (!resolve_camera(sample)) return false;
    auto& binding = camera_bindings;
    auto* object = sample.game_object;
    auto* transform = object_result(binding.get_transform.get(object, "get_Transform"), object);
    auto* rotation = binding.get_rotation.get(transform, "get_Rotation");
    if (!rotation || rotation->get_return_type() != binding.quaternion) return false;

    reframework::InvokeRet out{};
    if (!invoke(rotation, transform, out)) return false;
    std::memcpy(sample.rotation.data(), out.bytes.data(), sizeof(sample.rotation));
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
    const auto* log = host->functions;
    log->log_info("[DD2 VRS] Diagnostics | %s | output=%ux%u scene=%ux%u image=%ux%u tiles=%ux%u"
        " enabled=%u activation_mode=%d profile=%s preset=%d reason=%s camera=%s battle_source=%s in_combat=%u shade_transparent=%u allow_native_vrs=%u",
        snapshot.message, snapshot.output.width, snapshot.output.height,
        snapshot.scene.width, snapshot.scene.height, snapshot.image.width, snapshot.image.height,
        (snapshot.image.width + snapshot.tile - 1) / snapshot.tile,
        (snapshot.image.height + snapshot.tile - 1) / snapshot.tile, unsigned(settings_snapshot.enabled),
        settings_snapshot.activation_mode, profiles[settings_snapshot.profile_mode].name, snapshot.preset,
        reason_label(active_reason.load()), published_gameplay.camera_found ? "available" : "missing", published_gameplay.battle_source,
        unsigned(published_gameplay.in_combat), unsigned(settings_snapshot.shade_transparent_passes), unsigned(settings_snapshot.allow_native_vrs));
    log->log_info("[DD2 VRS] Targets | created_rtv=%llu created_dsv=%llu OM=%llu accepted=%llu mrt_accepted=%llu"
        " image_bound_at_om=%llu transparent_image_bound=%llu output_excluded=%llu viewport_fallback_candidates=%llu"
        " untracked_list=%llu color_count_outside_1_to_8=%llu no_depth=%llu"
        " missing_rtv=%llu missing_dsv=%llu view_layout=%llu depth_format=%llu extent_mismatch=%llu outside_scene_bounds=%llu",
        n(Diagnostic::rtv_created), n(Diagnostic::dsv_created), n(Diagnostic::om_calls),
        n(Diagnostic::om_accepted), n(Diagnostic::om_mrt_accepted), n(Diagnostic::om_image_bound),
        n(Diagnostic::om_transparent_image_bound), n(Diagnostic::om_output_excluded), n(Diagnostic::om_viewport_fallback),
        n(Diagnostic::om_untracked_list), n(Diagnostic::om_color_count),
        n(Diagnostic::om_no_depth), n(Diagnostic::om_missing_rtv), n(Diagnostic::om_missing_dsv),
        n(Diagnostic::om_view_layout), n(Diagnostic::om_depth_format),
        n(Diagnostic::om_extent_mismatch), n(Diagnostic::om_extent_outside_reference));
    log->log_info("[DD2 VRS] Indexed | calls=%llu eligible=%llu shaded=%llu"
        " untracked_list=%llu render_pass=%llu targets=%llu other_scene_extent=%llu"
        " preset_off=%llu image_unready_or_wrong_extent=%llu",
        n(Diagnostic::indexed_calls), n(Diagnostic::eligible), n(Diagnostic::shaded), n(Diagnostic::draw_untracked_list),
        n(Diagnostic::draw_render_pass), n(Diagnostic::draw_targets), n(Diagnostic::draw_other_scene_extent),
        n(Diagnostic::draw_preset_off), n(Diagnostic::draw_image_unready));
    log->log_info("[DD2 VRS] Pass VRS | nonindexed=%llu nonindexed_with_image=%llu execute_indirect=%llu"
        " indirect_with_image=%llu engine_rate_max=%llu engine_image_deferred=%llu"
        " render_pass_begins=%llu execute_bundle=%llu tagged_psos=%llu",
        n(Diagnostic::nonindexed_calls), n(Diagnostic::nonindexed_image_bound), n(Diagnostic::indirect_calls),
        n(Diagnostic::indirect_image_bound), n(Diagnostic::rate_combiner_preserved), n(Diagnostic::engine_image_deferred),
        n(Diagnostic::render_pass_begins), n(Diagnostic::bundle_calls), n(Diagnostic::pso_created));
    log->log_info("[DD2 VRS] Safety | max_rate=2x2 pipeline_policy=compute_only dispatch=%llu unbind_om=%llu unbind_pso=%llu"
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
        log->log_info("[DD2 VRS] Target sample | %s RTV=%s %ux%u DXGI=%u DSV=%s %ux%u DXGI=%u",
            names[i], sample.color_tracked ? "tracked" : "untracked", sample.color.width, sample.color.height,
            unsigned(sample.color_format), sample.depth_tracked ? "tracked" : "untracked", sample.depth.width,
            sample.depth.height, unsigned(sample.depth_format));
    }
    graphics::write_audit(graphics::take_audit(), host->functions->log_info,
                          settings_snapshot.activation_mode == always_on);
    if (settings_snapshot.engine.crowd.enabled && !perf_lua_present) {
        const auto& optimizer = published_gameplay.optimizer;
        const auto& c = optimizer.crowd;
        log->log_info("[DD2 VRS] Crowd | world=%s fps=%.1f avg=%.1f caps_ready=%u cap_write_failed=%u rule_hook=%u paused=%u pause_binding=%u npc=%d vanilla_npc=%d pawn=%d vanilla_pawn=%d changes=%d",
            world_label(optimizer.world),
            c.fps, c.average, unsigned(c.caps_ready), unsigned(c.cap_write_failed), unsigned(c.rule_hook), unsigned(c.paused),
            unsigned(c.pause_available), c.npc, c.vanilla_npc, c.pawn, c.vanilla_pawn, c.adjustments);
    }
}

Extent swapchain_extent(void* swapchain, bool refresh_buffers) {
    if (!swapchain) return {};
    Microsoft::WRL::ComPtr<IDXGISwapChain1> chain;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    const bool found = guarded([&] {
        auto* base = static_cast<IDXGISwapChain*>(swapchain);
        return SUCCEEDED(base->QueryInterface(IID_PPV_ARGS(&chain))) &&
            SUCCEEDED(chain->GetDesc1(&desc)) && desc.Width && desc.Height;
    });
    const Extent extent = found ? Extent{desc.Width, desc.Height} : Extent{};
    if (found && (refresh_buffers || extent != previous_output || swapchain != previous_swapchain)) {
        std::array<ID3D12Resource*, 16> buffers{};
        const size_t count = std::min<size_t>(desc.BufferCount, buffers.size());
        for (UINT i = 0; i < count; ++i) {
            Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
            guarded([&] {
                if (SUCCEEDED(chain->GetBuffer(i, IID_PPV_ARGS(&buffer)))) buffers[i] = buffer.Get();
                return true;
            });
        }
        graphics::set_swapchain_backbuffers(buffers.data(), count);
    }
    return extent;
}

void update_rendering(bool frame) {
    const auto& current = settings;
    if (!current.optimizer_enabled) {
        if (frame && renderer_initialized) graphics::frame(nullptr, nullptr, current, 0, previous_output);
        return;
    }
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
            host->functions->log_info("[DD2 VRS] 0.4.8 optimizer preview | %s | tier=%u tile=%u additional_rates=%u max_rate=2x2",
                                 status.message, status.tier, status.tile, unsigned(status.additional_rates));
    }
    const bool reset = renderer_reset.exchange(false) || swapchain != previous_swapchain;
    const auto output = swapchain_extent(swapchain, reset);
    if (reset || output != previous_output) {
        if (!output.width) graphics::set_swapchain_backbuffers(nullptr, 0);
        previous_output = output;
        previous_swapchain = swapchain;
        gameplay_requests.reset_world = true;
        published_gameplay.battle_source = waiting_combat;
        published_gameplay.optimizer.world = engine::WorldReadiness::waiting;
        published_gameplay.motion_active = published_gameplay.in_combat = published_gameplay.camera_found = false;
    }
    const auto preset = select_preset(current);
    if (!renderer_initialized) return;
    graphics::frame(device, queue, current, preset, output);
    const auto status = graphics::status();
    if (status.failed && status.message != last_renderer_error) {
        last_renderer_error = status.message;
        host->functions->log_error("[DD2 VRS] %s (tier %u, tile %u)", status.message, status.tier, status.tile);
    }
    if (frame) log_diagnostics(current);
}

GameplayFrame update_gameplay(const Settings& current, const GameplayRequests& requests,
                              Clock::time_point now) {
    if (requests.reset_world) {
        camera_bindings = {};
        previous = {};
        motion = {};
        reset_combat();
        engine::suspend_world();
        engine::invalidate();
    }
    if (requests.reset_combat) reset_combat();
    if (requests.edit) engine::edit(*requests.edit);
    if (requests.refresh) engine::refresh();
    if (requests.reseed_crowd) engine::reseed_crowd_limits();
    if (requests.cancel_snapshot) engine::cancel_snapshot_restore();
    if (requests.restore_snapshot) engine::restore_snapshot(*requests.restore_snapshot);

    const auto engine_state = engine::status();
    auto e = current.engine;
    if (!current.optimizer_enabled) {
        e.enabled = false;
        e.crowd.enabled = false;
    }
    const bool engine_allowed = !perf_lua_present && !engine_state.editing;
    const bool needs_engine = (!perf_lua_present && (e.enabled || e.crowd.enabled)) ||
        engine_state.active_tweaks || engine_state.pending_snapshot || engine_state.crowd.owned;
    engine::Object* player{};
    const bool vrs_enabled = current.optimizer_enabled && current.enabled;
    if (needs_engine || (current.optimizer_enabled && current.verbose_logging) ||
        (vrs_enabled && current.activation_mode == adaptive))
        guarded([&] { player = engine::gameplay_player(API::get()->tdb()); return true; });
    const bool needs_motion = (vrs_enabled && current.activation_mode != always_on) ||
        (engine_allowed && e.enabled && !e.always && !e.force_night && e.trigger_motion);
    CameraSample next{};
    const bool found = needs_motion && guarded([&] { return read_camera(next); });
    const float dt = std::chrono::duration<float>(now - previous_time).count();
    const auto camera_key = engine::object_key(next.camera);
    const bool contiguous = found && previous.key == camera_key && dt >= 0.001f && dt <= 0.2f;
    const float speed = contiguous ? velocity(previous.rotation, next.rotation, dt) : 0.0f;
    if (contiguous && speed * dt < 0.8f) motion.update(speed, dt, current);
    else motion = {};
    previous = found ? PreviousCameraSample{camera_key, next.rotation} : PreviousCameraSample{};
    previous_time = now;

    const bool combat_active = current.optimizer_enabled && update_combat(now, current, player);
    if (!needs_engine || !guarded([&] {
        engine::update(e, player, next.game_object, camera_render_owner, motion.active, combat_active, perf_lua_present,
                       std::chrono::duration<double>(now.time_since_epoch()).count());
        return true;
    })) engine::suspend_world();
    return {engine::status(), engine::baselines(), battle_source, speed, found, motion.active, combat_active};
}

void on_update_behavior() noexcept {
    if (!callbacks_ready.load(std::memory_order_acquire)) return;
    try {
        Settings current;
        GameplayRequests requests;
        {
            std::lock_guard lock{frame_mutex};
            current = settings;
            requests = std::exchange(gameplay_requests, {});
        }

        // Keep the render/UI lock out of the VM call path.
        GameplayFrame next;
        if (!guarded([&] { next = update_gameplay(current, requests, Clock::now()); return true; }))
            engine::suspend_world();
        std::lock_guard lock{frame_mutex};
        if (gameplay_requests.reset_world) return;
        if (gameplay_requests.reset_combat) {
            next.in_combat = false;
            next.battle_source = waiting_combat;
        }
        published_gameplay = next;
    } catch (...) {}
}

void on_begin_rendering() noexcept {
    if (!callbacks_ready.load(std::memory_order_acquire)) return;
    try {
        std::lock_guard lock{frame_mutex};
        if (guarded([] { update_rendering(true); return true; })) return;
    } catch (...) {}
    graphics::set_preset(0);
    active_reason.store(Activity::idle);
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
    engine::Status engine_status{};
    bool in_combat{};
    {
        std::lock_guard lock{frame_mutex};
        d = graphics::status();
        current = settings;
        reason = active_reason.load(std::memory_order_relaxed);
        combat_source = published_gameplay.battle_source;
        speed = published_gameplay.camera_speed;
        found = published_gameplay.camera_found;
        in_combat = published_gameplay.in_combat;
        engine_status = published_gameplay.optimizer;
    }
    const bool apply_recommended = ui.button("Recommended Steam Deck settings", {});
    if (apply_recommended) current = recommended_steam_deck_settings();
    const bool switch_optimizer = ui.button(current.optimizer_enabled ? "Switch Optimizer Off" : "Switch Optimizer On", {});
    if (switch_optimizer) current.optimizer_enabled = !current.optimizer_enabled;
    char text[256];
    if (!current.optimizer_enabled) {
        const bool restoring = engine_status.active_tweaks || engine_status.crowd.owned;
        const char* line = restoring ? "Optimizer off. Restoring game settings..." :
            d.hot_hooks_active ? "Optimizer off. Finishing D3D12 command lists..." : "Optimizer off";
        std::snprintf(text, sizeof(text), "%s", line);
    } else if (full_sharp_center(current.center)) {
        std::snprintf(text, sizeof(text), "State: Full-screen 1x1");
    } else if (d.preset && d.scene.width && d.image == d.scene) {
        const auto& profile = profiles[d.preset - 1];
        std::snprintf(text, sizeof(text), "State: ACTIVE (%s) | Profile: %s", reason_label(reason), profile.name);
    } else {
        std::snprintf(text, sizeof(text), "State: IDLE (1x1)%s", d.preset ? " | Waiting for scene image" : "");
    }
    ui.text(text, nullptr);
    std::snprintf(text, sizeof(text), "Combat: %s | Night: %s",
        in_combat ? "Yes" : "No", engine_status.clock_valid ? (engine_status.night ? "Yes" : "No") : "Unknown");
    ui.text(text, nullptr);
    if (d.failed) ui.text(d.message, nullptr);
    if (d.hot_hooks_out_of_sync) ui.text("D3D12 hook toggle failed. Retrying...", nullptr);
    ui.text("Activation Mode", nullptr);
    bool changed = apply_recommended || switch_optimizer;
    changed |= ui.radio("Adaptive (Motion + Combat)", &current.activation_mode, adaptive);
    changed |= ui.radio("Force Always On", &current.activation_mode, always_on);
    changed |= ui.radio("Motion Only", &current.activation_mode, motion_only);
    ui.text("VRS Profile", nullptr);
    changed |= ui.radio("Default (Balanced Soft)", &current.profile_mode, 0);
    changed |= ui.radio("Extreme (Max Performance)", &current.profile_mode, 1);
    changed |= ui.checkbox("Shade Alpha / Particle Passes", &current.shade_transparent_passes);
    changed |= ui.checkbox("Allow VRS at Native Resolution", &current.allow_native_vrs);
    changed |= draw_center_size(current, d);

    bool edit_engine = false, resume_engine = false, refresh_engine = false;
    bool write_snapshot = false, read_snapshot = false, cancel_snapshot = false;
    auto& e = current.engine;
    if (ui.header("Native Engine Optimizer", 0)) {
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
        changed |= ui.checkbox("Allow Heavy Render-Target Tweaks", &e.heavy);
        if (e.heavy) {
            changed |= ui.slider("Shadow Quality Target", &e.shadow_target, 0.0f, 4.0f, "%.0f", 0);
            changed |= ui.slider("Shadow Resolution Limit", &e.shadow_pixels, 384.0f, 4096.0f, "%.0f px", 0);
        }
        const auto show_binding = [&](size_t i) {
            if (!e.tweak_enabled(i) || !engine_status.tweak_status[i]) return;
            std::snprintf(text, sizeof(text), "%s | %s%s%s", engine_labels[i], engine_status.tweak_status[i],
                engine_status.properties[i] ? " | " : "", engine_status.properties[i] ? engine_status.properties[i] : "");
            ui.text(text, nullptr);
        };
        for (size_t i = 0; i < e.tweaks.size(); ++i) {
            if (heavy_tweak(i) && !e.heavy) continue;
            changed |= ui.checkbox(engine_labels[i], &e.tweaks[i]);
            show_binding(i);
            if (i == oov_streaming_off) show_binding(oov_texture_streaming_off);
        }
        std::snprintf(text, sizeof(text), "Active overrides: %d", engine_status.active_tweaks);
        ui.text(text, nullptr);
        if (engine_status.editing) {
            if (engine_status.pending_snapshot)
                cancel_snapshot = ui.button("Cancel Saved Baseline Restore", {});
            if (engine_status.active_tweaks || engine_status.pending_snapshot || engine_status.crowd.owned)
                ui.text("Restoring baseline", nullptr);
            else {
                ui.text("Baseline restored. Edit in-game settings now.", nullptr);
                resume_engine = ui.button("Finish Editing", {});
            }
        } else edit_engine = ui.button("Restore Baseline / Edit Settings", {});
        refresh_engine = ui.button("Retry Engine Bindings", {});
        write_snapshot = ui.button("Save Baseline Snapshot", {});
        if (!perf_lua_present && current.optimizer_enabled) {
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
        else if (c.enabled && engine_status.world != engine::WorldReadiness::ready) ui.text(world_label(engine_status.world), nullptr);
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
        bool game_pawn_baseline = !c.pawn_baseline.has_value();
        if (ui.checkbox("Use Game's Baseline Pawn Cap", &game_pawn_baseline)) {
            if (game_pawn_baseline) c.pawn_baseline.reset();
            else c.pawn_baseline = live.caps_ready ? live.vanilla_pawn : c.pawn_min;
            changed = true;
        }
        if (c.pawn_baseline) changed |= int_slider("Baseline Pawns", *c.pawn_baseline, c.pawn_min, c.pawn_max);
        if (ui.button("Restore Game NPC / Pawn Caps", {})) {
            c.npc = c.pawn = false;
            c.manual_npc_limit = 0;
            changed = true;
        }
        if (ui.header("Crowd Timing & Steps", 0)) {
            changed |= int_slider("NPC Step", c.npc_step, 1, 12);
            changed |= int_slider("Critical NPC Step", c.npc_emergency_step, 1, 20);
            changed |= int_slider("FPS Sample Frames", c.sample_window, 20, 240);
            changed |= int_slider("Cap Adjustment Frames", c.adjust_interval, 1, 600);
            changed |= int_slider("Cap Direction Hysteresis Frames", c.hysteresis, 0, 1200);
        }
    }
    const bool advanced = ui.header("Advanced / Debug", 0);
    if (advanced) {
        changed |= ui.checkbox("Enable foveated VRS", &current.enabled);
        changed |= ui.checkbox("Verbose Audit Logging", &current.verbose_logging);
        std::snprintf(text, sizeof(text), "VRS Tier %u | Tile %u x %u | Transition %s",
                      d.tier, d.tile, d.tile, d.additional_rates ? "2x1 / 1x2" : "2x2 fallback");
        ui.text(text, nullptr);
        std::snprintf(text, sizeof(text), "Scene %u x %u | Output %u x %u",
                      d.scene.width, d.scene.height, d.output.width, d.output.height);
        ui.text(text, nullptr);
        if (current.verbose_logging) {
            std::snprintf(text, sizeof(text), "Previous frame: %llu/%llu indexed draws shaded | %llu indirect calls with image",
                          static_cast<unsigned long long>(d.shaded), static_cast<unsigned long long>(d.candidates),
                          static_cast<unsigned long long>(d.indirect_with_image));
            ui.text(text, nullptr);
        }
        {
            std::snprintf(text, sizeof(text), "Combat source: %s", combat_source);
            ui.text(text, nullptr);
        }
        if (found) {
            std::snprintf(text, sizeof(text), "Camera: %.2f rad/s", speed);
            ui.text(text, nullptr);
        }
    }
    if (changed || edit_engine || resume_engine || refresh_engine || cancel_snapshot) {
        std::lock_guard lock{frame_mutex};
        const bool optimizer_changed = settings.optimizer_enabled != current.optimizer_enabled;
        if (optimizer_changed || settings.activation_mode != current.activation_mode || settings.enabled != current.enabled) {
            gameplay_requests.reset_combat = true;
            published_gameplay.in_combat = false;
            published_gameplay.battle_source = waiting_combat;
        }
        if (optimizer_changed || settings.verbose_logging != current.verbose_logging) next_diagnostics = {};
        sanitize(current);
        if (optimizer_changed) {
            graphics::set_optimizer_enabled(current.optimizer_enabled);
            if (!current.optimizer_enabled) {
                gameplay_requests.cancel_snapshot = true;
                gameplay_requests.restore_snapshot.reset();
                confirm_snapshot_restore = false;
            }
        }
        if (optimizer_changed || settings.engine != current.engine || refresh_engine) gameplay_requests.refresh = true;
        if (apply_recommended || settings.engine.crowd.npc_baseline != current.engine.crowd.npc_baseline ||
            settings.engine.crowd.pawn_baseline != current.engine.crowd.pawn_baseline)
            gameplay_requests.reseed_crowd = true;
        settings = current;
        if (edit_engine) gameplay_requests.edit = true;
        if (resume_engine) {
            gameplay_requests.edit = false;
            gameplay_requests.restore_snapshot.reset();
        }
        if (cancel_snapshot) {
            gameplay_requests.cancel_snapshot = true;
            gameplay_requests.restore_snapshot.reset();
            snapshot_status = "Saved baseline restore stopped";
        }
        graphics::set_transparent_passes(current.shade_transparent_passes);
        const auto preset = select_preset(current);
        graphics::set_preset(preset);
    }
    if (write_snapshot) {
        engine::Baselines values;
        { std::lock_guard lock{frame_mutex}; values = published_gameplay.baselines; }
        try { snapshot_status = save_baseline(values) ? "Baseline snapshot saved" : "No baseline snapshot saved"; }
        catch (...) { snapshot_status = "Could not save baseline snapshot"; }
    }
    if (read_snapshot) {
        try {
            if (const auto values = load_baseline()) {
                std::lock_guard lock{frame_mutex};
                gameplay_requests.restore_snapshot = *values;
                snapshot_status = "Restoring saved baseline on the game thread";
            } else snapshot_status = "No usable saved baseline";
        } catch (...) { snapshot_status = "Could not load baseline snapshot"; }
    }
    if (changed) {
        try { save_status = save_config(current) ? "Config saved" : "Could not save config"; }
        catch (...) { save_status = "Could not save config"; }
    }
    if (save_status && (advanced || std::string_view(save_status) == "Could not save config")) ui.text(save_status, nullptr);
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
    graphics::set_optimizer_enabled(settings.optimizer_enabled);
    std::error_code filesystem_error;
    perf_lua_present = std::filesystem::exists(config_path.parent_path().parent_path() /
        L"autorun" / L"perf.lua", filesystem_error);
    ui_ready = param->reframework_module && ui.bind(static_cast<HMODULE>(param->reframework_module));
    if (!ui_ready)
        API::get()->log_warn("[DD2 VRS] REFramework cimgui exports unavailable. JSON settings remain available.");

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            address, &module)) return false;
    const auto* functions = param->functions;
    if (!functions->on_pre_application_entry("UpdateBehavior", on_update_behavior) ||
        !functions->on_pre_application_entry("BeginRendering", on_begin_rendering)) return false;
    if (functions->on_device_reset) functions->on_device_reset(on_renderer_reset);
    if (ui_ready && !functions->on_imgui_draw_ui(on_imgui_draw_ui))
        API::get()->log_warn("[DD2 VRS] Could not register the REFramework ImGui callback.");
    {
        std::lock_guard lock{frame_mutex};
        guarded([] { update_rendering(false); return true; });
    }
    callbacks_ready.store(true, std::memory_order_release);
    API::get()->log_info("[DD2 VRS] Gameplay callbacks | UpdateBehavior=VM BeginRendering=D3D12 population_grace=2.0s");
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
