#pragma once

#include <cstdint>
#include <memory>
#include <utility>

namespace core::render {

/** @brief 外部图像所属的渲染 API，不表示跨 API 资源共享。 */
enum class GpuApi { None, OpenGL, Vulkan };

/**
 * @brief 当前窗口的 GPU 设备信息。仅在该窗口的 UI/渲染线程使用原生句柄。
 * identity 用于拒绝其他窗口/设备的资源；不延长设备生命周期。
 * Vulkan 句柄以整数保存，公共头不依赖 Vulkan SDK。
 */
struct GpuDeviceInfo {
    GpuApi api = GpuApi::None;
    std::uint64_t identity = 0;
    std::uintptr_t instance = 0;
    std::uintptr_t physicalDevice = 0;
    std::uintptr_t device = 0;
    std::uintptr_t graphicsQueue = 0;
    std::uint32_t graphicsQueueFamily = 0;
};

/**
 * @brief 借用的二维颜色纹理。尺寸为像素，使用 straight alpha。
 * OpenGL: texture 为当前上下文中的 GL_TEXTURE_2D，支持 RGBA8/RGB8/sRGB。
 * Vulkan: imageView 为同设备、单采样、可线性采样的二维颜色视图，必须保持
 * SHADER_READ_ONLY_OPTIMAL 布局，归属 graphicsQueueFamily。
 * 生产者负责在 UI 提交前完成同队列写入和写入到 fragment sampling 的 barrier。
 */
struct GpuImageDescriptor {
    GpuDeviceInfo device;
    int width = 0;
    int height = 0;
    std::uint32_t texture = 0;
    std::uint64_t imageView = 0;
};

/**
 * @brief 不可变的外部纹理描述及其生命周期引用，不复制或上传像素。
 * lifetime 负责持有并释放原生纹理及 Vulkan imageView/memory。框架不直接删除原生纹理，
 * 会保留引用至最后一次 GPU 使用结束。应用仍须在设备销毁前释放自己持有的引用。
 * 内容更新须同步 GPU 读写，并递增 image.texture() 的 revision；不能仅修改句柄。
 */
class GpuImage {
public:
    GpuImage(GpuImageDescriptor descriptor, std::shared_ptr<const void> lifetime)
        : descriptor_(std::move(descriptor)), lifetime_(std::move(lifetime)) {}
    const GpuImageDescriptor& descriptor() const { return descriptor_; }
    bool valid() const {
        return lifetime_ && descriptor_.device.identity != 0 &&
            descriptor_.width > 0 && descriptor_.height > 0 &&
            ((descriptor_.device.api == GpuApi::OpenGL && descriptor_.texture != 0) ||
             (descriptor_.device.api == GpuApi::Vulkan && descriptor_.device.device != 0 &&
              descriptor_.imageView != 0));
    }
private:
    GpuImageDescriptor descriptor_;
    std::shared_ptr<const void> lifetime_;
};

} // namespace core::render
