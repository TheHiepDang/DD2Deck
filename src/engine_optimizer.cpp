#include "engine_optimizer.hpp"
#include "graphics.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dd2::engine {
namespace {
using reframework::API;
using Method = API::Method;
using Field = API::Field;
using Type = API::TypeDefinition;

enum class Scalar { invalid, boolean, i8, u8, i16, u16, i32, u32, f32 };
Scalar scalar(Type* type) {
    if (!type) return Scalar::invalid;
    if (type->is_enum()) {
        // Use TDB metadata when available. Some native enums have a backing
        // field but no usable System.RuntimeType for get_underlying_type().
        auto* storage = type->find_field("value__");
        auto* underlying = storage && !storage->is_static() ? storage->get_type() : nullptr;
        type = underlying && underlying != type && !underlying->is_enum()
            ? underlying : type->get_underlying_type();
    }
    if (!type) return Scalar::invalid;
    const auto name = type->get_full_name();
    if (name == "System.Boolean") return Scalar::boolean;
    if (name == "System.SByte") return Scalar::i8;
    if (name == "System.Byte") return Scalar::u8;
    if (name == "System.Int16") return Scalar::i16;
    if (name == "System.UInt16") return Scalar::u16;
    if (name == "System.Int32") return Scalar::i32;
    if (name == "System.UInt32") return Scalar::u32;
    if (name == "System.Single") return Scalar::f32;
    return Scalar::invalid;
}

template<class T> T unpack(const void* data) {
    T value{};
    std::memcpy(&value, data, sizeof(value));
    return value;
}

std::optional<double> number(Scalar kind, const void* data) {
    if (!data) return {};
    switch (kind) {
    case Scalar::boolean: return unpack<uint8_t>(data) != 0;
    case Scalar::i8: return unpack<int8_t>(data);
    case Scalar::u8: return unpack<uint8_t>(data);
    case Scalar::i16: return unpack<int16_t>(data);
    case Scalar::u16: return unpack<uint16_t>(data);
    case Scalar::i32: return unpack<int32_t>(data);
    case Scalar::u32: return unpack<uint32_t>(data);
    case Scalar::f32: {
        const float value = unpack<float>(data);
        if (std::isfinite(value)) return value;
        return {};
    }
    default: return {};
    }
}

std::optional<uint32_t> pack(Scalar kind, double value) {
    if (!std::isfinite(value)) return {};
    if (kind == Scalar::f32) return std::bit_cast<uint32_t>(float(value));
    double min{}, max{};
    switch (kind) {
    case Scalar::boolean: max = 1; break;
    case Scalar::i8: min = INT8_MIN; max = INT8_MAX; break;
    case Scalar::u8: max = UINT8_MAX; break;
    case Scalar::i16: min = INT16_MIN; max = INT16_MAX; break;
    case Scalar::u16: max = UINT16_MAX; break;
    case Scalar::i32: min = INT32_MIN; max = INT32_MAX; break;
    case Scalar::u32: max = UINT32_MAX; break;
    default: return {};
    }
    value = std::round(value);
    if (value < min || value > max) return {};
    return uint32_t(int64_t(value));
}

size_t scalar_size(Scalar kind) {
    if (kind == Scalar::boolean || kind == Scalar::i8 || kind == Scalar::u8) return 1;
    if (kind == Scalar::i16 || kind == Scalar::u16) return 2;
    return 4;
}

Method* method(Type* type, std::initializer_list<const char*> names, unsigned args = 0) {
    if (type) for (const auto* name : names)
        if (auto* m = type->find_method(name); m && m->get_num_params() == args) return m;
    return nullptr;
}

Method* inherited_method(Type* type, std::initializer_list<const char*> names, unsigned args = 0) {
    for (auto* name : names) for (auto* base = type; base; base = base->get_parent_type())
        if (auto* candidate = method(base, {name}, args); candidate && !candidate->is_static()) return candidate;
    return nullptr;
}

bool invoke(Method* m, void* object, reframework::InvokeRet& ret, void* arg = nullptr, bool takes_arg = false) {
    return m && object && guarded([&] {
        // The C plugin table's invoke takes a raw this pointer. It uses the
        // same TDB invocation as Lua sdk.call_native_func, for both owners.
        const auto* methods = API::get()->sdk()->method;
        return methods && methods->invoke && methods->invoke(*m, object, takes_arg ? &arg : nullptr,
            takes_arg ? sizeof(arg) : 0, &ret, sizeof(ret)) == REFRAMEWORK_ERROR_NONE && !ret.exception_thrown;
    });
}

Object* object_call(Method* m, void* object, void* arg = nullptr, bool takes_arg = false) {
    reframework::InvokeRet ret{};
    return invoke(m, object, ret, arg, takes_arg) ? static_cast<Object*>(ret.ptr) : nullptr;
}

Type* managed_type(Object* object) {
    Type* type{};
    if (object) guarded([&] {
        const auto* sdk = API::get()->sdk()->managed_object;
        if (sdk->is_managed_object && !sdk->is_managed_object(object)) return false;
        type = object->get_type_definition();
        return type != nullptr;
    });
    return type;
}

Object* managed_instance(const API::TDB* db, const char* name) {
    auto& api = API::get();
    if (auto* instance = api->get_managed_singleton(name)) return instance;
    auto* type = db ? db->find_type(name) : nullptr;
    if (!type) return nullptr;
    const auto short_name = std::string_view{name}.substr(std::string_view{name}.rfind('.') + 1);
    const std::string getter_name = "get_s" + std::string(short_name);
    auto* getter = method(type, {getter_name.c_str()});
    if (!getter || !getter->is_static() || getter->get_return_type() != type) return nullptr;
    reframework::InvokeRet out{};
    return api->sdk()->method->invoke(*getter, nullptr, nullptr, 0, &out, sizeof(out)) == REFRAMEWORK_ERROR_NONE &&
        !out.exception_thrown ? static_cast<Object*>(out.ptr) : nullptr;
}

std::optional<double> read_method(Method* m, void* object, Scalar kind) {
    if (kind == Scalar::invalid) return {};
    reframework::InvokeRet ret{};
    return invoke(m, object, ret) ? number(kind, ret.bytes.data()) : std::nullopt;
}

struct Access {
    Method* get{}, *set{};
    Field* field{};
    Scalar kind{};
    explicit operator bool() const { return kind != Scalar::invalid; }
};

Access field_binding(Field* field, bool writable = false) {
    const auto* sdk = API::get()->sdk()->field;
    if (!field || !sdk || field->is_static() || (sdk->is_literal && field->is_literal())) return {};
    constexpr unsigned init_only = 0x20; // System.Reflection.FieldAttributes.InitOnly
    if (writable && sdk->get_flags && (field->get_flags() & init_only)) return {};
    return {nullptr, nullptr, field, scalar(field->get_type())};
}

Method* scalar_setter(Type* type, std::initializer_list<const char*> names, Scalar kind) {
    const auto matches = [kind](Method* set) {
        if (!set || set->is_static() || set->get_num_params() != 1) return false;
        const auto args = set->get_params();
        return args.size() == 1 && scalar(reinterpret_cast<Type*>(args[0].t)) == kind;
    };
    for (const auto* name : names) for (auto* base = type; base; base = base->get_parent_type()) {
        if (auto* set = base->find_method(name); matches(set)) return set;
    }
    // A bare-name lookup may select a different overload on a native type.
    for (auto* base = type; base; base = base->get_parent_type()) {
        for (auto* set : base->get_methods()) {
            const auto* name = set ? set->get_name() : nullptr;
            if (name && std::find(names.begin(), names.end(), std::string_view{name}) != names.end() &&
                matches(set)) return set;
        }
    }
    return nullptr;
}

Access accessor(Type* type, std::initializer_list<const char*> getters,
                std::initializer_list<const char*> setters, std::initializer_list<const char*> fields = {}) {
    for (auto* name : getters) {
        auto* get = inherited_method(type, {name});
        const auto kind = scalar(get ? get->get_return_type() : nullptr);
        if (kind == Scalar::invalid) continue;
        if (auto* set = scalar_setter(type, setters, kind)) return {get, set, nullptr, kind};
    }
    for (auto* name : fields) for (auto* base = type; base; base = base->get_parent_type()) {
        auto* field = base->find_field(name);
        auto binding = field_binding(field);
        if (!binding) continue;
        if (auto* set = scalar_setter(type, setters, binding.kind)) {
            binding.set = set;
            return binding;
        }
        if (field_binding(field, true)) return binding;
    }
    return {};
}

Access native_accessor(Type* type, const char* getter, std::initializer_list<const char*> setters) {
    // Native render methods can carry the Static flag even though the Lua
    // native binding supplies the singleton pointer. Match names and arity,
    // and use the getter's scalar ABI without relying on setter type handles.
    auto* get = method(type, {getter});
    auto* set = method(type, setters, 1);
    if (!get || !set) return {};
    auto* result = get->get_return_type();
    auto kind = scalar(result);
    if (kind == Scalar::invalid && result && result->is_enum() && result->get_full_name().starts_with("via.render."))
        kind = Scalar::i32;
    return {get, set, nullptr, kind};
}

const char* scalar_name(Scalar kind) {
    switch (kind) {
    case Scalar::boolean: return "bool";
    case Scalar::i8: return "i8";
    case Scalar::u8: return "u8";
    case Scalar::i16: return "i16";
    case Scalar::u16: return "u16";
    case Scalar::i32: return "i32";
    case Scalar::u32: return "u32";
    case Scalar::f32: return "f32";
    default: return "invalid";
    }
}

std::optional<double> read_field(Field* field, void* owner, Scalar kind) {
    std::optional<double> value;
    if (field && owner) guarded([&] {
        value = number(kind, field->get_data_raw(owner, false));
        return true;
    });
    return value;
}

std::optional<double> read(const Access& a, void* owner) {
    if (const auto value = read_method(a.get, owner, a.kind)) return value;
    if (const auto value = read_field(a.field, owner, a.kind)) return value;
    return {};
}

bool write(const Access& a, void* owner, double value) {
    const auto bits = pack(a.kind, value);
    if (!bits) return false;
    if (a.set) {
        reframework::InvokeRet ret{};
        return invoke(a.set, owner, ret, reinterpret_cast<void*>(uintptr_t(*bits)), true);
    }
    return a.field && guarded([&] {
        auto* data = a.field->get_data_raw(owner);
        if (!data) return false;
        std::memcpy(data, &*bits, scalar_size(a.kind));
        return true;
    });
}

bool same(double a, double b) { return std::abs(a - b) <= 0.0001; }

struct Change {
    uintptr_t owner{};
    double baseline{};
    std::optional<double> written;
    double retry{};
    unsigned failures{};

