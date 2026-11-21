#include "graphics.hpp"
#include "audit.hpp"

#include <MinHook.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <bit>
#include <condition_variable>
#include <memory>
#include <optional>
#include <mutex>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace dd2::graphics {
namespace {
using Microsoft::WRL::ComPtr;
using List = ID3D12GraphicsCommandList;
using List5 = ID3D12GraphicsCommandList5;
using Handle = D3D12_CPU_DESCRIPTOR_HANDLE;

constexpr D3D12_SHADING_RATE_COMBINER plugin_combiners[]{
    D3D12_SHADING_RATE_COMBINER_PASSTHROUGH, D3D12_SHADING_RATE_COMBINER_MAX};
constexpr GUID pipeline_key{0xe9dbfa29, 0xf65d, 0x4427, {0xa0, 0x04, 0x01, 0xe4, 0x4f, 0xae, 0x65, 0xf2}};
constexpr GUID signature_key{0xe91e7f96, 0xee3a, 0x46f0, {0xab, 0xe6, 0x33, 0x43, 0xd4, 0x86, 0x03, 0xcd}};
constexpr GUID allocator_images_key{0x704e9989, 0xf271, 0x4e37, {0xb9, 0x94, 0x74, 0x3c, 0xa4, 0x91, 0x38, 0x45}};

constexpr GUID list_state_key{0x8a4b2c1e, 0x91d3, 0x4f7a, {0xb2, 0x19, 0x6c, 0x44, 0x8e, 0x10, 0x5d, 0x31}};

struct ImageKey {
    Extent extent{};
    ShadingEllipse zone{};
    int profile_mode{};
    bool operator==(const ImageKey&) const = default;
};
static_assert(rate_1x1 == D3D12_SHADING_RATE_1X1 && rate_1x2 == D3D12_SHADING_RATE_1X2 &&
              rate_2x1 == D3D12_SHADING_RATE_2X1 && rate_2x2 == D3D12_SHADING_RATE_2X2);

struct Images : std::enable_shared_from_this<Images> {
    ImageKey key;
    ComPtr<ID3D12Resource> texture;
};
// Closed lists may execute after a new mask is published. The allocator owns
// the references until its next successful Reset or destruction.
class RecordedImages final : public IUnknown {
    std::atomic<ULONG> refs_{1};
public:
    std::vector<std::shared_ptr<const Images>> images;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        *object = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --refs_;
        if (!remaining) delete this;
        return remaining;
    }
};

struct Upload {
    std::shared_ptr<Images> images;
    ComPtr<ID3D12Resource> staging;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<List> list;
    ComPtr<ID3D12Fence> fence;
    bool submitted{};
};
struct View {
    Extent extent{};
    DXGI_FORMAT format{};
    Diagnostic rejection{Diagnostic::count};
    bool read_only_depth{};
    ID3D12Resource* resource{};
};
enum class TargetPass { none, opaque, transparent, output };
// Descriptor creation/copy serializes writers. Stable nodes allow OM recording
// threads to read a coherent value without taking the writer mutex.
class Views {
    struct Entry {
        SIZE_T address{};
        Entry* next{};
        std::atomic<uint64_t> sequence{}, extent{};
        std::atomic<unsigned> format{}, attributes{unsigned(Diagnostic::count)};
        std::atomic<ID3D12Resource*> resource{};
    };
    std::array<std::atomic<Entry*>, 4096> buckets_{};
    size_t size_{};

    static size_t bucket(SIZE_T address) {
        return ((address >> 4) ^ (address >> 16)) & 4095;
    }
    Entry* find(SIZE_T address) const {
        for (auto* entry = buckets_[bucket(address)].load(std::memory_order_acquire); entry; entry = entry->next)
            if (entry->address == address) return entry;
        return nullptr;
    }

public:
    std::optional<View> get(SIZE_T address) const {
        const auto* entry = find(address);
        if (!entry) return {};
        const auto before = entry->sequence.load();
        if (before & 1) return View{{}, DXGI_FORMAT_UNKNOWN, Diagnostic::om_view_layout};
        const auto extent = entry->extent.load();
        const auto format = static_cast<DXGI_FORMAT>(entry->format.load());
        const auto attributes = entry->attributes.load();
        auto* resource = entry->resource.load();
        if (entry->sequence.load() != before)
            return View{{}, DXGI_FORMAT_UNKNOWN, Diagnostic::om_view_layout};
        const auto rejection = static_cast<Diagnostic>(attributes & 0xff);
        if (rejection == Diagnostic::count) return {};
        return View{{uint32_t(extent >> 32), uint32_t(extent)}, format, rejection,
                    (attributes & 0x100) != 0, resource};
    }

    void set(SIZE_T address, const View& view) {
        auto* entry = find(address);
        const bool fresh = !entry;
        if (fresh) {
            const size_t limit = view.rejection == Diagnostic::om_accepted ? 65536 : 32768;
            if (view.rejection == Diagnostic::count || size_ >= limit) return;
            entry = new Entry;
            entry->address = address;
            entry->next = buckets_[bucket(address)].load(std::memory_order_relaxed);
            ++size_;
        }
        // Sequentially consistent fields keep the sequence check ordered on
        // both sides of the descriptor snapshot.
        entry->sequence.fetch_add(1);
        entry->extent.store(uint64_t(view.extent.width) << 32 | view.extent.height);
        entry->format.store(unsigned(view.format));
        entry->attributes.store(unsigned(view.rejection) | (view.read_only_depth ? 0x100 : 0));
        entry->resource.store(view.resource);
        entry->sequence.fetch_add(1);
        if (fresh) buckets_[bucket(address)].store(entry, std::memory_order_release);
    }
};
static_assert(size_t(Diagnostic::count) <= 0xff);

struct BindingDiagnostic {
    bool claimed{};
    BindingSample sample{};
};
constexpr unsigned override_gate_closed = 1u << 31;

struct ListState {
    unsigned generation{};
    List5* vrs{};
    ID3D12CommandAllocator* allocator{};
    const Images* recorded_images{};
    bool audit_list{};
    int list_type{-1};
    PipelineInfo pipeline{};
    AuditBinding audit_targets{};
    Extent extent{};
    TargetPass target_pass{};
    D3D12_VIEWPORT viewport{};
    bool viewport_fallback{};
    bool render_pass{};
    D3D12_SHADING_RATE game_rate{D3D12_SHADING_RATE_1X1};
    std::array<D3D12_SHADING_RATE_COMBINER, 2> game_combiners{};
    ID3D12Resource* game_image{};
    ID3D12Resource* bound_image{};
    bool native_engine_rate{}, native_engine_image{};
    bool vrs_overridden{};
};
void release_vrs_override(ListState& state);

class ListStateObject final : public IUnknown {
    std::atomic<ULONG> refs_{1};
public:
    std::shared_ptr<ListState> state = std::make_shared<ListState>();
    ~ListStateObject() { release_vrs_override(*state); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        *object = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --refs_;
        if (!remaining) delete this;
        return remaining;
    }
};
struct Runtime {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    UINT tier{}, tile{}, rtv_stride{}, dsv_stride{};
    bool additional{};
    std::atomic<const char*> message{"Waiting for the D3D12 device and direct queue"};
    std::atomic<bool> installed{}, disabled{}, shade_transparent{true}, allow_native{true};
    std::atomic<bool> paused{};
    std::mutex hook_toggle_mutex;
    std::vector<void*> hot_hooks;
    std::atomic<bool> hot_hooks_active{}, hot_hooks_out_of_sync{};
    std::atomic<unsigned> disable_countdown{}, list_generation{1};
    // The high bit blocks new overrides. The low bits count open command
    // lists that still need to restore their game-owned VRS state.
    std::atomic<unsigned> recording_overrides{};
    std::atomic<int> preset{};
    std::atomic<uint64_t> output{}, scene{};
    std::atomic<uint64_t> candidates{}, shaded{}, unknown_targets{}, indirect_with_image{};
    Extent stable_extent{};
    unsigned stable_frames{};
    uint64_t frame_candidates{}, frame_shaded{}, frame_targets{}, frame_indirect{};
    std::array<std::atomic<uint64_t>, size_t(Diagnostic::count)> counts{};
    Audit audit;
    std::atomic_uint32_t signature_id{}, signature_epoch{};

    std::mutex view_writers_mutex;
    Views rtvs, dsvs;
    std::array<std::atomic<ID3D12Resource*>, 16> backbuffers{};
    std::array<std::atomic<uint64_t>, 32> scene_observations{};
    std::mutex binding_samples_mutex;
    std::array<BindingDiagnostic, Diagnostics::binding_sample_count> binding_samples{};

    std::mutex worker_mutex;
    std::condition_variable worker_cv;
    ImageKey wanted{};
    std::shared_ptr<const Images> published_images;
    // Compared on the draw path. Only dereference an allocator-retained image.
    std::atomic<const Images*> images{};
    std::atomic<unsigned> image_readers{};
};

// Hooks and SDK callbacks have process lifetime. The loader has no unload callback.
Runtime& rt = *new Runtime;

bool claim_vrs_override(ListState& state) {
    auto count = rt.recording_overrides.load(std::memory_order_relaxed);
    do {
        if (count & override_gate_closed) return false;
        if (state.vrs_overridden) return true;
    } while (!rt.recording_overrides.compare_exchange_weak(count, count + 1, std::memory_order_relaxed));
    state.vrs_overridden = true;
    return true;
}

void release_vrs_override(ListState& state) {
    if (std::exchange(state.vrs_overridden, false))
        rt.recording_overrides.fetch_sub(1, std::memory_order_release);
}
// Debug/runtime wrappers can call a second hooked entry from the first. The
// audit counts the outer call while preserving the existing detour behavior.
class AuditCall {
    static inline thread_local bool active_{};
    bool owned_{};
public:
    AuditCall() {
        if (active_) rt.audit.nested_callback();
        else owned_ = active_ = true;
    }
    ~AuditCall() { if (owned_) active_ = false; }
    bool collect() const { return owned_; }
};

