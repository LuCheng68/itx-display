# ITX Display v0.6.1

Low-latency Android USB UVC video capture and display software focused on real-time frame interpolation and super-resolution for Xiaomi Pad 5 Pro (Android 15).

## Target configuration

- Application version: 0.6.1 (versionCode 21)
- Language: Java (Android UI and application layer)
- compileSdk / targetSdk: 35
- Native ABI: arm64-v8a only (Snapdragon 870 / ARM64 target)
- minSdk: 24
- JDK: 17
- UVC engine: locally maintained `libraries/droiduvc` module based on droiduvc 0.2.1
- Preferred input: MJPEG
- Requested mode: 1920x1080 at 50 FPS; actual negotiated mode is reported in the information panel
- Audio: not yet implemented
- Orientation: landscape; screen stays awake and immersive

## Implemented

- Detect USB devices exposing a Video Class interface.
- Display product name, VID/PID and USB interface count.
- Request Android CAMERA runtime permission and per-device USB permission.
- Open the granted USB connection with droiduvc.
- Read the renderable MJPEG/YUYV modes advertised by the capture device.
- Request 1920x1080 at 50 FPS with MJPEG preferred, then show the actual negotiated mode.
- Render the UVC stream into a TextureView.
- Keep the full video frame visible by sizing the TextureView layout itself to the negotiated stream aspect ratio; the parent view supplies black letterbox bars (no stretch, no crop).
- Keep device details and performance information hidden by default; use the top-right “显示信息” button to reveal or hide the information panel.
- Show the negotiated input mode and a TextureView update-rate estimate in the information overlay.
- Stop and release the UVC camera, Surface and USB connection as the app is paused or the surface is destroyed.
- Rescan when USB devices attach or detach.
- Make manual USB rescans idempotent: when the selected UVC preview is already opening or running, rescanning does not open a second UVC session or interrupt the active preview.
- Close the previous UVC camera, Surface and USB connection on the serialized camera executor before opening a replacement session.

## Performance metric note

The displayed FPS is counted from TextureView frame-update callbacks. It is a useful display update-rate estimate, not a hardware-level measurement of USB transport FPS or a guarantee that every input frame reached the display.

## v0.4.1 fixes

- Fixed repeated UVC open attempts during manual USB rescans, which could return `Busy (-6)` while the preview session was already active.
- Separated the active-preview state from the in-progress-opening state so rescan does not treat a running camera as idle.
- Adjusted preview sizing to match the actual negotiated video aspect ratio and keep letterboxing black.
- Set the `SurfaceTexture` default buffer dimensions for the requested stream and then for the negotiated stream mode.

## v0.4.2 startup stability changes

- Restored the original full-size TextureView with the centered aspect-ratio matrix fit used before v0.4.1.
- Removed the additional SurfaceTexture buffer-size mutations introduced in v0.4.1, including the update after the UVC stream starts.
- Kept the guard that prevents opening a second UVC session when the current preview is already opening or running.
- Guarded camera-executor submissions during Activity teardown to avoid rejected tasks from late SurfaceTexture lifecycle callbacks.

The app still needs to be built and tested on the Xiaomi Pad 5 Pro. These changes are a conservative startup-stability rollback, not proof that the reported crash is fixed. If the app still exits at launch, collect Logcat from the affected device to identify the exact exception.

## Hardware validation still required

The live UVC path has been exercised on the target setup, but the current source update still needs a fresh cloud build and installation test. The MS2130/Hagibis capture card must be checked for actual supported modes, negotiated resolution/frame rate, sustained frame rate and stability. A requested 1080p50 mode does not guarantee the capture card exposes or can sustain that mode.

## Product positioning

ITX Display is an Android display and video-processing application centered on **low-latency USB UVC capture, real-time frame interpolation, and super-resolution**. The goal is not only to show the capture-card signal, but to build a low-latency video pipeline that can optionally generate intermediate frames and enhance image detail on the tablet.

### Development priority

1. Make UVC capture, preview, reconnect and frame delivery reliable.
2. Measure and reduce end-to-end latency; build the frame-timing and queue infrastructure needed by real-time processing.
3. Establish correct HDMI/UVC color interpretation (pixel format, Full/Limited range, and color matrix) as part of the base video pipeline.
4. **Prioritize frame interpolation adaptation and real-time feasibility testing.**
5. Add super-resolution as the next major image-processing capability.
6. Integrate and tune the capture, interpolation, super-resolution and display stages together.
7. Add audio capture, playback and A/V synchronization last, after the visual pipeline is stable.