    void reconcile(void* current_owner, const Access& access, std::optional<double> value,
                   bool enable, double now) {
        if (!current_owner || !access || !value) return;
        const auto identity = object_key(current_owner);
        if (owner != identity) *this = {};
        // A game or another mod may have changed the value. Only undo a
        // value we still own. The external value becomes the next baseline.
        if (written && !same(*written, *value)) *this = {};
        if (!enable && !written) return;
        if (now < retry || (enable && failures >= 3)) return;
        if (!owner || !written) {
            owner = identity;
            baseline = *value;
        }
    }

    bool apply(const Access& access, void* current_owner, double current, double target, bool enable, double now) {
        if (!current_owner || object_key(current_owner) != owner || now < retry || (enable && failures >= 3)) return false;
        if (same(current, target)) {
            if (!enable) *this = {};
            return false;
        }
        const auto bits = pack(access.kind, target);
        if (!bits) return false;
        const auto expect = number(access.kind, &*bits);
        if (!expect || same(current, *expect)) return false;
        const bool changed = write(access, current_owner, target);
        const auto after = read(access, current_owner);
        if (after && same(*after, *expect)) {
            if (enable) {
                written = *after;
                failures = 0;
            } else *this = {};
            return changed;
        }
        if (after && !same(*after, baseline)) written = *after;
        else if (changed && !after) written = *expect;
        ++failures;
        retry = now + (enable ? 8.0 : 0.05);
        return false;
    }
};

enum class Owner { managed, native, config, component };
struct Spec {
    const char* type;
    Owner owner;
    std::initializer_list<const char*> get, set;
    std::initializer_list<const char*> fields;
};
const std::array<Spec, tweak_count> specs{{
    {"app.RenderingManager", Owner::managed, {"getSpeedTreeLODBias", "get_SpeedTreeLODBias", "get_SpeedTreeLodBias"},
        {"setSpeedTreeLODBias", "set_SpeedTreeLODBias"}, {"_SpeedTreeLodBias"}},
    {"app.LODProcessManager", Owner::managed, {"get_MotionQuality", "getMotionQuality", "get_MotionLOD"},
        {"set_MotionQuality", "setMotionQuality", "set_MotionLOD"}, {"_MotionQuality", "MotionQuality", "_MotionLOD"}},
    {"via.render.RenderConfig", Owner::config, {"get_ContactShadowSetting", "get_ContactShadows"},
        {"set_ContactShadowSetting", "set_ContactShadows"}, {}},
    {"via.render.RenderConfig", Owner::config, {"get_ShadowQuality"}, {"set_ShadowQuality"}, {}},
    {"via.render.LightRenderer", Owner::native, {"get_ShadowResolution"}, {"set_ShadowResolution"}, {}},
    {"via.render.SSAOControl", Owner::component, {"get_HalfResolution"}, {"set_HalfResolution"}, {}},
    {"via.render.SSRControl", Owner::component, {"get_HalfResolution"}, {"set_HalfResolution"}, {}},
    {"via.render.LightRenderer", Owner::native, {"get_ShadowFilter"}, {"set_ShadowFilter"}, {}},
    {"via.render.SSRControl", Owner::component, {"get_Type"}, {"set_Type"}, {}},
    {"via.render.SSRControl", Owner::component, {"get_ResolvePoint"}, {"set_ResolvePoint"}, {}},
    {"via.render.SSAOControl", Owner::component, {"get_DFAO"}, {"set_DFAO"}, {}},
    {"via.render.MPMR", Owner::native, {"get_OutOfViewStreaming"}, {"set_OutOfViewStreaming"}, {}},
    {"via.render.MPMR", Owner::native, {"get_OutOfViewTextureStreaming"}, {"set_OutOfViewTextureStreaming"}, {}}
}};

const API::TDB* tdb{};
std::array<Access, tweak_count> bindings{};
std::array<Change, tweak_count> changes{};
Baselines captured{}, snapshot_to_restore{};
std::array<const char*, tweak_count> properties{};
std::array<double, tweak_count> discovery_retry{};
std::array<std::string_view, tweak_count> native_binding_status{};
Method* render_config_get{};
Method* clock_hour{}, *clock_minute{};
Scalar hour_kind{}, minute_kind{};
std::vector<int> shadow_sizes;
Status state;
double next_update{}, next_clock{};
bool previous_active{};

Object* component(Object* go, const char* name) {
    if (!go) return nullptr;
    auto* get = method(go->get_type_definition(), {"getComponent(System.Type)"}, 1);
    auto* type_object = get ? API::get()->typeof(name) : nullptr;
    return type_object ? object_call(get, go, type_object, true) : nullptr;
}

void* owner(const Spec& spec, Object* camera_go, const char** unavailable = nullptr) {
    auto& api = API::get();
    switch (spec.owner) {
    case Owner::managed:
        return std::string_view(spec.type) == "app.LODProcessManager"
            ? managed_instance(tdb, spec.type) : api->get_managed_singleton(spec.type);
    case Owner::native: {
        auto* instance = api->get_native_singleton(spec.type);
        if (!instance && unavailable) *unavailable = "native_singleton_unavailable";
        return instance;
    }
    case Owner::config: {
        if (!render_config_get) {
            // Resolve the native getter exactly as Lua does, regardless of
            // the instance/static flag on the TDB method.
            render_config_get = method(tdb->find_type("via.render.Renderer"), {"get_RenderConfig"});
        }
        auto* renderer = api->get_native_singleton("via.render.Renderer");
        const auto* result = render_config_get ? render_config_get->get_return_type() : nullptr;
        const bool returns_reference = result && !result->is_valuetype();
        if (returns_reference) {
            reframework::InvokeRet out{};
            if (invoke(render_config_get, renderer, out) && out.ptr) return out.ptr;
        }
        if (auto* config = api->get_native_singleton(spec.type)) return config;
        if (unavailable) {
            *unavailable = !renderer ? "renderer_singleton_unavailable" :
                !render_config_get ? "renderer_config_getter_unavailable" :
                !returns_reference ? "renderer_config_return_type_unsupported" : "renderer_config_read_failed";
        }
        return nullptr;
    }
    case Owner::component: return component(camera_go, spec.type);
    }
    return nullptr;
}

void bind(size_t index, Object* go, double now) {
    const auto& spec = specs[index];
    const bool native = spec.owner == Owner::native || spec.owner == Owner::config;
    const auto report = [&](const char* result, const Access& access = {}) {
        if (!native) return;
        if (native_binding_status[index] == result) return;
        native_binding_status[index] = result;
        if (access) {
            auto* type = access.get->get_return_type();
            API::get()->log_info("[DD2 VRS] Native binding %s.%s | %s setter=%s return=%s scalar=%s static=%u/%u",
                spec.type, access.get->get_name(), result, access.set->get_name(),
                type ? type->get_full_name().c_str() : "unknown", scalar_name(access.kind),
                unsigned(access.get->is_static()), unsigned(access.set->is_static()));
        } else {
            API::get()->log_info("[DD2 VRS] Native binding %s.%s | %s", spec.type, *spec.get.begin(), result);
        }
    };
    if (bindings[index]) {
        report("resolved", bindings[index]);
        return;
    }
    if (now < discovery_retry[index]) return;
    discovery_retry[index] = now + 8.0;
    auto* definition = tdb->find_type(spec.type);
    if (!definition) {
        report("type_unavailable");
        return;
    }

    void* object{};
    const char* unavailable = "compatible_accessor_unavailable";
    const auto try_access = [&](const Access& access) {
        if (!access || boolean_tweak(index) != (access.kind == Scalar::boolean)) return false;
        if (!object) object = owner(spec, go, &unavailable);
        if (!object) return false;
        if (!read(access, object)) {
            unavailable = "getter_read_failed";
            return false;
        }
        bindings[index] = access;
        properties[index] = access.get ? access.get->get_name() : access.field->get_name();
        report("resolved", access);
        return true;
    };
    for (auto* getter : spec.get)
        if (try_access(native ? native_accessor(definition, getter, spec.set)
                              : accessor(definition, {getter}, spec.set))) return;
    for (auto* field : spec.fields)
        if (try_access(accessor(definition, {}, spec.set, {field}))) return;
    report(unavailable);
}

double target(size_t i, const EngineSettings& e, double baseline) {
    switch (i) {
    case speedtree: return std::max(baseline, double(std::round(e.speedtree_bias)));
    case motion_quality: return std::min(baseline, double(std::round(e.motion_target)));
    case contact_shadows: return 2;
    case shadow_quality: return std::min(baseline, double(std::round(e.shadow_target)));
    case ssr_disable: return 1; // SSRControlType::Disable
    case shadow_filter: // ShadowFilter::Custom
    case ssr_low_resolve: // SSRResolvePointNum::_1
    case dfao_disable:
    case oov_streaming_off:
    case oov_texture_streaming_off: return 0;
    case shadow_resolution: {
        if (shadow_sizes.empty()) {
            if (auto* type = tdb->find_type("via.render.ShadowResolution")) {
                for (auto* field : type->get_fields()) if (field && field->is_static()) {
                    guarded([&] {
                        const auto value = number(scalar(field->get_type()), field->get_data_raw(nullptr));
                        if (value && *value > 0 && *value < 65536 && *value == std::floor(*value))
                            shadow_sizes.push_back(int(*value));
                        return true;
                    });
                }
            }
            if (shadow_sizes.empty()) shadow_sizes = {384, 512, 768, 1024, 2048, 4096};
            std::sort(shadow_sizes.begin(), shadow_sizes.end());
        }
        const auto best = std::upper_bound(shadow_sizes.begin(), shadow_sizes.end(), std::min(baseline, double(e.shadow_pixels)));
        return best == shadow_sizes.begin() ? baseline : *std::prev(best);
    }
    default: return 1;
    }
}

void poll_clock(const EngineSettings& e, double now) {
    if (now < next_clock) return;
    next_clock = now + 2.0;
    if (!clock_hour) {
        auto* type = tdb->find_type("app.TimeManager");
        clock_hour = method(type, {"get_InGameHour"});
        clock_minute = method(type, {"get_InGameMinute"});
        if (clock_hour) hour_kind = scalar(clock_hour->get_return_type());
        if (clock_minute) minute_kind = scalar(clock_minute->get_return_type());
    }
    auto* clock = API::get()->get_managed_singleton("app.TimeManager");
    const auto h = read_method(clock_hour, clock, hour_kind);
    const auto m = read_method(clock_minute, clock, minute_kind);
    state.clock_valid = h && *h >= 0 && *h < 24;
    state.hour = state.clock_valid ? float(*h + (m && *m >= 0 && *m < 60 ? *m / 60 : 0)) : 0.0f;
    state.night = state.clock_valid && is_night(state.hour, e.night_start, e.night_end);
}

struct CombatBinding {
    const API::TDB* db{};
    Method* getter{};
    const char* source{};
} combat_binding;
struct GameplayBinding {
    const API::TDB* db{};
    Type* character{}, *game_object_type{}, *player_type{};
    Method* getter{}, *load_gui{}, *valid{}, *game_object{};
    Field* field{};
} gameplay_binding;
uintptr_t loading_player{};
const API::TDB* loading_tdb{};
constexpr double world_grace_seconds = 2.0;
double world_ready_at{}, last_world_update{};

// Rule changes can arrive on an engine thread. Defer all reads and writes to
// UpdateBehavior so a post-hook cannot race baseline restoration.
std::atomic<bool> rules_changed{};
void on_generate_rule(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    rules_changed.store(true, std::memory_order_release);
}

constexpr size_t cap_count = 4;
using CapValues = std::array<int, cap_count>;
constexpr const char* generate_manager = "app.GenerateManager";
constexpr std::array cap_fields{"_NPCGenerateLimit", "_RuleNPCGenerateLimit",
                                "_StrayPawnGenerateLimit", "_RuleNPCandStrayPawnLimit"};

struct GenerationCaps {
    std::array<Access, cap_count> fields{};
    uintptr_t owner{};
    CapValues baseline{}, written{};
    bool owned{}, ready{}, initialized{};
} caps;
CrowdController crowd;
Method* gui_pause{};
double next_caps{}, next_crowd_bind{}, last_crowd_frame{};
bool crowd_running{}, rule_hook_checked{};
std::vector<Method*> hooked_rules;

Access field_access(Type* type, const char* name) {
    return type ? field_binding(type->find_field(name)) : Access{};
}

void bind_crowd(double now, bool enable) {
    if (now < next_crowd_bind) return;
    next_crowd_bind = now + 8.0;
    if (!gui_pause) {
        auto* gui = tdb->find_type("app.GuiManager");
        auto* paused = method(gui, {"isPausedGUI", "get_IsPausedGUI"});
        if (paused && !paused->is_static() && scalar(paused->get_return_type()) == Scalar::boolean) {
            gui_pause = paused;
        }
    }
    auto* gen = tdb->find_type(generate_manager);
    if (!gen) return;
    if (!caps.ready) {
        for (size_t i = 0; i < cap_count; ++i) caps.fields[i] = field_access(gen, cap_fields[i]);
        caps.ready = std::all_of(caps.fields.begin(), caps.fields.end(), [](const auto& field) {
            return field && field.kind != Scalar::boolean && field.kind != Scalar::f32;
        });
    }
    if (!enable || !caps.ready || rule_hook_checked) return;
    rule_hook_checked = true;
    const auto* sdk = API::get()->sdk()->functions;
    auto* rule = method(gen, {"onRuleChanged", "OnRuleChanged"});
    if (!rule) for (auto* candidate : gen->get_methods()) {
        if (candidate && candidate->get_name() && std::string_view(candidate->get_name()) == "onRuleChanged") {
            rule = candidate;
            break;
        }
    }
    if (rule && sdk->add_hook && std::find(hooked_rules.begin(), hooked_rules.end(), rule) == hooked_rules.end() &&
        rule->add_hook(nullptr, on_generate_rule, false)) hooked_rules.push_back(rule);
    state.crowd.rule_hook = rule && std::find(hooked_rules.begin(), hooked_rules.end(), rule) != hooked_rules.end();
}

std::optional<CapValues> read_caps(Object* owner) {
    CapValues observed{};
    for (size_t i = 0; i < observed.size(); ++i) {
        const auto value = read(caps.fields[i], owner);
        if (!value || *value < 0 || *value > INT32_MAX) return {};
        observed[i] = int(*value);
    }
    return observed;
}

bool commit_caps(Object* owner, const CapValues& current, const CapValues& desired) {
    if (current == desired) return true;
    for (size_t i = 0; i < current.size(); ++i)
        if (current[i] != desired[i] && (!pack(caps.fields[i].kind, desired[i]) ||
            !caps.fields[i].field->get_data_raw(owner))) return false;
    bool ok = true;
    for (size_t i = 0; i < current.size(); ++i)
        if (current[i] != desired[i] && !write(caps.fields[i], owner, desired[i])) { ok = false; break; }
    const auto after = read_caps(owner);
    if (ok && after && *after == desired) return true;
    // A partial update must never leave the combined cap out of step.
    for (size_t i = 0; i < current.size(); ++i) {
        if (current[i] == desired[i]) continue;
        if (read(caps.fields[i], owner) == desired[i]) write(caps.fields[i], owner, current[i]);
    }
    return false;
}

void update_caps(const CrowdSettings& s, bool enable, double now) {
    auto& live = state.crowd;
    if (crowd_running != enable) {
        crowd = {};
        if (enable && caps.initialized) crowd.set_limits(s, caps.baseline[0], caps.baseline[2]);
        last_crowd_frame = next_caps = 0;
        crowd_running = enable;
    }
    const bool dirty = rules_changed.load(std::memory_order_relaxed) &&
        rules_changed.exchange(false, std::memory_order_acq_rel);
    const bool due = dirty || now >= next_caps;
    if (enable && due) {
        const auto paused = read_method(gui_pause, API::get()->get_managed_singleton("app.GuiManager"), Scalar::boolean);
        live.paused = paused != 0.0;
        live.pause_available = paused.has_value();
    } else if (!enable) {
        live.paused = live.pause_available = false;
    }
    if (enable && !live.paused) {
        if (last_crowd_frame > 0) crowd.observe(now - last_crowd_frame, s);
        last_crowd_frame = now;
    } else {
        last_crowd_frame = 0;
        crowd.headroom_seconds = 0;
    }
    live.fps = crowd.fps;
    live.average = crowd.average;
    live.minimum = crowd.minimum;
    live.maximum = crowd.maximum;
    live.adjustments = crowd.adjustments;
    live.mode = crowd.mode;
    live.owned = caps.owned;
    live.restoring = !enable && caps.owned;
    if ((!enable && !caps.owned && !caps.initialized) || !due) return;
    next_caps = now + (enable ? 0.5 : 0.05);
    if (!caps.ready || live.paused) return;
    auto* owner = API::get()->get_managed_singleton(generate_manager);
    if (!owner) return;
    if (const auto key = object_key(owner); caps.owner != key) {
        caps.owner = key;
        caps.owned = caps.initialized = false;
        live.owned = live.restoring = false;
    }
    const auto readback = read_caps(owner);
    state.crowd.caps_ready = readback.has_value();
    if (!readback) return;
    const auto& current = *readback;
    if (!caps.owned) caps.baseline = current;
    else for (size_t i = 0; i < current.size(); ++i)
        if (current[i] != caps.written[i]) caps.baseline[i] = current[i];
    if (enable && !caps.initialized) {
        crowd.set_limits(s, caps.baseline[0], caps.baseline[2]);
        caps.initialized = true;
    }
    crowd.vanilla_npc = caps.baseline[0];
    if (!s.manual_npc_limit) crowd.npc = crowd.clamp_npc(s, crowd.npc);
    state.crowd.vanilla_npc = caps.baseline[0];
    state.crowd.vanilla_pawn = caps.baseline[2];
    auto target = caps.baseline;
    if (enable && s.npc) {
        target[0] = s.manual_npc_limit ? s.manual_npc_limit : crowd.npc;
        target[1] = s.manual_npc_limit ? s.manual_npc_limit : std::min(target[0], caps.baseline[1]);
    }
    if (enable && s.pawn) target[2] = std::clamp(crowd.pawn, s.pawn_min, s.pawn_max);
    if (enable && (s.npc || s.pawn))
        target[3] = int(std::min<int64_t>(int64_t(target[0]) + target[2], INT32_MAX));
    state.crowd.cap_write_failed = !commit_caps(owner, current, target);
    if (state.crowd.cap_write_failed) {
        next_caps = now + (enable ? 8.0 : 0.05);
        return;
    }
    state.crowd.npc = target[0];
    state.crowd.pawn = target[2];
    caps.written = target;
    caps.owned = target != caps.baseline;
    live.owned = caps.owned;
    live.restoring = !enable && caps.owned;
    if (!enable) caps.initialized = false;
}

bool derives_from(Type* type, Type* base) {
    if (!base) return false;
    for (unsigned i = 0; type && i < 64; ++i, type = type->get_parent_type())
        if (type == base) return true;
    return false;
}

void update_crowd(const CrowdSettings& settings, bool enable, double now) {
    if (!enable && !caps.owned && !caps.initialized && !crowd_running) return;
    bind_crowd(now, enable);
    update_caps(settings, enable, now);
}
void report_tweaks() {
    state.active_tweaks = int(std::count_if(changes.begin(), changes.end(), [](const auto& x) { return x.written.has_value(); }));
    state.camera_tweaks_active = changes[ssao_half].written || changes[ssr_half].written ||
        changes[ssr_disable].written || changes[ssr_low_resolve].written || changes[dfao_disable].written;
    state.pending_snapshot = int(std::count_if(snapshot_to_restore.begin(), snapshot_to_restore.end(),
        [](const auto& x) { return x.has_value(); }));
    state.properties = properties;
    for (size_t i = 0; i < changes.size(); ++i) {
        const auto& change = changes[i];
        state.tweak_status[i] = change.failures ? "Write/readback failed" : change.written ? "Applied" :
            bindings[i] ? "Ready" : discovery_retry[i] > 0 ? "Binding unavailable" : "Waiting for engine";
    }
}
} // namespace

