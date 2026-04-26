#include "menu.hpp"

#include "../dependencies/imgui/imgui.h"
#include "../dependencies/imgui/imgui_impl_win32.h"
#include "../dependencies/imgui/imgui_internal.h"

#include <Windows.h>
#include <winhttp.h>
#include <wincodec.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "windowscodecs.lib")

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ig = ImGui;

namespace Menu {
    static bool DownloadURL(const std::string& url, std::vector<uint8_t>& out) {
        std::wstring wUrl(url.begin(), url.end());

        URL_COMPONENTSW uc = {};
        uc.dwStructSize = sizeof(uc);
        wchar_t host[256] = {};
        wchar_t path[2048] = {};
        uc.lpszHostName = host;
        uc.dwHostNameLength = 256;
        uc.lpszUrlPath = path;
        uc.dwUrlPathLength = 2048;
        if (!WinHttpCrackUrl(wUrl.c_str(), 0, 0, &uc))
            return false;

        HINTERNET hSession = WinHttpOpen(L"UHX/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return false;

        HINTERNET hConn = WinHttpConnect(hSession, host, uc.nPort, 0);
        if (!hConn) { WinHttpCloseHandle(hSession); return false; }

        DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", path,
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!hReq) { WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSession); return false; }

        bool ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0)
               && WinHttpReceiveResponse(hReq, nullptr);

        if (ok) {
            char buf[8192];
            DWORD avail = 0, bytesRead = 0;
            while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
                DWORD toRead = min(avail, (DWORD)sizeof(buf));
                if (!WinHttpReadData(hReq, buf, toRead, &bytesRead) || bytesRead == 0) break;
                out.insert(out.end(), buf, buf + bytesRead);
            }
            ok = !out.empty();
        }

        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSession);
        return ok;
    }

    static bool DecodeImageRGBA(const std::vector<uint8_t>& data,
                                std::vector<uint8_t>& rgba, int& w, int& h) {
        IWICImagingFactory* pFactory = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory))))
            return false;

        IWICStream* pStream = nullptr;
        pFactory->CreateStream(&pStream);
        pStream->InitializeFromMemory(
            reinterpret_cast<WICInProcPointer>(const_cast<uint8_t*>(data.data())),
            static_cast<DWORD>(data.size()));

        IWICBitmapDecoder* pDecoder = nullptr;
        bool ok = false;
        if (SUCCEEDED(pFactory->CreateDecoderFromStream(pStream, nullptr,
                WICDecodeMetadataCacheOnLoad, &pDecoder))) {
            IWICBitmapFrameDecode* pFrame = nullptr;
            if (SUCCEEDED(pDecoder->GetFrame(0, &pFrame))) {
                IWICFormatConverter* pConv = nullptr;
                if (SUCCEEDED(pFactory->CreateFormatConverter(&pConv))) {
                    pConv->Initialize(pFrame, GUID_WICPixelFormat32bppRGBA,
                        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
                    UINT width = 0, height = 0;
                    pFrame->GetSize(&width, &height);
                    if (width > 0 && height > 0) {
                        w = (int)width;
                        h = (int)height;
                        rgba.resize((size_t)width * height * 4);
                        ok = SUCCEEDED(pConv->CopyPixels(nullptr, width * 4,
                            static_cast<UINT>(rgba.size()), rgba.data()));
                    }
                    pConv->Release();
                }
                pFrame->Release();
            }
            pDecoder->Release();
        }

        pStream->Release();
        pFactory->Release();
        return ok;
    }

    struct ImgLoad {
        enum class State : int { Loading, Ready, Failed };
        std::atomic<State> state { State::Loading };
        std::vector<uint8_t> pixels;
        int w = 0, h = 0;
    };

    struct Notification {
        std::string title;
        std::string message;
        float durationSeconds;
        std::chrono::steady_clock::time_point start;
        std::chrono::steady_clock::time_point expiry;
        bool started = false;
        std::string imageUrl;
        std::shared_ptr<ImgLoad> img;
        void* imgTex = nullptr;
    };

    static std::vector<Notification> g_notifications;
    static std::mutex g_notificationsMutex;
    static std::unordered_map<std::string, void*> g_texCache;
    static TextureUploaderFn g_textureUploader = nullptr;

    ImFont* g_fontRegular;
    ImFont* g_fontBold;
    ImFont* g_fontBoldLg;

    void RegisterTextureUploader(TextureUploaderFn fn) {
        g_textureUploader = fn;
    }

    void InitializeContext(HWND hwnd) {
        if (ig::GetCurrentContext( ))
            return;

        ImGui::CreateContext( );
        ImGui_ImplWin32_Init(hwnd);

        ImGuiIO& io = ImGui::GetIO( );
        io.IniFilename  = io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        g_fontRegular = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\verdana.ttf", 13.0f);
        g_fontBold = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\verdanab.ttf", 13.0f);
        g_fontBoldLg = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\verdanab.ttf", 15.0f);
    }

    void AddNotification(const std::string& title, const std::string& message,
                         float duration, const std::string& imageUrl) {
        if (!imageUrl.empty()) {
            std::string t = title, m = message, u = imageUrl;
            float d = duration;
            std::thread([t, m, d, u]() {
                HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                bool comInited = SUCCEEDED(hr);

                auto img = std::make_shared<ImgLoad>();
                std::vector<uint8_t> raw;
                int w = 0, h = 0;
                if (DownloadURL(u, raw) && DecodeImageRGBA(raw, img->pixels, w, h)) {
                    img->w = w;
                    img->h = h;
                    img->state.store(ImgLoad::State::Ready, std::memory_order_release);
                } else {
                    img->state.store(ImgLoad::State::Failed, std::memory_order_release);
                }

                if (comInited) CoUninitialize();

                std::lock_guard<std::mutex> lock(g_notificationsMutex);
                Notification n;
                n.title           = t;
                n.message         = m;
                n.durationSeconds = d;
                n.imageUrl        = u;
                n.img             = std::move(img);
                g_notifications.push_back(std::move(n));
            }).detach();
        } else {
            std::lock_guard<std::mutex> lock(g_notificationsMutex);
            Notification n;
            n.title           = title;
            n.message         = message;
            n.durationSeconds = duration;
            g_notifications.push_back(std::move(n));
        }
    }

    static std::string TruncateToTwoLines(const std::string& text, float wrap_width) {
        const float line_h = ImGui::GetTextLineHeight( );
        const float max_h  = line_h * 2.0f + 1.0f;

        if (ImGui::CalcTextSize(text.c_str( ), nullptr, false, wrap_width).y <= max_h)
            return text;

        int lo = 0, hi = (int)text.size( );
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            std::string candidate = text.substr(0, mid) + "...";
            if (ImGui::CalcTextSize(candidate.c_str( ), nullptr, false, wrap_width).y <= max_h)
                lo = mid;
            else
                hi = mid - 1;
        }
        return text.substr(0, lo) + "...";
    }

    static void* TryUploadImage(Notification& n) {
        if (!n.img) return nullptr;
        if (n.imgTex) return n.imgTex;

        if (!n.imageUrl.empty()) {
            auto it = g_texCache.find(n.imageUrl);
            if (it != g_texCache.end()) {
                n.imgTex = it->second;
                return n.imgTex;
            }
        }

        if (n.img->state.load(std::memory_order_acquire) != ImgLoad::State::Ready)
            return nullptr;

        if (g_textureUploader && n.img->w > 0 && n.img->h > 0) {
            n.imgTex = g_textureUploader(n.img->pixels.data(), n.img->w, n.img->h);
            if (n.imgTex && !n.imageUrl.empty())
                g_texCache[n.imageUrl] = n.imgTex;
        }

        n.img->pixels.clear();
        n.img->pixels.shrink_to_fit();
        return n.imgTex;
    }

    void RenderNotifications( ) {
        std::lock_guard<std::mutex> lock(g_notificationsMutex);
        auto now = std::chrono::steady_clock::now( );

        if (!g_notifications.empty( )) {
            Notification& front = g_notifications.front( );
            if (front.started && now >= front.expiry)
                g_notifications.erase(g_notifications.begin( ));
        }

        if (g_notifications.empty( ))
            return;

        Notification& n = g_notifications.front( );

        if (!n.started) {
            n.start = now;
            n.expiry = now + std::chrono::milliseconds((int)(n.durationSeconds));
            n.started = true;
        }

        const float elapsed = std::chrono::duration<float>(now - n.start).count( );
        const float remaining = std::chrono::duration<float>(n.expiry - now).count( );

        const float FADE_IN_TIME = 0.18f;
        const float FADE_OUT_TIME = 0.18f;
        const float alpha_in = (elapsed < FADE_IN_TIME) ? ImClamp(elapsed / FADE_IN_TIME, 0.0f, 1.0f) : 1.0f;
        const float alpha_out = (remaining < FADE_OUT_TIME) ? ImClamp(remaining / FADE_OUT_TIME, 0.0f, 1.0f) : 1.0f;
        const float alpha = alpha_in * alpha_out;

        TryUploadImage(n);

        ImGuiIO& io = ImGui::GetIO( );

        const float outerPad = 12.0f;
        const float width = 320.0f;
        const float height = 90.0f;

        ImGui::SetNextWindowPos({io.DisplaySize.x - width - outerPad, io.DisplaySize.y - height - outerPad});
        ImGui::SetNextWindowSize({width, height});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 5.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.07f, 0.04f, alpha));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.72f, 0.58f, 0.15f, alpha));

        ImGui::Begin("##notif", nullptr,
                        ImGuiWindowFlags_NoDecoration |
                        ImGuiWindowFlags_NoInputs |
                        ImGuiWindowFlags_NoMove |
                        ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImDrawList* draw = ImGui::GetWindowDrawList( );
        ImVec2 wp = ImGui::GetWindowPos( );

        const ImU32 goldCol = IM_COL32(200, 160, 40, (int)(255 * alpha));
        const ImU32 dgoldCol = IM_COL32(140, 105, 15, (int)(255 * alpha));
        const ImU32 whiteCol = IM_COL32(255, 255, 255, (int)(200 * alpha));

        const float headerY = 7.0f;
        ImGui::PushFont(g_fontBold);
        ImGui::SetCursorPos({10.0f, headerY});
        ImGui::TextColored({0.78f, 0.62f, 0.16f, alpha}, "ACHIEVEMENT UNLOCKED");
        const float headerLineY = headerY + ImGui::GetTextLineHeight( ) + 4.0f;
        ImGui::PopFont( );

        draw->AddLine({wp.x + 10.0f, wp.y + headerLineY},
                      {wp.x + width - 10.0f, wp.y + headerLineY},
                      IM_COL32(200, 160, 40, (int)(120 * alpha)), 1.0f);

        const float bodyTop = headerLineY + 6.0f;
        const float iconSize = 44.0f;
        const float iconX = 10.0f;
        const float iconY = bodyTop + ((height - bodyTop) - iconSize) * 0.5f - 4.0f;
        const float textX = iconX + iconSize + 10.0f;
        const float textW = width - textX - 10.0f;
        const float titleY = bodyTop + 4.0f;
        const float bodyTextY = titleY + ImGui::GetTextLineHeight( ) + 3.0f;

        if (n.imgTex) {
            const ImVec2 imgMin = {wp.x + iconX, wp.y + iconY};
            const ImVec2 imgMax = {wp.x + iconX + iconSize, wp.y + iconY + iconSize};
            draw->AddImageRounded(
                reinterpret_cast<ImTextureID>(n.imgTex),
                imgMin, imgMax,
                {0.0f, 0.0f}, {1.0f, 1.0f},
                IM_COL32(255, 255, 255, (int)(255 * alpha)),
                6.0f
            );
        } else {
            const float cx = wp.x + iconX + iconSize * 0.5f;
            const float cy = wp.y + iconY + iconSize * 0.5f;

            draw->AddRectFilled({cx - 12, cy - 14}, {cx + 12, cy + 6}, goldCol, 2.0f);
            draw->AddRectFilled({cx - 14, cy - 16}, {cx + 14, cy - 12}, goldCol);
            draw->AddCircle({cx - 14, cy - 8}, 5.0f, goldCol, 12, 2.5f);
            draw->AddCircle({cx + 14, cy - 8}, 5.0f, goldCol, 12, 2.5f);
            draw->AddRectFilled({cx - 3, cy + 6}, {cx + 3, cy + 14}, dgoldCol);
            draw->AddRectFilled({cx - 10, cy + 14}, {cx + 10, cy + 17}, goldCol);
            draw->AddText({cx - 5, cy - 8}, whiteCol, "*");
        }

        ImGui::PushFont(g_fontBoldLg);
        ImGui::SetCursorPos({textX, titleY});
        ImGui::TextColored({1.0f, 1.0f, 1.0f, alpha}, "%s", n.title.c_str( ));
        ImGui::PopFont( );

        ImGui::PushFont(g_fontRegular);
        const std::string body = TruncateToTwoLines(n.message, textW);
        ImGui::SetCursorPos({textX, bodyTextY});
        ImGui::PushTextWrapPos(textX + textW);
        ImGui::TextColored({0.78f, 0.78f, 0.78f, alpha}, "%s", body.c_str( ));
        ImGui::PopTextWrapPos( );
        ImGui::PopFont( );

        ImGui::End( );
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
    }

    void Render( ) {
        RenderNotifications( );
    }
}
