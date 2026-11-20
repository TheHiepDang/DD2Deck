#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace dd2::graphics {

enum class BlurPassKind : uint8_t { dispatch, draw, draw_indexed, indirect_dispatch };

struct BlurProbeKey {
    uint64_t shader{};
    uint32_t x{}, y{}, z{};
    uint32_t width{}, height{}, format{};
    int list_type{};
    BlurPassKind kind{};
    bool operator==(const BlurProbeKey&) const = default;
};

struct BlurProbeSample { BlurProbeKey key; uint64_t calls{}; };
struct BlurProbeReport {
    std::array<BlurProbeSample, 512> samples{};
    uint64_t overflow{};
};

// A short, opt-in discovery capture. Shader IDs are candidates for comparing
// native blur OFF/ON; these CPU observations do not establish GPU pass order,
// velocity bindings, resource states, or permission to inject.
class BlurProbe {
    std::atomic<bool> enabled_{};
    std::mutex mutex_;
    BlurProbeReport report_{};
public:
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

    void begin() {
        std::lock_guard lock{mutex_};
        report_ = {};
        enabled_.store(true, std::memory_order_relaxed);
    }

    BlurProbeReport finish() {
        std::lock_guard lock{mutex_};
        enabled_.store(false, std::memory_order_relaxed);
        return report_;
    }

    void record(const BlurProbeKey& key) {
        std::lock_guard lock{mutex_};
        if (!enabled()) return;
        for (auto& sample : report_.samples) {
            if (!sample.calls) sample.key = key;
            if (sample.key == key) {
                ++sample.calls;
                return;
            }
        }
        ++report_.overflow;
    }
};

} // namespace dd2::graphics