void count(Diagnostic reason) {
    if (rt.audit.enabled.load(std::memory_order_relaxed))
        rt.counts[size_t(reason)].fetch_add(1, std::memory_order_relaxed);
}

template<class T> void* slot(T* object, Dd2Slot method) {
    return (*reinterpret_cast<void***>(object))[dd2_slots[method]];
}

void* allocator_reset_slot(ID3D12CommandAllocator* allocator) {
    return (*reinterpret_cast<void***>(allocator))[dd2_allocator_reset_slot];
}

uint64_t pack(Extent size) { return uint64_t(size.width) << 32 | size.height; }
Extent unpack(uint64_t bits) { return {uint32_t(bits >> 32), uint32_t(bits)}; }

bool scene_extent_candidate(Extent size, Extent output, bool allow_native) {
    return output.height && size.width >= 400 && size.height >= 240 && size.height <= output.height &&
        (size.width < output.width || (allow_native && size == output));
}

// Store extent and both counters together so a frame can drain a slot while
// other threads are still recording. Extents are validated to fit 13 bits.
constexpr uint64_t extent_mask = (uint64_t{1} << 26) - 1;
constexpr uint64_t count_mask = (uint64_t{1} << 19) - 1;
constexpr unsigned total_shift = 26;
constexpr unsigned mrt_shift = 45;

void observe_scene(Extent size, UINT render_targets) {
    const uint64_t extent = (uint64_t(size.width) << 13) | size.height;
    for (auto& observation : rt.scene_observations) {
        auto seen = observation.load(std::memory_order_relaxed);
        while (!seen || (seen & extent_mask) == extent) {
            const auto total = std::min(((seen >> total_shift) & count_mask) + 1, count_mask);
            const auto mrt = std::min((seen >> mrt_shift) + (render_targets > 1), count_mask);
            const auto value = extent | (total << total_shift) | (mrt << mrt_shift);
            if (observation.compare_exchange_weak(seen, value, std::memory_order_relaxed)) return;
        }
    }
}

Extent observation_extent(uint64_t value) {
    return {uint32_t((value & extent_mask) >> 13), uint32_t(value & 8191)};
}

void reset_scene_extent() {
    rt.scene.store(0, std::memory_order_relaxed);
    for (auto& observation : rt.scene_observations) observation.store(0, std::memory_order_relaxed);
    rt.stable_extent = {};
    rt.stable_frames = 0;
}

Extent update_scene_extent(Extent output, bool allow_native) {
    const auto packed_output = pack(output);
    const bool changed_native = allow_native != rt.allow_native.exchange(allow_native, std::memory_order_relaxed);
    if (packed_output != rt.output.load(std::memory_order_relaxed) || changed_native) {
        reset_scene_extent();
        rt.output.store(packed_output, std::memory_order_relaxed);
        return {};
    }
    const auto screen_like = [&](Extent e) {
        if (!e.height || !output.width || !output.height) return false;
        const float ratio = float(e.width) * output.height / (float(e.height) * output.width);
        return ratio >= 0.87f && ratio <= 1.15f;
    };
    const auto rank = [&](uint64_t sample) {
        const auto size = observation_extent(sample);
        const auto mrt = sample >> mrt_shift;
        return std::tuple{mrt != 0, screen_like(size), mrt, (sample >> total_shift) & count_mask,
                          uint64_t(size.width) * size.height};
    };
    uint64_t best{};
    for (auto& observation : rt.scene_observations) {
        const auto sample = observation.exchange(0, std::memory_order_relaxed);
        if (rank(best) < rank(sample)) best = sample;
    }
    const auto observed = observation_extent(best);
    if (scene_extent_candidate(observed, output, allow_native)) {
        if (observed == rt.stable_extent) rt.stable_frames = std::min(rt.stable_frames + 1, 4u);
        else {
            rt.stable_extent = observed;
            rt.stable_frames = 1;
        }
        if (rt.stable_frames >= 4) rt.scene.store(pack(observed), std::memory_order_relaxed);
    }
    return unpack(rt.scene.load(std::memory_order_relaxed));
}

D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CreationNodeMask = properties.VisibleNodeMask = 1;
    return properties;
}

bool upload_images(Upload& work, ImageKey key) {
    auto& device = rt.device;
    work.images = std::make_shared<Images>();
    work.images->key = key;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&work.allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, work.allocator.Get(),
                                       nullptr, IID_PPV_ARGS(&work.list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&work.fence)))) return false;

    const auto default_heap = heap(D3D12_HEAP_TYPE_DEFAULT);
    const auto upload_heap = heap(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = (key.extent.width + rt.tile - 1) / rt.tile;
    desc.Height = (key.extent.height + rt.tile - 1) / rt.tile;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8_UINT;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 size = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &size);
    if (!size || size == UINT64_MAX) return false;

    D3D12_RESOURCE_DESC upload{};
    upload.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload.Width = size;
    upload.Height = upload.DepthOrArraySize = upload.MipLevels = 1;
    upload.SampleDesc.Count = 1;
    upload.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    auto& image = work.images->texture;
    auto& staging = work.staging;
    if (FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&image))) ||
        FAILED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload,
                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging)))) return false;
    void* bytes{};
    const D3D12_RANGE no_read{0, 0};
    if (FAILED(staging->Map(0, &no_read, &bytes)) || !bytes) return false;
    auto* first_row = static_cast<uint8_t*>(bytes) + footprint.Offset;
    for (UINT y = 0; y < desc.Height; ++y) {
        auto* row = first_row + size_t(y) * footprint.Footprint.RowPitch;
        for (UINT x = 0; x < desc.Width; ++x)
            row[x] = rate_at(key.extent, rt.tile, x, y, key.zone, rt.additional);
    }
    staging->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = image.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = staging.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;
    work.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {image.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE};
    work.list->ResourceBarrier(1, &barrier);
    if (FAILED(work.list->Close()) || rt.disabled.load()) return false;
    ID3D12CommandList* lists[]{work.list.Get()};
    work.submitted = true;
    rt.queue->ExecuteCommandLists(1, lists);
    return SUCCEEDED(rt.queue->Signal(work.fence.Get(), 1));
}

bool upload_completed(ID3D12Fence* fence) {
    return guarded([&] {
        const auto completed = fence->GetCompletedValue();
        return completed >= 1 && completed != UINT64_MAX;
    });
}

bool wait_for_upload(ID3D12Fence* fence, HANDLE& pending_event) {
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return false;
    HRESULT registration{};
    if (!guarded([&] { registration = fence->SetEventOnCompletion(1, event); return true; })) {
        // A fault can occur after the runtime has accepted the registration.
        pending_event = event;
        return false;
    }
    if (SUCCEEDED(registration) && WaitForSingleObject(event, 5000) != WAIT_OBJECT_0) {
        // VKD3D-Proton can still signal the registered handle after a timeout.
        pending_event = event;
        return false;
    }
    CloseHandle(event);
    return SUCCEEDED(registration) && upload_completed(fence);
}

void publish_images(std::shared_ptr<const Images> images) {
    std::lock_guard lock{rt.worker_mutex};
    if (images && (rt.disabled.load() || images->key != rt.wanted)) return;
    auto previous = std::exchange(rt.published_images, std::move(images));
    rt.images.store(rt.published_images.get());
    // A recorder may still be taking a shared reference to the old raw pointer.
    while (previous && rt.image_readers.load()) std::this_thread::yield();
}

void image_worker() {
    std::array<std::shared_ptr<const Images>, 4> cache;
    size_t next_slot{};
    try {
        ImageKey previous{};
        for (;;) {
            ImageKey key{};
            {
                std::unique_lock lock{rt.worker_mutex};
                rt.worker_cv.wait(lock, [&] {
                    return rt.disabled.load() || (!rt.paused.load() && !(rt.wanted == previous));
                });
                if (rt.disabled.load()) break;
                key = rt.wanted;
                if (rt.worker_cv.wait_for(lock, std::chrono::milliseconds(200), [&] {
                    return rt.disabled.load() || rt.paused.load() || !(rt.wanted == key);
                }))
                    continue;
            }
            previous = key;
            const auto found = std::find_if(cache.begin(), cache.end(), [&](const auto& p) { return p && p->key == key; });
            if (found != cache.end()) {
                publish_images(*found);
                rt.message.store("Ready");
                continue;
            }

            auto work = std::make_unique<Upload>();
            HANDLE pending_event{};
            if (!guarded([&] { return upload_images(*work, key); }) ||
                !wait_for_upload(work->fence.Get(), pending_event)) {
                publish_images(nullptr);
                rt.message.store("VRS image upload failed");
                rt.disabled.store(true);
                // The detached worker can drain the fence after reporting failure.
                cache = {};
                if (pending_event && WaitForSingleObject(pending_event, INFINITE) == WAIT_OBJECT_0)
                    CloseHandle(pending_event);
                if (work->submitted && !upload_completed(work->fence.Get())) work.release();
                break;
            }
            auto& entry = cache[next_slot++ % cache.size()];
            entry = std::move(work->images);
            if (!rt.disabled.load()) {
                publish_images(entry);
                rt.message.store("Ready");
            }
        }
    } catch (...) {
        rt.disabled.store(true);
        publish_images(nullptr);
        rt.message.store("Could not allocate VRS image resources");
    }
}