Object* gameplay_player(const API::TDB* db) {
    auto& api = API::get();
    if (!db || !api->get_vm_context()) return nullptr;
    if (gameplay_binding.db != db) {
        gameplay_binding = {db};
        gameplay_binding.character = db->find_type("app.Character");
        gameplay_binding.game_object_type = db->find_type("via.GameObject");
        if (auto* gui = inherited_method(db->find_type("app.GuiManager"), {"get_IsLoadGui"});
            gui && scalar(gui->get_return_type()) == Scalar::boolean) gameplay_binding.load_gui = gui;
        if (auto* type = db->find_type("app.CharacterManager")) {
            auto* getter = inherited_method(type, {"get_ManualPlayer"});
            if (getter && derives_from(getter->get_return_type(), gameplay_binding.character))
                gameplay_binding.getter = getter;
            for (const auto* name : {"<ManualPlayer>k__BackingField", "_ManualPlayer"}) {
                auto* field = type->find_field(name);
                if (field && !field->is_static() && derives_from(field->get_type(), gameplay_binding.character)) {
                    gameplay_binding.field = field;
                    break;
                }
            }
        }
    }
    if (gameplay_binding.load_gui &&
        read_method(gameplay_binding.load_gui, api->get_managed_singleton("app.GuiManager"), Scalar::boolean) == 1.0)
        return nullptr;

    auto* manager = api->get_managed_singleton("app.CharacterManager");
    if (!manager) return nullptr;
    // Skip the title-screen getter when the backing slot is empty. Confirm a
    // populated slot with the getter when available, since it can outlive play.
    const auto* backing = gameplay_binding.field ? gameplay_binding.field->get_data_raw(manager, false) : nullptr;
    auto* player = backing ? unpack<Object*>(backing) : nullptr;
    if (gameplay_binding.field && !player) return nullptr;
    if (gameplay_binding.getter) player = object_call(gameplay_binding.getter, manager);
    auto* type = managed_type(player);
    if (!derives_from(type, gameplay_binding.character)) return nullptr;
    auto& binding = gameplay_binding;
    if (binding.player_type != type) {
        binding.player_type = type;
        binding.game_object = inherited_method(type, {"get_GameObject"});
        binding.valid = inherited_method(type, {"get_Valid"});
        if (binding.valid && scalar(binding.valid->get_return_type()) != Scalar::boolean) binding.valid = nullptr;
    }
    if (binding.valid && read_method(binding.valid, player, Scalar::boolean) != 1.0) return nullptr;
    auto* object = object_call(binding.game_object, player);
    return derives_from(managed_type(object), binding.game_object_type) ? player : nullptr;
}

