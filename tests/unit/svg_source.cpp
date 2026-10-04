#include "core/dsl.h"
#include "core/render/image_source.h"

#include <array>
#include <functional>
#include <iostream>

namespace {

const std::string heart = R"svg(<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg">
  <path fill="#ffffff" d="M12 21s-8-4.7-8-11a5 5 0 0 1 8-4 5 5 0 0 1 8 4c0 6.3-8 11-8 11z"/>
</svg>)svg";
const std::string replacement = R"svg(<svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg">
  <rect width="24" height="24" fill="#00ff00"/>
</svg>)svg";

bool check(bool condition, const std::string& message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

bool isSvg(const core::dsl::Element* element, const std::string& expected) {
    return element && element->kind == core::dsl::ElementKind::Svg &&
        element->svgSource == expected && element->imageSource.empty() && !element->imageStream;
}

} // namespace

int main() {
    core::dsl::Ui ui;
    ui.begin("svg-source");
    // size 返回 ImageBuilder&，后续 source 调用仍须保留 SVG 来源类型。
    ui.column("root").size(960.f, 640.f).padding(32.f).content([&] {
        ui.svg("inline.heart").size(54.f, 54.f).source(heart)
            .tint({1.f, 0.36f, 0.58f, 1.f}).contain().build();
    }).build();
    ui.svg("source.first").source(heart).size(54.f, 54.f).contain().build();
    ui.svg("source.replaced").source(heart).size(54.f, 54.f).source(replacement).build();
    ui.svg("source.cleared").source(heart).opacity(0.5f).source("").build();
    const auto stream = std::make_shared<core::render::ImageStream>();
    ui.svg("stream.then.source").stream(stream).source(heart).build();
    auto direct = ui.svg("stream.then.direct.source");
    direct.stream(stream);
    direct.source(heart).build();
    auto base = ui.svg("base.source");
    core::dsl::ImageBuilder& imageBuilder = base;
    imageBuilder.source(heart).build();

    using Builder = core::dsl::ImageBuilder;
    const std::array<std::function<Builder&(Builder&)>, 9> attributes{{
        [](Builder& b) -> Builder& { return b.position(10.f, 20.f); },
        [](Builder& b) -> Builder& { return b.width(54.f).height(54.f); },
        [](Builder& b) -> Builder& { return b.tint({1.f, 0.36f, 0.58f, 1.f}); },
        [](Builder& b) -> Builder& { return b.opacity(0.8f).radius(2.f); },
        [](Builder& b) -> Builder& { return b.contain(); },
        [](Builder& b) -> Builder& { return b.cover().stretch(); },
        [](Builder& b) -> Builder& { return b.translate(1.f, 2.f).scale(1.1f).rotate(0.2f); },
        [](Builder& b) -> Builder& { return b.clip().zIndex(2); },
        [](Builder& b) -> Builder& { return b.flipVertically().blur(1.f); }
    }};
    for (size_t i = 0; i < attributes.size(); ++i) {
        auto builder = ui.svg("attribute." + std::to_string(i));
        attributes[i](builder).source(heart).build();
    }
    ui.image("image.path").size(54.f, 54.f).source("assets/icon.svg").contain().build();
    ui.image("image.stream").source("old.png").stream(stream).build();
    ui.image("image.reset").stream(stream).source("new.png").build();
    ui.image("image.remote").tint({1.f, 1.f, 1.f, 1.f}).source("https://example.com/image.png").build();
    ui.end();
    ui.layout(960.f, 640.f);

    bool passed = true;
    for (const char* id : {"inline.heart", "source.first", "stream.then.source", "stream.then.direct.source", "base.source"})
        passed = check(isSvg(ui.find(id), heart), std::string(id) + ": SVG source lost or stale source retained") && passed;
    for (size_t i = 0; i < attributes.size(); ++i) {
        const auto id = "attribute." + std::to_string(i);
        passed = check(isSvg(ui.find(id), heart), id + ": SVG source lost after chained attribute") && passed;
    }
    passed = check(isSvg(ui.find("source.replaced"), replacement), "Replacing SVG source did not replace markup") && passed;
    passed = check(isSvg(ui.find("source.cleared"), ""), "Empty SVG source did not clear markup") && passed;
    const auto* original = ui.find("inline.heart");
    passed = check(original && original->frame.width == 54.f && original->frame.height == 54.f &&
        original->frame.x == 32.f && original->frame.y == 32.f && original->color.g == 0.36f &&
        original->imageFit == core::ImageFit::Contain, "Chained SVG layout/tint/fit changed") && passed;
    for (const auto& pair : {std::pair{"image.path", "assets/icon.svg"},
                             std::pair{"image.reset", "new.png"},
                             std::pair{"image.remote", "https://example.com/image.png"}}) {
        const auto* element = ui.find(pair.first);
        passed = check(element && element->kind == core::dsl::ElementKind::Image &&
            element->imageSource == pair.second && element->svgSource.empty() && !element->imageStream,
            std::string(pair.first) + ": ordinary image source changed") && passed;
    }
    const auto* streamed = ui.find("image.stream");
    passed = check(streamed && streamed->imageStream == stream && streamed->imageSource.empty() && streamed->svgSource.empty(),
                   "Image stream no longer clears static sources") && passed;
    if (!passed) return 1;

    const auto pixels = core::render::image::loadStaticSvg(original->id, original->svgSource, false);
    if (!check(pixels && pixels->pixels && pixels->byteCount == 512u * 512u * 4u, "Inline SVG failed to rasterize")) return 2;
    size_t visible = 0;
    for (size_t i = 3; i < pixels->byteCount; i += 4) visible += pixels->pixels.get()[i] > 0;
    if (!check(visible > 10000 && visible < 512u * 512u, "Heart is blank or lacks transparent background")) return 3;
    const auto changed = core::render::image::loadStaticSvg(original->id, replacement, false);
    if (!check(changed && changed != pixels && changed->pixels.get()[1] == 255 && changed->pixels.get()[3] == 255,
               "Same-key SVG replacement returned stale cached pixels")) return 4;
    std::cout << "SVG source order, replacement, image compatibility and raster pixels passed\n";
}