template<class D> View view_info(ID3D12Resource* resource, const D* desc) {
    View view{};
    view.rejection = Diagnostic::om_view_layout;
    if (!resource) return view;
    view.resource = resource;
    const auto shape = resource->GetDesc();
    view.format = desc && desc->Format != DXGI_FORMAT_UNKNOWN ? desc->Format : shape.Format;
    view.extent = {static_cast<uint32_t>(std::min<UINT64>(shape.Width, UINT32_MAX)), shape.Height};
    if (shape.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || shape.DepthOrArraySize != 1 ||
        shape.SampleDesc.Count != 1 || shape.Width > 7680 || shape.Height > 4320) return view;
    if (desc) {
        if constexpr (std::is_same_v<D, D3D12_RENDER_TARGET_VIEW_DESC>) {
            if (desc->ViewDimension != D3D12_RTV_DIMENSION_TEXTURE2D || desc->Texture2D.MipSlice != 0 ||
                desc->Texture2D.PlaneSlice != 0) return view;
        } else if (desc->ViewDimension != D3D12_DSV_DIMENSION_TEXTURE2D || desc->Texture2D.MipSlice != 0) {
            return view;
        }
    }
    const auto format = view.format;
    if constexpr (std::is_same_v<D, D3D12_RENDER_TARGET_VIEW_DESC>) {
        view.rejection = Diagnostic::om_accepted;
    } else {
        view.read_only_depth = desc && (desc->Flags & D3D12_DSV_FLAG_READ_ONLY_DEPTH) != 0;
        const bool supported = format == DXGI_FORMAT_D32_FLOAT || format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
            format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || format == DXGI_FORMAT_D16_UNORM;
        view.rejection = supported ? Diagnostic::om_accepted : Diagnostic::om_depth_format;
    }
    return view;
}

template<class D> void record_view(ID3D12Device* device, ID3D12Resource* resource, const D* desc,
                                    Handle handle, Views& views) noexcept {
    if (device != rt.device.Get() || rt.disabled.load()) return;
    View view{};
    guarded([&] { view = view_info(resource, desc); return true; });
    try {
        std::unique_lock lock{rt.view_writers_mutex};
        views.set(handle.ptr, view);
    } catch (...) { rt.disabled.store(true); }
}

using RtvFn = void (STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, Handle);
using DsvFn = void (STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, Handle);
using CopyFn = void (STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const Handle*, const UINT*,
                                          UINT, const Handle*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
using CopySimpleFn = void (STDMETHODCALLTYPE*)(ID3D12Device*, UINT, Handle, Handle, D3D12_DESCRIPTOR_HEAP_TYPE);
RtvFn orig_rtv{};
DsvFn orig_dsv{};
CopyFn orig_copy{};
CopySimpleFn orig_copy_simple{};
void STDMETHODCALLTYPE hook_rtv(ID3D12Device* device, ID3D12Resource* resource,
                                 const D3D12_RENDER_TARGET_VIEW_DESC* desc, Handle dst) {
    orig_rtv(device, resource, desc, dst);
    if (device == rt.device.Get()) count(Diagnostic::rtv_created);
    record_view(device, resource, desc, dst, rt.rtvs);
}
void STDMETHODCALLTYPE hook_dsv(ID3D12Device* device, ID3D12Resource* resource,
                                 const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, Handle dst) {
    orig_dsv(device, resource, desc, dst);
    if (device == rt.device.Get()) count(Diagnostic::dsv_created);
    record_view(device, resource, desc, dst, rt.dsvs);
}

void copy_views(ID3D12Device* device, UINT dest_count, const Handle* dest, const UINT* dest_sizes,
                UINT src_count, const Handle* src, const UINT* src_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type) noexcept {
    if (device != rt.device.Get() || rt.disabled.load() || !dest || !src ||
        (type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV && type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV)) return;
    auto& views = type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV ? rt.rtvs : rt.dsvs;
    const SIZE_T stride = type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV ? rt.rtv_stride : rt.dsv_stride;
    try {
        std::unique_lock lock{rt.view_writers_mutex};
        if (!guarded([&] {
            UINT index{}, offset{};
            for (UINT d = 0; d < dest_count; ++d) {
                const UINT count = dest_sizes ? dest_sizes[d] : 1;
                for (UINT j = 0; j < count; ++j) {
                    while (index < src_count && offset == (src_sizes ? src_sizes[index] : 1)) {
                        ++index;
                        offset = 0;
                    }
                    View view{};
                    if (index < src_count) {
                        view = views.get(src[index].ptr + stride * offset++).value_or(View{});
                    }
                    views.set(dest[d].ptr + stride * j, view);
                }
            }
            return true;
        })) rt.disabled.store(true);
    } catch (...) { rt.disabled.store(true); }
}

void STDMETHODCALLTYPE hook_copy(ID3D12Device* d, UINT dn, const Handle* dst, const UINT* ds,
                                  UINT sn, const Handle* src, const UINT* ss, D3D12_DESCRIPTOR_HEAP_TYPE t) {
    orig_copy(d, dn, dst, ds, sn, src, ss, t);
    copy_views(d, dn, dst, ds, sn, src, ss, t);
}
void STDMETHODCALLTYPE hook_copy_simple(ID3D12Device* d, UINT n, Handle dst, Handle src,
                                         D3D12_DESCRIPTOR_HEAP_TYPE t) {
    orig_copy_simple(d, n, dst, src, t);
    copy_views(d, 1, &dst, &n, 1, &src, &n, t);
}

using CreateGraphicsFn = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*,
                                                      REFIID, void**);
using CreateComputeFn = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*,
                                                     REFIID, void**);
using CreateStreamFn = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*,
                                                    REFIID, void**);
using CreateSignatureFn = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_SIGNATURE_DESC*,
                                                       ID3D12RootSignature*, REFIID, void**);
CreateGraphicsFn orig_create_graphics{};
CreateComputeFn orig_create_compute{};
CreateStreamFn orig_create_stream{};
CreateSignatureFn orig_create_signature{};

template<class D> void tag_pipeline(const D* desc, void** output) {
    if (!desc || !output || rt.disabled.load()) return;
    guarded([&] {
        if (!*output) return true;
        const auto info = classify(*desc);
        ComPtr<ID3D12PipelineState> pipeline;
        if (FAILED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&pipeline))) ||
            FAILED(pipeline->SetPrivateData(pipeline_key, sizeof(info), &info))) return false;
        count(Diagnostic::pso_created);
        return true;
    });
}

HRESULT STDMETHODCALLTYPE hook_create_graphics(ID3D12Device* d, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                                REFIID iid, void** output) {
    const HRESULT hr = orig_create_graphics(d, desc, iid, output);
    if (SUCCEEDED(hr) && d == rt.device.Get()) tag_pipeline(desc, output);
    return hr;
}
HRESULT STDMETHODCALLTYPE hook_create_compute(ID3D12Device* d, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                               REFIID iid, void** output) {
    const HRESULT hr = orig_create_compute(d, desc, iid, output);
    if (SUCCEEDED(hr) && d == rt.device.Get()) tag_pipeline(desc, output);
    return hr;
}
HRESULT STDMETHODCALLTYPE hook_create_stream(ID3D12Device2* d, const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
                                              REFIID iid, void** output) {
    const HRESULT hr = orig_create_stream(d, desc, iid, output);
    if (SUCCEEDED(hr) && static_cast<ID3D12Device*>(d) == rt.device.Get()) tag_pipeline(desc, output);
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_create_signature(ID3D12Device* d, const D3D12_COMMAND_SIGNATURE_DESC* desc,
                                                 ID3D12RootSignature* root, REFIID iid, void** output) {
    const HRESULT hr = orig_create_signature(d, desc, root, iid, output);
    if (FAILED(hr) || d != rt.device.Get() || !desc || !output || rt.disabled.load()) return hr;
    const bool tagged = guarded([&] {
        if (!*output) return true;
        ComPtr<ID3D12CommandSignature> signature;
        if (FAILED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&signature)))) return false;
        auto info = describe_signature(*desc);
        info.id = rt.signature_id.fetch_add(1, std::memory_order_relaxed) + 1;
        if (FAILED(signature->SetPrivateData(signature_key, sizeof(info), &info))) return false;
        rt.audit.tagged(info);
        return true;
    });
    // Invalidate cached tags and misses when a newly created object may reuse an old address.
    rt.signature_epoch.fetch_add(1, std::memory_order_release);
    if (!tagged) rt.audit.tag_failed();
    return hr;
}

SignatureInfo signature_info(ID3D12CommandSignature* signature) {
    if (!signature) return {};
    struct Entry {
        ID3D12CommandSignature* signature{};
        SignatureInfo info{};
    };
    struct Cache {
        uint32_t epoch{};
        std::array<Entry, 64> entries{};
    };
    thread_local Cache cache{};
    const auto epoch = rt.signature_epoch.load(std::memory_order_acquire);
    if (cache.epoch != epoch) {
        cache.epoch = epoch;
        cache.entries = {};
    }
    auto& entry = cache.entries[(reinterpret_cast<uintptr_t>(signature) >> 4) % cache.entries.size()];
    if (entry.signature == signature) return entry.info;
    SignatureInfo info{};
    UINT size = sizeof(info);
    if (!guarded([&] { return SUCCEEDED(signature->GetPrivateData(signature_key, &size, &info)); }) ||
        size != sizeof(info) || size_t(info.kind) >= size_t(IndirectKind::count)) info = {};
    entry = {signature, info};
    return info;
}

using RateFn = void (STDMETHODCALLTYPE*)(List5*, D3D12_SHADING_RATE, const D3D12_SHADING_RATE_COMBINER*);
using ImageFn = void (STDMETHODCALLTYPE*)(List5*, ID3D12Resource*);
RateFn orig_rate{};
ImageFn orig_image{};
void* rate_entry{};
void* image_entry{};
void* allocator_reset_entry{};
thread_local bool forwarding_vrs{};