std::optional<CombatReading> read_combat(const API::TDB* db, Object* player) {
    if (!db || !player) return {};
    if (combat_binding.db != db) {
        combat_binding = {db};
        if (auto* type = db->find_type("app.BattleManager")) {
            for (const auto& [name, source] : {
                    std::pair{"get_IsBattleMode", "app.BattleManager.get_IsBattleMode"},
                    {"get_IsInBattle", "app.BattleManager.get_IsInBattle"},
                    {"get_isInBattle", "app.BattleManager.get_isInBattle"},
                    {"get_IsBattle", "app.BattleManager.get_IsBattle"}}) {
                if (auto* getter = inherited_method(type, {name}); getter &&
                    scalar(getter->get_return_type()) == Scalar::boolean) {
                    combat_binding.getter = getter;
                    combat_binding.source = source;
                    break;
                }
            }
        }
    }
    if (!combat_binding.getter) return {};
    auto* manager = API::get()->get_managed_singleton("app.BattleManager");
    const auto value = read_method(combat_binding.getter, manager, Scalar::boolean);
    return value ? std::optional<CombatReading>{{*value != 0.0, combat_binding.source}} : std::nullopt;
}

void suspend_world() {
    loading_player = 0;
    last_crowd_frame = 0;
    loading_tdb = nullptr;
    world_ready_at = last_world_update = 0;
    state.world = WorldReadiness::waiting;
}

