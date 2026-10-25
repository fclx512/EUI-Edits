#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "dxguid.lib")

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace {

constexpr UINT kWindowWidth = 960;
constexpr UINT kWindowHeight = 640;
constexpr UINT kMenuX = 24;
constexpr UINT kMenuY = 30;
constexpr UINT kMenuWidth = 320;
constexpr UINT kMenuHeight = 400;
constexpr wchar_t kWindowClass[] = L"EuiFrozenBackdropProbeWindow";
constexpr wchar_t kWindowTitle[] = L"EXPERIMENT - Frozen Gaussian backdrop";

std::string hresultText(HRESULT value) {
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
           << static_cast<std::uint32_t>(value);
    return stream.str();
}

double elapsedMs(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

class Probe {
public:
    HRESULT initialize(HWND window, bool useDcTarget) {
        useDcTarget_ = useDcTarget;
        window_ = window;
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                       __uuidof(ID2D1Factory), nullptr,
                                       reinterpret_cast<void**>(factory_.GetAddressOf()));
        if (FAILED(hr)) return hr;

        const D2D1_RENDER_TARGET_PROPERTIES targetProperties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            96.0f, 96.0f, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
        if (useDcTarget_) {
            hr = factory_->CreateDCRenderTarget(&targetProperties, dcTarget_.GetAddressOf());
            target_ = dcTarget_;
        } else {
            const D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProperties =
                D2D1::HwndRenderTargetProperties(window,
                    D2D1::SizeU(kWindowWidth, kWindowHeight), D2D1_PRESENT_OPTIONS_NONE);
            hr = factory_->CreateHwndRenderTarget(targetProperties, hwndProperties, hwndTarget_.GetAddressOf());
            target_ = hwndTarget_;
        }
        if (FAILED(hr)) return hr;

        queryHresult_ = target_.As(&deviceContext_);
        if (SUCCEEDED(queryHresult_)) {
            effectHresult_ = deviceContext_->CreateEffect(CLSID_D2D1GaussianBlur,
                                                           gaussianEffect_.GetAddressOf());
        } else {
            effectHresult_ = queryHresult_;
        }

        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(writeFactory_.GetAddressOf()));
        if (FAILED(hr)) return hr;
        hr = writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                             16.0f, L"en-us", textFormat_.GetAddressOf());
        if (FAILED(hr)) return hr;
        textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        hr = createBrushes();
        if (FAILED(hr)) return hr;
        const auto cacheStart = Clock::now();
        hr = createFilterCache();
        cacheSetupMs_ = elapsedMs(cacheStart, Clock::now());
        return hr;
    }

    HRESULT drawFrame(int round, bool visibleDemo) {
        const D2D1_RECT_U sourceRect{kMenuX, kMenuY,
                                     kMenuX + kMenuWidth, kMenuY + kMenuHeight};
        const D2D1_POINT_2U copyPoint{0, 0};
        const D2D1_RECT_F menuRect = D2D1::RectF(
            static_cast<float>(kMenuX), static_cast<float>(kMenuY),
            static_cast<float>(kMenuX + kMenuWidth), static_cast<float>(kMenuY + kMenuHeight));

        HRESULT hr = S_OK, copyNoClipHr = E_PENDING, copyClipHr = E_PENDING, filterHr = E_PENDING;
        HRESULT endHr = S_OK;
        double copyNoClipMs = 0.0, copyClipMs = 0.0, filterMs = 0.0, compositeMs = 0.0;
        bool outputBitmapValid = false;
        const char* filterSource = "cached_content_reuse";
        const bool captureFilterRound = round == 0 || !hasSuccessfulFilter_;
        if (useDcTarget_) {
            HDC dc = GetDC(window_);
            if (dc == nullptr) hr = HRESULT_FROM_WIN32(GetLastError());
            else {
                RECT client{0, 0, static_cast<LONG>(kWindowWidth), static_cast<LONG>(kWindowHeight)};
                hr = dcTarget_->BindDC(dc, &client);
                activeDc_ = dc;
                if (FAILED(hr)) { ReleaseDC(window_, dc); activeDc_ = nullptr; }
            }
        }
        const bool beganDraw = SUCCEEDED(hr);
        if (beganDraw) target_->BeginDraw();
        const auto reuseStart = Clock::now();
        bool cacheReady = capture_.Get() != nullptr && outputTarget_.Get() != nullptr;
        const double cacheReuseValidationMs = elapsedMs(reuseStart, Clock::now());

        if (SUCCEEDED(hr) && cacheReady) {
            drawBackdrop();
            if (captureFilterRound) {
                auto copyStart = Clock::now();
                copyNoClipHr = capture_->CopyFromRenderTarget(&copyPoint, target_.Get(), &sourceRect);
                copyNoClipMs = elapsedMs(copyStart, Clock::now());

                // Model the active scissor used by the normal Direct2D presentation path.
                target_->PushAxisAlignedClip(menuRect, D2D1_ANTIALIAS_MODE_ALIASED);
                copyStart = Clock::now();
                copyClipHr = captureWithClip_->CopyFromRenderTarget(&copyPoint, target_.Get(), &sourceRect);
                copyClipMs = elapsedMs(copyStart, Clock::now());
                target_->PopAxisAlignedClip();

                ID2D1Bitmap* filterInput = SUCCEEDED(copyClipHr) ? captureWithClip_.Get()
                                                                 : capture_.Get();
                if (SUCCEEDED(copyClipHr)) filterSource = "active_clip";
                else if (SUCCEEDED(copyNoClipHr)) filterSource = "no_clip_fallback";

                if (SUCCEEDED(copyClipHr) || SUCCEEDED(copyNoClipHr)) {
                    const auto filterStart = Clock::now();
                    filterHr = buildGaussianBlur(filterInput);
                    filterMs = elapsedMs(filterStart, Clock::now());
                    if (SUCCEEDED(filterHr)) hasSuccessfulFilter_ = true;
                } else {
                    filterHr = copyClipHr;
                }
            }

            const auto compositeStart = Clock::now();
            if (hasSuccessfulFilter_ && outputBitmap_) {
                target_->DrawBitmap(outputBitmap_.Get(), &menuRect, 1.0f,
                                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
            }
            const D2D1_COLOR_F tint = darkTheme_
                ? D2D1::ColorF(0.075f, 0.085f, 0.11f, 0.76f)
                : D2D1::ColorF(0.93f, 0.95f, 0.98f, 0.72f);
            menuBrush_->SetColor(tint);
            target_->FillRectangle(menuRect, menuBrush_.Get());
            menuBrush_->SetColor(D2D1::ColorF(0.25f, 0.32f, 0.43f, 0.72f));
            target_->DrawRectangle(menuRect, menuBrush_.Get(), 1.0f);
            menuBrush_->SetColor(D2D1::ColorF(0.25f, 0.32f, 0.43f, 0.72f));
            textBrush_->SetColor(darkTheme_ ? D2D1::ColorF(0.91f, 0.93f, 0.97f, 0.95f)
                                            : D2D1::ColorF(0.12f, 0.15f, 0.20f, 0.95f));
            constexpr wchar_t heading[] = L"EXPERIMENTAL FROZEN BACKDROP";
            target_->DrawText(heading,
                               static_cast<UINT32>(std::wcslen(heading)),
                               textFormat_.Get(), D2D1::RectF(kMenuX + 18.0f, kMenuY + 16.0f,
                               kMenuX + kMenuWidth - 14.0f, kMenuY + 44.0f), textBrush_.Get());
            menuBrush_->SetColor(D2D1::ColorF(0.64f, 0.69f, 0.77f, 0.75f));
            target_->DrawLine(D2D1::Point2F(kMenuX + 12.0f, kMenuY + 52.0f),
                              D2D1::Point2F(kMenuX + kMenuWidth - 12.0f, kMenuY + 52.0f),
                              menuBrush_.Get(), 1.0f);
            for (int row = 0; row < 8; ++row) {
                const float y = static_cast<float>(kMenuY + 68 + row * 38);
                constexpr wchar_t command[] = L"Menu command - clear outside";
                constexpr wchar_t captured[] = L"Static captured backdrop";
                const wchar_t* rowText = row % 2 == 0 ? command : captured;
                target_->DrawText(rowText, static_cast<UINT32>(std::wcslen(rowText)), textFormat_.Get(),
                                   D2D1::RectF(kMenuX + 16.0f, y, kMenuX + kMenuWidth - 12.0f, y + 26.0f),
                                   textBrush_.Get());
                target_->DrawLine(D2D1::Point2F(kMenuX + 10.0f, y + 31.0f),
                                  D2D1::Point2F(kMenuX + kMenuWidth - 10.0f, y + 31.0f),
                                  menuBrush_.Get(), 0.5f);
            }
            compositeMs = elapsedMs(compositeStart, Clock::now());
            endHr = target_->EndDraw();
            if (activeDc_ != nullptr) { ReleaseDC(window_, activeDc_); activeDc_ = nullptr; }

            if (hasSuccessfulFilter_ && outputBitmap_) {
                const D2D1_SIZE_U size = outputBitmap_->GetPixelSize();
                const D2D1_PIXEL_FORMAT format = outputBitmap_->GetPixelFormat();
                const D2D1_PIXEL_FORMAT expectedFormat = target_->GetPixelFormat();
                outputBitmapValid = size.width == kMenuWidth && size.height == kMenuHeight &&
                    format.format == expectedFormat.format && format.alphaMode == expectedFormat.alphaMode;
            }
        } else {
            copyNoClipHr = copyClipHr = filterHr = FAILED(hr) ? hr : E_UNEXPECTED;
            endHr = beganDraw ? target_->EndDraw() : (FAILED(hr) ? hr : E_UNEXPECTED);
            if (activeDc_ != nullptr) { ReleaseDC(window_, activeDc_); activeDc_ = nullptr; }
        }

        std::cout << std::fixed << std::setprecision(4)
                  << "{\"event\":\"round\",\"round\":" << round
                  << ",\"mode\":\"" << (visibleDemo ? "show" : "hidden") << "\""
                  << ",\"query_hresult\":\"" << hresultText(queryHresult_) << "\""
                  << ",\"gaussian_effect_hresult\":\"" << hresultText(effectHresult_) << "\""
                  << ",\"copy_no_clip_hresult\":\"" << hresultText(copyNoClipHr) << "\""
                  << ",\"copy_active_clip_hresult\":\"" << hresultText(copyClipHr) << "\""
                  << ",\"filter_hresult\":\"" << hresultText(filterHr) << "\""
                  << ",\"end_draw_hresult\":\"" << hresultText(endHr) << "\""
                  << ",\"copy_no_clip_ms\":" << copyNoClipMs
                  << ",\"copy_active_clip_ms\":" << copyClipMs
                  << ",\"filter_ms\":" << filterMs
                  << ",\"composite_ms\":" << compositeMs
                  << ",\"filter_source\":\"" << filterSource << "\""
                  << ",\"cache_setup_ms\":" << cacheSetupMs_
                  << ",\"cache_reuse_validation_ms\":" << cacheReuseValidationMs
                  << ",\"filter_targets_available\":" << (cacheReady ? "true" : "false")
                  << ",\"capture_filter_round\":" << (captureFilterRound ? "true" : "false")
                  << ",\"cached_filter_content_reused\":" << (!captureFilterRound && hasSuccessfulFilter_ ? "true" : "false")
                  << ",\"target_kind\":\"" << (useDcTarget_ ? "d2d10_software_dc" : "d2d10_software_hwnd") << "\""
                  << ",\"output_bitmap_valid\":" << (outputBitmapValid ? "true" : "false")
                  << ",\"pixel_readback\":\"unavailable_via_d2d10_bitmap_api\"}\n";
        if (FAILED(hr)) return hr;
        if (captureFilterRound) {
            if (FAILED(copyNoClipHr) && FAILED(copyClipHr)) return FAILED(copyClipHr) ? copyClipHr : copyNoClipHr;
            if (FAILED(filterHr)) return filterHr;
        }
        return endHr;
    }

    void setDarkTheme(bool value) { darkTheme_ = value; }

private:
    HRESULT buildGaussianBlur(ID2D1Bitmap* input) {
        ComPtr<ID2D1DeviceContext> context;
        HRESULT hr = outputTarget_.As(&context);
        if (FAILED(hr)) return hr;
        ComPtr<ID2D1Effect> effect;
        hr = context->CreateEffect(CLSID_D2D1GaussianBlur, &effect);
        if (FAILED(hr)) return hr;
        effect->SetInput(0, input);
        hr = effect->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 8.0f);
        if (FAILED(hr)) return hr;
        hr = effect->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
        if (FAILED(hr)) return hr;
        outputBitmap_.Reset();
        context->BeginDraw();
        context->Clear(D2D1::ColorF(0, 0, 0, 0));
        context->DrawImage(effect.Get());
        hr = context->EndDraw();
        if (FAILED(hr)) return hr;
        return outputTarget_->GetBitmap(&outputBitmap_);
    }

    HRESULT createBrushes() {
        HRESULT hr = target_->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.16f, 0.25f, 1.0f),
                                                     gridBrush_.GetAddressOf());
        if (FAILED(hr)) return hr;
        hr = target_->CreateSolidColorBrush(D2D1::ColorF(0.85f, 0.37f, 0.20f, 1.0f),
                                            accentBrush_.GetAddressOf());
        if (FAILED(hr)) return hr;
        hr = target_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.2f), menuBrush_.GetAddressOf());
        if (FAILED(hr)) return hr;
        return target_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), textBrush_.GetAddressOf());
    }

    HRESULT createFilterCache() {
        const D2D1_PIXEL_FORMAT format = target_->GetPixelFormat();
        const D2D1_BITMAP_PROPERTIES bitmapProperties = D2D1::BitmapProperties(format, 96.0f, 96.0f);
        HRESULT hr = target_->CreateBitmap(D2D1::SizeU(kMenuWidth, kMenuHeight), nullptr, 0,
                                           bitmapProperties, capture_.GetAddressOf());
        if (FAILED(hr)) return hr;
        hr = target_->CreateBitmap(D2D1::SizeU(kMenuWidth, kMenuHeight), nullptr, 0,
                                   bitmapProperties, captureWithClip_.GetAddressOf());
        if (FAILED(hr)) return hr;

        const D2D1_SIZE_F fullSize{static_cast<float>(kMenuWidth), static_cast<float>(kMenuHeight)};
        const D2D1_SIZE_U fullPixelSize{kMenuWidth, kMenuHeight};
        return target_->CreateCompatibleRenderTarget(
            &fullSize, &fullPixelSize, &format,
            D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, outputTarget_.GetAddressOf());
    }

    void drawBackdrop() {
        target_->Clear(D2D1::ColorF(0.055f, 0.070f, 0.095f, 1.0f));
        for (int index = 0; index < 24; ++index) {
            const float offset = static_cast<float>(index * 31);
            const float y = std::fmod(offset * 1.7f, static_cast<float>(kWindowHeight));
            const float x = std::fmod(offset * 2.3f, static_cast<float>(kWindowWidth));
            const float thickness = index % 4 == 0 ? 4.0f : 1.0f;
            target_->DrawLine(D2D1::Point2F(0, y), D2D1::Point2F(kWindowWidth, y),
                              index % 5 == 0 ? accentBrush_.Get() : gridBrush_.Get(), thickness);
            target_->DrawLine(D2D1::Point2F(x, 0), D2D1::Point2F(x, kWindowHeight),
                              index % 6 == 0 ? accentBrush_.Get() : gridBrush_.Get(), thickness);
        }
    }

    ComPtr<ID2D1Factory> factory_;
    ComPtr<ID2D1RenderTarget> target_;
    ComPtr<ID2D1HwndRenderTarget> hwndTarget_;
    ComPtr<ID2D1DCRenderTarget> dcTarget_;
    ComPtr<ID2D1DeviceContext> deviceContext_;
    ComPtr<ID2D1Effect> gaussianEffect_;
    ComPtr<IDWriteFactory> writeFactory_;
    ComPtr<IDWriteTextFormat> textFormat_;
    ComPtr<ID2D1SolidColorBrush> gridBrush_;
    ComPtr<ID2D1SolidColorBrush> accentBrush_;
    ComPtr<ID2D1SolidColorBrush> menuBrush_;
    ComPtr<ID2D1SolidColorBrush> textBrush_;
    ComPtr<ID2D1Bitmap> capture_;
    ComPtr<ID2D1Bitmap> captureWithClip_;
    ComPtr<ID2D1BitmapRenderTarget> outputTarget_;
    ComPtr<ID2D1Bitmap> outputBitmap_;
    HRESULT queryHresult_ = E_NOINTERFACE;
    HRESULT effectHresult_ = E_NOINTERFACE;
    double cacheSetupMs_ = 0.0;
    bool darkTheme_ = true;
    bool hasSuccessfulFilter_ = false;
    bool useDcTarget_ = true;
    HWND window_ = nullptr;
    HDC activeDc_ = nullptr;
};

