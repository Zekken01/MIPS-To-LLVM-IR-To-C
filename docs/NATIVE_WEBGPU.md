# Native WebGPU GS renderer (SDBZ HLE)

The native WebGPU backend is extracted and adapted from
[twist84/ps2-web-recomp, SDBZ](https://github.com/twist84/ps2-web-recomp/tree/SDBZ)
and uses **wgpu-native**. There is no browser frontend, WebAssembly,
Emscripten, JavaScript, SDL, or window required.

## Implementation

- `src/runtime/ps2_webgpu/gs/gpu/gpu.cpp`: native WebGPU compute
  upload, copy, CLUT, raster, resolve, snapshot, decode, and GS display
  pipelines; native wgpu surface support remains available in the source.
- `src/runtime/ps2_webgpu/gs/gpu/gs.wgsl`: the reference GS compute
  shader, including VRAM swizzle, texture formats and primitive raster.
- `src/runtime/ps2_webgpu/gs/gpu/blit.wgsl`: the reference present/blit
  shader; no window is needed for diagnostic readback.
- `src/runtime/ps2_webgpu/gs/swizzle.cpp`: swizzle tables needed by the
  imported compute renderer.
- `src/runtime/ee_webgpu_bridge.cpp`: adapts the recompiler's **committed**
  GIF events to the reference GS register, primitive and image-transfer API.
  It has no path to fake DMA success, IRQs, VBlank, or guest execution.
- `src/tools/ee_game_runner_main.cpp`: optional native, offscreen
  GPU-processed GS VRAM snapshot. It does not claim that the guest presented
  a frame.
- `tests/smoke/ee_webgpu_native_smoke.cpp`: real GPU upload, raster and
  readback test without game files or a window.
- `.github/workflows/native-webgpu.yml`: native Linux and Windows
  build verification and Linux software-Vulkan smoke testing.

The existing CPU GS state and 4-MiB VRAM remain authoritative for the
emulated machine. The GPU bridge is a **one-way render mirror**, fed only
after the CPU GS transaction has committed. A failure to initialise the GPU
does not change the semantics of a normal HLE boot.

## Windows (MSVC / LLVM)

Download the `wgpu-native` **Windows x86-64 MSVC** release compatible
with the native WebGPU workflow (currently `v29.0.1.1`), and extract its
`include/` and `lib/` directories into
`third_party/wgpu-native/`.

Then, from a Windows command prompt in the repository root:

```bat
git pull
set EE_WEBGPU_SNAPSHOT=1
set EE_WEBGPU_ROOT=%CD%\third_party\wgpu-native
run_ps2_game.bat
```

This configures `-DEE_ENABLE_WEBGPU=ON`, builds the native renderer,
and requests a GPU snapshot at
`build\ps2-game-hle\gs-webgpu.ppm`.

The existing `gs-display.ppm` (CPU GS VRAM display readback) and
`gs-upload.ppm` (completed host-to-local texture transfer) remain
separate diagnostics. Neither an upload nor a GPU snapshot on its own
proves that a real guest present event occurred.

For a standalone native GPU smoke test:

```bat
cmake --build build --config Release --target ee_webgpu_native_smoke
ctest --test-dir build -C Release -R ee_webgpu_native_smoke --output-on-failure
```

Disable the opt-in by clearing `EE_WEBGPU_SNAPSHOT` or setting it to 0,
then rerun `run_ps2_game.bat`. The launcher explicitly configures
`-DEE_ENABLE_WEBGPU=OFF` for that mode, even if CMake previously cached
the GPU-enabled build.

## Current acceptance boundaries

The native backend and its synthetic GPU smoke tests exist, but the
user's next full SDBZ native run with `EE_WEBGPU_SNAPSHOT=1` is still
needed to validate a **game-driven** GPU image. The GPU implementation
cannot invent GS primitives, renderer commands, VBlank events, or valid
framebuffer state that the guest has not produced. Keep the existing
HLE and DMA tests and the strict matched-log workflow.