struct VrsForwardScope {
    bool previous{std::exchange(forwarding_vrs, true)};
    ~VrsForwardScope() { forwarding_vrs = previous; }
};

void native_rate(List5* list, D3D12_SHADING_RATE rate, const D3D12_SHADING_RATE_COMBINER* combiners) {
    const VrsForwardScope forward;
    orig_rate(list, rate, combiners);
}

void native_image(List5* list, ID3D12Resource* image) {
    const VrsForwardScope forward;
    orig_image(list, image);
}

struct ListCacheEntry {
    List* list{};
    ListState* state{};
    std::weak_ptr<ListState> lifetime;
};
thread_local std::array<ListCacheEntry, 32> list_cache;

ListCacheEntry& list_cache_slot(List* list) {
    return list_cache[(reinterpret_cast<uintptr_t>(list) >> 4) & (list_cache.size() - 1)];
}

ListState* list_state(List* list, bool create = false) noexcept {
    auto& slot = list_cache_slot(list);
    const auto generation = rt.list_generation.load(std::memory_order_acquire);
    // D3D12 serializes recording of a given list. The weak reference detects
    // destruction or a close on another recording thread, even if the list
    // address has since been reused.
    if (slot.list == list && !slot.lifetime.expired())
        return create || slot.state->generation == generation ? slot.state : nullptr;
    try {
        ComPtr<IUnknown> stored;
        UINT size = sizeof(IUnknown*);
        if (FAILED(list->GetPrivateData(list_state_key, &size, stored.GetAddressOf())) || !stored) {
            if (!create || rt.disabled.load(std::memory_order_relaxed)) return nullptr;
            ComPtr<ListStateObject> fresh;
            fresh.Attach(new ListStateObject);
            if (FAILED(list->SetPrivateDataInterface(list_state_key, fresh.Get()))) return nullptr;
            stored = fresh;
        } else if (size != sizeof(IUnknown*)) return nullptr;
        const auto& state = static_cast<ListStateObject*>(stored.Get())->state;
        slot = {list, state.get(), state};
        return create || state->generation == generation ? slot.state : nullptr;
    } catch (...) {
        rt.disabled.store(true);
        return nullptr;
    }
}

void remember_pipeline(ListState& state, ID3D12PipelineState* pipeline) {
    state.pipeline = {};
    if (!pipeline) return;
    PipelineInfo info{};
    UINT size = sizeof(info);
    guarded([&] {
        const auto result = pipeline->GetPrivateData(pipeline_key, &size, &info);
        if (SUCCEEDED(result) && size == sizeof(info)) state.pipeline = info;
        return true;
    });
}

void initialize_list(List* list, ListState& state, ID3D12CommandAllocator* allocator,
                     ID3D12PipelineState* pipeline) {
    const auto generation = state.generation;
    release_vrs_override(state);
    state = {};
    state.generation = generation;
    if (!guarded([&] {
        state.list_type = list->GetType();
        if (state.list_type != D3D12_COMMAND_LIST_TYPE_DIRECT &&
            state.list_type != D3D12_COMMAND_LIST_TYPE_COMPUTE) return false;
        ComPtr<ID3D12Device> device;
        if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))) || device.Get() != rt.device.Get()) return false;
        state.audit_list = true;
        if (state.list_type != D3D12_COMMAND_LIST_TYPE_DIRECT) return true;
        ComPtr<List5> vrs;
        if (FAILED(list->QueryInterface(IID_PPV_ARGS(&vrs))) || static_cast<List*>(vrs.Get()) != list ||
            slot(vrs.Get(), Slot_RSSetShadingRate) != rate_entry ||
            slot(vrs.Get(), Slot_RSSetShadingRateImage) != image_entry) return true;
        if (!allocator || allocator_reset_slot(allocator) != allocator_reset_entry) return true;
        state.allocator = allocator;
        state.vrs = vrs.Get();
        return true;
    })) return;
    remember_pipeline(state, pipeline);
}

bool restore_game_vrs(ListState& state) {
    if (!state.vrs_overridden) return true;
    state.bound_image = nullptr;
    if (!guarded([&] {
        native_image(state.vrs, state.game_image);
        native_rate(state.vrs, state.game_rate, state.game_combiners.data());
        return true;
    })) {
        rt.disabled.store(true);
        return false;
    }
    state.native_engine_image = state.game_image != nullptr;
    state.native_engine_rate = state.game_rate != D3D12_SHADING_RATE_1X1 ||
        state.game_combiners[0] != D3D12_SHADING_RATE_COMBINER_PASSTHROUGH ||
        state.game_combiners[1] != D3D12_SHADING_RATE_COMBINER_PASSTHROUGH;
    release_vrs_override(state);
    return true;
}

void unbind_vrs(ListState& state, Diagnostic reason = Diagnostic::count, bool clear_engine = false) {
    if (rt.paused.load(std::memory_order_relaxed)) {
        restore_game_vrs(state);
        return;
    }
    if (!state.bound_image && !(clear_engine && (state.native_engine_rate || state.native_engine_image))) return;
    if (!claim_vrs_override(state)) {
        restore_game_vrs(state);
        return;
    }
    state.bound_image = nullptr;
    state.native_engine_rate = state.native_engine_image = false;
    if (!guarded([&] {
        native_image(state.vrs, nullptr);
        native_rate(state.vrs, D3D12_SHADING_RATE_1X1, nullptr);
        return true;
    })) rt.disabled.store(true);
    if (reason != Diagnostic::count) count(reason);
}

Diagnostic pipeline_diagnostic(const ListState& state) {
    if (state.pipeline.rejection != Diagnostic::eligible) return state.pipeline.rejection;
    if (state.pipeline.kind != PipelineKind::graphics) return Diagnostic::draw_stream;
    const auto& targets = state.audit_targets;
    if (state.pipeline.color_count != targets.color_count || state.pipeline.depth_format != targets.depth.format)
        return Diagnostic::draw_pso_format_mismatch;
    for (size_t i = 0; i < state.pipeline.color_count; ++i)
        if (state.pipeline.colors[i] != targets.colors[i].format) return Diagnostic::draw_pso_format_mismatch;
    return Diagnostic::eligible;
}

bool scene_viewport(const D3D12_VIEWPORT& viewport, Extent scene) {
    return viewport.TopLeftX == 0.0f && viewport.TopLeftY == 0.0f &&
        viewport.Width == static_cast<float>(scene.width) && viewport.Height == static_cast<float>(scene.height);
}

