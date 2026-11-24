#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <libusb.h>
#include <libuvc/libuvc.h>
#include <libuvc/libuvc_internal.h>

#include "blit.h"
#include "stream_mode.h"
#include "rife_engine.h"
#include "gles_preview_renderer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <memory>
#include <vector>

#define TAG "UvcCamera"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern "C" uvc_frame_desc_t *uvc_find_frame_desc(uvc_device_handle_t *devh, uint16_t format_id, uint16_t frame_id);

namespace {

// Native state behind the Kotlin `UvcCamera`. The libuvc frame callback runs on a
// libuvc thread, so `mutex` guards `window` and `rgb` against start/stop/close.
struct Camera {
    uvc_context_t *ctx = nullptr;
    uvc_device_handle_t *devh = nullptr;
    ANativeWindow *window = nullptr;
    uvc_frame_t *rgb = nullptr;
    JavaVM *javaVm = nullptr;
    jobject rawFrameListener = nullptr;
    jmethodID rawFrameCallback = nullptr;
    int failedFrames = 0;
    uint64_t frameCount = 0;
    uint64_t observerTotalNs = 0;
    uint64_t observerMaxNs = 0;
    uint64_t decodeTotalNs = 0;
    uint64_t decodeMaxNs = 0;
    uint64_t blitTotalNs = 0;
    uint64_t blitMaxNs = 0;
    uint64_t totalFrameNs = 0;
    uint64_t totalFrameMaxNs = 0;
    std::atomic<bool> rgbExperimentCompensation{false};
    std::shared_ptr<droiduvc::RifeInferenceEngine> rifeEngine;
    std::unique_ptr<droiduvc::GlesPreviewRenderer> glesRenderer;
    bool glesFallbackGeometrySet = false;
    bool rifeInferenceEnabled = false;
    std::mutex mutex;
};

void throwIOException(JNIEnv *env, const char *what, uvc_error_t err) {
    char msg[160];
    snprintf(msg, sizeof(msg), "%s: %s (%d)", what, uvc_strerror(err), err);
    env->ThrowNew(env->FindClass("java/io/IOException"), msg);
}

uvc_error_t getStreamCtrl(uvc_device_handle_t *devh, uvc_stream_ctrl_t *ctrl,
                          int width, int height, int fps, int preferredFormat) {
    // The preferred format (if any) is tried first, then the default order MJPEG, YUYV, any.
    std::vector<enum uvc_frame_format> formats;
    if (preferredFormat == droiduvc::kMjpeg) formats.push_back(UVC_FRAME_FORMAT_MJPEG);
    if (preferredFormat == droiduvc::kYuyv) formats.push_back(UVC_FRAME_FORMAT_YUYV);
    for (auto fmt : {UVC_FRAME_FORMAT_MJPEG, UVC_FRAME_FORMAT_YUYV, UVC_FRAME_FORMAT_ANY}) {
        if (std::find(formats.begin(), formats.end(), fmt) == formats.end()) formats.push_back(fmt);
    }

    // 1. Try exact format/size/fps
    for (auto fmt : formats) {
        if (uvc_get_stream_ctrl_format_size(devh, ctrl, fmt, width, height, fps) == UVC_SUCCESS) {
            return UVC_SUCCESS;
        }
    }

    // 2. Try exact format/size with fps = 0 (accept first available rate)
    for (auto fmt : formats) {
        if (uvc_get_stream_ctrl_format_size(devh, ctrl, fmt, width, height, 0) == UVC_SUCCESS) {
            return UVC_SUCCESS;
        }
    }

    // 3. Try the sizes the camera supports, closest to the request first
    uvc_streaming_interface_t *stream_if = nullptr;
    uvc_format_desc_t *format = nullptr;
    uvc_frame_desc_t *frame = nullptr;
    std::vector<droiduvc::FrameSize> supported;
    DL_FOREACH(devh->info->stream_ifs, stream_if) {
        DL_FOREACH(stream_if->format_descs, format) {
            DL_FOREACH(format->frame_descs, frame) {
                supported.push_back({frame->wWidth, frame->wHeight});
            }
        }
    }

    for (const auto &size : droiduvc::orderBySimilarity(supported, width, height)) {
        for (auto fmt : formats) {
            if (uvc_get_stream_ctrl_format_size(devh, ctrl, fmt, size.width, size.height, 0) == UVC_SUCCESS) {
                return UVC_SUCCESS;
            }
        }
    }

    return UVC_ERROR_INVALID_MODE;
}

// libuvc's uvc_get_ctrl/uvc_set_ctrl use an infinite timeout, which would let a camera that never answers block
// stop()/close() (they share the Kotlin lock) forever. Do the same class/interface control transfer with a bound;
// a timeout surfaces as ControlException.Reason.TIMEOUT.
constexpr unsigned int kControlTimeoutMs = 1000;
constexpr uint8_t kGetClassInterface = LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE;
constexpr uint8_t kSetClassInterface = LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE;

int controlTransfer(uvc_device_handle_t *devh, uint8_t requestType, uint8_t request, int unit, int selector,
                    uint8_t *data, int length) {
    return libusb_control_transfer(devh->usb_devh, requestType, request,
                                   static_cast<uint16_t>(selector << 8),
                                   static_cast<uint16_t>(unit << 8 | devh->info->ctrl_if.bInterfaceNumber),
                                   data, static_cast<uint16_t>(length), kControlTimeoutMs);
}

// Throws com.mikimn.droiduvc.ControlException(message, libusb error code) for a failed control transfer.
void throwControlException(JNIEnv *env, int libusbError) {
    jclass cls = env->FindClass("com/mikimn/droiduvc/ControlException");
    if (cls == nullptr) return;  // NoClassDefFoundError is already pending
    jmethodID ctor = env->GetMethodID(cls, "<init>", "(Ljava/lang/String;I)V");
    if (ctor == nullptr) return;
    char msg[96];
    snprintf(msg, sizeof(msg), "UVC control transfer failed: %s (%d)", libusb_error_name(libusbError), libusbError);
    jstring text = env->NewStringUTF(msg);
    jobject exception = env->NewObject(cls, ctor, text, static_cast<jint>(libusbError));
    if (exception != nullptr) env->Throw(static_cast<jthrowable>(exception));
}

int classifyFormat(const uvc_format_desc_t *format) {
    if (format->bDescriptorSubtype == UVC_VS_FORMAT_MJPEG) return droiduvc::kMjpeg;
    if (format->bDescriptorSubtype == UVC_VS_FORMAT_UNCOMPRESSED &&
        memcmp(format->guidFormat, "YUY2", 4) == 0) {
        return droiduvc::kYuyv;
    }
    return droiduvc::kOtherFormat;
}

const uvc_format_desc_t *findFormat(uvc_device_handle_t *devh, uint8_t formatIndex) {
    uvc_streaming_interface_t *stream_if = nullptr;
    uvc_format_desc_t *format = nullptr;
    DL_FOREACH(devh->info->stream_ifs, stream_if) {
        DL_FOREACH(stream_if->format_descs, format) {
            if (format->bFormatIndex == formatIndex) return format;
        }
    }
    return nullptr;
}


/**
 * Sends a borrowed view of the raw UVC payload to the optional Java listener.
 * The ByteBuffer references libuvc's current frame memory and becomes invalid as soon
 * as this callback returns. Java listeners must copy immediately and must not retain it.
 * This callback is intentionally invoked before uvc_any2rgb() changes the frame format.
 */
void dispatchRawFrame(Camera *cam, uvc_frame_t *frame) {
    if (!cam->javaVm || !cam->rawFrameListener || !cam->rawFrameCallback ||
        !frame || !frame->data || frame->data_bytes == 0) return;

    JNIEnv *env = nullptr;
    bool detachThread = false;
    const jint state = cam->javaVm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (state == JNI_EDETACHED) {
        if (cam->javaVm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            LOGE("Unable to attach UVC callback thread to the JVM");
            return;
        }
        detachThread = true;
    } else if (state != JNI_OK || env == nullptr) {
        return;
    }

    jobject listener = env->NewLocalRef(cam->rawFrameListener);
    jobject payload = env->NewDirectByteBuffer(frame->data, static_cast<jlong>(frame->data_bytes));
    if (listener != nullptr && payload != nullptr) {
        jlong captureTimeNs = 0;
        if (frame->capture_time_finished.tv_sec != 0 ||
            frame->capture_time_finished.tv_nsec != 0) {
            captureTimeNs =
                    static_cast<jlong>(frame->capture_time_finished.tv_sec) * 1000000000LL +
                    static_cast<jlong>(frame->capture_time_finished.tv_nsec);
        }
        env->CallVoidMethod(listener, cam->rawFrameCallback,
                            payload,
                            static_cast<jint>(frame->width),
                            static_cast<jint>(frame->height),
                            static_cast<jint>(frame->frame_format),
                            static_cast<jlong>(frame->sequence),
                            captureTimeNs);
        if (env->ExceptionCheck()) {
            LOGE("RawFrameListener threw an exception; dropping observer callback");
            env->ExceptionClear();
        }
    }
    if (payload != nullptr) env->DeleteLocalRef(payload);
    if (listener != nullptr) env->DeleteLocalRef(listener);
    if (env->ExceptionCheck()) {
        LOGE("JNI exception while dispatching raw UVC frame");
        env->ExceptionClear();
    }
    if (detachThread) cam->javaVm->DetachCurrentThread();
}

// Converts the incoming frame (YUYV/MJPEG/...) to RGB and blits it to the window.
void onFrame(uvc_frame_t *frame, void *user) {
    auto *cam = static_cast<Camera *>(user);
    std::lock_guard<std::mutex> lock(cam->mutex);
    if (!cam->window) {
        if (cam->failedFrames++ < 5) LOGE("Frame arrived but no window");
        return;
    }

    using Clock = std::chrono::steady_clock;
    const auto frameStart = Clock::now();

    const auto observerStart = Clock::now();
    // Observe the raw UVC payload before conversion to RGB.
    dispatchRawFrame(cam, frame);
    const uint64_t observerNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - observerStart).count());
    cam->observerTotalNs += observerNs;
    cam->observerMaxNs = std::max(cam->observerMaxNs, observerNs);

    if (!cam->rgb || cam->rgb->width != frame->width || cam->rgb->height != frame->height) {
        if (cam->rgb) uvc_free_frame(cam->rgb);
        cam->rgb = uvc_allocate_frame(frame->width * frame->height * 3);
    }
    if (!cam->rgb) return;

    const auto decodeStart = Clock::now();
    uvc_error_t err = uvc_any2rgb(frame, cam->rgb);
    const uint64_t decodeNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - decodeStart).count());
    cam->decodeTotalNs += decodeNs;
    cam->decodeMaxNs = std::max(cam->decodeMaxNs, decodeNs);
    if (err != UVC_SUCCESS) {
        if (cam->failedFrames++ < 5) LOGE("Dropping frame: %s", uvc_strerror(err));
        return;
    }

    // Optional RIFE throughput test. Its result remains diagnostic-only; the
    // active preview path below is independent of this inference experiment.
    if (cam->rifeInferenceEnabled && cam->rifeEngine) {
        cam->rifeEngine->submit(
                static_cast<const uint8_t *>(cam->rgb->data),
                cam->rgb->width, cam->rgb->height, cam->rgb->step, frame->sequence);
    }

    const auto blitStart = Clock::now();
    if (cam->glesRenderer && cam->glesRenderer->active() &&
        cam->glesRenderer->submit(static_cast<const uint8_t *>(cam->rgb->data),
                                  cam->rgb->width, cam->rgb->height, cam->rgb->step)) {
        const uint64_t blitNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - blitStart).count());
        cam->blitTotalNs += blitNs;
        cam->blitMaxNs = std::max(cam->blitMaxNs, blitNs);
        const uint64_t totalNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - frameStart).count());
        cam->totalFrameNs += totalNs;
        cam->totalFrameMaxNs = std::max(cam->totalFrameMaxNs, totalNs);
        ++cam->frameCount;
        return;
    }
    // If the GLES worker stopped after a Surface/EGL error, restore the
    // known CPU RGBX buffer format before using ANativeWindow_lock again.
    if (cam->glesRenderer && !cam->glesRenderer->active() &&
        !cam->glesFallbackGeometrySet) {
        const int geometryResult = ANativeWindow_setBuffersGeometry(
                cam->window, cam->rgb->width, cam->rgb->height, WINDOW_FORMAT_RGBX_8888);
        if (geometryResult != 0) {
            LOGE("Unable to restore RGBX Surface geometry after GLES failure: %d", geometryResult);
        }
        cam->glesFallbackGeometrySet = true;
    }
    ANativeWindow_Buffer buf;
    if (ANativeWindow_lock(cam->window, &buf, nullptr) != 0) {
        if (cam->failedFrames++ < 5) LOGE("ANativeWindow_lock failed");
        return;
    }

    droiduvc::blitRgbToRgbx(static_cast<const uint8_t *>(cam->rgb->data), cam->rgb->step,
                            cam->rgb->width, cam->rgb->height, static_cast<uint8_t *>(buf.bits),
                            static_cast<size_t>(buf.stride) * 4, buf.width, buf.height,
                            cam->rgbExperimentCompensation.load(std::memory_order_relaxed));
    ANativeWindow_unlockAndPost(cam->window);

    const uint64_t blitNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - blitStart).count());
    cam->blitTotalNs += blitNs;
    cam->blitMaxNs = std::max(cam->blitMaxNs, blitNs);

    const uint64_t totalNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - frameStart).count());
    cam->totalFrameNs += totalNs;
    cam->totalFrameMaxNs = std::max(cam->totalFrameMaxNs, totalNs);
    ++cam->frameCount;
}

