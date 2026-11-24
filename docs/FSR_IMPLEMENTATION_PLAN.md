# ITX Display FSR 1 implementation plan

Status: Phase A GLES pass-through has been confirmed on the Xiaomi Pad 5 Pro / Adreno 650. Phase B has an initial, opt-in AMD FSR1 RCAS fragment pass for native 1920x1080 input. A v0.6.3 device report showed RCAS shader unavailable. Code inspection found malformed escaped newlines in the GLSL source prefix; v0.6.4 corrects these and adds shader-stage error codes. The RGB experiment LUT is also now applied in the GLES path (and before RCAS when both are enabled). These fixes still require an Android build and device validation. EASU 720p-to-1080p is not yet implemented.

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
- [x] Device report confirmed GLES3 GPU pass-through active: 4272 frames submitted, 4269 swaps, 2 replaced frames, zero GL/EGL error.
- [x] Separate upload-call and draw-plus-swap timings are present. `glTexImage2D/glTexSubImage2D` timing is CPU time inside the GL upload calls, not a GPU hardware timer; draw-plus-swap includes any `eglSwapBuffers` wait and is not pure shader execution time.
- [ ] Verify output orientation/aspect/color against the CPU preview and validate Surface recreation under the current diagnostics.

### Phase B - FSR 1 shaders
- [x] Vendor the official AMD `ffx_a.h`, `ffx_fsr1.h` and MIT license; embed the headers into runtime GLSL source so no non-standard shader include extension is required.
- [x] Add an opt-in, native-1080p RCAS fragment pass behind a UI toggle. Default remains off; initial sharpening attenuation is conservative (2.5 stops).
- [x] Apply the same 256-entry RGB experiment LUT in the GLES pass-through shader, and in the RCAS input callback when both features are enabled.
- [x] Correct the malformed escaped-newline GLSL prefix found after v0.6.3 testing; expose shader stage error codes (fragment compile=1, vertex compile=2, link=3, output FBO=4) and Logcat compile logs.
- [x] Add RCAS availability, applied-frame and CPU draw-call timing diagnostics. The CPU draw-call timing is not a GPU hardware timer.
- [ ] Build v0.6.4 and device-test the RCAS shader on Adreno 650; compare RGB correction on/off, fine text, grayscale ramps, edges and MJPEG ringing with FSR off/on.
- [ ] Implement EASU for 1280x720 -> 1920x1080, followed by RCAS. Native 1920x1080 should skip EASU and run RCAS only.
- [ ] Add a real user-adjustable conservative sharpening attenuation control after the initial visual quality test.
- [ ] Validate shader output against AMD reference images/test vectors before enabling it for live preview.

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
- [x] Native 1080p RCAS mode, availability, applied-frame count and CPU draw-call time.
- [ ] EASU timings and 720p-to-1080p FSR mode.
- [ ] GPU renderer/vendor/version and detailed shader compile errors.