Diagnostic coverage_gate(const ListState* state) {
    if (!state || !state->vrs || state->list_type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return Diagnostic::draw_untracked_list;
    if (state->render_pass) return Diagnostic::draw_render_pass;
    if (!state->extent.width) return Diagnostic::draw_targets;
    if (pack(state->extent) != rt.scene.load(std::memory_order_relaxed)) return Diagnostic::draw_other_scene_extent;
    if (state->viewport_fallback && !scene_viewport(state->viewport, state->extent)) return Diagnostic::draw_other_scene_extent;
    if (state->target_pass == TargetPass::transparent && !rt.shade_transparent.load(std::memory_order_relaxed))
        return Diagnostic::draw_targets;
    // Cached PSOs and unfamiliar graphics streams may lack usable metadata.
    // The tracked internal scene and the command hooks establish coverage.
    return state->pipeline.kind == PipelineKind::compute ? Diagnostic::draw_stream : Diagnostic::eligible;
}

bool retain_recorded(ID3D12CommandAllocator* allocator, const std::shared_ptr<const Images>& selected) {
    return guarded([&] {
        ComPtr<IUnknown> stored;
        UINT size = sizeof(IUnknown*);
        const auto result = allocator->GetPrivateData(allocator_images_key, &size, stored.GetAddressOf());
        if (FAILED(result) || !stored) {
            ComPtr<RecordedImages> fresh;
            fresh.Attach(new RecordedImages);
            fresh->images.push_back(selected);
            return SUCCEEDED(allocator->SetPrivateDataInterface(allocator_images_key, fresh.Get()));
        }
        if (size != sizeof(IUnknown*)) return false;
        auto& recorded = static_cast<RecordedImages*>(stored.Get())->images;
        if (std::find(recorded.begin(), recorded.end(), selected) == recorded.end()) recorded.push_back(selected);
        return true;
    });
}

bool retain_current_images(ListState& state, int profile_mode) noexcept {
    try {
        // Pair these operations with publish_images' store and reader check.
        rt.image_readers.fetch_add(1);
        const auto* published = rt.images.load();
        auto selected = published ? published->weak_from_this().lock() : std::shared_ptr<const Images>{};
        rt.image_readers.fetch_sub(1);
        if (!selected || selected->key.extent != state.extent || selected->key.profile_mode != profile_mode)
            return false;
        if (!retain_recorded(state.allocator, selected)) return false;
        state.recorded_images = selected.get();
        return true;
    } catch (...) {
        return false;
    }
}

bool refresh_scene_vrs(ListState& state) {
    const auto reject = [&] { unbind_vrs(state); return false; };
    if (rt.paused.load(std::memory_order_relaxed) || rt.disabled.load(std::memory_order_relaxed) ||
        coverage_gate(&state) != Diagnostic::eligible)
        return reject();
    const int preset = rt.preset.load(std::memory_order_relaxed);
    if (!preset) return reject();
    const int profile_mode = preset - 1;
    const auto* published = rt.images.load(std::memory_order_acquire);
    if (!published || (published != state.recorded_images && !retain_current_images(state, profile_mode)))
        return reject();
    const auto& key = state.recorded_images->key;
    if (key.extent != state.extent || key.profile_mode != profile_mode) return reject();
    auto* image = state.recorded_images->texture.Get();
    if (!image) return reject();
    if (image == state.bound_image) return true;
    if (!claim_vrs_override(state)) return reject();
    const bool initial = state.bound_image == nullptr;
    state.bound_image = image;
    if (guarded([&] {
        if (initial) native_rate(state.vrs, D3D12_SHADING_RATE_1X1, plugin_combiners);
        native_image(state.vrs, image);
        return true;
    })) return true;
    unbind_vrs(state);
    rt.disabled.store(true);
    return false;
}

Gate audit_gate(Diagnostic coverage, const ListState* state) {
    switch (coverage) {
    case Diagnostic::draw_untracked_list: return Gate::untracked_list;
    case Diagnostic::draw_render_pass: return Gate::render_pass;
    case Diagnostic::draw_targets: return binding_gate(state->audit_targets);
    case Diagnostic::draw_other_scene_extent: return Gate::other_scene_extent;
    case Diagnostic::draw_indirect_signature: return Gate::indirect_signature;
    case Diagnostic::eligible: return Gate::eligible;
    default: return Gate::pipeline;
    }
}

AuditVrs vrs_state(const ListState& state) {
    return {unsigned(state.game_rate), unsigned(state.game_combiners[0]), unsigned(state.game_combiners[1]),
        state.game_image != nullptr};
}

void audit_draw(size_t path, const ListState* state, Diagnostic coverage,
                SignatureInfo signature = {}, UINT max_commands = 0, bool has_count = false) {
    if (!rt.audit.enabled.load(std::memory_order_relaxed)) return;
    const auto kind = state ? state->pipeline.kind : PipelineKind::unknown;
    const int list_type = state ? state->list_type : -1;
    rt.audit.call(path, kind, list_type, max_commands, has_count, state && state->bound_image);
    const auto gate = coverage == Diagnostic::count ? Gate::signature_only : audit_gate(coverage, state);
    if (gate != Gate::signature_only) rt.audit.verdict(path, gate);
    const auto game_vrs = state ? vrs_state(*state) : AuditVrs{};
    const bool engine_vrs = gate == Gate::eligible &&
        (game_vrs.rate || game_vrs.primitive || game_vrs.image_combiner || game_vrs.image_bound);
    if (engine_vrs) rt.audit.vrs(path, game_vrs, pipeline_diagnostic(*state) == Diagnostic::eligible);
    rt.audit.sample(path, engine_vrs ? Gate::engine_vrs : gate, [&] {
        AuditSample sample{};
        sample.path = path;
        sample.gate = gate;
        sample.signature = signature;
        sample.max_commands = max_commands;
        sample.count_buffer = has_count;
        sample.list_type = list_type;
        const auto mask = rt.preset.load(std::memory_order_relaxed);
        sample.preset = mask;
        if (!state) return sample;
        sample.bindings = state->audit_targets;
        sample.viewport = state->viewport;
        sample.pipeline = state->pipeline;
        sample.pipeline_gate = state->extent.width ? pipeline_diagnostic(*state) : state->pipeline.rejection;
        sample.vrs = game_vrs;
        {
            std::lock_guard lock{rt.worker_mutex};
            const auto& images = rt.published_images;
            sample.plugin_image_ready = images && state->extent.width && images->key.extent == state->extent &&
                (!mask || images->key.profile_mode == sample.preset - 1) && images->texture;
        }
        sample.plugin_image_bound = state->bound_image != nullptr;
        return sample;
    });
}

void before_indexed_draw(ListState& state, UINT index_count, bool collect_audit) {
    if (rt.paused.load(std::memory_order_relaxed) || rt.disabled.load(std::memory_order_relaxed) || !index_count) {
        unbind_vrs(state);
        return;
    }
    const bool bound = refresh_scene_vrs(state);
    if (!collect_audit) return;
    const auto coverage = coverage_gate(&state);
    audit_draw(0, &state, coverage);
    if (coverage != Diagnostic::eligible) return count(coverage);
    rt.candidates.fetch_add(1, std::memory_order_relaxed);
    count(Diagnostic::eligible);
    if (!bound) return count(rt.preset.load(std::memory_order_relaxed) ?
        Diagnostic::draw_image_unready : Diagnostic::draw_preset_off);
    rt.shaded.fetch_add(1, std::memory_order_relaxed);
    count(Diagnostic::shaded);
}

using ResetFn = HRESULT (STDMETHODCALLTYPE*)(List*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using AllocatorResetFn = HRESULT (STDMETHODCALLTYPE*)(ID3D12CommandAllocator*);
using CloseFn = HRESULT (STDMETHODCALLTYPE*)(List*);
using PsoFn = void (STDMETHODCALLTYPE*)(List*, ID3D12PipelineState*);
using OmFn = void (STDMETHODCALLTYPE*)(List*, UINT, const Handle*, BOOL, const Handle*);
using ViewportsFn = void (STDMETHODCALLTYPE*)(List*, UINT, const D3D12_VIEWPORT*);
using IndexedFn = void (STDMETHODCALLTYPE*)(List*, UINT, UINT, UINT, INT, UINT);
using DrawFn = void (STDMETHODCALLTYPE*)(List*, UINT, UINT, UINT, UINT);
using DispatchFn = void (STDMETHODCALLTYPE*)(List*, UINT, UINT, UINT);
using IndirectFn = void (STDMETHODCALLTYPE*)(List*, ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64,
                                           ID3D12Resource*, UINT64);
using BundleFn = void (STDMETHODCALLTYPE*)(List*, List*);
using BeginPassFn = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*, UINT,
    const D3D12_RENDER_PASS_RENDER_TARGET_DESC*, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*, D3D12_RENDER_PASS_FLAGS);
using EndPassFn = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*);
using MeshFn = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList6*, UINT, UINT, UINT);
using RayPipelineFn = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*, ID3D12StateObject*);
using RaysFn = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*, const D3D12_DISPATCH_RAYS_DESC*);

ResetFn orig_reset{};
AllocatorResetFn orig_allocator_reset{};
CloseFn orig_close{};
PsoFn orig_clear{}, orig_pso{};
OmFn orig_om{};
ViewportsFn orig_viewports{};
IndexedFn orig_indexed{};
DrawFn orig_draw{};
DispatchFn orig_dispatch{};
IndirectFn orig_indirect{};
BundleFn orig_bundle{};
BeginPassFn orig_begin_pass{};
EndPassFn orig_end_pass{};
MeshFn orig_mesh{};
RayPipelineFn orig_ray_pipeline{};
RaysFn orig_rays{};