void releaseWindow(Camera *cam) {
    std::lock_guard<std::mutex> lock(cam->mutex);
    if (cam->glesRenderer) {
        cam->glesRenderer->stop();
        cam->glesRenderer.reset();
    }
    if (cam->window) ANativeWindow_release(cam->window);
    cam->window = nullptr;
    if (cam->rgb) uvc_free_frame(cam->rgb);
    cam->rgb = nullptr;
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_mikimn_droiduvc_NativeUvc_open(JNIEnv *env, jobject, jint fd) {
    // Android forbids enumerating /dev/bus/usb; we only wrap the fd from UsbManager.
    libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);

    auto *cam = new Camera();
    if (env->GetJavaVM(&cam->javaVm) != JNI_OK) {
        delete cam;
        env->ThrowNew(env->FindClass("java/io/IOException"), "Unable to obtain JavaVM");
        return 0;
    }
    uvc_error_t err = uvc_init(&cam->ctx, nullptr);
    if (err == UVC_SUCCESS) err = uvc_wrap(fd, cam->ctx, &cam->devh);
    if (err != UVC_SUCCESS) {
        if (cam->ctx) uvc_exit(cam->ctx);
        delete cam;
        throwIOException(env, "Failed to open UVC device", err);
        return 0;
    }
    return reinterpret_cast<jlong>(cam);
}

