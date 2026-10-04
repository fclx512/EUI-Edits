#include "core/dsl.h"
#include "core/dsl_runtime.h"
#include "core/render/image.h"
#include "eui/image.h"
#include "core/render/render_backend.h"

#include <array>
#include <iostream>

namespace {
class Backend final : public core::render::RenderBackend {
public:
    bool initialize() override { return true; }
    bool valid() const override { return true; }
    void makeCurrent() override {}
    void beginFrame(const core::render::RenderSurface&) override {}
    void present() override {}
    bool ensureRenderCache(int, int) override { return true; }
    bool renderCacheWasRecreated() const override { return false; }
    void releaseRenderCache() override {}
    void beginRenderCacheFrame(int, int, const std::vector<core::Rect>&) override {}
    void endRenderCacheFrame() override {}
    void blitRenderCache(int, int, core::render::RenderCacheBlitMode, const std::vector<core::Rect>&) override {}
    void clear(const core::Color&) override {}
    void setScissor(bool, const core::Rect&, int) override {}
    void prepareBackdropBlur(const core::Rect&, float, int, int) override {}
    void drawRoundedRect(const core::render::RoundedRectDrawCommand&, int, int) override {}
    void drawPolygon(const core::render::PolygonDrawCommand&, int, int) override {}
    void drawText(const core::render::TextDrawCommand&, int, int) override {}
    eui::GpuDeviceInfo gpuDeviceInfo() const override { return {eui::GpuApi::OpenGL, deviceIdentity_}; }
    bool acceptsGpuImage(const eui::GpuImage& image) override {
        return image.valid() && image.descriptor().device.identity == deviceIdentity_;
    }
    TextureHandle createGpuTexture(const std::shared_ptr<const eui::GpuImage>& image) override {
        if (!acceptsGpuImage(*image)) return nullptr;
        ++imports;
        return new std::shared_ptr<const eui::GpuImage>(image);
    }
    TextureHandle createTexture(const unsigned char*, int, int) override { ++uploads; return nullptr; }
    void destroyTexture(TextureHandle handle) override {
        ++releases;
        delete static_cast<std::shared_ptr<const eui::GpuImage>*>(handle);
    }
    void drawTexture(TextureHandle, const float* data, std::size_t count, const core::Color&,
                     const core::Rect&, float, float, int, int) override {
        ++draws;
        std::copy(data, data + count, vertices.begin());
    }
    int imports = 0, releases = 0, uploads = 0, draws = 0;
    std::array<float, 42> vertices{};
};

bool check(bool passed, const char* message) {
    if (!passed) std::cerr << message << '\n';
    return passed;
}
}

int main() {
    bool ok = true;
    auto owner = std::make_shared<int>(7);
    ok &= check(!eui::image::importGpuImage({}, owner), "import without active backend");
    Backend backend;
    core::render::ScopedRenderBackend scope(backend);
    eui::GpuImageDescriptor descriptor{eui::image::gpuDevice(), 8, 4, 42};
    auto image = eui::image::importGpuImage(descriptor, owner);
    ok &= check(image != nullptr, "valid import rejected");
    ok &= check(!eui::image::importGpuImage(descriptor, {}), "missing owner accepted");
    descriptor.width = 0;
    ok &= check(!eui::image::importGpuImage(descriptor, owner), "zero size accepted");
    descriptor.width = 8;
    ++descriptor.device.identity;
    ok &= check(!eui::image::importGpuImage(descriptor, owner), "wrong device accepted");

    core::dsl::Ui ui;
    auto stream = std::make_shared<eui::ImageStream>();
    ui.begin("gpu");
    ui.image("gpu").source("old.png").stream(stream).texture(image, 9).build();
    ui.image("file").texture(image).source("new.png").build();
    ui.image("stream").texture(image).stream(stream).build();
    ui.image("clear").texture(image).texture(nullptr).build();
    ui.image("bing").texture(image).bingDaily().build();
    ui.end();
    const auto* element = ui.find("gpu");
    ok &= check(element && element->gpuImage == image && element->gpuImageRevision == 9 &&
        !element->imageStream && element->imageSource.empty(), "GPU source did not replace CPU source");
    for (const char* id : {"file", "stream", "clear", "bing"})
        ok &= check(!ui.find(id)->gpuImage, "stale GPU source retained");

    core::ImagePrimitive primitive;
    primitive.initialize();
    primitive.setBounds(0, 0, 8, 4);
    primitive.setGpuImage(image, 1);
    primitive.render(8, 4);
    auto version = primitive.contentVersion();
    primitive.setGpuImage(image, 1);
    ok &= check(primitive.contentVersion() == version && !primitive.updateTexture() &&
        !primitive.isAnimating() && primitive.isRetainedLayerReady(), "static image keeps updating");
    primitive.setGpuImage(image, 2);
    ok &= check(primitive.contentVersion() != version, "revision did not invalidate cache");
    primitive.setFlipVertically(true);
    primitive.render(8, 4);
    ok &= check(backend.imports == 1 && backend.uploads == 0 && backend.vertices[6] == 1.0f,
        "revision recreated texture, uploaded CPU data, or flip failed");
    primitive.setSource("");
    primitive.updateTexture();
    primitive.render(8, 4);
    ok &= check(backend.releases == 1 && backend.draws == 2, "source clear retained borrowed image");
    primitive.setGpuImage(image);
    primitive.render(8, 4);
    primitive.setStream(stream);
    ok &= check(backend.releases == 2, "stream switch did not release GPU wrapper");
    primitive.destroy();

    core::dsl::Runtime runtime;
    auto shutdownOwner = std::make_shared<int>(9);
    std::weak_ptr<int> weakOwner = shutdownOwner;
    descriptor.device = backend.gpuDeviceInfo();
    auto shutdownImage = eui::image::importGpuImage(descriptor, shutdownOwner);
    runtime.compose("shutdown", 8.f, 4.f, [&](core::dsl::Ui& tree, const core::dsl::Screen&) {
        tree.image("gpu").texture(shutdownImage).build();
    });
    runtime.setKeyEventHandler([shutdownOwner](const core::KeyEvent&) {});
    shutdownImage.reset();
    shutdownOwner.reset();
    ok &= check(!weakOwner.expired(), "test image owner was not retained");
    runtime.shutdown(false);
    ok &= check(weakOwner.expired(), "Runtime shutdown retained UI tree or callbacks past device lifetime");
    return ok ? 0 : 1;
}
