#include "rife_engine.h"

#include "rife.h"
#include "gpu.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace droiduvc {

struct RifeInferenceEngine::Impl {
    struct FrameSlot {
        std::vector<uint8_t> rgb;
        int width = 0;
        int height = 0;
        uint64_t sequence = 0;
    };

    mutable std::mutex mutex;
    std::condition_variable condition;
    std::array<FrameSlot, 3> slots;
    std::deque<int> freeSlots{0, 1, 2};
    std::deque<int> readySlots;
    std::unique_ptr<RIFE> model;
    std::thread worker;
    bool running = false;
    bool enabled = false;
    bool resetRequested = false;
    bool gpuInstanceCreated = false;
    int width = 0;
    int height = 0;
    int gpuIndex = -1;
    std::string deviceInfo;

    uint64_t submitted = 0;
    uint64_t processed = 0;
    uint64_t skipped = 0;
    uint64_t dropped = 0;
    uint64_t inferenceTotalNs = 0;
    uint64_t inferenceMaxNs = 0;
    uint64_t copyTotalNs = 0;
    uint64_t copyMaxNs = 0;
    uint64_t pairCount = 0;

    void releaseQueuedLocked() {
        while (!readySlots.empty()) {
            freeSlots.push_back(readySlots.front());
            readySlots.pop_front();
        }
    }

    void run() {
        int previous = -1;
        uint64_t pairIndex = 0;
        for (;;) {
            int current = -1;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [this] {
                    return !running || resetRequested || (enabled && !readySlots.empty());
                });
                if (!running) break;
                if (resetRequested) {
                    releaseQueuedLocked();
                    if (previous >= 0) {
                        freeSlots.push_back(previous);
                        previous = -1;
                    }
                    pairIndex = 0;
                    resetRequested = false;
                }
                if (!enabled || readySlots.empty()) continue;
                current = readySlots.front();
                readySlots.pop_front();
            }

            if (previous < 0) {
                previous = current;
                continue;
            }

            const FrameSlot& first = slots[static_cast<size_t>(previous)];
            const FrameSlot& second = slots[static_cast<size_t>(current)];
            const bool contiguous = second.sequence == first.sequence + 1
                    && first.width == second.width && first.height == second.height;
            bool shouldProcess = false;
            if (contiguous) {
                ++pairIndex;
                // 4 inferred intervals out of every 5: 50 input + 40 generated ~= 90 fps.
                shouldProcess = (pairIndex % 5) != 0;
            }

            if (shouldProcess) {
                const auto start = std::chrono::steady_clock::now();
                ncnn::Mat input0(first.width, first.height,
                                 const_cast<uint8_t*>(first.rgb.data()), static_cast<size_t>(3), 3);
                ncnn::Mat input1(second.width, second.height,
                                 const_cast<uint8_t*>(second.rgb.data()), static_cast<size_t>(3), 3);
                ncnn::Mat output(first.width, first.height, static_cast<size_t>(3), 3);
                const int result = model->process(input0, input1, 0.5f, output);
                const uint64_t elapsed = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - start).count());
                std::lock_guard<std::mutex> lock(mutex);
                if (result == 0 && !output.empty()) {
                    ++processed;
                    inferenceTotalNs += elapsed;
                    inferenceMaxNs = std::max(inferenceMaxNs, elapsed);
                } else {
                    ++dropped;
                }
            } else {
                std::lock_guard<std::mutex> lock(mutex);
                if (contiguous) ++skipped;
                else ++dropped;
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                ++pairCount;
                freeSlots.push_back(previous);
                previous = current;
            }
        }

        std::lock_guard<std::mutex> lock(mutex);
        releaseQueuedLocked();
        if (previous >= 0) freeSlots.push_back(previous);
    }
};

RifeInferenceEngine::RifeInferenceEngine() : impl_(new Impl()) {}

RifeInferenceEngine::~RifeInferenceEngine() {
    if (!impl_) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->running = false;
        impl_->enabled = false;
        impl_->condition.notify_all();
    }
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->model.reset();
    if (impl_->gpuInstanceCreated) {
        ncnn::destroy_gpu_instance();
        impl_->gpuInstanceCreated = false;
    }
}