// Returns {format, width, height, fps} for every mode the camera advertises. A frame size with a
// continuous interval range is reported at its default and its fastest rate.
JNIEXPORT jintArray JNICALL
Java_com_mikimn_droiduvc_NativeUvc_listModes(JNIEnv *env, jobject, jlong handle) {
    auto *cam = reinterpret_cast<Camera *>(handle);
    std::vector<jint> modes;
    auto add = [&](int format, const uvc_frame_desc_t *frame, uint32_t interval) {
        const int fps = droiduvc::intervalToFps(interval);
        if (fps <= 0) return;
        modes.insert(modes.end(), {format, frame->wWidth, frame->wHeight, fps});
    };
    uvc_streaming_interface_t *stream_if = nullptr;
    uvc_format_desc_t *format = nullptr;
    uvc_frame_desc_t *frame = nullptr;
    DL_FOREACH(cam->devh->info->stream_ifs, stream_if) {
        DL_FOREACH(stream_if->format_descs, format) {
            const int code = classifyFormat(format);
            DL_FOREACH(format->frame_descs, frame) {
                if (frame->intervals != nullptr) {
                    for (const uint32_t *i = frame->intervals; *i != 0; i++) add(code, frame, *i);
                } else {
                    add(code, frame, frame->dwDefaultFrameInterval);
                    if (frame->dwMinFrameInterval != frame->dwDefaultFrameInterval) {
                        add(code, frame, frame->dwMinFrameInterval);
                    }
                }
            }
        }
    }
    jintArray result = env->NewIntArray(static_cast<jsize>(modes.size()));
    if (result == nullptr) return nullptr;  // OutOfMemoryError is already pending
    env->SetIntArrayRegion(result, 0, static_cast<jsize>(modes.size()), modes.data());
    return result;
}