Frame interpolation and super-resolution are planned capabilities, not yet implemented. Their real-time performance and visual quality must be measured on the target tablet before being advertised as supported.

### HDMI / UVC color compatibility baseline

A real-device test found that the captured video looked over-bright when the PC graphics output was set to RGB: light gray and white became difficult to distinguish. The app UI remained normal, and other USB camera apps showed the same video issue. Switching the graphics output to YCbCr restored the captured image. At 1080p, the user observed little apparent detail difference between YCbCr 4:4:4, 4:2:2 and 4:2:0 in the tested content.

This is a confirmed workaround, not yet a confirmed root-cause diagnosis. The graphics driver does not expose a usable RGB range selector in this setup. The roadmap therefore includes software-side RGB compatibility and range/matrix validation without requiring the PC to stop using RGB.

Implementation must inspect the actual UVC payload format (for example MJPEG or YUY2/YUYV) and any reliable color metadata. RGB vs. YCbCr encoding and Full vs. Limited quantization range are separate properties. If reliable metadata is unavailable, provide explicit manual choices rather than claiming content-only histogram analysis can always detect the correct range. Validate with grayscale ramps and raw UVC frame values before enabling corrections.

Color normalization belongs before frame interpolation and super-resolution. A GPU shader path is preferred to avoid CPU per-pixel work and unnecessary frame copies. If the capture card has already clipped highlight values before USB delivery, software cannot recover those lost gray levels; keep the currently verified YCbCr path as a compatibility fallback.

## Roadmap

- First: low-latency UVC pipeline, accurate timing and performance diagnostics.
- Next: frame interpolation proof-of-concept, then optional real-time integration.
- Then: super-resolution proof-of-concept and optional real-time integration.
- Later: tune the combined processing pipeline and expose user-selectable quality/latency settings.
- Last: USB Audio Class capture, playback, volume control and A/V synchronization, after confirming the capture card's audio interface.

## Build locally

Use JDK 17 and Android SDK 35:

```bash
gradle assembleDebug
```

APK output:

`app/build/outputs/apk/debug/app-debug.apk`

## Codemagic

The repository root contains `codemagic.yaml`. It is configured for a macOS M2 instance and the `itx-display-debug` workflow.

- Pushes to the `main` branch are configured to trigger the debug APK workflow.
- The repository must be connected in Codemagic and its GitHub webhook must be enabled for automatic triggering.
- A manual build can also be started in Codemagic by selecting the `main` branch and the `itx-display-debug` workflow.
- The debug APK is collected as a build artifact.

## USB permissions

Android requires the CAMERA runtime permission and per-device USB permission for UVC access. The app does not request microphone permission.

## License notes

droiduvc is Apache-2.0, and its bundled native dependencies have their own licenses. Review the upstream notices and LGPL obligations before redistributing a production APK.

## v0.4.3 preview aspect-ratio fix

- Replaced the TextureView content matrix with a centered layout size calculated from the actual negotiated UVC width and height.
- The preview view now occupies only the fitted frame; the black root view remains visible as letterboxing on a screen with a different aspect ratio.
- Recalculates the fitted bounds when the root viewport changes.
- Keeps the UVC duplicate-open guard and does not modify SurfaceTexture buffer dimensions.



## v0.4.4 UVC color diagnostics baseline

- Added a read-only UVC diagnostics panel showing camera manufacturer/product strings, VID/PID, UVC version, negotiated stream mode, and whether common color-related UVC controls are declared by the device.
- Added an explicit user action to read current raw UVC values for brightness, contrast, gamma, saturation, hue, gain and backlight compensation on the camera executor. Values are diagnostic only; the app does not change hardware settings.
- Added a copy-diagnostics action so a device report can be shared for troubleshooting.
- The report explicitly states that droiduvc 0.2.1 converts MJPEG/YUYV frames to RGB in its native callback and does not expose raw UVC frame bytes or a reliable HDMI RGB Full/Limited metadata flag to the app.
- This is a diagnostic foundation, not an RGB range correction feature. Software correction must not be enabled until the actual conversion path and grayscale ramp values are validated. If the capture card has already clipped highlights before USB delivery, software cannot restore them.


