#include "motion_blur_gpu.hpp"

#include <d3dcompiler.h>
#include <bit>
#include <cstring>

namespace dd2::graphics {
namespace {
using Microsoft::WRL::ComPtr;
constexpr auto readable = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

constexpr char scale_shader[] = R"hlsl(
cbuffer BlurParams : register(b0) {
    uint2 VelocitySize;
    uint2 SceneSize;
    uint TileShift;
    uint MaskColumns;
    float Boost;
};

Texture2D<float2> Velocity : register(t0);
ByteAddressBuffer Mask : register(t1);
RWTexture2D<float2> BlurVelocity : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 thread : SV_DispatchThreadID) {
    if (any(thread.xy >= VelocitySize)) return;

    // Use integer coordinates to map the center of each velocity texel
    // into a scene tile, even when the upscaler changes the two extents.
    uint2 scenePixel = (2 * thread.xy + 1) * SceneSize / (2 * VelocitySize);
    uint2 tile = scenePixel >> TileShift;
    uint index = tile.y * MaskColumns + tile.x;
    uint word = Mask.Load(index & ~3u);
    uint rate = (word >> ((index & 3u) * 8u)) & 255u;
    float weight = rate == 5u ? 1.0 : (rate == 0u ? 0.0 : 0.5);
    float2 velocity = Velocity.Load(int3(thread.xy, 0));
    BlurVelocity[thread.xy] = rate == 0u ? velocity : velocity * (1.0 + Boost * weight);
}
)hlsl";

D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CreationNodeMask = properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, 0, before, after};
    return barrier;
}
} // namespace

bool VelocityScalePipeline::initialize(ID3D12Device* device) {
    if (!device) return false;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{DXGI_FORMAT_R16G16_FLOAT};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
        !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) ||
        !(support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) ||
        !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) return false;

    struct Compiler {
        HMODULE module{LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
        ~Compiler() { if (module) FreeLibrary(module); }
    } compiler;
    if (!compiler.module) return false;
    const auto compile = reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler.module, "D3DCompile"));
    ComPtr<ID3DBlob> shader;
    if (!compile || FAILED(compile(scale_shader, sizeof(scale_shader) - 1, "ScaleVRSVelocity.hlsl",
        nullptr, nullptr, "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, nullptr))) return false;

    const D3D12_DESCRIPTOR_RANGE ranges[]{
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2}
    };
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {2, ranges};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants = {0, 0, 7};
    const D3D12_ROOT_SIGNATURE_DESC desc{2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> signature;
    ComPtr<ID3D12RootSignature> new_root;
    ComPtr<ID3D12PipelineState> new_pso;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, nullptr)) ||
        FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                           IID_PPV_ARGS(&new_root)))) return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
    state.pRootSignature = new_root.Get();
    state.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    if (FAILED(device->CreateComputePipelineState(&state, IID_PPV_ARGS(&new_pso)))) return false;
    root = std::move(new_root);
    pso = std::move(new_pso);
    return true;
}

