# DD2 VRS border motion blur preview

The preview adds saved controls, camera tracking, a read-only D3D12 hook probe, and a velocity-scaling compute stage tested on a D3D12 device. The checkbox enables the requested blur strength readout. Actual localized blur in DD2 requires a verified native pass adapter.

## Capture the native pass

1. Install the preview DLL in `reframework/plugins/` in place of the stable VRS DLL.
2. Load a gameplay scene and keep the same upscaler, output resolution and graphics settings throughout the comparison.
3. With DD2's own **Motion Blur** off, open **Adaptive VRS & Border Motion Blur** and select **Capture Motion Blur Pipelines** once. Wait for the capture to finish. Repeat with DD2's Motion Blur on in the same view.
4. Collect both `[DD2 VRS Blur Probe]` blocks from `re2_framework_log.txt`. The compute and fullscreen-pixel IDs narrow down candidates. Zero denotes an unidentified pipeline.
5. Verify the candidate in a PIX or RenderDoc GPU frame capture. Record the game build and active upscaler, the exact shader bytecode identity, the command-list type and binding layout, the velocity SRV register and subresource, the barrier state at the point of injection, and the blur output. Use the GPU capture to establish execution order from upscale completion through the candidate to tone mapping and the UI, including work across command lists and queues.

Matching requires the captured shader, binding layout, velocity resource and verified post-upscaler position.

## Camera and GPU stage

The plugin samples the primary camera's normalized world rotation at `BeginRendering`. It derives forward and up axes to measure signed yaw, pitch and roll in the previous camera's basis. The envelope uses exponential smoothing and a finite fade. A missing camera or camera change clears the envelope. VRS can use camera telemetry with the blur option disabled.

`VelocityScaleCopy` in `src/motion_blur_gpu.cpp` contains the compute shader and preparation code. It samples the original velocity as an SRV and writes an independent typed UAV. The packed mask uses `adaptive_rate_at` with the selected VRS variant and maps velocity pixel centers to internal scene tiles. The 1x1 center and leading edge retain their original velocity. Transition tiles receive half the boost, and 2x2 tiles receive the full boost. Each processed texel performs one velocity load and one store. The scaler skips the dispatch at effective strength of 0.01 or less, or when the multiplier is 1x.

The only injected transitions are on the private copy. It changes from shader-readable to UAV for the compute dispatch, then back to shader-readable before the native pass reads it. These transitions order the copy's stores and reads on the recording command list. The engine velocity SRV and live shading-rate image keep their original resources and states.

A verified adapter must record the compute work outside a render pass, restore the affected PSO, root signatures, heaps and invalidated descriptor tables, bind the private SRV for the native pass, and restore that binding afterward. It must retain the copy, descriptors, and root signature and PSO until GPU completion. On resize or device reset, retire old resources by allocator or fence and rebuild against the new source and extents. This adapter and its Steam Deck verification remain pending until the game capture establishes the correct pass and binding.
