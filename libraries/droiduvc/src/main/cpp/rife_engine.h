#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace droiduvc {

class RifeInferenceEngine {
public:
    RifeInferenceEngine();
    ~RifeInferenceEngine();

    RifeInferenceEngine(const RifeInferenceEngine&) = delete;
    RifeInferenceEngine& operator=(const RifeInferenceEngine&) = delete;

    bool initialize(const std::string& modelDirectory, std::string& error);
    void setEnabled(bool enabled);
    void submit(const uint8_t* rgb, int width, int height, size_t strideBytes, uint64_t sequence);
    std::array<uint64_t, 9> stats() const;
    std::string deviceInfo() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace droiduvc