## v0.4.5 diagnostics panel layout fix

- Limited the information panel to the visible viewport: up to 78% of the available height and 760 dp wide, with margins.
- Put long UVC mode and device diagnostics text inside a vertical ScrollView.
- Kept the read-color-controls, copy-diagnostics-report and rescan buttons in a fixed footer, so the copy action remains visible without scrolling the report.
- Kept the information panel hidden by default and retained the existing video preview and UVC session behavior.


## v0.4.6 UVC control range diagnostics

- Extended the read-only UVC control report to include each supported control's current value, minimum, maximum, step and device-reported default when available.
- Current value and range are queried independently; a device that supports GET_CUR but does not answer a range request still retains its current-value result.
- The app continues to perform diagnostics only. It does not write or reset UVC controls.


### Measured Hagibis UVC color-control baseline (user device)

Device: MACROSILICON / Hagibis, VID:PID 345F:2130, UVC 1.00. Active input mode: 1920×1080 MJPEG at 50 FPS; the device reports 84 advertised modes.

Read-only UVC control query returned:

| Control | Supported | Current | Range | Step | Device default |
|---|---:|---:|---:|---:|---:|
| Brightness | Yes | 50 | 0–100 | 1 | 50 |
| Contrast | Yes | 50 | 0–100 | 1 | 50 |
| Saturation | Yes | 50 | 0–100 | 1 | 50 |
| Hue | Yes | 50 | 0–100 | 1 | 50 |
| Gamma | Not declared | — | — | — | — |
| Gain | Not declared | — | — | — | — |
| Backlight compensation | Not declared | — | — | — | — |

Interpretation: all four readable controls are currently at the device-reported default. This gives no evidence that ITX Display changed these controls away from their defaults. It does not prove that 50 is a perceptually neutral value, nor does it identify the root cause of the RGB-only highlight issue. Keep the current YCbCr output as the verified workaround while investigating raw UVC payload values. No automatic control writes or resets should be made.


## v0.5.0 raw UVC payload pipeline (development branch)

- Replaced the Maven droiduvc binary dependency with a local, patchable Android library module based on upstream droiduvc v0.2.1. The upstream Apache-2.0 license is retained in the module; its libjpeg-turbo, libusb and libuvc dependencies are fetched by CMake at pinned upstream commits.
- Added a native JNI raw-frame observer invoked before `uvc_any2rgb()`. The observer receives a borrowed direct ByteBuffer over the libuvc frame and must copy it synchronously before returning.
- Added an app-side three-slot reusable buffer pool and a two-frame bounded latest-frame queue. When full, the oldest queued analysis frame is discarded instead of building latency.
- Added a worker-side MJPEG header parser that reports JPEG chroma sampling (4:2:0 / 4:2:2 / 4:4:4 when present), frame size, callback FPS, analysis FPS, sequence gaps and queue drops.
- The original droiduvc Surface preview remains active in parallel. This is the raw payload acquisition/queue foundation only: it does not yet decode the raw payload for display, perform RGB range correction, or run GPU color conversion/interpolation.


## v0.5.1 native performance and diagnostics UI

- The Codemagic build log showed the locally built libjpeg-turbo using CMAKE_BUILD_TYPE=Debug with no optimization flags. The native video hot path is now explicitly compiled with `-O2` in debuggable APKs: libjpeg-turbo is built as Release; libusb, libuvc, droiduvc and the RGB blitter are optimized.
- Disabled unused libjpeg-turbo tools and tests in the Android build.
- Separated live raw-UVC metrics from the manual UVC color-control diagnostic text. FPS updates no longer overwrite the color-control values shown after the user taps “Read UVC color controls”.
- This is a performance hypothesis supported by the build flags, not yet a verified FPS improvement. The current preview is still CPU MJPEG decode + RGB conversion + CPU blit; GPU shader rendering is not implemented yet.


## v0.5.2 Snapdragon 870 diagnostics and RGB highlight probe

