# ITX Display FSR 1 implementation plan

Status: Phase A pass-through GLES renderer prototype is implemented on the development branch. It attempts GLES for preview and falls back to the CPU blitter if EGL/GLES initialization fails. The code has not yet been independently build-verified or confirmed by a renderer-specific device report. FSR EASU/RCAS is not implemented.

## User-selected scope

- 1280x720 capture -> 1920x1080 output using AMD FidelityFX Super Resolution 1 (EASU followed by RCAS).
- Native 1920x1080 capture -> 1920x1080 output using RCAS sharpening only.
- Balanced image quality and latency.
- Preserve the current 1080p50 UVC preview as the default and as an always-available fallback.
- Do not run RIFE at the same time as FSR in the first implementation.

## Why FSR 1

FSR 1 is a spatial, per-frame algorithm. EASU reconstructs edges while scaling; RCAS performs contrast-adaptive sharpening. It needs neither motion vectors nor frame history, unlike temporal upscalers. The upstream AMD implementation supports Vulkan and provides shader source under the MIT license.

References:
- https://gpuopen.com/fidelityfx-superresolution/
- https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-spatial/
- https://github.com/GPUOpen-Effects/FidelityFX-FSR

## Current pipeline limitation

The current native callback performs MJPEG/YUYV -> RGB conversion on CPU, then writes pixels into an ANativeWindow using ANativeWindow_lock()/unlockAndPost(). This is a CPU blit path; it cannot simply insert a Vulkan compute pass into the existing Surface lock operation.

The first FSR integration therefore needs a separate GPU-backed renderer rather than attaching a shader to the existing CPU blitter.

## Target architecture

1. UVC callback decodes the current frame to RGB using the existing known-good path.
2. Callback copies/publishes the newest RGB frame into a small bounded frame queue and returns quickly. Queue policy is latest-frame-wins; never accumulate latency.
3. A dedicated render thread owns EGL/GLES resources and an EGL window surface backed by the Android Surface/ANativeWindow.
4. Upload the RGB frame to an input texture. Keep color/range handling unchanged initially; no automatic RGB/YCbCr or Full/Limited transforms.
5. For 1280x720 input: dispatch EASU into a 1920x1080 intermediate/output texture, then RCAS into the final texture.
6. For 1920x1080 input: skip EASU and run RCAS only.
7. Draw the final texture to the EGL window surface with aspect-preserving viewport/letterboxing. Retain the current CPU preview path as a runtime fallback.
8. All GL objects are created, used and destroyed on the render thread. Surface loss/recreation must stop and recreate the EGL surface safely.

## Implementation sequence

### Phase A - renderer foundation
- [x] Add a dedicated GLES render thread with EGL window-surface/context lifecycle and a simple pass-through shader.
- [x] Add a bounded, latest-frame-wins RGB handoff from the UVC callback; the render queue is limited to one pending frame.
- [x] Add CPU fallback when GLES initialization fails, and restore RGBX buffer geometry if the GLES render thread fails later.
- [x] Expose renderer state, submitted/rendered/replaced frame counts, GL texture-upload call time, and draw-plus-swap time in native diagnostics.
- [ ] Build the current branch and verify on Xiaomi Pad 5 Pro that GLES is actually active, output orientation/aspect/color match the CPU preview, and the Surface lifecycle is stable.
- [ ] Compare diagnostic measurements before treating the GPU path as validated. `glTexImage2D/glTexSubImage2D` timing is CPU time inside the GL upload calls, not a GPU hardware timer; draw-plus-swap includes any `eglSwapBuffers` wait and is not pure shader execution time.

### Phase B - FSR 1 shaders
- Port the upstream EASU and RCAS shader logic to GLSL ES 3.1 compute shaders compatible with the Android GLES implementation, or use a Vulkan compute backend if the GLES path cannot meet correctness/performance requirements.
- Preserve AMD's MIT copyright/license notice and include the upstream license in THIRD_PARTY_NOTICES.
- EASU only for 720p -> 1080p. RCAS only for native 1080p sharpening.
- Expose sharpening attenuation as a conservative user setting; do not hard-code maximum sharpening.
- Validate shader output against AMD reference images/test vectors before enabling it for live preview.

### Phase C - device validation
- Confirm the capture card actually negotiates 1280x720 at a stable frame rate; do not synthesize a 720p input by downscaling 1080p for the upscale quality test.
- Measure decode, RGB upload, EASU, RCAS, final draw/present, total frame time, queue wait, dropped frames and thermal behavior separately.
- Target <=20 ms average processing/presentation budget for 50 FPS, while reporting p95/p99 and worst-case stalls. Averages alone are insufficient.
- Compare CPU fallback, GPU pass-through, 720p EASU+RCAS, and 1080p RCAS-only.
- FSR is optional and must automatically fall back to the original CPU preview if EGL/GLES initialization or shader compilation fails.

## Important quality constraints

- FSR 1 is not frame interpolation and cannot generate intermediate frames.
- Upscaling 720p to 1080p can improve edge reconstruction, not recover detail absent from the 720p source.
- RCAS can exaggerate MJPEG ringing, chroma artifacts and noise. Start with low sharpening and compare on text, fine lines, grayscale ramps and natural video.
- AMD's guidance expects perceptual (sRGB-like) input. The app's existing color/range interpretation must remain unchanged until independently validated.
- No claim of real-time performance is made until the target Xiaomi Pad 5 Pro / Adreno 650 has been tested.

## Diagnostics status and remaining fields

- [x] Renderer backend: CPU fallback / GLES active / GLES stopped and CPU fallback.
- [x] Submitted, successfully swapped, and replaced-frame counts.
- [x] CPU time spent in GL texture upload calls and draw-plus-`eglSwapBuffers` time.
- [ ] Input/output dimensions and actual preview FPS on the GLES thread.
- [ ] GPU timer-query measurements if supported; otherwise clearly retain CPU-side GL call timings.
- [ ] EASU / RCAS timings and FSR mode after those shader passes are implemented.
- [ ] GPU renderer/vendor/version and detailed shader compile errors.