void update(const EngineSettings& settings, Object* player, Object* camera_object, Object* (*find_camera_object)(),
            bool motion, bool combat, bool blocked, double now) {
    const auto* current_tdb = API::get()->tdb();
    const bool have_vm = current_tdb && API::get()->get_vm_context();
    if (!player || !have_vm) {
        suspend_world();
    } else {
        // A save load can interrupt UpdateBehavior. Restart the grace period
        // after a gap or when the player or VM changes.
        const auto player_key = object_key(player);
        if (loading_player != player_key || loading_tdb != current_tdb ||
            now - last_world_update >= world_grace_seconds) {
            suspend_world();
            loading_player = player_key;
            loading_tdb = current_tdb;
            world_ready_at = now + world_grace_seconds;
        }
        last_world_update = now;
        state.world = now >= world_ready_at ? WorldReadiness::ready : WorldReadiness::settling;
    }
    const bool ready = state.world == WorldReadiness::ready;
    const bool restoring = !settings.enabled && state.active_tweaks != 0;
    if (!have_vm) {
        if (restoring) {
            for (size_t i = 0; i < changes.size(); ++i)
                if (specs[i].owner == Owner::component) changes[i] = {};
            report_tweaks();
        }
        return;
    }
    const auto& population = settings.crowd;
    const bool crowd_enabled = population.enabled && (population.npc || population.pawn) && !blocked && !state.editing;
    const bool restoring_caps = !crowd_enabled && caps.owned;
    if (!ready && !restoring && !restoring_caps) return;
    if (current_tdb != tdb) {
        if (tdb) {
            changes = {};
            caps = {};
            state.crowd.owned = state.crowd.restoring = false;
        }
        invalidate();
        tdb = current_tdb;
    }
    if (ready && settings.enabled && !blocked && settings.trigger_night) poll_clock(settings, now);
    const bool active = ready && settings.enabled && !blocked && !state.editing &&
        (settings.always || settings.force_night || (settings.trigger_motion && motion) || (settings.trigger_combat && combat) ||
         (settings.trigger_night && state.night));
    if (restoring || (ready && (now >= next_update || active != previous_active))) {
        previous_active = active;
        next_update = now + 1.0;
        bool camera_resolved = camera_object != nullptr;
        for (size_t i = 0; i < bindings.size(); ++i) {
            auto& change = changes[i];
            const bool configured = ready && settings.enabled && !blocked && settings.tweak_enabled(i) &&
                                    (!heavy_tweak(i) || settings.heavy);
            const bool on = active && configured;
            if (!configured && !change.written && (!ready || blocked || !snapshot_to_restore[i])) continue;
            const bool camera_tweak = specs[i].owner == Owner::component;
            if (!on && change.written && now < change.retry && (ready || !camera_tweak)) continue;
            bool found_camera_owner = false;
            guarded([&] {
                if (camera_tweak) {
                    if (!camera_resolved) {
                        camera_resolved = true;
                        if (find_camera_object) camera_object = find_camera_object();
                    }
                    if (!camera_object) return false;
                }
                bind(i, camera_object, now);
                if (!bindings[i]) return false;
                const auto& access = bindings[i];
                auto* obj = owner(specs[i], camera_object);
                found_camera_owner = camera_tweak && obj;
                const auto value = obj ? read(access, obj) : std::nullopt;
                if (!value && !change.written) {
                    bindings[i] = {};
                    properties[i] = nullptr;
                    return false;
                }
                change.reconcile(obj, access, value, on, now);
                if (on && value && change.owner) captured[i] = change.baseline;
                if (value && (on || change.written))
                    change.apply(access, obj, *value, on ? target(i, settings, change.baseline) : change.baseline, on, now);
                if (ready && obj && value && !blocked && !change.written && snapshot_to_restore[i]) {
                    const auto expected = *snapshot_to_restore[i];
                    const auto latest = read(access, obj);
                    const auto after = latest && same(*latest, expected) ? latest :
                        (write(access, obj, expected) ? read(access, obj) : std::nullopt);
                    if (after && same(*after, expected)) snapshot_to_restore[i].reset();
                }
                return true;
            });
            // Camera components may have been destroyed during a world
            // transition. Never hold a stale restore against them.
            if (restoring && !ready && camera_tweak && !found_camera_owner) change = {};
        }
    }
    if (ready || restoring_caps) update_crowd(population, ready && crowd_enabled, now);
    report_tweaks();
}

