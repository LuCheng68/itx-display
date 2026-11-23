# ITX Display v0.4.1

Android USB UVC capture display for Xiaomi Pad 5 Pro (Android 15).

## Target configuration

- Application version: 0.4.1 (versionCode 5)
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
- Keep the full video frame visible by sizing the TextureView to the negotiated stream aspect ratio; unused space remains black (no stretch, no crop).
- Set the SurfaceTexture buffer size to the target mode before streaming, then update it to the actual negotiated mode.
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

## Hardware validation still required

The live UVC path has been exercised on the target setup, but the current source update still needs a fresh cloud build and installation test. The MS2130/Hagibis capture card must be checked for actual supported modes, negotiated resolution/frame rate, sustained frame rate and stability. A requested 1080p50 mode does not guarantee the capture card exposes or can sustain that mode.

## Roadmap

- Mode picker for supported resolution, format and frame rate.
- Better performance diagnostics (input/output FPS, frame timing and dropped-frame estimates where measurable).
- USB Audio Class capture, volume control and audio/video synchronization, after confirming the capture card's audio interface.
- Frame interpolation and super-resolution as optional, separately benchmarked processing stages. They are not yet implemented.

## Build locally

Use JDK 17 and Android SDK 35:

```bash
gradle assembleDebug
```

APK output:

`app/build/outputs/apk/debug/app-debug.apk`

## Codemagic

The repository root contains `codemagic.yaml`. It is configured for a macOS M2 instance and the `itx-display-debug` workflow. Add this repository in Codemagic, select the `main` branch and run the workflow. The debug APK is collected as a build artifact.

## USB permissions

Android requires the CAMERA runtime permission and per-device USB permission for UVC access. The app does not request microphone permission.

## License notes

droiduvc is Apache-2.0, and its bundled native dependencies have their own licenses. Review the upstream notices and LGPL obligations before redistributing a production APK.
