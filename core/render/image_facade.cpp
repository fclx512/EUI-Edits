#include "eui/image.h"

#include "core/render/image.h"
#include "core/render/image_source.h"
#include "core/render/render_backend.h"

namespace eui::image {

GpuDeviceInfo gpuDevice() {
    auto* backend = core::render::activeRenderBackend();
    return backend ? backend->gpuDeviceInfo() : GpuDeviceInfo{};
}

std::shared_ptr<const GpuImage> importGpuImage(GpuImageDescriptor descriptor,
                                              std::shared_ptr<const void> lifetime) {
    auto* backend = core::render::activeRenderBackend();
    auto image = std::make_shared<const GpuImage>(std::move(descriptor), std::move(lifetime));
    return backend && image->valid() && backend->acceptsGpuImage(*image) ? image : nullptr;
}

bool isSourceReady(const std::string& source) {
    return core::ImagePrimitive::isSourceReady(source);
}

bool hasSourceFailed(const std::string& source) {
    return core::ImagePrimitive::hasSourceFailed(source);
}

bool retrySource(const std::string& source) {
    return core::ImagePrimitive::retrySource(source);
}

bool consumeRemoteImageReady() {
    return core::ImagePrimitive::consumeRemoteImageReady();
}

Color themeColor(const std::string& source, Color fallback, bool flipVertically, bool* pending) {
    return core::render::image::sampleThemeColor(source, fallback, flipVertically, pending);
}

} // namespace eui::image
