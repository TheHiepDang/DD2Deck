# DD2 Foveated VRS

A native x64 REFramework plugin for Dragon's Dogma 2. It applies Direct3D 12 shading-rate images to qualifying opaque and transparent passes on devices with Tier 2 VRS. The package includes an MSVC Release DLL in `reframework/plugins/` and the C++20 source.

The optimizer uses a static shading-rate image for the selected profile and applies native render tweaks and NPC and stray-pawn generation caps. The Steam Deck preset avoids per-frame camera motion sampling and actor sweeps. The master switch pauses optimization and restores game settings the plugin owns.

## Install on Steam Deck

1. In Steam Desktop Mode, open Dragon's Dogma 2's **Properties → Installed Files → Browse**. This opens the directory containing `DD2.exe`.
2. Install the [DD2-compatible REFramework build](https://github.com/praydog/REFramework-nightly/releases) if needed. This plugin uses the **1.15.0** SDK and the host's native ImGui exports. Verify that REFramework opens in-game before installing the plugin.
3. If REFramework's `dinput8.dll` is already loading under Proton, keep your existing launch options. If you are setting up REFramework for the first time, use `WINEDLLOVERRIDES="dinput8=n,b" %command%` in **Properties → General → Launch Options**. Merge that override with any existing Wine overrides.
4. Close the game. Copy `reframework/plugins/dd2_foveated_vrs.dll` from the package into the same relative path under the game directory. Create the `plugins` directory if needed.
5. Launch DD2, load a save and open **DD2 Foveated VRS** in REFramework. The panel shows the current activation reason and profile. **Advanced / Debug** shows the tier, tile size and scene size. Enable verbose logging for draw counters.

To uninstall, close DD2 and remove the plugin DLL. Settings apply during gameplay and save on change to `reframework/data/dd2_foveated_vrs.json`. **Recommended Steam Deck settings** at the top of the panel applies and saves the preset. It selects **Force Always On**, an 85% sharp center and **Always Apply Engine Tweaks**. The adaptive NPC cap starts at 20, subject to the game cap, and the stray-pawn baseline starts at 2. An existing `force_always_on: true` setting migrates to **Force Always On** unless the file already specifies an activation mode.

**Switch Optimizer Off** stops new VRS overrides and begins restoring owned engine settings and crowd caps on the next gameplay update. The command-list hooks detach after two rendering frames once outstanding recordings have restored the game’s VRS state. **Switch Optimizer On** reattaches the hooks and resumes the saved configuration.

Open **Center Size** to adjust **Sharp Center Size**, from 0% for an entirely coarse VRS mask to 100% for full-screen 1x1 shading. **Lock Center Aspect Ratio** is on by default. Uncheck it for separate **Center Width** and **Center Height** controls. Either axis at 0% removes the sharp center. Set both axes to 100% for full-screen 1x1. Re-locking uses the larger axis size. **Reset Center to Profile Default** restores the profile's starting mask. Older settings retain their prior mask until the controls are edited.

## Test in DD2

1. Load an outdoor scene. Check for **VRS Tier 2**, a valid tile size and a detected scene size. The plugin caps its rates at `2x2`.
2. Select **Force Always On**. Check for **ACTIVE (Forced)**. Enable **Verbose Audit Logging** to inspect shaded draw counters. Switch between **Default** and **Extreme** in the same view. Check the outer geometry, center, HUD and menus.
3. Enable your normal FSR 3 and frame-generation settings. Repeat stationary and fast pans and check HUD sharpness, disocclusion and motion trails. Compare GPU frame time and frame pacing in the same scene with identical graphics settings. For a clean baseline, measure once with the DLL removed to include the installed hooks' CPU cost in the comparison.
4. Select **Adaptive (Motion + Combat)** or **Motion Only** to test camera-motion activation. Pan the camera, then hold it still and check that VRS responds to the configured threshold and hold time.
5. **Advanced / Debug** shows the scene and output sizes. Turn off upscaling and test at 1280×800 or 1280×720. **Allow VRS at Native Resolution** defaults to on. Compare both settings with **Force Always On**. With verbose logging enabled, `image_bound_at_om` counts accepted bindings with a ready image, `transparent_image_bound` covers transparent passes, and `output_excluded` counts known presentation buffers and output-sized composites rejected by the native pass filter.

A device that reports Tier 0 or Tier 1 stays inactive. The plugin needs tracked descriptors and a ready image for the current scene size. Persistent descriptors created before the hooks attach can remain untracked until the engine recreates them. The plugin applies the override to qualifying classic output-merger bindings and resets it before D3D12 render-pass API calls and bundles. Force mode changes camera gating and preserves the target checks.

Enable **Verbose Audit Logging** under **Advanced / Debug** to collect and write five-second summaries under `[DD2 VRS]` in `re2_framework_log.txt`. The checkbox defaults to off. Startup and error messages remain enabled. The `Safety` line shows `pipeline_policy=compute_only`, counts image clears, and reports indirect calls skipped for tracked dispatch, mesh, ray, or unsupported signatures.

- `Audit path` separates direct calls and indirect actions. It records the first target filter hit, whether the plugin image was bound, and the observed graphics or compute PSO. A signature created before the hook attached can appear as `indirect_untracked`.
- `Audit signature` records the busiest captured signatures, including their action, argument types and stride. `max_commands_sum` is the sum of the CPU-supplied upper bounds. A GPU count buffer can reduce actual execution. The audit reads signature metadata and leaves GPU argument and count buffers untouched.
- `Audit VRS state` and paired draw and state samples correlate the rate, both combiners and image binding with RTV and DSV formats and extents, viewport, and PSO metadata. `pso_diagnostic` describes the available metadata. `first_gate` and `plugin_image_bound` show what happened on the call. Shader IDs come from bytecode headers. Representative sample and signature lists have explicit caps.

The indirect binding counters report CPU calls recorded with the plugin image. Captured DRAW and DRAW_INDEXED signatures and untracked signatures qualify on validated direct scene passes with a matching image. A tracked compute PSO or dispatch signature clears the plugin image.

## VRS activation and engine optimizer

**Force Always On** uses the selected profile without sampling camera motion. **Adaptive (Motion + Combat)** and **Motion Only** can activate the same mask based on gameplay. The background worker rebuilds the mask when the internal extent, profile or center changes. Same-device resize resets scene selection. If the D3D12 device or queue is replaced, restart the game to reinitialize VRS hooks.

The engine optimizer includes SpeedTree, motion quality, contact shadows, shadow quality and resolution, and optional half-resolution SSAO and SSR. Contact Shadows and Shadow Quality use the renderer's RenderConfig. Shadow Resolution and Disable Shadow Filtering use the native LightRenderer singleton. The engine panel reports each binding and whether its value was applied. **Restore Baseline / Edit Settings** pauses native engine and crowd writes and restores values the plugin still owns. **Save Baseline Snapshot** and **Restore Saved Baseline** use `reframework/data/AdaptivePerformanceOptimizer_baseline.json` for graphics settings. **Restore Game NPC / Pawn Caps** clears the manual cap and restores the four cap fields. The automatic NPC cap starts and recovers at or below the observed vanilla cap. **Manual NPC Cap** remains an explicit override.

The optional native cuts use the primary camera's SSRControl and SSAOControl for **Disable SSR**, **SSR 1-Point Resolve**, and **Disable DFAO**. The camera is resolved when those settings are reconciled. **Disable Out-of-View Streaming** uses one toggle for both MPMR streaming flags and reports their bindings separately. Both flags restore their own captured values when the override ends.

The **CPU & Crowd Optimizer** uses elapsed time between gameplay frames to calculate FPS. The default target is 29 FPS, with critical mode below 25 FPS and recovery above 31 FPS. Critical drops can lower caps after the warmup window. Raising caps requires at least five seconds of sustained headroom, a completed adjustment interval, and the configured reversal hysteresis. The rule-change hook, when available, schedules a cap check on the next `UpdateBehavior` callback. The plugin polls the caps every half second, writes only changed fields and verifies all four fields before reporting applied caps. The combined cap is kept in sync when only one of the NPC or stray-pawn controls is enabled.

## Migrate from `perf.lua` v3.0-SD

1. Close DD2. Keep a backup of the currently installed DLL and `perf.lua`. Copy the preview DLL to `reframework/plugins/`.
2. For a native-only test, move `perf.lua` outside `reframework/autorun/` and keep `reframework/data/AdaptivePerformanceOptimizer.json`. The DLL imports that file's v3.0-SD settings at startup. Explicit values in `dd2_foveated_vrs.json` take precedence. Adjusting a control saves the current native settings.
3. In a populated town, check the **CPU & Crowd Optimizer** panel for real NPC and stray-pawn caps. Verify that drops reduce the caps, headroom delays cap increases, and **Restore Game NPC / Pawn Caps** returns the original values. Compare FPS and frame-time spikes in the same route with the same SteamOS frame cap.

If `reframework/autorun/perf.lua` exists at startup, the DLL disables native engine and crowd writes to avoid duplicate optimization. Leave the backup recoverable until the native-only checks pass on your Steam Deck.

## Build

The project requires CMake 3.21+, Git, and a 64-bit Windows C++20 toolchain. CMake fetches MinHook 1.3.4 and nlohmann/json 3.11.3. The included REFramework SDK headers are unmodified. The UI calls the host's cimgui exports inside the registered draw callback so every widget uses the host's ImGui context and allocator.

### Windows with Visual Studio 2022

Install **Desktop development with C++** and a recent Windows SDK with `ID3D12GraphicsCommandList6`, such as Windows SDK 10.0.22000.0 or later.

```powershell
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64
cmake --build build-msvc --config Release
cmake --install build-msvc --config Release --prefix stage --component Runtime
```

The build outputs `build-msvc/Release/dd2_foveated_vrs.dll`. The install command stages `stage/reframework/plugins/dd2_foveated_vrs.dll`.

### Linux with MinGW-w64

Use a recent x86_64 MinGW-w64 toolchain with C++20, the POSIX thread runtime, WRL `ComPtr` and modern D3D12 headers. On a Debian-based build machine or container, install `cmake`, `ninja-build`, `git` and `g++-mingw-w64-x86-64-posix`. Check that `x86_64-w64-mingw32-g++ --version` resolves. The GCC and binutils paths are configurable with the standard CMake compiler variables.

```sh
cmake -S . -B build-mingw -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-x64.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw
cmake --install build-mingw --prefix stage --component Runtime
```

The build outputs `build-mingw/dd2_foveated_vrs.dll`. Copy the staged DLL with the same installation steps.

## Behavior and limits

| Profile | Default center radius | Transition radius | Outer ring |
| --- | --- | --- | --- |
| Default (Balanced Soft) | 45% of reference radius at 1x1 | 70% of reference radius, alternating 2x1 and 1x2 | 2x2 |
| Extreme (Max Performance) | 32% of reference radius at 1x1 | 50% of reference radius, alternating 2x1 and 1x2 | 2x2 |

The center is at 50% of scene width and 46% of scene height. The preset reference radius starts at half the render width and shrinks as needed to fit the initial transition. In the custom 0–100% range, the radius scales up to the farthest screen corner. The transition keeps its profile ratio and can extend past the frame as the center grows. The device's D3D12 feature report selects the checkerboard or a 2x2 transition fallback.

The worker prepares one mask for the selected profile, scene extent and center size. `render_width` and `render_height` in JSON specify the initial image size. Scene detection uses the live DXGI swapchain dimensions, prefers recurring writable-depth MRTs, and requires four consistent observations before selecting a new extent. An output size change invalidates the scene selection until it is confirmed again.

The shared engine trigger polls `app.BattleManager` every second in gameplay using cached metadata for `get_IsBattleMode()` and a short list of typed compatibility getters. It polls `app.TimeManager` every two seconds when the engine night trigger is enabled. Edit `motion_threshold` (default 2.2 rad/s), `hold_seconds` (0.35), and `combat_exit_delay` (2.0) in the JSON file to tune the original motion and combat activation rules.

The plugin modifies command-list shading-rate state and manages its shading-rate images. A qualifying binding has tracked single-sampled 2D color targets at the confirmed scene size, of at least 400×240. With native VRS enabled, full-size targets need valid matching depth or a recognized G-buffer MRT layout. `shade_transparent_passes` controls the read-only depth and depthless paths. The final DXGI swapchain resources are excluded by identity. At native resolution, a single depthless full-size composite stays at full shading rate.

The selected combat getter appears as `battle_source` in verbose diagnostics and in **Advanced / Debug**. Use the source and `in_combat` to check transitions into and out of an encounter.

The pipeline gate excludes tracked compute PSOs. The plugin clears its image and returns to 1x1 before compute, raytracing, mesh, and other excluded work. While bound, it uses PASSTHROUGH/MAX combiners and caps its base rate at 2x2.

The in-game RE Engine bindings and Steam Deck performance require testing in DD2.

## Reference code

- [Official REFramework SDK headers](https://github.com/praydog/REFramework/tree/master/include/reframework) and [example C++ plugin](https://github.com/praydog/REFramework/tree/master/examples/example_plugin)
- [REFramework primary camera implementation](https://github.com/praydog/REFramework/blob/master/shared/sdk/SceneManager.cpp) and [native cimgui bindings](https://github.com/praydog/REFramework/blob/master/src/cimgui/cimgui.h)
- [D3D12 variable-rate shading](https://learn.microsoft.com/en-us/windows/win32/direct3d12/vrs) and [VKD3D-Proton](https://github.com/HansKristian-Work/vkd3d-proton)