void edit(bool enabled) {
    state.editing = enabled;
    if (!enabled) {
        snapshot_to_restore = {};
        captured = {};
    }
    refresh();
}
void refresh() {
    next_update = next_clock = 0;
    next_caps = next_crowd_bind = 0;
    discovery_retry = {};
    native_binding_status = {};
    if (!state.crowd.rule_hook) rule_hook_checked = false;
    for (auto& change : changes) { change.retry = 0; change.failures = 0; }
}
void reseed_crowd_limits() {
    caps.initialized = false;
    next_caps = 0;
}
void invalidate() {
    bindings = {};
    properties = {};
    render_config_get = clock_hour = clock_minute = nullptr;
    shadow_sizes.clear();
    gui_pause = nullptr;
    rule_hook_checked = false;
    caps.ready = false;
    state.clock_valid = state.night = false;
    refresh();
}

Status status() { return state; }
Baselines baselines() { return captured; }
void restore_snapshot(const Baselines& saved) {
    snapshot_to_restore = saved;
    state.pending_snapshot = int(std::count_if(saved.begin(), saved.end(), [](const auto& x) { return x.has_value(); }));
    edit(true);
}
void cancel_snapshot_restore() {
    snapshot_to_restore = {};
    state.pending_snapshot = 0;
}
} // namespace dd2::engine