HRESULT STDMETHODCALLTYPE hook_reset(List* l, ID3D12CommandAllocator* a, ID3D12PipelineState* p) {
    const auto generation = rt.list_generation.load(std::memory_order_acquire);
    const HRESULT hr = orig_reset(l, a, p);
    if (SUCCEEDED(hr)) {
        ListState initial;
        initial.generation = generation;
        if (!rt.disabled.load(std::memory_order_relaxed) && !rt.paused.load(std::memory_order_relaxed) &&
            generation == rt.list_generation.load(std::memory_order_acquire))
            initialize_list(l, initial, a, p);
        if (auto* state = list_state(l, initial.audit_list)) {
            release_vrs_override(*state);
            *state = initial;
        }
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE hook_allocator_reset(ID3D12CommandAllocator* allocator) {
    const HRESULT hr = orig_allocator_reset(allocator);
    if (SUCCEEDED(hr)) guarded([&] {
        allocator->SetPrivateDataInterface(allocator_images_key, nullptr);
        return true;
    });
    return hr;
}
HRESULT STDMETHODCALLTYPE hook_close(List* l) {
    auto* state = list_state(l);
    if (state) unbind_vrs(*state);
    const HRESULT result = orig_close(l);
    if (state && SUCCEEDED(result)) {
        release_vrs_override(*state);
        *state = {};
        auto& slot = list_cache_slot(l);
        if (slot.list == l) slot = {};
        l->SetPrivateDataInterface(list_state_key, nullptr);
    }
    return result;
}
void STDMETHODCALLTYPE hook_clear(List* l, ID3D12PipelineState* p) {
    auto* s = list_state(l);
    if (s) unbind_vrs(*s, Diagnostic::unbind_pso);
    orig_clear(l, p);
    if (s) {
        auto* allocator = s->allocator;
        initialize_list(l, *s, allocator, p);
    }
}
void STDMETHODCALLTYPE hook_pso(List* l, ID3D12PipelineState* p) {
    if (auto* s = list_state(l); s && s->audit_list) {
        remember_pipeline(*s, p);
        if (rt.paused.load(std::memory_order_relaxed) || rt.disabled.load(std::memory_order_relaxed) ||
            coverage_gate(s) != Diagnostic::eligible)
            unbind_vrs(*s, Diagnostic::unbind_pso);
    }
    orig_pso(l, p);
}

void STDMETHODCALLTYPE hook_viewports(List* l, UINT n, const D3D12_VIEWPORT* v) {
    orig_viewports(l, n, v);
    if (auto* s = list_state(l); s && s->vrs) {
        s->viewport = {};
        if (n == 1 && v) s->viewport = *v;
        if (rt.paused.load(std::memory_order_relaxed) ||
            (s->viewport_fallback && !scene_viewport(s->viewport, s->extent))) unbind_vrs(*s);
    }
}

AuditView audit_view(const View* view) {
    return view ? AuditView{view->extent, view->format, true} : AuditView{};
}

bool track_binding(ListState& state, UINT count_rtv, const Handle* rtv, BOOL contiguous, const Handle* dsv,
                   bool possible_scene, bool capture) {
    auto& audit = state.audit_targets;
    std::array<Handle, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> color{};
    Handle depth{};
    const size_t n = rtv ? std::min<size_t>(count_rtv, color.size()) : 0;
    for (size_t i = 0; i < n; ++i)
        color[i] = contiguous ? Handle{rtv->ptr + rt.rtv_stride * i} : rtv[i];
    if (dsv) depth = *dsv;
    std::array<const View*, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> colors{};
    const View* depth_view{};
    std::array<std::optional<View>, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> color_views{};
    for (size_t i = 0; i < n; ++i) {
        color_views[i] = rt.rtvs.get(color[i].ptr);
        if (color_views[i]) colors[i] = &*color_views[i];
        if (capture) audit.colors[i] = audit_view(colors[i]);
    }
    const auto tracked_depth = dsv ? rt.dsvs.get(depth.ptr) : std::nullopt;
    if (tracked_depth) depth_view = &*tracked_depth;
    if (capture) audit.depth = audit_view(depth_view);
    const auto output = unpack(rt.output.load(std::memory_order_relaxed));
    const auto end = colors.begin() + n;
    if (std::any_of(colors.begin(), end, [&](const View* view) {
        return view && view->resource && std::any_of(rt.backbuffers.begin(), rt.backbuffers.end(),
            [&](const auto& buffer) { return buffer.load(std::memory_order_relaxed) == view->resource; });
    })) {
        state.target_pass = TargetPass::output;
        count(Diagnostic::om_output_excluded);
        return false;
    }
    if (!possible_scene) return false;
    const auto reject = [&](Diagnostic reason, const View* color_view) {
        if (!capture) return;
        if (audit.reason == Diagnostic::count) audit.reason = reason;
        count(reason);
        auto& binding = rt.binding_samples[size_t(reason) - size_t(Diagnostic::om_missing_rtv)];
        std::lock_guard lock{rt.binding_samples_mutex};
        if (binding.claimed) return;
        binding.claimed = true;
        binding.sample = {reason, color_view ? color_view->extent : Extent{}, audit.depth.extent,
            color_view ? color_view->format : DXGI_FORMAT_UNKNOWN, audit.depth.format,
            color_view != nullptr, depth_view != nullptr};
    };
    const auto bad_color = std::find_if(colors.begin(), end, [](const View* view) {
        return !view || view->rejection != Diagnostic::om_accepted;
    });
    if (bad_color != end || (dsv && (!depth_view || depth_view->rejection != Diagnostic::om_accepted))) {
        const auto scene = unpack(rt.scene.load(std::memory_order_relaxed));
        const bool allow_native = rt.allow_native.load(std::memory_order_relaxed);
        // Pre-hook descriptors can lack metadata. A viewport can confirm the
        // established scene, but cannot establish it or override known views.
        if (depth.ptr && scene_extent_candidate(scene, output, allow_native) &&
            std::all_of(color.begin(), color.begin() + n, [](Handle handle) { return handle.ptr != 0; }) &&
            std::all_of(colors.begin(), end, [&](const View* view) {
                return !view || (view->rejection == Diagnostic::om_accepted && view->extent == scene);
            }) && (!depth_view || (depth_view->rejection == Diagnostic::om_accepted && depth_view->extent == scene))) {
            state.extent = scene;
            state.target_pass = depth_view && depth_view->read_only_depth ? TargetPass::transparent : TargetPass::opaque;
            state.viewport_fallback = true;
            if (capture) audit.reason = Diagnostic::om_accepted;
            count(Diagnostic::om_viewport_fallback);
            return scene_viewport(state.viewport, scene);
        }
        if (capture) rt.unknown_targets.fetch_add(1, std::memory_order_relaxed);
        if (bad_color != end)
            reject(*bad_color ? (*bad_color)->rejection : Diagnostic::om_missing_rtv, *bad_color);
        if (dsv && !depth_view) reject(Diagnostic::om_missing_dsv, colors[0]);
        else if (depth_view && depth_view->rejection != Diagnostic::om_accepted &&
                 audit.reason != depth_view->rejection)
            reject(depth_view->rejection, colors[0]);
        return false;
    }
    const Extent extent = depth_view ? depth_view->extent : colors[0]->extent;
    const auto different_extent = std::find_if(colors.begin(), end,
        [&](const View* view) { return view->extent != extent; });
    if (different_extent != end) {
        reject(Diagnostic::om_extent_mismatch, *different_extent);
        return false;
    }
    const bool native = extent == output;
    const bool allow_native = rt.allow_native.load(std::memory_order_relaxed);
    if (!scene_extent_candidate(extent, output, allow_native)) {
        reject(Diagnostic::om_extent_outside_reference, colors[0]);
        return false;
    }
    if (!native && !depth_view && n > 1) {
        if (capture) {
            audit.reason = Diagnostic::om_no_depth;
            count(Diagnostic::om_no_depth);
        }
        return false;
    }
    if (native && !depth_view) {
        // A depthless full-size target needs actual G-buffer evidence. A single
        // output-format RTV may be the final composite or a presentation buffer.
        bool distinctive = false;
        const bool gbuffer = n > 1 && std::all_of(colors.begin(), end, [&](const View* view) {
            switch (view->format) {
            case DXGI_FORMAT_R11G11B10_FLOAT:
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            case DXGI_FORMAT_R16G16B16A16_SNORM:
            case DXGI_FORMAT_R32G32_UINT:
            case DXGI_FORMAT_R10G10B10A2_UNORM:
                distinctive = true;
                return true;
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                return true;
            default: return false;
            }
        });
        if (!gbuffer || !distinctive) {
            state.target_pass = TargetPass::output;
            count(Diagnostic::om_output_excluded);
            return false;
        }
    }
    count(Diagnostic::om_accepted);
    if (n > 1) count(Diagnostic::om_mrt_accepted);
    if (capture) audit.reason = Diagnostic::om_accepted;
    state.extent = extent;
    state.target_pass = !depth_view || depth_view->read_only_depth ? TargetPass::transparent : TargetPass::opaque;
    if (state.target_pass == TargetPass::opaque || native) observe_scene(extent, static_cast<UINT>(n));
    return true;
}

void STDMETHODCALLTYPE hook_om(List* l, UINT n, const Handle* rtv, BOOL contiguous, const Handle* dsv) {
    orig_om(l, n, rtv, contiguous, dsv);
    count(Diagnostic::om_calls);
    auto* s = list_state(l);
    if (!s || !s->vrs) return count(Diagnostic::om_untracked_list);
    s->extent = {};
    s->target_pass = TargetPass::none;
    s->viewport_fallback = false;
    const auto reject = [&](bool clear_engine = false) { unbind_vrs(*s, Diagnostic::unbind_om, clear_engine); };
    if (rt.disabled.load(std::memory_order_relaxed) || rt.paused.load(std::memory_order_relaxed)) return reject();
    const bool capture = rt.audit.enabled.load(std::memory_order_relaxed);
    const auto preset = rt.preset.load(std::memory_order_relaxed);
    if (!capture && !preset && rt.scene.load(std::memory_order_relaxed)) return reject();
    if (capture) {
        s->audit_targets = {};
        s->audit_targets.color_count = n;
        s->audit_targets.has_depth = dsv != nullptr;
    }
    const bool valid_count = n > 0 && n <= D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT && rtv;
    if (!valid_count) {
        if (capture) {
            s->audit_targets.reason = Diagnostic::om_color_count;
            count(Diagnostic::om_color_count);
            track_binding(*s, n, rtv, contiguous, dsv, false, true);
        }
    } else if (track_binding(*s, n, rtv, contiguous, dsv, true, capture) &&
               preset && coverage_gate(s) == Diagnostic::eligible && refresh_scene_vrs(*s)) {
        count(Diagnostic::om_image_bound);
        if (s->target_pass == TargetPass::transparent) count(Diagnostic::om_transparent_image_bound);
        return;
    }
    reject(preset != 0);
}
void STDMETHODCALLTYPE hook_indexed(List* l, UINT n, UINT instances, UINT start, INT vertex, UINT first) {
    std::optional<AuditCall> audit_call;
    if (rt.audit.enabled.load(std::memory_order_relaxed)) audit_call.emplace();
    const bool capture = audit_call && audit_call->collect();
    count(Diagnostic::indexed_calls);
    if (auto* s = list_state(l)) {
        before_indexed_draw(*s, instances ? n : 0, capture);
    }
    else if (n && instances) {
        count(Diagnostic::draw_untracked_list);
        if (capture)
            audit_draw(0, nullptr, Diagnostic::draw_untracked_list);
    }
    orig_indexed(l, n, instances, start, vertex, first);
}
void STDMETHODCALLTYPE hook_draw(List* l, UINT n, UINT instances, UINT start, UINT first) {
    std::optional<AuditCall> audit_call;
    if (rt.audit.enabled.load(std::memory_order_relaxed)) audit_call.emplace();
    const bool capture = audit_call && audit_call->collect();
    count(Diagnostic::nonindexed_calls);
    auto* s = list_state(l);
    if (n && instances && s && refresh_scene_vrs(*s)) count(Diagnostic::nonindexed_image_bound);
    if (n && instances && capture)
        audit_draw(1, s, coverage_gate(s));
    orig_draw(l, n, instances, start, first);
}
void STDMETHODCALLTYPE hook_dispatch(List* l, UINT x, UINT y, UINT z) {
    count(Diagnostic::dispatch_calls);
    if (auto* s = list_state(l)) {
        unbind_vrs(*s, Diagnostic::unbind_dispatch);
        if (s->pipeline.kind == PipelineKind::graphics) remember_pipeline(*s, nullptr);
    }
    orig_dispatch(l, x, y, z);
}
void STDMETHODCALLTYPE hook_indirect(List* l, ID3D12CommandSignature* sig, UINT n, ID3D12Resource* args,
                                     UINT64 offset, ID3D12Resource* count_buffer, UINT64 count_offset) {
    std::optional<AuditCall> audit_call;
    if (rt.audit.enabled.load(std::memory_order_relaxed)) audit_call.emplace();
    const bool capture = audit_call && audit_call->collect();
    count(Diagnostic::indirect_calls);
    auto* s = list_state(l);
    auto coverage = coverage_gate(s);
    const bool consider_vrs = coverage == Diagnostic::eligible && n &&
        (rt.preset.load(std::memory_order_relaxed) || s->bound_image);
    const auto info = consider_vrs || capture ? signature_info(sig) : SignatureInfo{};
    // Game-owned signatures may predate the hook. In that case use the
    // validated direct scene pass and tracked PSO from coverage_gate.
    const bool signature_allows_vrs = info.kind == IndirectKind::untracked ||
        info.kind == IndirectKind::draw || info.kind == IndirectKind::indexed;
    if (coverage == Diagnostic::eligible && !signature_allows_vrs && (consider_vrs || capture)) {
        coverage = Diagnostic::draw_indirect_signature;
        count(Diagnostic::indirect_signature_skipped);
    }
    if (consider_vrs && signature_allows_vrs && refresh_scene_vrs(*s)) {
        if (capture) {
            count(Diagnostic::indirect_image_bound);
            rt.indirect_with_image.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (s) unbind_vrs(*s, Diagnostic::unbind_indirect);
    if (capture) {
        rt.audit.used(info.id);
        audit_draw(2 + size_t(info.kind), s, coverage, info, n, count_buffer != nullptr);
    }
    orig_indirect(l, sig, n, args, offset, count_buffer, count_offset);
}
void STDMETHODCALLTYPE hook_bundle(List* l, List* bundle) {
    count(Diagnostic::bundle_calls);
    if (auto* s = list_state(l)) {
        const auto generation = s->generation;
        if (restore_game_vrs(*s)) *s = {};
        s->generation = generation;
        s->extent = {}; // a bundle can inherit and change graphics state
    }
    orig_bundle(l, bundle);
}
void STDMETHODCALLTYPE hook_begin_pass(ID3D12GraphicsCommandList4* l, UINT n,
    const D3D12_RENDER_PASS_RENDER_TARGET_DESC* color, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* depth,
    D3D12_RENDER_PASS_FLAGS flags) {
    count(Diagnostic::render_pass_begins);
    if (auto* s = list_state(l)) {
        unbind_vrs(*s);
        s->extent = {};
        s->target_pass = TargetPass::none;
        s->viewport_fallback = false;
        s->audit_targets = {};
        s->render_pass = true;
    }
    orig_begin_pass(l, n, color, depth, flags);
}
void STDMETHODCALLTYPE hook_end_pass(ID3D12GraphicsCommandList4* l) {
    orig_end_pass(l);
    if (auto* s = list_state(l)) s->render_pass = false;
}
void STDMETHODCALLTYPE hook_mesh(ID3D12GraphicsCommandList6* l, UINT x, UINT y, UINT z) {
    if (auto* s = list_state(l)) unbind_vrs(*s, Diagnostic::unbind_dispatch);
    orig_mesh(l, x, y, z);
}
void STDMETHODCALLTYPE hook_ray_pipeline(ID3D12GraphicsCommandList4* l, ID3D12StateObject* p) {
    if (auto* s = list_state(l)) {
        unbind_vrs(*s, Diagnostic::unbind_pso);
        remember_pipeline(*s, nullptr);
    }
    orig_ray_pipeline(l, p);
}
void STDMETHODCALLTYPE hook_rays(ID3D12GraphicsCommandList4* l, const D3D12_DISPATCH_RAYS_DESC* desc) {
    if (auto* s = list_state(l)) {
        unbind_vrs(*s, Diagnostic::unbind_dispatch);
        remember_pipeline(*s, nullptr);
    }
    orig_rays(l, desc);
}
void STDMETHODCALLTYPE hook_rate(List5* l, D3D12_SHADING_RATE rate, const D3D12_SHADING_RATE_COMBINER* combiners) {
    if (forwarding_vrs) return orig_rate(l, rate, combiners);
    if (auto* s = list_state(l); s && s->vrs) {
        const bool paused = rt.paused.load(std::memory_order_relaxed);
        if (paused) unbind_vrs(*s);
        std::array<D3D12_SHADING_RATE_COMBINER, 2> requested{};
        if (combiners && !guarded([&] {
            std::copy_n(combiners, 2, requested.begin());
            return true;
        })) {
            if (restore_game_vrs(*s)) s->vrs = nullptr;
        } else {
            const bool active = s->bound_image && refresh_scene_vrs(*s);
            s->game_rate = rate;
            s->game_combiners = requested;
            if (!paused && s->target_pass == TargetPass::output && rt.preset.load(std::memory_order_relaxed) &&
                !rt.disabled.load(std::memory_order_relaxed) && claim_vrs_override(*s)) {
                return native_rate(l, D3D12_SHADING_RATE_1X1, nullptr);
            }
            if (active) {
                count(Diagnostic::rate_combiner_preserved);
                const auto capped = rate == D3D12_SHADING_RATE_1X1 || rate == D3D12_SHADING_RATE_1X2 ||
                    rate == D3D12_SHADING_RATE_2X1 ? rate : D3D12_SHADING_RATE_2X2;
                return native_rate(l, capped, plugin_combiners);
            }
            s->native_engine_rate = rate != D3D12_SHADING_RATE_1X1 ||
                requested[0] != D3D12_SHADING_RATE_COMBINER_PASSTHROUGH ||
                requested[1] != D3D12_SHADING_RATE_COMBINER_PASSTHROUGH;
        }
    }
    native_rate(l, rate, combiners);
}
void STDMETHODCALLTYPE hook_image(List5* l, ID3D12Resource* image) {
    if (forwarding_vrs) return orig_image(l, image);
    if (auto* s = list_state(l); s && s->vrs) {
        const bool paused = rt.paused.load(std::memory_order_relaxed);
        if (paused) unbind_vrs(*s);
        const bool active = s->bound_image && refresh_scene_vrs(*s);
        s->game_image = image;
        if (!paused && s->target_pass == TargetPass::output && rt.preset.load(std::memory_order_relaxed) &&
            !rt.disabled.load(std::memory_order_relaxed) && claim_vrs_override(*s)) {
            return native_image(l, nullptr);
        }
        if (active) return count(Diagnostic::engine_image_deferred);
        s->native_engine_image = image != nullptr;
    }
    native_image(l, image);
}

bool install_hooks() {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<List5> list;
    ComPtr<ID3D12Device2> device2;
    ComPtr<ID3D12GraphicsCommandList6> list6;
    auto* device = rt.device.Get();
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&list))) || FAILED(list->Close())) return false;
    device->QueryInterface(IID_PPV_ARGS(&device2));
    list->QueryInterface(IID_PPV_ARGS(&list6));

    struct Hook { void* entry; void* detour; void** original; };
    allocator_reset_entry = allocator_reset_slot(allocator.Get());
#define DD2_HOOK(object, method, name) {slot(object, Slot_##method), \
        reinterpret_cast<void*>(&hook_##name), reinterpret_cast<void**>(&orig_##name)}
    const Hook persistent_hooks[]{
        DD2_HOOK(device, CreateRenderTargetView, rtv),
        DD2_HOOK(device, CreateDepthStencilView, dsv),
        DD2_HOOK(device, CopyDescriptors, copy),
        DD2_HOOK(device, CopyDescriptorsSimple, copy_simple),
        DD2_HOOK(device, CreateGraphicsPipelineState, create_graphics),
        {allocator_reset_entry, reinterpret_cast<void*>(&hook_allocator_reset),
            reinterpret_cast<void**>(&orig_allocator_reset)},
    };
    const Hook hot_hooks[]{
        DD2_HOOK(list.Get(), Reset, reset),
        DD2_HOOK(list.Get(), Close, close),
        DD2_HOOK(list.Get(), ClearState, clear),
        DD2_HOOK(list.Get(), SetPipelineState, pso),
        DD2_HOOK(list.Get(), RSSetViewports, viewports),
        DD2_HOOK(list.Get(), OMSetRenderTargets, om),
        DD2_HOOK(list.Get(), DrawIndexedInstanced, indexed),
        DD2_HOOK(list.Get(), DrawInstanced, draw),
        DD2_HOOK(list.Get(), Dispatch, dispatch),
        DD2_HOOK(list.Get(), ExecuteIndirect, indirect),
        DD2_HOOK(list.Get(), ExecuteBundle, bundle),
        DD2_HOOK(list.Get(), BeginRenderPass, begin_pass),
        DD2_HOOK(list.Get(), EndRenderPass, end_pass),
        DD2_HOOK(list.Get(), SetPipelineState1, ray_pipeline),
        DD2_HOOK(list.Get(), DispatchRays, rays),
        DD2_HOOK(list.Get(), RSSetShadingRate, rate),
        DD2_HOOK(list.Get(), RSSetShadingRateImage, image),
    };
    const auto result = MH_Initialize();
    if (result != MH_OK && result != MH_ERROR_ALREADY_INITIALIZED) return false;
    const bool enable_hot = !rt.paused.load(std::memory_order_relaxed);
    std::vector<void*> installed;
    installed.reserve(std::size(persistent_hooks) + std::size(hot_hooks) + 4);
    rt.hot_hooks.reserve(std::size(hot_hooks) + 1);
    const auto add = [&](const Hook& h, bool hot = false) {
        if (MH_CreateHook(h.entry, h.detour, h.original) != MH_OK) return false;
        installed.push_back(h.entry);
        if (hot) rt.hot_hooks.push_back(h.entry);
        return (hot && !enable_hot) || MH_QueueEnableHook(h.entry) == MH_OK;
    };
    const auto rollback = [&] {
        for (auto* entry : installed) MH_QueueDisableHook(entry);
        MH_ApplyQueued();
        // Keep trampolines valid for detours already entered on recording threads.
        return false;
    };
    for (const auto& hook : persistent_hooks) if (!add(hook)) return rollback();
    if (device2 && !add(DD2_HOOK(device2.Get(), CreatePipelineState, create_stream))) return rollback();
    rt.audit.signature_hook = add(DD2_HOOK(device, CreateCommandSignature, create_signature));
    rt.audit.compute_pipeline_hook = add(DD2_HOOK(device, CreateComputePipelineState, create_compute));
    for (const auto& hook : hot_hooks) if (!add(hook, true)) return rollback();
    if (list6 && !add(DD2_HOOK(list6.Get(), DispatchMesh, mesh), true)) return rollback();
#undef DD2_HOOK
    rate_entry = slot(list.Get(), Slot_RSSetShadingRate);
    image_entry = slot(list.Get(), Slot_RSSetShadingRateImage);

    if (MH_ApplyQueued() != MH_OK) return rollback();
    rt.hot_hooks_active.store(enable_hot, std::memory_order_release);
    return true;
}

bool set_hot_hooks_enabled(bool enable) {
    std::lock_guard lock{rt.hook_toggle_mutex};
    if (!rt.installed.load(std::memory_order_acquire)) return true;
    const bool active = rt.hot_hooks_active.load(std::memory_order_relaxed);
    if (active == enable && !rt.hot_hooks_out_of_sync.load(std::memory_order_relaxed)) return true;
    const auto queue = [](bool desired) {
        bool queued = true;
        for (auto* entry : rt.hot_hooks) {
            const auto result = desired ? MH_QueueEnableHook(entry) : MH_QueueDisableHook(entry);
            if (result != MH_OK) queued = false;
        }
        return queued;
    };
    if (!queue(enable) || MH_ApplyQueued() != MH_OK) {
        // An apply can fail after patching a subset of the targets. Restore
        // the last committed group and let the next rendering frame retry.
        if (queue(active)) MH_ApplyQueued();
        rt.hot_hooks_out_of_sync.store(true, std::memory_order_release);
        return false;
    }
    if (!enable) rt.list_generation.fetch_add(1, std::memory_order_release);
    rt.hot_hooks_active.store(enable, std::memory_order_release);
    rt.hot_hooks_out_of_sync.store(false, std::memory_order_release);
    return true;
}
} // namespace

