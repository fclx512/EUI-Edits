#pragma once

#include "core/render/render_backend.h"

namespace core::render::d2d {

// Windows-only experimental renderer. Text layout stays in the existing model;
// this boundary consumes the same glyph atlas and UI commands as OpenGL.
class D2DRenderBackend final : public RenderBackend {
public:
    explicit D2DRenderBackend(core::window::Handle window);
    ~D2DRenderBackend() override;
    bool initialize() override;
    bool valid() const override;
    bool supportsRetainedLayers() const override { return false; }
    void makeCurrent() override {}
    void beginFrame(const RenderSurface& surface) override;
    void present() override;
    bool frameReady() const override;
    bool framePresented() const override;
    bool ensureRenderCache(int width, int height) override;
    bool renderCacheWasRecreated() const override;
    void releaseRenderCache() override;
    void beginRenderCacheFrame(int width, int height, const std::vector<core::Rect>& repaintRects = {}) override;
    void endRenderCacheFrame() override;
    void blitRenderCache(int width, int height, RenderCacheBlitMode mode = RenderCacheBlitMode::Full,
                         const std::vector<core::Rect>& dirtyRects = {}) override;
    void clear(const core::Color& color) override;
    void setScissor(bool enabled, const core::Rect& rect, int framebufferHeight) override;
    void prepareBackdropBlur(const core::Rect&, float, int, int) override;
    void drawRoundedRect(const RoundedRectDrawCommand& command, int, int) override;
    void drawPolygon(const PolygonDrawCommand& command, int, int) override;
    void drawText(const TextDrawCommand& command, int, int) override;
    TextureHandle createTexture(const unsigned char* pixels, int width, int height) override;
    bool updateTexture(TextureHandle handle, const unsigned char* pixels, int width, int height) override;
    void destroyTexture(TextureHandle handle) override;
    void drawTexture(TextureHandle handle, const float* vertices, std::size_t count,
                     const core::Color& tint, const core::Rect& rect, float radius, float blur, int, int) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace core::render::d2d