- Added native timing counters for the raw-frame observer/JNI copy, MJPEG-to-RGB decode, Surface lock/blit/post, and total native frame callback. The counters are cumulative and presented as averages plus the decode maximum; this identifies the actual hot stage before applying CPU affinity or adding worker threads.
- The raw-frame header analyzer now runs at Android background thread priority. No CPU-core affinity is forced: Android/Qualcomm's scheduler remains free to place threads on the appropriate Kryo 585 cluster.
- Added a user-triggered one-frame grayscale chart probe. It decodes only one selected MJPEG frame at half resolution on the background analysis worker and samples ten fixed patch centers (expected values 16, 32, 64, 96, 128, 160, 192, 220, 240, 255). It does not add continuous JPEG decoding to the 50 FPS path.
- This probe distinguishes whether the USB MJPEG payload still contains separable highlight levels. If the 220/240/255 samples collapse inside the raw MJPEG decode, the clipping happened before Android rendering; if they remain separated, investigate the existing native conversion and Surface display path.
- No automatic RGB range compensation has been enabled. Black appearing normal while highlights collapse is consistent with high-end clipping, but it is not sufficient evidence to choose Full/Limited expansion blindly.


## v0.5.3 RGB highlight diagnosis and ARM64-only packaging

### Measured grayscale baseline from the Hagibis / MACROSILICON UVC device

Test mode: 1920×1080 MJPEG at 50 FPS, JPEG sampling 4:2:2. The probe decodes one copied original UVC MJPEG payload with Android BitmapFactory at half resolution, before the droiduvc native `uvc_any2rgb()` conversion and before Surface presentation. It samples ten center patches from the grayscale chart.

| Expected | RGB HDMI input | YCbCr 4:4:4 HDMI input |
|---:|---:|---:|
| 16 | 6 | 22 |
| 32 | 26 | 38 |
| 64 | 63 | 71 |
| 96 | 100 | 102 |
| 128 | 137 | 134 |
| 160 | 174 | 164 |
| 192 | 211 | 197 |
| 220 | 243 | 225 |
| 240 | 255 | 244 |
| 255 | 255 | 254 |

Interpretation:
- The RGB-input defect is already present in the UVC MJPEG payload as decoded by the probe; it is not first introduced by the app's later native MJPEG-to-RGB conversion or TextureView/Surface display stage.
- In RGB mode, expected 240 and 255 both measure 255. The high-end has a clipped plateau in this sample; later processing cannot recover the distinction once both levels have become the same value.
- The curve is broadly consistent with a range/level expansion plus an offset and high-end clipping, but this measurement alone does not uniquely identify whether the responsible stage is the capture card's HDMI receiver, its internal image processing/JPEG encoder, or HDMI range signaling. Do not blindly apply a Full/Limited transform to already-decoded JPEG RGB values.
- In YCbCr 4:4:4 mode, the measured top levels remain distinct (244 and 254), matching the user's observation that this path looks more correct. Keep YCbCr as the verified fallback.
- The diagnostic classifier now explicitly flags a 240/255 high-end plateau instead of incorrectly reporting that the highlight steps remain distinguishable.

### Frame continuity observation

A 50 FPS report showed 1,284 callbacks and 1,284 analyzed frames with zero app-side queue drops, but 18 UVC sequence gaps (latest sequence 1,302). The queue's drop count and UVC sequence discontinuities are different metrics. This suggests possible gaps upstream of the app analysis queue and should be rechecked over a longer capture; it is not yet proof of USB transport loss.

### Packaging

- Restrict both the app and local droiduvc native build to `arm64-v8a`; x86_64 is not packaged for the target Snapdragon 870 tablet.
- No changes to the UVC brightness, contrast, saturation or hue controls are made automatically.


## v0.5.5 frame-cadence and queue-latency diagnostics

- Added callback inter-frame interval average and maximum measurements using Android's monotonic clock.
- Added a separate capture-timestamp cadence measurement when libuvc supplies positive, strictly increasing capture-finished timestamps; devices/backends that do not supply usable timestamps are reported explicitly.
- Added average and maximum time from raw-frame callback entry to analysis-worker start. This includes payload copying and queue wait, and is not presented as end-to-end display latency.
- These metrics establish the timing budget for the next frame-interpolation prototype. They do not perform interpolation, add a display-buffering frame, or change the existing preview path.


## v0.5.6 Vulkan device capability probe

