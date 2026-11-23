# ITX Display v0.2.0

Android USB UVC capture display for Xiaomi Pad 5 Pro (Android 15).

## Target configuration

- Application version: 0.2.0 (versionCode 2)
- Language: Java (Android UI and application layer)
- compileSdk / targetSdk: 35
- minSdk: 24
- JDK: 17
- UVC engine dependency: `io.github.mikimn:droiduvc:0.2.1`
- Preferred input: MJPEG
- Requested mode: 1920x1080 at 50 FPS; actual negotiated mode is reported in-app
- Audio: not used
- Orientation: landscape; screen stays awake and immersive

## Implemented

- Detect USB devices exposing a Video Class interface.
- Display product name, VID/PID and USB interface count.
- Request Android CAMERA runtime permission and per-device USB permission.
- Open the granted USB connection with droiduvc.
- Read the renderable MJPEG/YUYV modes advertised by the capture device.
- Request 1920x1080 at 50 FPS with MJPEG preferred, then show the actual negotiated mode.
- Render the UVC stream into a TextureView.
- Stop and release the UVC camera, Surface and USB connection as the app is paused or the surface is destroyed.
- Rescan when USB devices attach or detach.

## Hardware validation still required

The source now contains the live UVC preview path, but it has not yet been confirmed by a successful cloud build and an on-device test. The MS2130/Hagibis capture card must be tested on the tablet to verify the actual mode list, negotiated resolution/frame rate, sustained frame rate, and stability. A requested 1080p50 mode does not guarantee the capture card exposes or can sustain that mode.

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

Android requires the CAMERA runtime permission and per-device USB permission for UVC access. The app does not request microphone permission and does not capture audio.

## License notes

droiduvc is Apache-2.0, and its bundled native dependencies have their own licenses. Review the upstream notices and LGPL obligations before redistributing a production APK.
