#pragma once

#include <android/native_window.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include "fsr_shader_sources.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace droiduvc {

/**
 * Single-slot, latest-frame-wins GLES preview renderer.
 *
 * EGL and GLES objects are created, used, and destroyed exclusively on the
 * render thread. submit() only copies the decoded RGB frame into a bounded
 * one-frame mailbox; it never waits for the GPU or SurfaceFlinger.
 */
class GlesPreviewRenderer {
public:
    GlesPreviewRenderer() = default;
    GlesPreviewRenderer(const GlesPreviewRenderer &) = delete;
    GlesPreviewRenderer &operator=(const GlesPreviewRenderer &) = delete;

    ~GlesPreviewRenderer() { stop(); }

    bool start(ANativeWindow *window) {
        stop();
        if (!window) return false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            window_ = window;
            ANativeWindow_acquire(window_);
            stopping_ = false;
            initialized_ = false;
            initOk_ = false;
            hasFrame_ = false;
            frame_.clear();
        }
        worker_ = std::thread(&GlesPreviewRenderer::run, this);
        std::unique_lock<std::mutex> lock(mutex_);
        const bool signaled = initCv_.wait_for(lock, std::chrono::seconds(3),
                                               [this] { return initialized_; });
        if (!signaled || !initOk_) {
            stopping_ = true;
            frameCv_.notify_all();
            lock.unlock();
            if (worker_.joinable()) worker_.join();
            return false;
        }
        active_.store(true, std::memory_order_release);
        return true;
    }

    // rgbStride is measured in bytes. The source memory is borrowed only for
    // this call; the renderer owns its queued copy after submit() returns.
    bool submit(const uint8_t *rgb, int width, int height, int rgbStride) {
        if (!active_.load(std::memory_order_acquire) || !rgb ||
            width <= 0 || height <= 0 || rgbStride < width * 3) return false;
        const size_t packedStride = static_cast<size_t>(width) * 3;
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return false;
        if (frame_.empty() && !spare_.empty()) frame_.swap(spare_);
        frame_.resize(packedStride * static_cast<size_t>(height));
        for (int y = 0; y < height; ++y) {
            std::memcpy(frame_.data() + static_cast<size_t>(y) * packedStride,
                        rgb + static_cast<size_t>(y) * rgbStride, packedStride);
        }
        frameWidth_ = width;
        frameHeight_ = height;
        if (hasFrame_) replacedFrames_.fetch_add(1, std::memory_order_relaxed);
        hasFrame_ = true; // Replacing the pending frame bounds queue depth to one.
        submittedFrames_.fetch_add(1, std::memory_order_relaxed);
        frameCv_.notify_one();
        return true;
    }

    bool active() const { return active_.load(std::memory_order_acquire); }
    void setRgbExperimentCompensation(bool enabled) {
        rgbCompensationEnabled_.store(enabled, std::memory_order_release);
    }
    bool setFsrEnabled(bool enabled) {
        if (enabled && !fsrAvailable_.load(std::memory_order_acquire)) return false;
        fsrEnabled_.store(enabled, std::memory_order_release);
        return true;
    }

    // {submitted, rendered, replaced, uploadTotalNs, uploadMaxNs,
    //  drawAndPresentTotalNs, drawAndPresentMaxNs, lastError,
    //  fsrAppliedFrames, fsrDrawCallTotalNs, fsrDrawCallMaxNs, fsrAvailable, fsrEnabled, fsrInitError}
    std::array<uint64_t, 14> stats() const {
        return {
                submittedFrames_.load(std::memory_order_relaxed),
                renderedFrames_.load(std::memory_order_relaxed),
                replacedFrames_.load(std::memory_order_relaxed),
                uploadTotalNs_.load(std::memory_order_relaxed),
                uploadMaxNs_.load(std::memory_order_relaxed),
                renderTotalNs_.load(std::memory_order_relaxed),
                renderMaxNs_.load(std::memory_order_relaxed),
                static_cast<uint64_t>(lastError_.load(std::memory_order_relaxed)),
                fsrAppliedFrames_.load(std::memory_order_relaxed),
                fsrDrawCallTotalNs_.load(std::memory_order_relaxed),
                fsrDrawCallMaxNs_.load(std::memory_order_relaxed),
                fsrAvailable_.load(std::memory_order_relaxed) ? 1u : 0u,
                fsrEnabled_.load(std::memory_order_relaxed) ? 1u : 0u,
                static_cast<uint64_t>(fsrInitError_.load(std::memory_order_relaxed))
        };
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            frameCv_.notify_all();
        }
        if (worker_.joinable()) worker_.join();
        active_.store(false, std::memory_order_release);
    }