// Returns the negotiated mode as {format, width, height, fps}.
JNIEXPORT jintArray JNICALL
Java_com_mikimn_droiduvc_NativeUvc_start(JNIEnv *env, jobject, jlong handle,
                                                  jobject surface, jint width, jint height,
                                                  jint fps, jint preferredFormat) {
    auto *cam = reinterpret_cast<Camera *>(handle);
    uvc_stop_streaming(cam->devh);
    releaseWindow(cam);

    uvc_stream_ctrl_t ctrl;
    uvc_error_t err = getStreamCtrl(cam->devh, &ctrl, width, height, fps, preferredFormat);
    if (err != UVC_SUCCESS) {
        LOGE("No usable stream mode for %dx%d@%d", width, height, fps);
        throwIOException(env, "Unsupported stream mode", err);
        return nullptr;
    }

    uvc_frame_desc_t *frame_desc = uvc_find_frame_desc(cam->devh, ctrl.bFormatIndex, ctrl.bFrameIndex);
    if (frame_desc) {
        width = frame_desc->wWidth;
        height = frame_desc->wHeight;
    }

    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    if (!window) {
        env->ThrowNew(env->FindClass("java/io/IOException"), "Surface is not valid");
        return nullptr;
    }
    cam->failedFrames = 0;
    cam->glesFallbackGeometrySet = false;
    // First try the dedicated GLES renderer. If EGL/GLES is unavailable, restore
    // the original RGBX software-buffer geometry and keep the CPU preview path.
    auto renderer = std::make_unique<droiduvc::GlesPreviewRenderer>();
    const bool glesReady = renderer->start(window);
    if (!glesReady) {
        ANativeWindow_setBuffersGeometry(window, width, height, WINDOW_FORMAT_RGBX_8888);
        LOGE("GLES renderer initialization failed; falling back to CPU blit");
        renderer.reset();
    } else {
        LOGI("GLES pass-through renderer initialized");
    }
    {
        std::lock_guard<std::mutex> lock(cam->mutex);
        cam->window = window;
        cam->glesRenderer = std::move(renderer);
        if (cam->glesRenderer) {
            cam->glesRenderer->setRgbExperimentCompensation(
                    cam->rgbExperimentCompensation.load(std::memory_order_relaxed));
        }
    }

    err = uvc_start_streaming(cam->devh, &ctrl, onFrame, cam, 0);
    if (err != UVC_SUCCESS) {
        releaseWindow(cam);
        LOGE("uvc_start_streaming failed: %s", uvc_strerror(err));
        throwIOException(env, "Failed to start streaming", err);
        return nullptr;
    }
    const uvc_format_desc_t *negotiated = findFormat(cam->devh, ctrl.bFormatIndex);
    const jint mode[4] = {negotiated ? classifyFormat(negotiated) : droiduvc::kOtherFormat, width, height,
                          droiduvc::intervalToFps(ctrl.dwFrameInterval)};
    jintArray result = env->NewIntArray(4);
    if (result == nullptr) return nullptr;  // OutOfMemoryError is already pending
    env->SetIntArrayRegion(result, 0, 4, mode);
    return result;
}

