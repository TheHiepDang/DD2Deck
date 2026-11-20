#pragma once

#include "graphics.hpp"
#include <wrl/client.h>

namespace dd2::graphics {

struct VelocityScalePipeline {
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;

    bool initialize(ID3D12Device* device);
};

// One prepared copy owns its source reference, mask, and descriptors. The
// native-pass adapter must retain it until the recording allocator is safe
// to reset and discard/rebuild it when any input or extent changes.
class VelocityScaleCopy {
    Microsoft::WRL::ComPtr<ID3D12Resource> source_, velocity_, mask_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> views_;
    Extent extent_{}, scene_{};
    unsigned tile_{}, columns_{};
    bool full_screen_{};
public:
    bool initialize(ID3D12Device* device, ID3D12Resource* source, Extent scene, unsigned tile,
                    const ShadingEllipse& ellipse, bool additional_rates, unsigned variant = 0);
    ID3D12Resource* output() const { return velocity_.Get(); }

    // Call only at a capture-verified native pass site on a compatible
    // command list outside a render pass. The source must already be in a
    // NON_PIXEL_SHADER_RESOURCE-compatible state. This changes the bound
    // descriptor heaps, compute root signature, and PSO. Restore the game
    // bindings (including invalidated tables) before forwarding its pass.
    bool record(ID3D12GraphicsCommandList* list, const VelocityScalePipeline& pipeline,
                float multiplier, float alpha) const;
};

} // namespace dd2::graphics
