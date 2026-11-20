#include "gpu_timer.hpp"

#include <algorithm>
#include <limits>

namespace dd2::graphics {
namespace {
constexpr uint64_t removed = (std::numeric_limits<uint64_t>::max)();
constexpr unsigned queries_per_frame = 2;
constexpr size_t pair_bytes = sizeof(uint64_t) * queries_per_frame;
}

bool GpuTimer::Context::initialize(ID3D12Device* d, ID3D12CommandQueue* q) {
    device = d;
    queue = q;
    if (q->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        FAILED(q->GetTimestampFrequency(&frequency)) || !frequency) return false;
    const D3D12_QUERY_HEAP_DESC desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP,
        static_cast<UINT>(slots.size()) * queries_per_frame, 0};
    if (FAILED(d->CreateQueryHeap(&desc, IID_PPV_ARGS(&query))) ||
        FAILED(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = slots.size() * pair_bytes;
    buffer.Height = buffer.SampleDesc.Count = 1;
    buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                           nullptr, IID_PPV_ARGS(&readback)))) return false;
    for (unsigned i = 0; i < slots.size(); ++i) {
        auto& slot = slots[i];
        if (FAILED(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator))) ||
            FAILED(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&slot.begin)))) return false;
        slot.begin->EndQuery(query.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i * queries_per_frame);
        if (FAILED(slot.begin->Close()) ||
            FAILED(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&slot.end)))) return false;
        slot.end->EndQuery(query.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i * queries_per_frame + 1);
        slot.end->ResolveQueryData(query.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i * queries_per_frame,
                                   queries_per_frame, readback.Get(), i * pair_bytes);
        if (FAILED(slot.end->Close())) return false;
    }
    return true;
}

void GpuTimer::close(Context& ctx) {
    armed_.store(nullptr, std::memory_order_release);
    if (ctx.active < 0) return;
    auto& slot = ctx.slots[ctx.active];
    ctx.active = -1;
    ID3D12CommandList* end = slot.end.Get();
    ctx.queue->ExecuteCommandLists(1, &end);
    slot.fence = ++ctx.serial;
    ctx.fault = FAILED(ctx.queue->Signal(ctx.fence.Get(), slot.fence));
}

void GpuTimer::reap() {
    std::erase_if(retired_, [](const auto& ctx) {
        const uint64_t completed = ctx->fence->GetCompletedValue();
        return completed == removed || (!ctx->fault && completed >= ctx->serial);
    });
}

std::optional<float> GpuTimer::begin(ID3D12Device* device, ID3D12CommandQueue* queue, bool enabled) {
    std::lock_guard lock{mutex_};
    armed_.store(nullptr, std::memory_order_release);
    reap();
    ++frame_;
    if (context_ && (context_->device.Get() != device || context_->queue.Get() != queue)) {
        close(*context_);
        retired_.push_back(std::move(context_));
    }
    if (!enabled || !device || !queue) {
        if (context_) {
            close(*context_);
            context_->min_frame = frame_ + 1;
        }
        return {};
    }
    if (!context_) {
        if (failed_queue_ == queue) return {};
        auto context = std::make_unique<Context>();
        if (!context->initialize(device, queue)) {
            failed_queue_ = queue;
            return {};
        }
        failed_queue_ = nullptr;
        context_ = std::move(context);
    }
    auto& ctx = *context_;
    if (ctx.active >= 0) {
        // No Present ended the interval. Retire its slot without feeding a
        // multi-frame measurement to the controller.
        close(ctx);
        ctx.min_frame = frame_;
    }
    const uint64_t complete = ctx.fence->GetCompletedValue();
    if (ctx.fault || complete == removed) return {};
    std::optional<float> result;
    size_t newest = ctx.slots.size();
    for (size_t i = 0; i < ctx.slots.size(); ++i) {
        const auto& slot = ctx.slots[i];
        if (slot.fence && slot.frame >= ctx.min_frame && slot.frame > ctx.latest_read &&
            frame_ - slot.frame >= 3 && complete >= slot.fence &&
            (newest == ctx.slots.size() || slot.frame > ctx.slots[newest].frame)) newest = i;
    }
    if (newest < ctx.slots.size()) {
        const D3D12_RANGE range{newest * pair_bytes, (newest + 1) * pair_bytes};
        void* data{};
        if (SUCCEEDED(ctx.readback->Map(0, &range, &data))) {
            const auto* stamps = static_cast<const uint64_t*>(data) + newest * queries_per_frame;
            if (stamps[1] > stamps[0])
                result = float(double(stamps[1] - stamps[0]) * 1000.0 / double(ctx.frequency));
            const D3D12_RANGE no_write{0, 0};
            ctx.readback->Unmap(0, &no_write);
        }
        ctx.latest_read = ctx.slots[newest].frame;
    }
    const auto& slot = ctx.slots[frame_ % ctx.slots.size()];
    if (!slot.fence || complete >= slot.fence) armed_.store(queue, std::memory_order_release);
    return result;
}

bool GpuTimer::submit_if_armed(ID3D12CommandQueue* queue, UINT count,
                               ID3D12CommandList* const* lists, Execute execute) {
    if (!count || armed_.load(std::memory_order_acquire) != queue) return false;
    std::lock_guard lock{mutex_};
    if (!armed_.compare_exchange_strong(queue, nullptr, std::memory_order_acq_rel)) return false;
    auto& ctx = *context_;
    ctx.active = int(frame_ % ctx.slots.size());
    auto& slot = ctx.slots[ctx.active];
    slot.frame = frame_;
    ID3D12CommandList* start = slot.begin.Get();
    execute(queue, 1, &start);
    execute(queue, count, lists);
    return true;
}

void GpuTimer::present() {
    std::lock_guard lock{mutex_};
    if (context_) close(*context_);
}

void GpuTimer::reset() {
    std::lock_guard lock{mutex_};
    failed_queue_ = nullptr;
    if (!context_) return;
    close(*context_);
    context_->min_frame = frame_ + 1;
}

} // namespace dd2::graphics