// Returns {cameraTerminalId, cameraControlsBitmap, processingUnitId, processingControlsBitmap}; a missing
// terminal/unit is reported as id 0 and an empty bitmap.

JNIEXPORT void JNICALL
Java_com_mikimn_droiduvc_NativeUvc_setRawFrameListener(JNIEnv *env, jobject, jlong handle,
                                                        jobject listener) {
    auto *cam = reinterpret_cast<Camera *>(handle);
    if (cam == nullptr) return;

    // onFrame() holds the same mutex while invoking the observer, so removing the listener
    // waits for any in-flight borrowed ByteBuffer callback to complete.
    std::lock_guard<std::mutex> lock(cam->mutex);
    if (cam->rawFrameListener != nullptr) {
        env->DeleteGlobalRef(cam->rawFrameListener);
        cam->rawFrameListener = nullptr;
        cam->rawFrameCallback = nullptr;
    }
    if (listener == nullptr) return;

    jclass listenerClass = env->GetObjectClass(listener);
    if (listenerClass == nullptr) return;
    jmethodID callback = env->GetMethodID(
            listenerClass, "onFrame", "(Ljava/nio/ByteBuffer;IIIJJ)V");
    if (callback == nullptr) {
        env->DeleteLocalRef(listenerClass);
        return;
    }
    jobject globalListener = env->NewGlobalRef(listener);
    env->DeleteLocalRef(listenerClass);
    if (globalListener == nullptr) return;

    cam->rawFrameListener = globalListener;
    cam->rawFrameCallback = callback;
}

