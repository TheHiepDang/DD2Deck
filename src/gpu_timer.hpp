#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <d3d12.h>
#include <wrl/client.h>

namespace dd2::graphics {

// Frames are bracketed on the same direct queue. Reads are at least three
// frame starts old and are attempted only after the slot's fence completes.
class GpuTimer {
    using List = ID3D12GraphicsCommandList;
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    struct Slot {
        Ptr<ID3D12CommandAllocator> allocator;
        Ptr<List> begin, end;
        uint64_t frame{}, fence{};
    };
    struct Context {
        std::array<Slot, 4> slots;
        Ptr<ID3D12QueryHeap> query;
        Ptr<ID3D12Resource> readback;
        Ptr<ID3D12Fence> fence;
        Ptr<ID3D12Device> device;
        Ptr<ID3D12CommandQueue> queue;
        uint64_t frequency{}, serial{}, latest_read{}, min_frame{};
        int active{-1};
        bool fault{};
        bool initialize(ID3D12Device*, ID3D12CommandQueue*);
    };
    std::mutex mutex_;
    std::unique_ptr<Context> context_;
    std::vector<std::unique_ptr<Context>> retired_;
    std::atomic<ID3D12CommandQueue*> armed_{};
    ID3D12CommandQueue* failed_queue_{};
    uint64_t frame_{};
    void close(Context&);
    void reap();
public:
    using Execute = void (STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    std::optional<float> begin(ID3D12Device*, ID3D12CommandQueue*, bool enabled);
    bool submit_if_armed(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*, Execute);
    void present();
    void reset();
};

} // namespace dd2::graphics
