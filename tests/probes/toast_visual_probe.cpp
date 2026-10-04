#include "components/toast.h"
#include "components/vector_icon.h"
#include "core/dsl_runtime.h"
#include "core/render/render_backend.h"
#include "core/window/window_backend.h"
#if defined(EUI_WINDOW_BACKEND_WIN32)
#include "core/window/win32_host.h"
namespace host = core::window::win32;
#else
#define GLFW_INCLUDE_NONE
#include "core/window/glfw_host.h"
namespace host = core::window::glfwHost;
#endif
#include <chrono>
#include <cstdlib>
#include <thread>

int main(int argc, char** argv) {
    int frames = 3; float scale = 1, fontSize = 14; bool dark = false;
    for (int i=1; i<argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--frames" && i+1<argc) frames=std::atoi(argv[++i]);
        if (arg == "--scale" && i+1<argc) scale=static_cast<float>(std::atof(argv[++i]));
        if (arg == "--font" && i+1<argc) fontSize=static_cast<float>(std::atof(argv[++i]));
        if (arg == "--dark") dark=true;
    }
    if (!host::initialize()) return 1;
    core::window::WindowCreateRequest request;
    request.width=800; request.height=520; request.title="EUI-Edits notification fixture";
    request.renderApi=core::render::windowRenderApi();
    auto window=core::window::createWindow(request);
    if (!window) return 2;
    auto backend=core::render::createRenderBackend(window);
    if (!backend || !backend->initialize()) return 3;
    core::render::ScopedRenderBackend active(*backend);
    core::dsl::Runtime runtime;
    if (!runtime.initialize(window)) return 4;
    core::window::installInputCallbacks(window);
    bool visible=true;
    for (int frame=0; frames<0 || frame<frames; ++frame) {
        host::pollEvents(); if (host::shouldClose(static_cast<host::Window*>(window))) break;
        int width=0,height=0; float dpiX=1,dpiY=1;
        host::framebufferSize(static_cast<host::Window*>(window), &width,&height);
        host::contentScale(static_cast<host::Window*>(window), &dpiX,&dpiY);
        const float unit=std::max(0.5f,dpiX*scale);
        core::TextPrimitive::setLayoutPixelScale(unit);
        const auto tokens=dark?components::theme::dark():components::theme::light();
        runtime.compose("notification-fixture",width/unit,height/unit,[&](core::dsl::Ui& ui,const core::dsl::Screen& screen) {
            components::toast(ui,"sample").visible(visible).screen(screen.width,screen.height).theme(tokens)
                .title("注销未完成").message("请先在 Windows 默认应用设置中将 .txt 和 .md 改选为其他程序，再注销 EUI-Edits。")
                .fontFamily("Microsoft YaHei").fontSize(fontSize).titleFontSize(fontSize+3)
                .transition(core::Transition::none()).onDismiss([&]{visible=false;})
                .iconRenderer([](core::dsl::Ui& iconUi,const std::string& id,float x,float y,float size,core::Color color) {
                    iconUi.rect(id+".ring").position(x,y).size(size,size).radius(size*.5f)
                        .color({0,0,0,0}).border(1.5f,color).build();
                    components::vector_icon::segment(iconUi,id+".stem",x,y,size,size,
                        {size*.5f,size*.46f},{size*.5f,size*.76f},size*.08f,color);
                    iconUi.rect(id+".dot").position(x+size*.46f,y+size*.26f).size(size*.08f,size*.08f)
                        .radius(size*.04f).color(color).build();
                }).build();
            const auto visit=[](auto&& self,core::dsl::Element& element)->void {
                element.transition=core::Transition::none(); element.smoothStateColors=false;
                for (auto& child:element.children) self(self,*child);
            };
            for (auto& root:ui.roots()) visit(visit,*root);
        });
        runtime.update(window,1.f/60,unit,unit);
        backend->beginFrame({window,core::window::nativeWindowInfo(window),width,height,unit});
        runtime.render(width,height,unit,tokens.background); backend->present();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    core::window::uninstallInputCallbacks(window);
    runtime.shutdown(); backend.reset(); core::window::destroyWindow(window);host::shutdownHost();return 0;
}
