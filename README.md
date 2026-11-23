# ITX Display v0.5.0

Low-latency Android USB UVC video capture and display software focused on real-time frame interpolation and super-resolution for Xiaomi Pad 5 Pro (Android 15).

## Target configuration

- Application version: 0.5.0 (versionCode 11)
- Language: Java (Android UI and application layer)
- compileSdk / targetSdk: 35
- minSdk: 24
- JDK: 17
- UVC engine dependency: `io.github.mikimn:droiduvc:0.2.1`
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
