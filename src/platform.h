#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*dd2_guarded_fn)(void*);
int dd2_guarded_call(dd2_guarded_fn fn, void* data);

#define DD2_D3D_SLOTS(X) \
    X(ID3D12Device, CreateRenderTargetView) \
    X(ID3D12Device, CreateDepthStencilView) \
    X(ID3D12Device, CopyDescriptors) \
    X(ID3D12Device, CopyDescriptorsSimple) \
    X(ID3D12Device, CreateGraphicsPipelineState) \
    X(ID3D12Device, CreateComputePipelineState) \
    X(ID3D12Device, CreateCommandSignature) \
    X(ID3D12Device2, CreatePipelineState) \
    X(ID3D12CommandQueue, ExecuteCommandLists) \
    X(ID3D12GraphicsCommandList, Reset) \
    X(ID3D12GraphicsCommandList, Close) \
    X(ID3D12GraphicsCommandList, ClearState) \
    X(ID3D12GraphicsCommandList, DrawIndexedInstanced) \
    X(ID3D12GraphicsCommandList, DrawInstanced) \
    X(ID3D12GraphicsCommandList, Dispatch) \
    X(ID3D12GraphicsCommandList, ExecuteIndirect) \
    X(ID3D12GraphicsCommandList, ExecuteBundle) \
    X(ID3D12GraphicsCommandList, SetPipelineState) \
    X(ID3D12GraphicsCommandList, RSSetViewports) \
    X(ID3D12GraphicsCommandList, OMSetRenderTargets) \
    X(ID3D12GraphicsCommandList4, BeginRenderPass) \
    X(ID3D12GraphicsCommandList4, EndRenderPass) \
    X(ID3D12GraphicsCommandList4, SetPipelineState1) \
    X(ID3D12GraphicsCommandList4, DispatchRays) \
    X(ID3D12GraphicsCommandList5, RSSetShadingRate) \
    X(ID3D12GraphicsCommandList5, RSSetShadingRateImage) \
    X(ID3D12GraphicsCommandList6, DispatchMesh)

enum Dd2Slot {
#define DD2_SLOT_ID(type, name) Slot_##name,
    DD2_D3D_SLOTS(DD2_SLOT_ID)
#undef DD2_SLOT_ID
    Slot_Count
};

extern const unsigned dd2_slots[Slot_Count];
extern const unsigned dd2_allocator_reset_slot;

#ifdef __cplusplus
}
#endif
