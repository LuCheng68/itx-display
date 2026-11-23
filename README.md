# ITX Display v0.1

Android USB UVC capture display for Xiaomi Pad 5 Pro (Android 15).

## Target configuration

- Language: Java (Android UI and application layer)
- compileSdk / targetSdk: 35
- minSdk: 24
- JDK: 17
- UVC engine dependency: `io.github.mikimn:droiduvc:0.2.1`
- Preferred input: MJPEG
- Target mode: 1920x1080 at 50 FPS, when exposed by the capture card
- Audio: not used
- Orientation: landscape; screen stays awake and immersive

## Current milestone

The initial cloud-buildable project skeleton is committed. It currently implements USB video-interface discovery, VID/PID display, USB permission request, and attach/detach rescanning.

**Live UVC preview is not yet implemented in this commit.** The next milestone connects the granted `UsbDeviceConnection` to droiduvc, enumerates supported MJPEG/YUYV modes, chooses the closest 1080p50 MJPEG mode, and streams to a `TextureView`. A successful Gradle build does not prove the MS2130 capture card supports 1080p50; that must be checked on the actual tablet and capture card.

## Build

JDK 17 and Android SDK 35 are required. From the repository root:

```bash
gradle assembleDebug
```

APK output:

`app/build/outputs/apk/debug/app-debug.apk`

## Codemagic

The repository root contains `codemagic.yaml`. Add this repository to Codemagic as a native Android project and start the `itx-display-debug` workflow. The debug APK is collected as a build artifact.

## USB permissions

Android requires the CAMERA runtime permission and per-device USB permission for UVC access. The app does not request microphone permission and does not capture audio.

## License notes

droiduvc is Apache-2.0, and its bundled native dependencies have their own licenses. Review the upstream notices and LGPL obligations before redistributing a production APK.