JNIEXPORT jintArray JNICALL
Java_com_mikimn_droiduvc_NativeUvc_controlInfo(JNIEnv *env, jobject, jlong handle) {
    auto *cam = reinterpret_cast<Camera *>(handle);
    const uvc_input_terminal_t *terminal = uvc_get_camera_terminal(cam->devh);
    const uvc_processing_unit_t *unit = uvc_get_processing_units(cam->devh);
    const jint info[4] = {
        terminal ? terminal->bTerminalID : 0,
        terminal ? static_cast<jint>(terminal->bmControls & 0xFFFFFFFFu) : 0,
        unit ? unit->bUnitID : 0,
        unit ? static_cast<jint>(unit->bmControls & 0xFFFFFFFFu) : 0,
    };
    jintArray result = env->NewIntArray(4);
    if (result == nullptr) return nullptr;  // OutOfMemoryError is already pending
    env->SetIntArrayRegion(result, 0, 4, info);
    return result;
}

// GET_* request (`request` is the UVC request code, e.g. 0x81 = GET_CUR) of a control; returns the bytes read.
JNIEXPORT jbyteArray JNICALL
Java_com_mikimn_droiduvc_NativeUvc_getControl(JNIEnv *env, jobject, jlong handle, jint unit,
                                                 jint selector, jint request, jint length) {
    auto *cam = reinterpret_cast<Camera *>(handle);
    std::vector<uint8_t> buf(static_cast<size_t>(length));
    const int ret = controlTransfer(cam->devh, kGetClassInterface, static_cast<uint8_t>(request), unit, selector,
                                    buf.data(), length);
    if (ret < 0) {
        throwControlException(env, ret);
        return nullptr;
    }
    jbyteArray result = env->NewByteArray(ret);
    if (result == nullptr) return nullptr;  // OutOfMemoryError is already pending
    env->SetByteArrayRegion(result, 0, ret, reinterpret_cast<const jbyte *>(buf.data()));
    return result;
}

JNIEXPORT void JNICALL
Java_com_mikimn_droiduvc_NativeUvc_setControl(JNIEnv *env, jobject, jlong handle, jint unit,
                                                 jint selector, jbyteArray data) {
    auto *cam = reinterpret_cast<Camera *>(handle);
    const jsize length = env->GetArrayLength(data);
    std::vector<uint8_t> buf(static_cast<size_t>(length));
    env->GetByteArrayRegion(data, 0, length, reinterpret_cast<jbyte *>(buf.data()));
    const int ret = controlTransfer(cam->devh, kSetClassInterface, UVC_SET_CUR, unit, selector, buf.data(), length);
    if (ret < 0) throwControlException(env, ret);
}

// Reads the device descriptor of the open device without reopening it; false if it cannot be read.
bool readDeviceDescriptor(Camera *cam, libusb_device_handle **usb, libusb_device_descriptor *desc) {
    *usb = uvc_get_libusb_handle(cam->devh);
    libusb_device *device = *usb ? libusb_get_device(*usb) : nullptr;
    return device != nullptr && libusb_get_device_descriptor(device, desc) == 0;
}