bool VelocityScaleCopy::initialize(ID3D12Device* device, ID3D12Resource* source, Extent scene, unsigned tile,
                                    const ShadingEllipse& ellipse, bool additional_rates, unsigned variant) {
    constexpr auto maximum = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    if (!device || !source || variant >= mask_variants || !scene.width || !scene.height ||
        scene.width > maximum || scene.height > maximum ||
        (tile != 8 && tile != 16 && tile != 32) || !(ellipse.x_radius >= 0) || !(ellipse.y_radius >= 0) ||
        !(ellipse.transition_ratio > 1)) return false;
    auto desc = source->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Format != DXGI_FORMAT_R16G16_FLOAT ||
        desc.MipLevels != 1 || desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1 ||
        !desc.Width || !desc.Height || desc.Width > maximum || desc.Height > maximum ||
        (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) return false;
    ComPtr<ID3D12Device> owner;
    if (FAILED(source->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get() != device) return false;

    VelocityScaleCopy prepared;
    prepared.source_ = source;
    prepared.scene_ = scene;
    prepared.extent_ = {static_cast<uint32_t>(desc.Width), desc.Height};
    prepared.tile_ = tile;
    prepared.full_screen_ = ellipse.full_screen;
    prepared.columns_ = (scene.width + tile - 1) / tile;
    const unsigned rows = (scene.height + tile - 1) / tile;
    const unsigned mask_bytes = (prepared.columns_ * rows + 3) & ~3u;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    const auto local_heap = heap(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(device->CreateCommittedResource(&local_heap, D3D12_HEAP_FLAG_NONE, &desc,
        readable, nullptr, IID_PPV_ARGS(&prepared.velocity_)))) return false;

    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = mask_bytes;
    buffer.Height = buffer.SampleDesc.Count = 1;
    buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto upload_heap = heap(D3D12_HEAP_TYPE_UPLOAD);
    if (FAILED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&prepared.mask_)))) return false;
    void* mapped{};
    const D3D12_RANGE no_read{0, 0};
    if (FAILED(prepared.mask_->Map(0, &no_read, &mapped))) return false;
    auto* mask = static_cast<uint8_t*>(mapped);
    std::memset(mask, 0, mask_bytes);
    for (unsigned y = 0; y < rows; ++y)
        for (unsigned x = 0; x < prepared.columns_; ++x)
            mask[y * prepared.columns_ + x] = adaptive_rate_at(scene, tile, x, y, ellipse, additional_rates, variant);
    prepared.mask_->Unmap(0, nullptr);

    const D3D12_DESCRIPTOR_HEAP_DESC views{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(device->CreateDescriptorHeap(&views, IID_PPV_ARGS(&prepared.views_)))) return false;
    auto handle = prepared.views_->GetCPUDescriptorHandleForHeapStart();
    const auto stride = device->GetDescriptorHandleIncrementSize(views.Type);
    device->CreateShaderResourceView(source, nullptr, handle);
    handle.ptr += stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC mask_view{};
    mask_view.Format = DXGI_FORMAT_R32_TYPELESS;
    mask_view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    mask_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    mask_view.Buffer.NumElements = mask_bytes / 4;
    mask_view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device->CreateShaderResourceView(prepared.mask_.Get(), &mask_view, handle);
    handle.ptr += stride;
    device->CreateUnorderedAccessView(prepared.velocity_.Get(), nullptr, nullptr, handle);
    *this = std::move(prepared);
    return true;
}

bool VelocityScaleCopy::record(ID3D12GraphicsCommandList* list, const VelocityScalePipeline& pipeline,
                              float multiplier, float alpha) const {
    if (!list || !views_ || full_screen_ || !pipeline.root || !pipeline.pso || !std::isfinite(alpha) ||
        !std::isfinite(multiplier) || alpha <= 0.01f || multiplier <= 1.0f) return false;
    const auto type = list->GetType();
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
    const float boost = (std::min(multiplier, 3.0f) - 1.0f) * std::min(alpha, 1.0f);
    const uint32_t constants[]{extent_.width, extent_.height, scene_.width, scene_.height,
        static_cast<uint32_t>(std::countr_zero(tile_)), columns_, std::bit_cast<uint32_t>(boost)};
    ID3D12DescriptorHeap* heaps[]{views_.Get()};
    // Only the plug-in copy transitions. The engine velocity and the live
    // VRS image are untouched. The final transition orders the UAV stores
    // before the native pass reads the copy, including on VKD3D-Proton.
    const auto begin = transition(velocity_.Get(), readable, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const auto end = transition(velocity_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, readable);
    list->ResourceBarrier(1, &begin);
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(pipeline.root.Get());
    list->SetComputeRootDescriptorTable(0, views_->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRoot32BitConstants(1, static_cast<UINT>(std::size(constants)), constants, 0);
    list->SetPipelineState(pipeline.pso.Get());
    list->Dispatch((extent_.width + 7) / 8, (extent_.height + 7) / 8, 1);
    list->ResourceBarrier(1, &end);
    return true;
}

} // namespace dd2::graphics