Probe* gProbe = nullptr;
bool gShow = false;
int gShowRound = 0;

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(window, &paint);
            if (gShow && gProbe != nullptr) gProbe->drawFrame(gShowRound++, true);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
    }
}

HWND createWindow() {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;

    RECT bounds{0, 0, static_cast<LONG>(kWindowWidth), static_cast<LONG>(kWindowHeight)};
    const DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&bounds, style, FALSE);
    HWND window = CreateWindowExW(0, kWindowClass, kWindowTitle, style,
                                  CW_USEDEFAULT, CW_USEDEFAULT,
                                  bounds.right - bounds.left, bounds.bottom - bounds.top,
                                  nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    return window;
}

int parseRounds(int argc, wchar_t** argv, bool& show, bool& dark, bool& useDcTarget) {
    int rounds = 20;
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        if (argument == L"--show") show = true;
        else if (argument == L"--light") dark = false;
        else if (argument == L"--dark") dark = true;
        else if (argument == L"--hwnd") useDcTarget = false;
        else if (argument == L"--dc" || argument == L"--software") useDcTarget = true;
        else if (argument == L"--rounds" && index + 1 < argc) {
            rounds = std::max(1, _wtoi(argv[++index]));
        }
    }
    return rounds;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    bool show = false;
    bool dark = true;
    bool useDcTarget = true;
    const int rounds = parseRounds(argc, argv, show, dark, useDcTarget);
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
        std::cout << "{\"event\":\"init_error\",\"stage\":\"CoInitializeEx\",\"hresult\":\""
                  << hresultText(comHr) << "\"}\n";
        return 2;
    }

    HWND window = createWindow();
    if (window == nullptr) {
        const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        std::cout << "{\"event\":\"init_error\",\"stage\":\"CreateWindowExW\",\"hresult\":\""
                  << hresultText(hr) << "\"}\n";
        if (SUCCEEDED(comHr)) CoUninitialize();
        return 2;
    }

    Probe probe;
    probe.setDarkTheme(dark);
    const HRESULT initHr = probe.initialize(window, useDcTarget);
    if (FAILED(initHr)) {
        std::cout << "{\"event\":\"init_error\",\"stage\":\"D2D_initialization\",\"hresult\":\""
                  << hresultText(initHr) << "\"}\n";
        DestroyWindow(window);
        if (SUCCEEDED(comHr)) CoUninitialize();
        return 2;
    }

    std::cout << "{\"event\":\"cache\",\"status\":\"created\",\"compatible_levels\":"
              << 1 << ",\"filter\":\"gaussian_sigma_8_frozen\"}\n";
    if (show) {
        gProbe = &probe;
        gShow = true;
        ShowWindow(window, SW_SHOW);
        InvalidateRect(window, nullptr, FALSE);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    } else {
        for (int round = 0; round < rounds; ++round) {
            const HRESULT frameHr = probe.drawFrame(round, false);
            if (FAILED(frameHr)) {
                std::cout << "{\"event\":\"run_error\",\"round\":" << round
                          << ",\"hresult\":\"" << hresultText(frameHr) << "\"}\n";
                DestroyWindow(window);
                if (SUCCEEDED(comHr)) CoUninitialize();
                return 3;
            }
        }
    }

    DestroyWindow(window);
    if (SUCCEEDED(comHr)) CoUninitialize();
    return 0;
}

#else
#include <iostream>
int main() { std::cout << "SKIP: frozen backdrop probe requires Windows\n"; return 0; }
#endif