private:
    struct RgbCurvePoint { uint8_t input; uint8_t output; };

    static std::array<uint8_t, 256> buildRgbLut() {
        static constexpr std::array<RgbCurvePoint, 10> points{{
                {0, 0}, {6, 16}, {26, 32}, {63, 64}, {100, 96},
                {137, 128}, {174, 160}, {211, 192}, {243, 220}, {255, 248}
        }};
        std::array<uint8_t, 256> lut{};
        for (int value = 0; value < 256; ++value) {
            size_t upper = 1;
            while (upper < points.size() - 1 && value > points[upper].input) ++upper;
            const auto a = points[upper - 1];
            const auto b = points[upper];
            const int dx = static_cast<int>(b.input) - a.input;
            const int dy = static_cast<int>(b.output) - a.output;
            const int offset = value - a.input;
            const int mapped = static_cast<int>(a.output) +
                    (dx > 0 ? (offset * dy + dx / 2) / dx : 0);
            lut[static_cast<size_t>(value)] = static_cast<uint8_t>(std::clamp(mapped, 0, 255));
        }
        return lut;
    }

    static GLuint compileShader(GLenum type, const char *source) {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint ok = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    static GLuint createProgram() {
        static const char *vertex =
                "#version 300 es\n"
                "layout(location=0) in vec2 aPosition;\n"
                "layout(location=1) in vec2 aTexCoord;\n"
                "out vec2 vTexCoord;\n"
                "void main(){ gl_Position=vec4(aPosition,0.0,1.0); vTexCoord=aTexCoord; }\n";
        static const char *fragment =
                "#version 300 es\n"
                "precision highp float;\n"
                "in vec2 vTexCoord; uniform sampler2D uTexture; uniform sampler2D uRgbLut;\n"
                "uniform int uApplyRgbCompensation; out vec4 fragColor;\n"
                "float mapRgb(float v){ int i=int(clamp(floor(v*255.0+0.5),0.0,255.0)); return texelFetch(uRgbLut,ivec2(i,0),0).r; }\n"
                "void main(){ vec3 c=texture(uTexture,vTexCoord).rgb; if(uApplyRgbCompensation!=0)c=vec3(mapRgb(c.r),mapRgb(c.g),mapRgb(c.b)); fragColor=vec4(c,1.0); }\n";
        GLuint vs = compileShader(GL_VERTEX_SHADER, vertex);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragment);
        if (!vs || !fs) {
            if (vs) glDeleteShader(vs);
            if (fs) glDeleteShader(fs);
            return 0;
        }
        GLuint program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            glDeleteProgram(program);
            return 0;
        }
        return program;
    }

    GLuint createRcasProgram() {
        static const char *prefix =
                "#version 300 es\\n"
                "precision highp float;\\n"
                "precision highp int;\\n"
                "#define A_GPU 1\\n"
                "#define A_GLSL 1\\n"
                "#define FSR_RCAS_F 1\\n";
        static const char *body = R"ITXRCAS(
uniform sampler2D uInputTexture;
uniform sampler2D uRgbLut;
uniform int uApplyRgbCompensation;
uniform float uSharpnessStops;

AF4 FsrRcasLoadF(ASU2 p) {
    ivec2 size = textureSize(uInputTexture, 0);
    return texelFetch(uInputTexture, clamp(p, ivec2(0), size - ivec2(1)), 0);
}
AF1 mapRgbFsr(AF1 v) {
    int i = int(clamp(floor(v * 255.0 + 0.5), 0.0, 255.0));
    return texelFetch(uRgbLut, ivec2(i, 0), 0).r;
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {
    if (uApplyRgbCompensation != 0) {
        r = mapRgbFsr(r);
        g = mapRgbFsr(g);
        b = mapRgbFsr(b);
    }
}

layout(location=0) out vec4 fragColor;
void main() {
    AU2 pixel = AU2(gl_FragCoord.xy);
    AU4 constants;
    FsrRcasCon(constants, uSharpnessStops);
    AF1 r, g, b;
    FsrRcasF(r, g, b, pixel, constants);
    fragColor = vec4(r, g, b, 1.0);
}
)ITXRCAS";
        GLuint shader = glCreateShader(GL_FRAGMENT_SHADER);
        const GLchar *parts[] = {prefix, kFfxPortabilityHeader, kFfxFsr1Header, body};
        glShaderSource(shader, 4, parts, nullptr);
        glCompileShader(shader);
        GLint compiled = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            GLchar log[2048] = {};
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            __android_log_print(ANDROID_LOG_ERROR, "UvcGles", "FSR1 RCAS shader compile failed: %s", log);
            fsrInitError_.store(1, std::memory_order_relaxed);
            glDeleteShader(shader);
            return 0;
        }
        static const char *vertexSource =
                "#version 300 es\\n"
                "layout(location=0) in vec2 aPosition;\\n"
                "layout(location=1) in vec2 aTexCoord;\\n"
                "void main(){ gl_Position=vec4(aPosition,0.0,1.0); }\\n";
        GLuint vertexShader = compileShader(GL_VERTEX_SHADER, vertexSource);
        if (!vertexShader) {
            fsrInitError_.store(2, std::memory_order_relaxed);
            glDeleteShader(shader);
            return 0;
        }
        GLuint program = glCreateProgram();
        glAttachShader(program, vertexShader);
        glAttachShader(program, shader);
        glLinkProgram(program);
        glDeleteShader(vertexShader);
        glDeleteShader(shader);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            GLchar log[2048] = {};
            glGetProgramInfoLog(program, sizeof(log), nullptr, log);
            __android_log_print(ANDROID_LOG_ERROR, "UvcGles", "FSR1 RCAS program link failed: %s", log);
            fsrInitError_.store(3, std::memory_order_relaxed);
            glDeleteProgram(program);
            return 0;
        }
        fsrInitError_.store(0, std::memory_order_relaxed);
        return program;
    }

    bool ensureRcasTarget(int width, int height) {
        if (width == fsrTargetWidth_ && height == fsrTargetHeight_ &&
            fsrFramebuffer_ != 0 && fsrTexture_ != 0) return true;
        if (!fsrTexture_) glGenTextures(1, &fsrTexture_);
        if (!fsrFramebuffer_) glGenFramebuffers(1, &fsrFramebuffer_);
        glBindTexture(GL_TEXTURE_2D, fsrTexture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER, fsrFramebuffer_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, fsrTexture_, 0);
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            lastError_.store(static_cast<int>(status), std::memory_order_relaxed);
            fsrInitError_.store(4, std::memory_order_relaxed);
            return false;
        }
        fsrTargetWidth_ = width;
        fsrTargetHeight_ = height;
        return true;
    }

    bool initializeEgl() {
        display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr)) return false;
        const EGLint configAttrs[] = {
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                EGL_ALPHA_SIZE, 8, EGL_NONE
        };
        EGLConfig config = nullptr;
        EGLint count = 0;
        if (!eglChooseConfig(display_, configAttrs, &config, 1, &count) || count < 1) return false;

        EGLint nativeFormat = 0;
        if (!eglGetConfigAttrib(display_, config, EGL_NATIVE_VISUAL_ID, &nativeFormat)) return false;
        if (ANativeWindow_setBuffersGeometry(window_, 0, 0, nativeFormat) != 0) return false;

        const EGLint contextAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, contextAttrs);
        if (context_ == EGL_NO_CONTEXT) return false;
        surface_ = eglCreateWindowSurface(display_, config, window_, nullptr);
        if (surface_ == EGL_NO_SURFACE) return false;
        if (!eglMakeCurrent(display_, surface_, surface_, context_)) return false;

        program_ = createProgram();
        if (!program_) return false;
        rcasProgram_ = createRcasProgram();
        fsrAvailable_.store(rcasProgram_ != 0, std::memory_order_release);

        static const GLfloat vertices[] = {
                -1.f, -1.f, 0.f, 1.f,
                 1.f, -1.f, 1.f, 1.f,
                -1.f,  1.f, 0.f, 0.f,
                 1.f,  1.f, 1.f, 0.f
        };
        glGenVertexArrays(1, &vao_);
        glBindVertexArray(vao_);
        glGenBuffers(1, &vbo_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                              reinterpret_cast<void *>(2 * sizeof(GLfloat)));
        glGenTextures(1, &texture_);
        glBindTexture(GL_TEXTURE_2D, texture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glActiveTexture(GL_TEXTURE1);
        glGenTextures(1, &rgbLutTexture_);
        glBindTexture(GL_TEXTURE_2D, rgbLutTexture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        const auto rgbLut = buildRgbLut();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 256, 1, 0, GL_RED,
                     GL_UNSIGNED_BYTE, rgbLut.data());
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture_);
        return glGetError() == GL_NO_ERROR;
    }

    bool render(const std::vector<uint8_t> &pixels, int width, int height) {
        using Clock = std::chrono::steady_clock;
        if (pixels.empty() || width <= 0 || height <= 0) return false;

        const auto uploadStart = Clock::now();
        glActiveTexture(GL_TEXTURE0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glBindTexture(GL_TEXTURE_2D, texture_);
        if (width != textureWidth_ || height != textureHeight_) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0,
                         GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
            textureWidth_ = width;
            textureHeight_ = height;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                            GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
        }
        const uint64_t uploadNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - uploadStart).count());
        uploadTotalNs_.fetch_add(uploadNs, std::memory_order_relaxed);
        updateMax(uploadMaxNs_, uploadNs);

        EGLint outputWidth = 0, outputHeight = 0;
        if (!eglQuerySurface(display_, surface_, EGL_WIDTH, &outputWidth) ||
            !eglQuerySurface(display_, surface_, EGL_HEIGHT, &outputHeight) ||
            outputWidth <= 0 || outputHeight <= 0) {
            lastError_.store(static_cast<int>(eglGetError()), std::memory_order_relaxed);
            return false;
        }

        bool fsrApplied = false;
        const bool fsrRequested = fsrEnabled_.load(std::memory_order_acquire) &&
                                  width == 1920 && height == 1080;
        const auto fsrStart = Clock::now();
        if (fsrRequested) {
            if (rcasProgram_ == 0 || !ensureRcasTarget(width, height)) {
                fsrAvailable_.store(false, std::memory_order_release);
                fsrEnabled_.store(false, std::memory_order_release);
                __android_log_print(ANDROID_LOG_ERROR, "UvcGles",
                                    "FSR1 RCAS target unavailable; disabling RCAS");
            } else {
                glBindFramebuffer(GL_FRAMEBUFFER, fsrFramebuffer_);
            glViewport(0, 0, width, height);
            glUseProgram(rcasProgram_);
            glActiveTexture(GL_TEXTURE0);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture_);
            glUniform1i(glGetUniformLocation(rcasProgram_, "uInputTexture"), 0);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, rgbLutTexture_);
            glUniform1i(glGetUniformLocation(rcasProgram_, "uRgbLut"), 1);
            glUniform1i(glGetUniformLocation(rcasProgram_, "uApplyRgbCompensation"),
                        rgbCompensationEnabled_.load(std::memory_order_acquire) ? 1 : 0);
            glActiveTexture(GL_TEXTURE0);
            // 2.5 stops is intentionally conservative for MJPEG input.
            glUniform1f(glGetUniformLocation(rcasProgram_, "uSharpnessStops"), 2.5f);
            glBindVertexArray(vao_);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            const GLenum fsrError = glGetError();
            if (fsrError == GL_NO_ERROR) {
                fsrApplied = true;
            } else {
                lastError_.store(static_cast<int>(fsrError), std::memory_order_relaxed);
                fsrEnabled_.store(false, std::memory_order_release);
                __android_log_print(ANDROID_LOG_ERROR, "UvcGles",
                                    "FSR1 RCAS draw failed (GL error 0x%x); disabling RCAS", fsrError);
            }
                const uint64_t fsrNs = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - fsrStart).count());
                if (fsrApplied) {
                    fsrAppliedFrames_.fetch_add(1, std::memory_order_relaxed);
                    fsrDrawCallTotalNs_.fetch_add(fsrNs, std::memory_order_relaxed);
                    updateMax(fsrDrawCallMaxNs_, fsrNs);
                }
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        const auto renderStart = Clock::now();
        glViewport(0, 0, outputWidth, outputHeight);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        const float sourceRatio = static_cast<float>(width) / height;
        const float outputRatio = static_cast<float>(outputWidth) / outputHeight;
        int viewportWidth = outputWidth;
        int viewportHeight = outputHeight;
        if (outputRatio > sourceRatio) viewportWidth = static_cast<int>(outputHeight * sourceRatio);
        else viewportHeight = static_cast<int>(outputWidth / sourceRatio);
        glViewport((outputWidth - viewportWidth) / 2, (outputHeight - viewportHeight) / 2,
                   viewportWidth, viewportHeight);
        glUseProgram(program_);
        glActiveTexture(GL_TEXTURE0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, fsrApplied ? fsrTexture_ : texture_);
        glUniform1i(glGetUniformLocation(program_, "uTexture"), 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, rgbLutTexture_);
        glUniform1i(glGetUniformLocation(program_, "uRgbLut"), 1);
        glUniform1i(glGetUniformLocation(program_, "uApplyRgbCompensation"),
                    (!fsrApplied && rgbCompensationEnabled_.load(std::memory_order_acquire)) ? 1 : 0);
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        const GLenum glError = glGetError();
        const EGLBoolean swapped = eglSwapBuffers(display_, surface_);
        const EGLint eglError = swapped ? EGL_SUCCESS : eglGetError();
        const uint64_t renderNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - renderStart).count());
        if (glError != GL_NO_ERROR || !swapped) {
            lastError_.store(glError != GL_NO_ERROR ? static_cast<int>(glError)
                                                    : static_cast<int>(eglError),
                             std::memory_order_relaxed);
            return false;
        }
        renderTotalNs_.fetch_add(renderNs, std::memory_order_relaxed);
        updateMax(renderMaxNs_, renderNs);
        renderedFrames_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    static void updateMax(std::atomic<uint64_t> &target, uint64_t value) {
        uint64_t current = target.load(std::memory_order_relaxed);
        while (current < value &&
               !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
    }

    void cleanupEgl() {
        if (display_ != EGL_NO_DISPLAY) {
            eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (texture_) glDeleteTextures(1, &texture_);
            if (rgbLutTexture_) glDeleteTextures(1, &rgbLutTexture_);
            if (vbo_) glDeleteBuffers(1, &vbo_);
            if (vao_) glDeleteVertexArrays(1, &vao_);
            if (program_) glDeleteProgram(program_);
            if (rcasProgram_) glDeleteProgram(rcasProgram_);
            if (fsrTexture_) glDeleteTextures(1, &fsrTexture_);
            if (fsrFramebuffer_) glDeleteFramebuffers(1, &fsrFramebuffer_);
            if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
            if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
            eglTerminate(display_);
        }
        display_ = EGL_NO_DISPLAY;
        surface_ = EGL_NO_SURFACE;
        context_ = EGL_NO_CONTEXT;
        texture_ = vbo_ = vao_ = program_ = 0;
        rcasProgram_ = fsrTexture_ = fsrFramebuffer_ = rgbLutTexture_ = 0;
        textureWidth_ = textureHeight_ = fsrTargetWidth_ = fsrTargetHeight_ = 0;
        fsrAvailable_.store(false, std::memory_order_release);
        fsrEnabled_.store(false, std::memory_order_release);
    }

    void run() {
        const bool ok = initializeEgl();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            initialized_ = true;
            initOk_ = ok;
            initCv_.notify_all();
        }
        if (ok) {
            for (;;) {
                int width = 0, height = 0;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    frameCv_.wait(lock, [this] { return stopping_ || hasFrame_; });
                    if (stopping_) break;
                    renderBuffer_.swap(frame_);
                    width = frameWidth_;
                    height = frameHeight_;
                    hasFrame_ = false;
                }
                const bool rendered = render(renderBuffer_, width, height);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    // Recycle the completed buffer without overwriting a newer
                    // frame that may have arrived while the GPU was rendering.
                    if (frame_.empty()) frame_.swap(renderBuffer_);
                    else if (spare_.empty()) spare_.swap(renderBuffer_);
                }
                if (!rendered) break;
            }
        }
        cleanupEgl();
        ANativeWindow *window = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            window = window_;
            window_ = nullptr;
            stopping_ = true;
        }
        if (window) ANativeWindow_release(window);
        active_.store(false, std::memory_order_release);
    }

    mutable std::mutex mutex_;
    std::condition_variable frameCv_;
    std::condition_variable initCv_;
    std::thread worker_;
    ANativeWindow *window_ = nullptr;
    std::vector<uint8_t> frame_;
    std::vector<uint8_t> renderBuffer_;
    std::vector<uint8_t> spare_;
    int frameWidth_ = 0;
    int frameHeight_ = 0;
    bool hasFrame_ = false;
    bool stopping_ = true;
    bool initialized_ = false;
    bool initOk_ = false;
    std::atomic<bool> active_{false};
    std::atomic<uint64_t> submittedFrames_{0};
    std::atomic<uint64_t> renderedFrames_{0};
    std::atomic<uint64_t> replacedFrames_{0};
    std::atomic<uint64_t> uploadTotalNs_{0};
    std::atomic<uint64_t> uploadMaxNs_{0};
    std::atomic<uint64_t> renderTotalNs_{0};
    std::atomic<uint64_t> renderMaxNs_{0};
    std::atomic<int> lastError_{0};
    std::atomic<bool> fsrEnabled_{false};
    std::atomic<bool> fsrAvailable_{false};
    std::atomic<bool> rgbCompensationEnabled_{false};
    std::atomic<int> fsrInitError_{0};
    std::atomic<uint64_t> fsrAppliedFrames_{0};
    std::atomic<uint64_t> fsrDrawCallTotalNs_{0};
    std::atomic<uint64_t> fsrDrawCallMaxNs_{0};

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;
    GLuint program_ = 0;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint texture_ = 0;
    GLuint rcasProgram_ = 0;
    GLuint fsrTexture_ = 0;
    GLuint fsrFramebuffer_ = 0;
    GLuint rgbLutTexture_ = 0;
    int textureWidth_ = 0;
    int textureHeight_ = 0;
    int fsrTargetWidth_ = 0;
    int fsrTargetHeight_ = 0;
};

} // namespace droiduvc
