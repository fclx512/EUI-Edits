#pragma once

#include <memory>

namespace core::platform {

struct ProcessUsageSample {
    bool hasCpuPercent = false;
    bool hasGpuPercent = false;
    double cpuPercent = 0.0;
    double gpuPercent = 0.0;
};

class ProcessUsageSampler {
public:
    ProcessUsageSampler();
    ~ProcessUsageSampler();

    ProcessUsageSampler(const ProcessUsageSampler&) = delete;
    ProcessUsageSampler& operator=(const ProcessUsageSampler&) = delete;

    void reset();
    ProcessUsageSample sample(double elapsedSeconds);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace core::platform
