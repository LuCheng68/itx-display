#pragma once

#include <android/native_window.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <algorithm>
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
        hasFrame_ = true; // Replacing the pending frame bounds queue depth to one.
        frameCv_.notify_one();
        return true;
    }

    bool active() const { return active_.load(std::memory_order_acquire); }

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
                "precision mediump float;\n"
                "in vec2 vTexCoord; uniform sampler2D uTexture; out vec4 fragColor;\n"
                "void main(){ fragColor=vec4(texture(uTexture,vTexCoord).rgb,1.0); }\n";
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
        return glGetError() == GL_NO_ERROR;
    }

    void render(const std::vector<uint8_t> &pixels, int width, int height) {
        if (pixels.empty() || width <= 0 || height <= 0) return;
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

        EGLint outputWidth = 0, outputHeight = 0;
        eglQuerySurface(display_, surface_, EGL_WIDTH, &outputWidth);
        eglQuerySurface(display_, surface_, EGL_HEIGHT, &outputHeight);
        if (outputWidth <= 0 || outputHeight <= 0) return;

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
        glBindTexture(GL_TEXTURE_2D, texture_);
        glUniform1i(glGetUniformLocation(program_, "uTexture"), 0);
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        eglSwapBuffers(display_, surface_);
    }

    void cleanupEgl() {
        if (display_ != EGL_NO_DISPLAY) {
            eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (texture_) glDeleteTextures(1, &texture_);
            if (vbo_) glDeleteBuffers(1, &vbo_);
            if (vao_) glDeleteVertexArrays(1, &vao_);
            if (program_) glDeleteProgram(program_);
            if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
            if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
            eglTerminate(display_);
        }
        display_ = EGL_NO_DISPLAY;
        surface_ = EGL_NO_SURFACE;
        context_ = EGL_NO_CONTEXT;
        texture_ = vbo_ = vao_ = program_ = 0;
        textureWidth_ = textureHeight_ = 0;
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
                render(renderBuffer_, width, height);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    // Recycle the completed buffer without overwriting a newer
                    // frame that may have arrived while the GPU was rendering.
                    if (frame_.empty()) frame_.swap(renderBuffer_);
                    else if (spare_.empty()) spare_.swap(renderBuffer_);
                }
                if (eglGetError() != EGL_SUCCESS) break;
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

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;
    GLuint program_ = 0;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint texture_ = 0;
    int textureWidth_ = 0;
    int textureHeight_ = 0;
};

} // namespace droiduvc