// Returns {vendorId, productId, bcdUVC}; the ids are -1 when the device descriptor cannot be read.
JNIEXPORT jintArray JNICALL
Java_com_mikimn_droiduvc_NativeUvc_deviceInfo(JNIEnv *env, jobject, jlong handle) {
    auto *cam = reinterpret_cast<Camera *>(handle);
    libusb_device_handle *usb = nullptr;
    libusb_device_descriptor desc{};
    const bool ok = readDeviceDescriptor(cam, &usb, &desc);
    const jint info[3] = {ok ? desc.idVendor : -1, ok ? desc.idProduct : -1, cam->devh->info->ctrl_if.bcdUVC};
    jintArray result = env->NewIntArray(3);
    if (result == nullptr) return nullptr;  // OutOfMemoryError is already pending
    env->SetIntArrayRegion(result, 0, 3, info);
    return result;
}

// Returns {manufacturer, product, serialNumber}; a string the device does not provide (or that cannot be read
// over the already-open handle) is null.
JNIEXPORT jobjectArray JNICALL
Java_com_mikimn_droiduvc_NativeUvc_deviceStrings(JNIEnv *env, jobject, jlong handle) {
    auto *cam = reinterpret_cast<Camera *>(handle);
    libusb_device_handle *usb = nullptr;
    libusb_device_descriptor desc{};
    const bool ok = readDeviceDescriptor(cam, &usb, &desc);
    const uint8_t indexes[3] = {desc.iManufacturer, desc.iProduct, desc.iSerialNumber};
    jclass stringClass = env->FindClass("java/lang/String");
    if (stringClass == nullptr) return nullptr;
    jobjectArray result = env->NewObjectArray(3, stringClass, nullptr);
    if (result == nullptr) return nullptr;
    for (int i = 0; ok && i < 3; i++) {
        unsigned char buf[256];
        if (indexes[i] != 0 && libusb_get_string_descriptor_ascii(usb, indexes[i], buf, sizeof(buf)) > 0) {
            jstring text = env->NewStringUTF(reinterpret_cast<const char *>(buf));
            env->SetObjectArrayElement(result, i, text);
            env->DeleteLocalRef(text);
        }
    }
    return result;
}

JNIEXPORT jstring JNICALL
Java_com_mikimn_droiduvc_NativeUvc_loadRifeModel(JNIEnv *env, jobject, jlong handle, jstring modelDirectory) {
    auto *cam = reinterpret_cast<Camera *>(handle);
    if (cam == nullptr || modelDirectory == nullptr) return nullptr;

    const char *path = env->GetStringUTFChars(modelDirectory, nullptr);
    if (path == nullptr) return nullptr;
    std::string modelPath(path);
    env->ReleaseStringUTFChars(modelDirectory, path);

    auto engine = std::make_shared<droiduvc::RifeInferenceEngine>();
    std::string error;
    if (!engine->initialize(modelPath, error)) {
        env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), error.c_str());
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(cam->mutex);
        cam->rifeInferenceEnabled = false;
        cam->rifeEngine = engine;
    }
    const std::string info = engine->deviceInfo();
    return env->NewStringUTF(info.c_str());
}

JNIEXPORT void JNICALL
Java_com_mikimn_droiduvc_NativeUvc_setRifeInferenceEnabled(
        JNIEnv *, jobject, jlong handle, jboolean enabled) {
    auto *cam = reinterpret_cast<Camera *>(handle);
    if (cam == nullptr) return;
    std::shared_ptr<droiduvc::RifeInferenceEngine> engine;
    {
        std::lock_guard<std::mutex> lock(cam->mutex);
        engine = cam->rifeEngine;
        cam->rifeInferenceEnabled = enabled == JNI_TRUE && engine != nullptr;
    }
    if (engine) engine->setEnabled(enabled == JNI_TRUE);
}

JNIEXPORT jlongArray JNICALL
Java_com_mikimn_droiduvc_NativeUvc_rifeInferenceStats(JNIEnv *env, jobject, jlong handle) {
    auto *cam = reinterpret_cast<Camera *>(handle);
    if (cam == nullptr) return nullptr;
    std::shared_ptr<droiduvc::RifeInferenceEngine> engine;
    {
        std::lock_guard<std::mutex> lock(cam->mutex);
        engine = cam->rifeEngine;
    }
    const std::array<uint64_t, 9> values = engine
            ? engine->stats() : std::array<uint64_t, 9>{};
    jlong out[9];
    for (int i = 0; i < 9; ++i) out[i] = static_cast<jlong>(values[static_cast<size_t>(i)]);
    jlongArray result = env->NewLongArray(9);
    if (result != nullptr) env->SetLongArrayRegion(result, 0, 9, out);
    return result;
}