bool start(ID3D12Device* device, ID3D12CommandQueue* queue, const Settings& settings) {
    if (!device || !queue) return false;
    if (rt.installed.load() || rt.disabled.load()) return true;
    const bool started = guarded([&] {
        if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
        ComPtr<ID3D12Device> queue_device;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&queue_device))) || queue_device.Get() != device) return false;
        D3D12_FEATURE_DATA_D3D12_OPTIONS6 caps{};
        const HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &caps, sizeof(caps));
        rt.tier = static_cast<unsigned>(caps.VariableShadingRateTier);
        rt.tile = caps.ShadingRateImageTileSize;
        rt.additional = caps.AdditionalShadingRatesSupported != FALSE;
        if (FAILED(hr) || caps.VariableShadingRateTier < D3D12_VARIABLE_SHADING_RATE_TIER_2 ||
            (rt.tile != 8 && rt.tile != 16 && rt.tile != 32)) {
            rt.message.store("D3D12 Tier 2 VRS with a valid shading-rate image tile size is required");
            return false;
        }
        rt.rtv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        rt.dsv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        if (!rt.rtv_stride || !rt.dsv_stride) return false;
        rt.device = device;
        rt.queue = queue;
        set_transparent_passes(settings.shade_transparent_passes);
        return install_hooks();
    });
    if (!started) {
        if (rt.tier >= 2 && rt.tile) rt.message.store("Could not initialize the D3D12 command-list hooks");
        rt.disabled.store(true);
        return true;
    }
    rt.message.store("Preparing shading-rate images");
    rt.installed.store(true);
    try {
        std::thread(image_worker).detach();
    } catch (...) {
        rt.disabled.store(true);
        rt.message.store("Could not start the shading-rate image worker");
    }
    return true;
}

