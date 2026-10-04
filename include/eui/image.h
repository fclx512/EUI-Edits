#pragma once

#include "eui/types.h"
#include "core/render/image_stream.h"
#include "core/render/gpu_image.h"

#include <string>

namespace eui {

using ImageFit = core::ImageFit;
using ImagePixelFormat = core::render::ImagePixelFormat;
using ImageColorSpace = core::render::ImageColorSpace;
using ImageColorRange = core::render::ImageColorRange;
using ImageFrame = core::render::ImageFrame;
using ImageStream = core::render::ImageStream;
using GpuApi = core::render::GpuApi;
using GpuDeviceInfo = core::render::GpuDeviceInfo;
using GpuImageDescriptor = core::render::GpuImageDescriptor;
using GpuImage = core::render::GpuImage;

namespace image {

/** @brief 在当前窗口 UI/渲染线程查询 GPU 互操作信息；没有活动后端时返回空信息。 */
GpuDeviceInfo gpuDevice();
/**
 * @brief 校验并借用外部 GPU 图像；无活动后端、设备不匹配或描述无效时返回 nullptr。
 * @note 不等待生产者，也不转换图像布局。必须满足 GpuImageDescriptor 的同步约定。
 */
std::shared_ptr<const GpuImage> importGpuImage(GpuImageDescriptor descriptor,
                                              std::shared_ptr<const void> lifetime);

bool isSourceReady(const std::string& source);
bool hasSourceFailed(const std::string& source);
bool retrySource(const std::string& source);
bool consumeRemoteImageReady();
Color themeColor(const std::string& source,
                 Color fallback,
                 bool flipVertically = false,
                 bool* pending = nullptr);

} // namespace image

} // namespace eui
