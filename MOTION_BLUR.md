# DD2 VRS border motion blur preview (archived)

The Steam Deck optimization removed this adapter and its capture controls. This document describes the previous implementation.

The adapter defaults to DD2's post-upscaler motion blur pass. Turn on **Motion Blur** in DD2's Graphics settings. Keep **Motion Quality Target** at 1 or uncheck the **Motion Quality** tweak in Native Engine Optimizer so the game can run its blur pass.

## Capture the native pass

1. Install the preview DLL in `reframework/plugins/` in place of the stable VRS DLL.
2. Load a gameplay scene and keep the same upscaler, output resolution and graphics settings throughout the comparison.
3. With DD2's own **Motion Blur** off, open **Adaptive VRS & Border Motion Blur** and select **Capture Motion Blur Pipelines** once. Wait for the capture to finish. Repeat with DD2's Motion Blur on in the same view.
4. Collect both `[DD2 VRS Blur Probe]` blocks from `re2_framework_log.txt`. The compute and fullscreen-pixel IDs narrow down candidates. Zero denotes an unidentified pipeline. `vel_slot=I[J]` gives the root parameter and the descriptor offset of a candidate velocity SRV.
5. Verify the candidate in a PIX or RenderDoc GPU frame capture. Record the game build and active upscaler, the exact shader bytecode identity, the command-list type and binding layout, the velocity SRV register and subresource, the barrier state at the point of injection, and the blur output. Use the GPU capture to establish execution order from upscale completion through the candidate to tone mapping and the UI, including work across command lists and queues.

The built-in adapter matches an untagged, pre-hook PSO at `ceil(output_width/16) × ceil(output_height/16) × 1`. It expects a scene-sized R16G16_FLOAT velocity texture at slot 5, offset 0. Verbose diagnostics report successful injections as `blur_velocity_scaled`.

For a custom pass, set `motion_blur_cs_id` in `reframework/data/dd2_foveated_vrs.json` to the verified compute shader ID and enter the zero-based binding in `motion_blur_velocity_slot` and `motion_blur_velocity_offset`. A nonzero ID requires an exact match. Existing slot and offset overrides take precedence over the defaults. Set the slot to `-1` to disable both matching paths. Restart DD2 after editing the file.

The recording hooks require a direct list, observed bindings, and a ready VRS mask for the current scene. The SRV must cover the sole mip of an R16G16_FLOAT 2D texture. With root signature metadata, CBV/SRV/UAV tables must be bounded, contiguous and static, with up to 1,024 descriptors across the tables. For signatures created before the hook, the adapter infers up to 16 contiguous tracked descriptors per resource table. The original sampler heap is reused. When configuring a custom pass, verify the table ranges and shader-readable source state in the GPU capture.

## Camera and GPU stage

The plugin samples the primary camera's normalized world rotation at `UpdateBehavior` and publishes the envelope to `BeginRendering`. It derives forward and up axes to measure signed yaw, pitch and roll in the previous camera's basis. The envelope uses exponential smoothing and a finite fade. A missing camera or camera change clears the envelope. VRS can use camera telemetry with the blur option disabled.

`VelocityScaleCopy` in `src/motion_blur_gpu.cpp` contains the compute shader and preparation code. It samples the original velocity as an SRV and writes an independent typed UAV. The packed mask uses `adaptive_rate_at` with the selected VRS variant and maps velocity pixel centers to internal scene tiles. The 1x1 center and leading edge retain their original velocity. Transition tiles receive half the boost, and 2x2 tiles receive the full boost. Each processed texel performs one velocity load and one store. The scaler skips the dispatch at effective strength of 0.01 or less, or when the multiplier is 1x.

The only injected transitions are on the private copy. It changes from shader-readable to UAV for the compute dispatch, then back to shader-readable before the native pass reads it. These transitions order the copy's stores and reads on the recording command list. The engine velocity SRV and live shading-rate image keep their original resources and states.

The adapter records the scaler outside a render pass, restores the compute signature, PSO and root arguments, and runs the native pass with a private resource heap reconstructed from tracked descriptors. The velocity entry points to the scaled copy. After the native dispatch, the adapter restores the original game heaps and both compute and graphics descriptor tables. The command allocator holds the private heaps and copy resources until a successful reset or destruction. The Steam Deck and the actual DD2 pass still require verification using an in-game capture.