void set_preset(int preset) {
    const auto profile = std::clamp(preset, 0, int(profiles.size()));
    rt.preset.store(profile && !rt.paused.load() && !rt.disabled.load() && rt.hot_hooks_active.load() ? profile : 0);
}

void set_optimizer_enabled(bool enabled) {
    if (enabled) {
        rt.disable_countdown.store(0, std::memory_order_release);
        if (!set_hot_hooks_enabled(true)) return;
    } else {
        rt.recording_overrides.fetch_or(override_gate_closed);
        rt.preset.store(0);
    }
    {
        std::lock_guard lock{rt.worker_mutex};
        rt.paused.store(!enabled);
        if (enabled) rt.recording_overrides.fetch_and(~override_gate_closed);
    }
    rt.worker_cv.notify_one();
    if (enabled) return;
    rt.disable_countdown.store(2, std::memory_order_release);
    rt.audit.enabled.store(false, std::memory_order_relaxed);
    rt.candidates.store(0, std::memory_order_relaxed);
    rt.shaded.store(0, std::memory_order_relaxed);
    rt.unknown_targets.store(0, std::memory_order_relaxed);
    rt.indirect_with_image.store(0, std::memory_order_relaxed);
    rt.frame_candidates = rt.frame_shaded = rt.frame_targets = rt.frame_indirect = 0;
}

void set_transparent_passes(bool enabled) {
    rt.shade_transparent.store(enabled, std::memory_order_relaxed);
}

void set_swapchain_backbuffers(ID3D12Resource* const* buffers, size_t count) {
    reset_scene_extent();
    for (size_t i = 0; i < rt.backbuffers.size(); ++i)
        rt.backbuffers[i].store(i < count ? buffers[i] : nullptr, std::memory_order_relaxed);
}

void frame(ID3D12Device* device, ID3D12CommandQueue* queue, const Settings& settings, int preset, Extent output) {
    if (rt.paused.load() && rt.installed.load()) {
        if (settings.optimizer_enabled) {
            if (!rt.disabled.load()) set_optimizer_enabled(true);
        } else if (rt.hot_hooks_active.load() || rt.hot_hooks_out_of_sync.load()) {
            const auto remaining = rt.disable_countdown.load(std::memory_order_acquire);
            if (remaining) rt.disable_countdown.store(remaining - 1, std::memory_order_release);
            // Leave the recording hooks available if a list spans more
            // than the grace period. Only that list's recorder can unbind.
            if (remaining <= 1 && rt.recording_overrides.load(std::memory_order_acquire) == override_gate_closed)
                set_hot_hooks_enabled(false);
        }
    }
    if (rt.paused.load() || !rt.installed.load() || rt.disabled.load()) {
        rt.audit.enabled.store(false, std::memory_order_relaxed);
        return;
    }
    if ((device && device != rt.device.Get()) || (queue && queue != rt.queue.Get())) {
        rt.preset.store(0);
        rt.audit.enabled.store(false, std::memory_order_relaxed);
        rt.disabled.store(true);
        reset_scene_extent();
        publish_images(nullptr);
        rt.message.store("D3D12 device or queue changed. Restart to reattach.");
        rt.worker_cv.notify_one();
        return;
    }
    set_preset(full_sharp_center(settings.center) ? 0 : preset);
    set_transparent_passes(settings.shade_transparent_passes);
    rt.audit.enabled.store(settings.verbose_logging, std::memory_order_relaxed);
    const Extent scene = update_scene_extent(output, settings.allow_native_vrs);
    const Extent extent = scene.width ? scene : settings.reference;
    ImageKey key{extent};
    key.profile_mode = std::clamp(settings.profile_mode, 0, int(profiles.size()) - 1);
    key.zone = shading_ellipse(extent, rt.tile, profiles[key.profile_mode], settings.center);
    {
        std::lock_guard lock{rt.worker_mutex};
        if (!(key == rt.wanted)) {
            rt.wanted = key;
            rt.worker_cv.notify_one();
        }
    }
    rt.frame_candidates = rt.candidates.exchange(0);
    rt.frame_shaded = rt.shaded.exchange(0);
    rt.frame_targets = rt.unknown_targets.exchange(0);
    rt.frame_indirect = rt.indirect_with_image.exchange(0);
}

Status status() {
    std::lock_guard lock{rt.worker_mutex};
    const auto& images = rt.published_images;
    const bool failed = rt.disabled.load();
    const auto mask = rt.preset.load();
    return {rt.tier, rt.tile, rt.additional, failed ? 0 : mask,
        rt.message.load(), images ? images->key.extent : Extent{},
        unpack(rt.scene.load(std::memory_order_relaxed)), unpack(rt.output.load(std::memory_order_relaxed)),
        rt.frame_candidates, rt.frame_shaded, rt.frame_targets, rt.frame_indirect, failed,
        !failed && rt.wanted.extent.width && (!images || images->key != rt.wanted),
        rt.hot_hooks_active.load(), rt.hot_hooks_out_of_sync.load()};
}

Diagnostics take_diagnostics() {
    Diagnostics report;
    std::lock_guard lock{rt.binding_samples_mutex};
    for (size_t i = 0; i < report.counts.size(); ++i)
        report.counts[i] = rt.counts[i].exchange(0, std::memory_order_relaxed);
    for (size_t i = 0; i < report.bindings.size(); ++i) {
        auto& binding = rt.binding_samples[i];
        report.bindings[i] = std::exchange(binding.sample, {});
        binding.claimed = false;
    }
    return report;
}

AuditReport take_audit() { return rt.audit.take(); }
} // namespace dd2::graphics