JNIEXPORT void JNICALL
Java_com_mikimn_droiduvc_NativeUvc_setRgbExperimentCompensation(
        JNIEnv *, jobject, jlong handle, jboolean enabled) {
    auto *cam = reinterpret_cast<Camera *>(handle);
    if (cam != nullptr) {
        const bool value = enabled == JNI_TRUE;
        cam->rgbExperimentCompensation.store(value, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(cam->mutex);
        if (cam->glesRenderer) cam->glesRenderer->setRgbExperimentCompensation(value);
    }
}

JNIEXPORT void JNICALL
Java_com_mikimn_droiduvc_NativeUvc_setFsrEnabled(
        JNIEnv *, jobject, jlong handle, jboolean enabled) {
    auto *cam = reinterpret_cast<Camera *>(handle);
    if (cam == nullptr) return;
    std::lock_guard<std::mutex> lock(cam->mutex);
    if (!cam->glesRenderer) return;
    const bool accepted = cam->glesRenderer->setFsrEnabled(enabled == JNI_TRUE);
    if (enabled == JNI_TRUE && !accepted) {
        LOGE("FSR1 RCAS is unavailable; retaining the unsharpened GLES preview");
    }
}

JNIEXPORT jlongArray JNICALL
Java_com_mikimn_droiduvc_NativeUvc_performanceStats(JNIEnv *env, jobject, jlong handle) {
    auto *cam = reinterpret_cast<Camera *>(handle);
    if (cam == nullptr) return nullptr;
    std::lock_guard<std::mutex> lock(cam->mutex);
    jlong values[24] = {
            static_cast<jlong>(cam->frameCount),
            static_cast<jlong>(cam->observerTotalNs),
            static_cast<jlong>(cam->observerMaxNs),
            static_cast<jlong>(cam->decodeTotalNs),
            static_cast<jlong>(cam->decodeMaxNs),
            static_cast<jlong>(cam->blitTotalNs),
            static_cast<jlong>(cam->blitMaxNs),
            static_cast<jlong>(cam->totalFrameNs),
            static_cast<jlong>(cam->totalFrameMaxNs),
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    // 0 = CPU fallback, 1 = GLES renderer active, 2 = GLES renderer stopped/failed.
    if (cam->glesRenderer) {
        values[9] = cam->glesRenderer->active() ? 1 : 2;
        const auto gpu = cam->glesRenderer->stats();
        for (size_t i = 0; i < gpu.size(); ++i) {
            values[10 + i] = static_cast<jlong>(gpu[i]);
        }
    }
    jlongArray result = env->NewLongArray(24);
    if (result != nullptr) env->SetLongArrayRegion(result, 0, 24, values);
    return result;
}

JNIEXPORT void JNICALL
Java_com_mikimn_droiduvc_NativeUvc_stop(JNIEnv *, jobject, jlong handle) {
    auto *cam = reinterpret_cast<Camera *>(handle);
    uvc_stop_streaming(cam->devh);  // joins the callback thread; must not hold the mutex
    releaseWindow(cam);
}

JNIEXPORT void JNICALL
Java_com_mikimn_droiduvc_NativeUvc_close(JNIEnv *env, jobject, jlong handle) {
    auto *cam = reinterpret_cast<Camera *>(handle);
    uvc_stop_streaming(cam->devh);
    releaseWindow(cam);
    cam->rifeInferenceEnabled = false;
    cam->rifeEngine.reset();
    uvc_close(cam->devh);
    uvc_exit(cam->ctx);
    if (cam->rawFrameListener != nullptr) {
        env->DeleteGlobalRef(cam->rawFrameListener);
        cam->rawFrameListener = nullptr;
        cam->rawFrameCallback = nullptr;
    }
    delete cam;
}

}  // extern "C"
