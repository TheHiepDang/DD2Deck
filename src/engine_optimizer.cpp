#include "engine_optimizer.hpp"
#include "graphics.hpp"

#include <bit>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
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
    if (type->is_enum()) type = type->get_underlying_type();
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

bool invoke(Method* m, void* object, reframework::InvokeRet& ret, void* arg = nullptr, bool takes_arg = false) {
    return m && object && guarded([&] {
        return API::get()->sdk()->method->invoke(*m, object, takes_arg ? &arg : nullptr,
            takes_arg ? sizeof(arg) : 0, &ret, sizeof(ret)) == REFRAMEWORK_ERROR_NONE && !ret.exception_thrown;
    });
}

Object* object_call(Method* m, void* object, void* arg = nullptr, bool takes_arg = false) {
    reframework::InvokeRet ret{};
    return invoke(m, object, ret, arg, takes_arg) ? static_cast<Object*>(ret.ptr) : nullptr;
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

Access accessor(Type* type, std::initializer_list<const char*> getters,
                std::initializer_list<const char*> setters, const char* field_name = nullptr) {
    if (!type) return {};
    auto* get = method(type, getters);
    auto* set = method(type, setters, 1);
    if (get && set && !get->is_static() && !set->is_static() && get->get_return_type()) {
        const auto args = set->get_params();
        if (args.size() == 1 && args[0].t == static_cast<REFrameworkTypeDefinitionHandle>(*get->get_return_type())) {
            const auto kind = scalar(get->get_return_type());
            if (kind != Scalar::invalid) return {get, set, nullptr, kind};
        }
    }
    if (field_name)
        if (auto* field = type->find_field(field_name); field && !field->is_static())
            return {nullptr, nullptr, field, scalar(field->get_type())};
    return {};
}

std::optional<double> read(const Access& a, void* owner) {
    if (a.get) return read_method(a.get, owner, a.kind);
    std::optional<double> value;
    if (a.field) guarded([&] {
        value = number(a.kind, a.field->get_data_raw(owner));
        return true;
    });
    return value;
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
    void* owner{};
    double baseline{};
    std::optional<double> written;
    double retry{};
    unsigned failures{};

    void reconcile(void* current_owner, const Access& access, std::optional<double> value,
                   bool enable, double now) {
        if (!current_owner || !access || !value) return;
        if (owner != current_owner) *this = {};
        // A game or another mod may have changed the value. Only undo a
        // value we still own. The external value becomes the next baseline.
        if (written && !same(*written, *value)) *this = {};
        if (!enable && !written) return;
        if (now < retry || (enable && failures >= 3)) return;
        if (!owner || !written) {
            owner = current_owner;
            baseline = *value;
        }
    }

    void apply(const Access& access, double current, double target, bool enable, double now) {
        if (now < retry || (enable && failures >= 3) || !owner) return;
        if (same(current, target)) {
            if (!enable) *this = {};
            return;
        }
        const auto bits = pack(access.kind, target);
        if (!bits) return;
        const auto expect = number(access.kind, &*bits);
        if (!expect || same(current, *expect)) return;
        const bool changed = write(access, owner, target);
        const auto after = read(access, owner);
        if (after && same(*after, *expect)) {
            if (enable) {
                written = *after;
                failures = 0;
            } else *this = {};
        } else {
            if (after && !same(*after, baseline)) written = *after;
            else if (changed && !after) written = *expect;
            ++failures;
            retry = now + 8.0;
        }
    }
};

enum class Owner { managed, native, config, component };
struct Spec {
    const char* type;
    Owner owner;
    std::initializer_list<const char*> get, set;
    const char* field{};
};
const std::array<Spec, tweak_count> specs{{
    {"app.RenderingManager", Owner::managed, {"getSpeedTreeLODBias", "get_SpeedTreeLODBias", "get_SpeedTreeLodBias"},
        {"setSpeedTreeLODBias", "set_SpeedTreeLODBias"}, "_SpeedTreeLodBias"},
    {"app.LODProcessManager", Owner::managed, {}, {}, "MotionQuality"},
    {"via.render.RenderConfig", Owner::config, {"get_ContactShadowSetting", "get_ContactShadows"},
        {"set_ContactShadowSetting", "set_ContactShadows"}},
    {"via.landscape.FoliageManager", Owner::native, {}, {}},
    {"via.effect.EffectManager", Owner::native, {}, {}},
    {"via.render.RenderConfig", Owner::config, {"get_ShadowQuality"}, {"set_ShadowQuality"}},
    {"via.render.LightRenderer", Owner::native, {"get_ShadowResolution"}, {"set_ShadowResolution"}},
    {"via.render.SSAOControl", Owner::component, {"get_HalfResolution"}, {"set_HalfResolution"}},
    {"via.render.SSRControl", Owner::component, {"get_HalfResolution"}, {"set_HalfResolution"}}
}};

const API::TDB* tdb{};
std::array<Access, tweak_count> bindings{};
std::array<Change, tweak_count> changes{};
Baselines captured{}, snapshot_to_restore{};
std::array<Owner, tweak_count> owners{};
std::array<const char*, tweak_count> types{};
std::array<const char*, tweak_count> properties{};
std::array<double, tweak_count> discovery_retry{};
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

void* owner(Owner kind, const char* type, Object* camera_go) {
    auto& api = API::get();
    switch (kind) {
    case Owner::managed: return api->get_managed_singleton(type);
    case Owner::native: return api->get_native_singleton(type);
    case Owner::config:
        if (!render_config_get) render_config_get = method(tdb->find_type("via.render.Renderer"), {"get_RenderConfig"});
        return object_call(render_config_get, api->get_native_singleton("via.render.Renderer"));
    case Owner::component: return component(camera_go, type);
    }
    return nullptr;
}

bool contains(std::string_view p, std::string_view term) { return p.find(term) != p.npos; }

bool property_candidate(std::string_view p, bool foliage_property) {
    if (foliage_property) {
        if (contains(p, "shadow") || contains(p, "raytrac") || contains(p, "collision")) return false;
        return (contains(p, "grass") || contains(p, "foliage") || contains(p, "groundcover") ||
                contains(p, "vegetation")) &&
            (p.ends_with("culldistance") || p.ends_with("cullingdistance") ||
             p.ends_with("drawdistance") || p.ends_with("renderdistance") || p.ends_with("lodbias"));
    }
    return (contains(p, "effect") || contains(p, "particle")) &&
           (contains(p, "lod") || contains(p, "quality") || contains(p, "cull"));
}

bool discover(size_t index, Owner kind, const char* type, Object* go) {
    auto* obj = owner(kind, type, go);
    auto* definition = tdb->find_type(type);
    if (!obj || !definition) return false;
    for (auto* m : definition->get_methods()) {
        if (!m || !m->get_name()) continue;
        const std::string_view name{m->get_name()};
        if (!name.starts_with("get_")) continue;
        std::string property{name.substr(4)};
        std::transform(property.begin(), property.end(), property.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!property_candidate(property, index == foliage)) continue;
        const std::string setter = "set_" + std::string(name.substr(4));
        const auto a = accessor(definition, {name.data()}, {setter.c_str()});
        const auto baseline = a && a.kind != Scalar::boolean ? read(a, obj) : std::nullopt;
        if (!baseline || (index == foliage && (*baseline < 1 || *baseline > 20000))) continue;
        bindings[index] = a;
        properties[index] = m->get_name();
        owners[index] = kind;
        types[index] = type;
        return true;
    }
    return false;
}

void bind(size_t i, Object* go, double now) {
    if (bindings[i] || now < discovery_retry[i]) return;
    discovery_retry[i] = now + 8.0;
    auto& spec = specs[i];
    if (i == foliage) {
        discover(i, Owner::native, spec.type, go) || discover(i, Owner::native, "via.render.FoliageManager", go) ||
            discover(i, Owner::managed, "app.RenderingManager", go);
    } else if (i == effects) {
        discover(i, Owner::native, spec.type, go) || discover(i, Owner::config, "via.render.RenderConfig", go);
    } else {
        bindings[i] = accessor(tdb->find_type(spec.type), spec.get, spec.set, spec.field);
        owners[i] = spec.owner;
        types[i] = spec.type;
        if (bindings[i]) properties[i] = bindings[i].get ? bindings[i].get->get_name() : spec.field;
    }
}

double target(size_t i, const EngineSettings& e, double baseline) {
    switch (i) {
    case speedtree: return std::max(baseline, double(std::round(e.speedtree_bias)));
    case motion_quality: return std::min(baseline, double(std::round(e.motion_target)));
    case contact_shadows: return 2;
    case foliage: return baseline < 10 ? std::min(5.0, baseline + 1.5) :
        std::max(50.0, std::min(baseline, std::round(baseline * e.foliage_retention / 100.0)));
    case effects: return std::max(0.0, baseline - 1.0);
    case shadow_quality: return std::min(baseline, double(std::round(e.shadow_target)));
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

struct NpcBinding {
    const char* manager{};
    Method* list{};
    Field* list_field{};
    size_t cursor{};
    double retry{};
} npcs;

struct PositionBinding { Type* type{}; Method* method{}; };
std::array<PositionBinding, 16> position_bindings{};
struct ActorBinding { Type* type{}; Method* is_player{}, *is_pawn{}, *game_object{}; bool verified{}; };
std::array<ActorBinding, 16> actor_bindings{};
struct ListBinding { Type* type{}; Method* count{}, *item{}; Scalar count_kind{}; bool verified{}; } list_binding;
Type* motion_type{};
Access motion_rate{};

struct Throttled {
    Object* actor{}, *transform{}, *motion{};
    Access update_rate;
    Change value;
};
std::array<Throttled, 8> throttled;

Object* actor_list(NpcBinding& binding, double now, std::initializer_list<const char*> managers,
                   std::initializer_list<const char*> getters, std::initializer_list<const char*> fields) {
    if (!binding.manager && now >= binding.retry) {
        binding.retry = now + 8.0;
        for (const char* name : managers) {
            auto* type = tdb->find_type(name);
            if (!type) continue;
            if (!API::get()->get_managed_singleton(name)) continue;
            auto* get = method(type, getters);
            if (get && (get->is_static() || !get->get_return_type() || get->get_return_type()->is_valuetype())) get = nullptr;
            Field* field{};
            for (auto* candidate : fields) {
                field = type->find_field(candidate);
                if (field && !field->is_static() && field->get_type() && !field->get_type()->is_valuetype()) break;
                field = nullptr;
            }
            if (!get && !field) continue;
            binding.manager = name;
            binding.list = get;
            binding.list_field = field;
            break;
        }
    }
    if (!binding.manager) return nullptr;
    auto* m = API::get()->get_managed_singleton(binding.manager);
    if (binding.list) if (auto* list = object_call(binding.list, m)) return list;
    const auto* data = m && binding.list_field ? binding.list_field->get_data_raw(m) : nullptr;
    return data ? unpack<Object*>(data) : nullptr;
}

Object* npc_list(double now) {
    return actor_list(npcs, now, {"app.NPCManager", "app.NpcManager", "app.CharacterManager"},
        {"get_NpcList", "get_NPCList", "get_ActiveNpcList"}, {"_NpcList", "_NPCList"});
}

size_t list_size(Object* list, ListBinding& binding) {
    if (!list) return 0;
    auto* type = list->get_type_definition();
    if (binding.type != type) {
        binding = {type, method(type, {"get_Count"}), method(type, {"get_Item"}, 1)};
        if (binding.count && binding.item && !binding.count->is_static() && !binding.item->is_static()) {
            const auto args = binding.item->get_params();
            binding.count_kind = scalar(binding.count->get_return_type());
            binding.verified = args.size() == 1 && scalar(reinterpret_cast<Type*>(args[0].t)) == Scalar::i32 &&
                binding.item->get_return_type() && !binding.item->get_return_type()->is_valuetype();
        }
    }
    const auto n = binding.verified ? read_method(binding.count, list, binding.count_kind) : std::nullopt;
    return n && *n > 0 && *n <= 4096 ? size_t(*n) : 0;
}

bool vector_position(Object* transform, V3& value) {
    if (!transform) return false;
    auto* type = transform->get_type_definition();
    auto& binding = position_bindings[(reinterpret_cast<uintptr_t>(type) >> 4) % position_bindings.size()];
    if (binding.type != type) {
        binding = {type};
        if (auto* m = method(type, {"get_Position"}); m && m->get_return_type()) {
            const auto name = m->get_return_type()->get_full_name();
            if (name == "via.vec3" || name == "via.Vec3" || name == "via.Vector3") binding.method = m;
        }
    }
    reframework::InvokeRet out{};
    if (!invoke(binding.method, transform, out)) return false;
    value = unpack<V3>(out.bytes.data());
    return std::all_of(value.begin(), value.end(), [](float x) { return std::isfinite(x); });
}

bool behind(const Camera& camera, Object* transform, float dot, float distance) {
    V3 p{};
    if (!camera.valid || !vector_position(transform, p)) return false;
    for (size_t i = 0; i < p.size(); ++i) p[i] -= camera.position[i];
    const float length = std::hypot(p[0], p[1], p[2]);
    return length > distance &&
           (p[0] * camera.forward[0] + p[1] * camera.forward[1] + p[2] * camera.forward[2]) < length * dot;
}

void release(Throttled& entry) {
    entry.motion->release();
    entry.transform->release();
    entry.actor->release();
    entry = {};
}

void update_npcs(const EngineSettings& e, const Camera& camera, bool enable, double now) {
    // Check already-throttled actors before acquiring that frame's quarter.
    // This lets camera turns restore visible actors without waiting a cycle.
    for (auto& entry : throttled) if (entry.actor) {
        guarded([&] {
            if (enable && behind(camera, entry.transform, e.npc_dot + 0.10f, e.npc_distance))
                return true;
            const auto value = read(entry.update_rate, entry.motion);
            entry.value.reconcile(entry.motion, entry.update_rate, value, false, now);
            if (value && entry.value.written)
                entry.value.apply(entry.update_rate, *value, entry.value.baseline, false, now);
            if (!entry.value.written) release(entry);
            return true;
        });
    }
    if (!enable || !camera.valid) return;
    auto* list = npc_list(now);
    if (!list) return;
    const size_t length = list_size(list, list_binding);
    if (!length) return;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(50);
    for (size_t i = 0; i < (length + 3) / 4; ++i) {
        if (i && std::chrono::steady_clock::now() >= deadline) break;
        const auto index = npcs.cursor++ % length;
        auto free = std::find_if(throttled.begin(), throttled.end(), [](const auto& e) { return !e.actor; });
        if (free == throttled.end()) break;
        guarded([&] {
            auto* actor = object_call(list_binding.item, list, reinterpret_cast<void*>(index), true);
            if (!actor || std::any_of(throttled.begin(), throttled.end(),
                                      [&](const auto& e) { return e.actor == actor; })) return true;
            auto* def = actor->get_type_definition();
            auto& binding = actor_bindings[(reinterpret_cast<uintptr_t>(def) >> 4) % actor_bindings.size()];
            if (binding.type != def) {
                binding = {def, method(def, {"get_IsPlayer", "get_IsPlayerCharacter"}),
                    method(def, {"get_IsPawn", "get_IsPawnCharacter"}), method(def, {"get_GameObject"})};
                binding.verified = binding.is_player && binding.is_pawn && binding.game_object &&
                    scalar(binding.is_player->get_return_type()) == Scalar::boolean &&
                    scalar(binding.is_pawn->get_return_type()) == Scalar::boolean;
            }
            // Role checks must both resolve. A mixed list or an unidentified
            // actor cannot receive a motion change.
            if (!binding.verified || read_method(binding.is_player, actor, Scalar::boolean) != 0.0 ||
                read_method(binding.is_pawn, actor, Scalar::boolean) != 0.0) return true;
            auto* go = object_call(binding.game_object, actor);
            auto* transform = go ? object_call(method(go->get_type_definition(), {"get_Transform"}), go) : nullptr;
            if (!behind(camera, transform, e.npc_dot, e.npc_distance)) return true;
            auto* motion = component(go, "via.motion.Motion");
            if (!motion) return true;
            auto* motion_def = motion->get_type_definition();
            if (motion_def != motion_type) {
                motion_type = motion_def;
                motion_rate = accessor(motion_def, {"get_UpdateRate"}, {"set_UpdateRate"});
            }
            const auto access = motion_rate;
            // Only a verified floating-point motion update rate is eligible.
            if (access.kind != Scalar::f32) return true;
            const auto rate = read(access, motion);
            if (!rate || *rate <= 0.0 || *rate > 240.0) return true;
            state.npc_bindings = true;
            actor->add_ref();
            transform->add_ref();
            motion->add_ref();
            *free = {actor, transform, motion, access, {}};
            free->value.reconcile(motion, access, rate, true, now);
            free->value.apply(access, *rate, *rate * 0.5, true, now);
            if (!free->value.written) release(*free);
            return true;
        });
    }
}

// Rule changes can arrive on an engine thread. Defer all reads and writes to
// BeginRendering so a post-hook cannot race baseline restoration.
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
    Object* owner{};
    CapValues baseline{}, written{};
    bool owned{}, ready{}, initialized{};
} caps;
CrowdController crowd;
Method* gui_pause{};
double next_caps{}, next_crowd_bind{}, last_crowd_frame{};
unsigned crowd_frames{};
bool crowd_running{}, rule_hook_checked{};
std::vector<Method*> hooked_rules;

Access field_access(Type* type, const char* name) {
    auto* f = type ? type->find_field(name) : nullptr;
    return f && !f->is_static() ? Access{nullptr, nullptr, f, scalar(f->get_type())} : Access{};
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
    if (!enable && !caps.owned && !caps.initialized) return;
    const bool dirty = rules_changed.exchange(false, std::memory_order_acq_rel);
    if (!dirty && now < next_caps) return;
    next_caps = now + 0.5;
    if (!caps.ready) return;
    auto* owner = API::get()->get_managed_singleton(generate_manager);
    if (!owner) return;
    if (caps.owner != owner) {
        caps.owner = owner;
        caps.owned = caps.initialized = false;
    }
    const auto readback = read_caps(owner);
    state.crowd.caps_ready = readback.has_value();
    if (!readback) return;
    const auto& current = *readback;
    if (!caps.owned) caps.baseline = current;
    else for (size_t i = 0; i < current.size(); ++i)
        if (current[i] != caps.written[i]) caps.baseline[i] = current[i];
    if (enable && !caps.initialized) {
        crowd.set_limits(s, caps.baseline[2]);
        caps.initialized = true;
    }
    state.crowd.vanilla_npc = caps.baseline[0];
    state.crowd.vanilla_pawn = caps.baseline[2];
    auto target = caps.baseline;
    if (enable && s.npc) target[0] = target[1] = s.manual_npc_limit ? s.manual_npc_limit :
        std::clamp(crowd.npc, s.npc_min, s.npc_max);
    if (enable && s.pawn) target[2] = std::clamp(crowd.pawn, s.pawn_min, s.pawn_max);
    if (enable && (s.npc || s.pawn))
        target[3] = int(std::min<int64_t>(int64_t(target[0]) + target[2], INT32_MAX));
    state.crowd.cap_write_failed = !commit_caps(owner, current, target);
    if (state.crowd.cap_write_failed) { next_caps = now + 8.0; return; }
    state.crowd.npc = target[0];
    state.crowd.pawn = target[2];
    caps.written = target;
    caps.owned = target != caps.baseline;
    if (!enable) caps.initialized = false;
}

struct HeldObject {
    Object* ptr{};
    HeldObject() = default;
    HeldObject(const HeldObject&) = delete;
    HeldObject& operator=(const HeldObject&) = delete;
    HeldObject(HeldObject&& other) noexcept : ptr(std::exchange(other.ptr, nullptr)) {}
    HeldObject& operator=(HeldObject&& other) noexcept {
        if (this != &other) { reset(); ptr = std::exchange(other.ptr, nullptr); }
        return *this;
    }
    ~HeldObject() { reset(); }
    void reset(Object* obj = nullptr) {
        if (ptr == obj) return;
        if (ptr) guarded([&] { ptr->release(); return true; });
        ptr = nullptr;
        if (obj) guarded([&] { obj->add_ref(); ptr = obj; return true; });
    }
};

constexpr unsigned entity_kind_count = 4;
struct EntityBinding {
    Type* type{};
    Method* distance{}, *game_object{};
    Access distance_field;
    Scalar distance_kind{};
    std::array<Method*, 3> lod{};
    std::array<Field*, 3> lod_fields{};
};
struct Part {
    HeldObject target;
    Access access;
    Change change;
    bool looked_up{};
};
struct ScopedEntity {
    HeldObject actor;
    std::array<Part, entity_kind_count> parts;
    unsigned seen{};
};
std::array<EntityBinding, 32> entity_bindings{};
struct PartBinding { Type* type{}; Access access; };
std::array<std::array<PartBinding, 16>, entity_kind_count> part_bindings{};
std::vector<ScopedEntity> entities;
std::array<NpcBinding, 3> distance_lists{};
std::array<ListBinding, 3> distance_list_types{};
std::array<Object*, 3> list_identities{};
unsigned sweep{}, source_index{}, list_index{};
size_t restore_cursor{};
double last_sweep{};
bool scanning{};
Status::Crowd sweep_status;

Object* distance_list(unsigned source, double now) {
    switch (source) {
    case 0: return actor_list(distance_lists[0], now, {"app.EnemyManager"}, {"get_EnemyList"}, {"_EnemyList"});
    case 1: return npc_list(now);
    default: return actor_list(distance_lists[2], now, {"app.PawnManager", "app.CharacterManager"},
        {"get_StrayPawnList", "get_ActiveStrayPawnList"}, {"_StrayPawnList", "_ActiveStrayPawnList"});
    }
}

EntityBinding& entity_binding(Object* actor) {
    auto* type = actor->get_type_definition();
    auto& b = entity_bindings[(reinterpret_cast<uintptr_t>(type) >> 4) % entity_bindings.size()];
    if (!type || b.type == type) return b;
    b = {};
    b.type = type;
    b.distance = method(type, {"get_DistanceSqFromPlayer", "getDistanceSqFromPlayer"});
    if (b.distance && !b.distance->is_static()) b.distance_kind = scalar(b.distance->get_return_type());
    b.distance_field = field_access(type, "<DistanceSqFromPlayer>k__BackingField");
    if (!b.distance_field) b.distance_field = field_access(type, "_DistanceSqFromPlayer");
    b.game_object = method(type, {"get_GameObject"});
    constexpr const char* names[]{"FacialLOD", "JointConstraintsLOD", "UpperBodyTransformerLOD"};
    for (size_t i = 0; i < b.lod.size(); ++i) {
        const std::string name = names[i];
        auto* getter = method(type, {("get_" + name).c_str()});
        if (getter && !getter->is_static() && getter->get_return_type() && !getter->get_return_type()->is_valuetype())
            b.lod[i] = getter;
        for (const std::string& field_name : {name, "<" + name + ">k__BackingField"}) {
            auto* f = type->find_field(field_name.c_str());
            if (f && !f->is_static() && f->get_type() && !f->get_type()->is_valuetype()) {
                b.lod_fields[i] = f;
                break;
            }
        }
    }
    return b;
}

Access lod_access(Type* type, unsigned part) {
    if (!type) return {};
    if (part == 3) {
        auto a = accessor(type, {"get_FreezeRate", "getFreezeRate"}, {"set_FreezeRate", "setFreezeRate"});
        return a.kind == Scalar::f32 ? a : Access{};
    }
    const auto getters = part == 0 ? std::initializer_list<const char*>{"get_IsEnableFacial", "get_EnableFacial", "isEnableFacial"} :
        std::initializer_list<const char*>{"get_IsEnable", "get_Enable", "getIsEnable", "isEnable"};
    const auto setters = part == 0 ? std::initializer_list<const char*>{"setEnableFacial"} :
        std::initializer_list<const char*>{"setEnable"};
    const auto a = accessor(type, getters, setters);
    if (a.kind == Scalar::boolean) return a;
    // Some RE Engine builds expose a setter and a backing boolean only.
    // The field is used for readback. The engine setter still owns writes.
    auto* set = method(type, setters, 1);
    if (!set || set->is_static()) return {};
    const auto params = set->get_params();
    if (params.size() != 1 || scalar(reinterpret_cast<Type*>(params[0].t)) != Scalar::boolean) return {};
    const auto fields = part == 0 ? std::initializer_list<const char*>{"IsEnableFacial", "_IsEnableFacial", "<IsEnableFacial>k__BackingField"} :
        std::initializer_list<const char*>{"IsEnable", "_IsEnable", "<IsEnable>k__BackingField"};
    for (auto* name : fields) {
        const auto f = field_access(type, name);
        if (f.kind == Scalar::boolean) return {nullptr, set, f.field, f.kind};
    }
    return {};
}

void update_part(Part& part, std::optional<double> target, double now) {
    if (!part.target.ptr || !part.access || (!target && !part.change.written)) return;
    const auto value = read(part.access, part.target.ptr);
    part.change.reconcile(part.target.ptr, part.access, value, target.has_value(), now);
    if (value && (target || part.change.written))
        part.change.apply(part.access, *value, target.value_or(part.change.baseline), target.has_value(), now);
}

void visit_entity(Object* actor, const CrowdSettings& settings, double now) {
    if (!actor) return;
    auto found = std::find_if(entities.begin(), entities.end(), [&](const auto& e) { return e.actor.ptr == actor; });
    if (found != entities.end() && found->seen == sweep) return;
    const auto& b = entity_binding(actor);
    if (!b.type) return;
    auto distance = read_method(b.distance, actor, b.distance_kind);
    if (!distance) distance = read(b.distance_field, actor);
    if (!distance || *distance < 0.0) return;
    ++sweep_status.processed;
    const float dsq = float(*distance);
    const float freeze = settings.chain ? chain_freeze(dsq, settings) : 0.0f;
    const std::array targets{settings.lod && dsq > settings.facial_distance * settings.facial_distance,
                             settings.lod && dsq > settings.joint_distance * settings.joint_distance,
                             settings.lod && dsq > settings.upper_distance * settings.upper_distance,
                             freeze > 0.0f};
    if (found == entities.end()) {
        if (entities.size() >= 256 || std::none_of(targets.begin(), targets.end(), [](bool x) { return x; })) return;
        entities.emplace_back();
        found = std::prev(entities.end());
        found->actor.reset(actor);
        if (!found->actor.ptr) { entities.pop_back(); return; }
    }
    found->seen = sweep;
    for (unsigned i = 0; i < found->parts.size(); ++i) {
        auto& part = found->parts[i];
        if (targets[i] && !part.looked_up) {
            Object* object = nullptr;
            if (i == 3) object = component(object_call(b.game_object, actor), "via.motion.Chain");
            else {
                object = object_call(b.lod[i], actor);
                if (!object && b.lod_fields[i]) {
                    if (const auto* raw = b.lod_fields[i]->get_data_raw(actor)) object = unpack<Object*>(raw);
                }
            }
            if (object) {
                auto* type = object->get_type_definition();
                auto& by_type = part_bindings[i];
                auto& binding = by_type[(reinterpret_cast<uintptr_t>(type) >> 4) % by_type.size()];
                if (binding.type != type) binding = {type, lod_access(type, i)};
                part.access = binding.access;
                if (part.access) part.target.reset(object);
            }
            part.looked_up = true;
        }
        if (part.target.ptr) sweep_status.bound_lods |= 1u << i;
        const auto target = targets[i] ? std::optional<double>{i == 3 ? freeze : 0.0} : std::nullopt;
        update_part(part, target, now);
        if (!part.change.written) continue;
        if (i == 0) ++sweep_status.facial;
        if (i == 1) ++sweep_status.joint;
        if (i == 2) ++sweep_status.upper;
        if (i == 3 && *part.change.written >= 1.0) ++sweep_status.frozen;
        if (i == 3 && *part.change.written < 1.0) ++sweep_status.partial;
    }
}

void update_entities(const CrowdSettings& s, bool enable, double now) {
    if (!enable || (!s.lod && !s.chain)) scanning = false;
    if (!scanning && entities.empty() && (!enable || (!s.lod && !s.chain))) {
        state.crowd.processed = state.crowd.facial = state.crowd.joint = state.crowd.upper = 0;
        state.crowd.partial = state.crowd.frozen = 0;
        return;
    }
    if (enable && (s.lod || s.chain) && !scanning && crowd_frames >= unsigned(s.lod_interval)) {
        crowd_frames = 0;
        scanning = true;
        source_index = list_index = 0;
        list_identities = {};
        ++sweep;
        sweep_status = {};
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(150);
    unsigned work = 0;
    while (scanning && source_index < distance_lists.size() && work < 16 && std::chrono::steady_clock::now() < deadline) {
        const auto source = source_index;
        auto* list = distance_list(source, now);
        auto& binding = distance_list_types[source];
        const auto size = list_size(list, binding);
        if (list && binding.verified) sweep_status.list_sources |= 1u << source;
        if (list != list_identities[source]) { list_index = 0; list_identities[source] = list; }
        while (list_index < size && work++ < 16) {
            guarded([&] {
                visit_entity(object_call(binding.item, list, reinterpret_cast<void*>(uintptr_t(list_index)), true), s, now);
                return true;
            });
            ++list_index;
            if (std::chrono::steady_clock::now() >= deadline) break;
        }
        if (list_index < size) break;
        ++source_index;
        list_index = 0;
    }
    if (scanning && source_index == distance_lists.size()) {
        scanning = false;
        last_sweep = now;
        auto& live = state.crowd;
        live.processed = sweep_status.processed;
        live.facial = sweep_status.facial;
        live.joint = sweep_status.joint;
        live.upper = sweep_status.upper;
        live.partial = sweep_status.partial;
        live.frozen = sweep_status.frozen;
        live.bound_lods = sweep_status.bound_lods;
        live.list_sources = sweep_status.list_sources;
    }
    for (; !entities.empty() && work++ < 16 && std::chrono::steady_clock::now() < deadline;) {
        restore_cursor %= entities.size();
        auto& entry = entities[restore_cursor];
        const bool expired = !enable || (!s.lod && !s.chain) || (last_sweep > 0 && now - last_sweep > 60) ||
                             (!scanning && entry.seen != sweep);
        for (unsigned i = 0; i < entry.parts.size(); ++i)
            if (expired || (i == 3 ? !s.chain : !s.lod)) update_part(entry.parts[i], {}, now);
        if (expired && std::none_of(entry.parts.begin(), entry.parts.end(), [](const auto& p) { return p.change.written.has_value(); })) {
            if (restore_cursor + 1 < entities.size()) entry = std::move(entities.back());
            entities.pop_back();
        } else ++restore_cursor;
    }
    if (!enable || (!s.lod && !s.chain)) {
        state.crowd.processed = state.crowd.facial = state.crowd.joint = state.crowd.upper = 0;
        state.crowd.partial = state.crowd.frozen = 0;
    }
}

void update_crowd(const CrowdSettings& s, bool enable, double now) {
    if (!enable && !caps.owned && !caps.initialized && entities.empty()) {
        if (crowd_running) { crowd = {}; crowd_frames = 0; crowd_running = false; }
        state.crowd.paused = state.crowd.restoring = state.crowd.owned = false;
        return;
    }
    bind_crowd(now, enable && (s.npc || s.pawn));
    const auto paused = enable ? read_method(gui_pause, API::get()->get_managed_singleton("app.GuiManager"), Scalar::boolean) :
        std::optional<double>{0.0};
    if (crowd_running != enable) {
        crowd = {};
        if (enable && caps.initialized) crowd.set_limits(s, caps.baseline[2]);
        crowd_frames = 0;
        next_caps = 0;
        last_crowd_frame = 0;
        crowd_running = enable;
    }
    state.crowd.paused = paused != 0.0;
    state.crowd.pause_available = paused.has_value();
    if (state.crowd.paused) {
        last_crowd_frame = 0;
        crowd.headroom_seconds = 0;
        return;
    }
    const bool caps_enabled = enable && (s.npc || s.pawn);
    const double dt = last_crowd_frame > 0 ? now - last_crowd_frame : 0;
    last_crowd_frame = now;
    if (enable) {
        const auto adjustments = crowd.adjustments;
        crowd.observe(dt, s);
        if (crowd.adjustments != adjustments) next_caps = 0;
        if (crowd.count >= 20) ++crowd_frames;
    }
    update_caps(s, caps_enabled, now);
    update_entities(s, enable && crowd.count >= 20, now);
    auto& live = state.crowd;
    live.fps = crowd.fps;
    live.average = crowd.average;
    live.minimum = crowd.minimum;
    live.maximum = crowd.maximum;
    live.adjustments = crowd.adjustments;
    live.mode = crowd.mode;
    live.owned = caps.owned || !entities.empty();
    live.restoring = !enable && live.owned;
}
} // namespace

bool position(Object* transform, V3& value) { return vector_position(transform, value); }

void update(const EngineSettings& settings, const Camera& camera, bool motion, bool combat, bool blocked, double now) {
    const auto* current_tdb = API::get()->tdb();
    if (!current_tdb || !API::get()->get_vm_context()) return;
    if (current_tdb != tdb) {
        if (tdb) {
            changes = {};
            for (auto& entry : throttled) if (entry.actor) guarded([&] { release(entry); return true; });
            entities.clear();
            caps = {};
        }
        invalidate();
        tdb = current_tdb;
    }
    if (settings.enabled && !blocked && settings.trigger_night) poll_clock(settings, now);
    const bool active = settings.enabled && !blocked && !state.editing &&
        (settings.always || settings.force_night || (settings.trigger_motion && motion) || (settings.trigger_combat && combat) ||
         (settings.trigger_night && state.night));
    if (now >= next_update || active != previous_active) {
        previous_active = active;
        next_update = now + 1.0;
        for (size_t i = 0; i < bindings.size(); ++i) {
            auto& change = changes[i];
            const bool on = active && settings.tweaks[i] && (i < shadow_quality || settings.heavy) &&
                            (i != effects || !settings.effects_combat_only || combat);
            if (!on && !change.written && (blocked || !snapshot_to_restore[i])) continue;
            guarded([&] {
                bind(i, camera.game_object, now);
                if (!bindings[i]) return false;
                const auto& access = bindings[i];
                auto* obj = owner(owners[i], types[i], camera.game_object);
                const auto value = obj ? read(access, obj) : std::nullopt;
                change.reconcile(obj, access, value, on, now);
                if (on && value && change.owner) captured[i] = change.baseline;
                if (value && (on || change.written))
                    change.apply(access, *value, on ? target(i, settings, change.baseline) : change.baseline, on, now);
                if (obj && value && !blocked && !change.written && snapshot_to_restore[i]) {
                    const auto expected = *snapshot_to_restore[i];
                    const auto latest = read(access, obj);
                    const auto after = latest && same(*latest, expected) ? latest :
                        (write(access, obj, expected) ? read(access, obj) : std::nullopt);
                    if (after && same(*after, expected)) snapshot_to_restore[i].reset();
                }
                return true;
            });
        }
    }
    update_npcs(settings, camera, settings.npc && !blocked && !state.editing, now);
    update_crowd(settings.crowd, settings.crowd.enabled && !blocked && !state.editing, now);
    state.active_tweaks = int(std::count_if(changes.begin(), changes.end(), [](const auto& x) { return x.written.has_value(); }));
    state.camera_tweaks_active = changes[ssao_half].written.has_value() || changes[ssr_half].written.has_value();
    state.throttled_npcs = int(std::count_if(throttled.begin(), throttled.end(), [](const auto& x) { return x.actor != nullptr; }));
    state.pending_snapshot = int(std::count_if(snapshot_to_restore.begin(), snapshot_to_restore.end(),
        [](const auto& x) { return x.has_value(); }));
    state.properties = properties;
    for (size_t i = 0; i < changes.size(); ++i) {
        const auto& change = changes[i];
        state.tweak_status[i] = change.failures ? "Write/readback failed" : change.written ? "Applied" :
            bindings[i] ? "Ready" : "Binding unavailable";
    }
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
    if (!state.crowd.rule_hook) rule_hook_checked = false;
    for (auto& entity : entities) for (auto& part : entity.parts) {
        if (!part.target.ptr) part.looked_up = false;
        part.change.retry = 0;
        part.change.failures = 0;
    }
    for (auto& change : changes) { change.retry = 0; change.failures = 0; }
}
void invalidate() {
    bindings = {};
    properties = {};
    render_config_get = clock_hour = clock_minute = nullptr;
    npcs = {};
    position_bindings = {};
    actor_bindings = {};
    list_binding = {};
    motion_type = nullptr;
    motion_rate = {};
    shadow_sizes.clear();
    gui_pause = nullptr;
    rule_hook_checked = false;
    caps.ready = false;
    distance_lists = {};
    distance_list_types = {};
    entity_bindings = {};
    part_bindings = {};
    scanning = false;
    state.clock_valid = state.night = state.npc_bindings = false;
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