- Added a read-only native Vulkan probe using the Android system `libvulkan.so` loader.
- The UVC diagnostics report now lists each Vulkan physical device, API version, driver version, vendor/device IDs, compute and graphics queue availability, local device memory, and selected optional extensions.
- The probe runs during device diagnostics only. It does not access video frames, create a compute workload, load ncnn/RIFE, or change the existing UVC preview path.
- This is a compatibility check before integrating an Android-callable RIFE/ncnn inference library. Vulkan enumeration success alone is not proof that a particular RIFE model can run at 1080p50.


## v0.5.7 Java/Kotlin Vulkan probe interop fix

- Exposed `UvcCamera.vulkanDeviceInfo()` as a Java-visible static method with `@JvmStatic`, matching the call site in `MainActivity`.


## v0.5.8 frame-jitter diagnostics and report identity

- Clarified that libuvc `capture_time_finished` is a host-side frame-completion timestamp, not a camera hardware clock.
- Added maximum libuvc frame-completion interval and cumulative interval counts above 30, 40 and 60 ms for both callback-arrival and libuvc completion timestamps. This helps distinguish occasional callback scheduling spikes from broader frame-delivery jitter.
- Added app version/versionCode to the copied UVC diagnostic report so reports can be associated with the APK that produced them.
- Added the native decode/blit timing summary to the copied report, alongside the existing UVC queue and timing metrics.


## v0.6.0 RIFE 4.6 Vulkan throughput experiment

- Added pinned RIFE ncnn Vulkan and ncnn source dependencies for ARM64 Android builds, using the Android system Vulkan loader and the already-tested Adreno 650 compute path.
- Added an explicit user-triggered download of the RIFE 4.6 model into app-private storage. Both model files are checked for exact size and Git blob SHA-1 before they are passed to native code.
- Added a native RIFE 4.6 Vulkan engine with a three-slot RGB frame pool and a bounded latest-frame queue. The worker processes four of every five consecutive 50 FPS input intervals (40 generated-frame inference attempts per second, approximately 90 FPS total target) and skips one interval in five.
- Added inference and RGB-copy timing, throughput, skip and drop counters to the diagnostic report.
- **This release is a throughput-validation build only.** RIFE output is currently measured and discarded; the existing UVC Surface path still displays original frames at the input frame rate. Model loading and inference remain off by default. No claim is made that Adreno 650 can sustain 1080p90 until the device test confirms it.
- The RIFE and ncnn source projects retain their upstream licenses; review the included upstream license files before redistributing.


## v0.6.1 RIFE FP16 arithmetic and bounded stress test

- The pinned RIFE source previously forced `use_fp16_arithmetic = false`. CMake now applies a small, checked source patch so ncnn enables FP16 arithmetic only when the selected Vulkan device reports `support_fp16_arithmetic()`; unsupported devices keep the upstream-safe setting.
- The RIFE device summary reports whether FP16 arithmetic is enabled, making the hardware capability and selected path visible in the copied diagnostic report.
- The manual 1080p RIFE throughput stress test now stops automatically after 15 seconds. This prevents a forgotten benchmark from continuously saturating the Adreno GPU and disturbing the low-latency UVC preview.
- This remains a throughput-validation experiment: generated frames are measured and discarded. The preview still displays the original UVC stream. FP16 numerical quality and speed must be checked on the Xiaomi Pad 5 Pro; no 1080p90 playback claim is made.


## v0.6.2 low-resolution RIFE throughput experiment

- Keep the UVC negotiated capture and Surface preview at the original resolution (1920×1080 on the current device). The optional RIFE benchmark now reduces only its private inference copy to an aspect-preserving maximum of 320×180 before enqueueing it; the original display buffer is not modified.
- The reduction is nearest-neighbour and intentionally lightweight on the UVC callback thread. This is a throughput experiment, not the final image-quality resampler.
- Reduce benchmark cadence to one attempted intermediate frame per two consecutive input intervals: up to 25 inference attempts/s and a theoretical 75 FPS total with a 50 FPS source. This is a more conservative target than the v0.6.0 90 FPS experiment.
- The diagnostic device string reports the RIFE input dimensions. The RGB copy metric now measures the low-resolution reduction/copy, not a full-resolution frame copy.
- The generated 320×180 outputs are still discarded; they are not presented or upscaled. A 15-second automatic benchmark limit remains enabled.
- This build is intended to establish whether the Adreno 650 can sustain low-resolution inference without disrupting the 1080p UVC preview. It does not claim 75 FPS or 90 FPS playback until measured on device.
