# ITX Display FSR 1 implementation plan

Status: design baseline for the FSR implementation on the development branch. This document is not a claim that FSR rendering is already implemented or device-validated.

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
- Add an opt-in GLES renderer behind a feature flag; default remains the existing CPU renderer.
- Add a bounded RGB frame handoff from the UVC callback to the render thread.
- Add EGL context/surface lifecycle, RGB texture upload, simple pass-through shader and timing counters.
- Validate that pass-through GPU rendering matches the existing preview before enabling FSR shaders.

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

## Initial diagnostics to add

- Renderer: CPU fallback / GLES / Vulkan.
- Input and output dimensions.
- Texture upload ms, EASU ms, RCAS ms, draw/present ms.
- Queue depth, dropped frames, rendered FPS.
- FSR mode: Off / 720p EASU+RCAS / 1080p RCAS.
- GPU renderer/vendor/version and shader compile errors.