bool RifeInferenceEngine::initialize(const std::string& modelDirectory, std::string& error) {
    if (!impl_) {
        error = "RIFE 状态不可用";
        return false;
    }
    if (impl_->model) {
        error = "RIFE 模型已经初始化";
        return false;
    }

    const int createResult = ncnn::create_gpu_instance();
    if (createResult != 0) {
        error = "ncnn 无法创建 Vulkan 实例，返回值 " + std::to_string(createResult);
        return false;
    }
    impl_->gpuInstanceCreated = true;

    const int gpuCount = ncnn::get_gpu_count();
    if (gpuCount <= 0) {
        error = "ncnn 没有枚举到 Vulkan GPU";
        return false;
    }
    impl_->gpuIndex = ncnn::get_default_gpu_index();
    if (impl_->gpuIndex < 0 || impl_->gpuIndex >= gpuCount) impl_->gpuIndex = 0;

    const ncnn::GpuInfo& gpu = ncnn::get_gpu_info(impl_->gpuIndex);
    impl_->deviceInfo = std::string("ncnn Vulkan：") + gpu.device_name()
            + " · API " + std::to_string(VK_VERSION_MAJOR(gpu.api_version()))
            + "." + std::to_string(VK_VERSION_MINOR(gpu.api_version()));

    impl_->model.reset(new RIFE(impl_->gpuIndex, false, false, false, 1, false, true));
    const int loadResult = impl_->model->load(modelDirectory);
    if (loadResult != 0) {
        impl_->model.reset();
        error = "RIFE 4.6 模型加载失败，返回值 " + std::to_string(loadResult);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->running = true;
        impl_->enabled = false;
        impl_->resetRequested = false;
    }
    impl_->worker = std::thread([state = impl_.get()] { state->run(); });
    return true;
}

void RifeInferenceEngine::setEnabled(bool enabled) {
    if (!impl_) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const bool next = enabled && impl_->running && impl_->model != nullptr;
        if (impl_->enabled != next) {
            impl_->enabled = next;
            impl_->resetRequested = true;
        }
        impl_->condition.notify_all();
    }
}

void RifeInferenceEngine::submit(const uint8_t* rgb, int width, int height,
                                 size_t strideBytes, uint64_t sequence) {
    if (!impl_ || rgb == nullptr || width <= 0 || height <= 0 ||
        strideBytes < static_cast<size_t>(width) * 3) return;

    const size_t rowBytes = static_cast<size_t>(width) * 3;
    if (rowBytes > 32u * 1024u * 1024u / static_cast<size_t>(height)) return;

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->running || !impl_->enabled) return;
    if (impl_->width == 0) {
        impl_->width = width;
        impl_->height = height;
    }
    if (impl_->width != width || impl_->height != height) {
        ++impl_->dropped;
        return;
    }

    if (impl_->freeSlots.empty() && !impl_->readySlots.empty()) {
        impl_->freeSlots.push_back(impl_->readySlots.front());
        impl_->readySlots.pop_front();
        ++impl_->dropped;
    }
    if (impl_->freeSlots.empty()) {
        ++impl_->dropped;
        return;
    }

    const int slotIndex = impl_->freeSlots.front();
    impl_->freeSlots.pop_front();
    Impl::FrameSlot& slot = impl_->slots[static_cast<size_t>(slotIndex)];
    const auto copyStart = std::chrono::steady_clock::now();
    slot.rgb.resize(rowBytes * static_cast<size_t>(height));
    for (int y = 0; y < height; ++y) {
        std::memcpy(slot.rgb.data() + static_cast<size_t>(y) * rowBytes,
                    rgb + static_cast<size_t>(y) * strideBytes, rowBytes);
    }
    const uint64_t copyNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - copyStart).count());
    slot.width = width;
    slot.height = height;
    slot.sequence = sequence;
    impl_->copyTotalNs += copyNs;
    impl_->copyMaxNs = std::max(impl_->copyMaxNs, copyNs);
    ++impl_->submitted;
    impl_->readySlots.push_back(slotIndex);
    impl_->condition.notify_one();
}

std::array<uint64_t, 9> RifeInferenceEngine::stats() const {
    if (!impl_) return {};
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return {impl_->submitted, impl_->processed, impl_->skipped, impl_->dropped,
            impl_->inferenceTotalNs, impl_->inferenceMaxNs,
            impl_->copyTotalNs, impl_->copyMaxNs, impl_->pairCount};
}

std::string RifeInferenceEngine::deviceInfo() const {
    if (!impl_) return "RIFE 未初始化";
    return impl_->deviceInfo;
}

}  // namespace droiduvc
