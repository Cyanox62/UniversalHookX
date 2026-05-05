#include "../../../backend.hpp"
#include "../../../console/console.hpp"

#ifdef ENABLE_BACKEND_DDRAW
#include <Windows.h>
#include <ddraw.h>
#pragma comment(lib, "ddraw.lib")
#pragma comment(lib, "dxguid.lib")

#include <d3d9.h>
#pragma comment(lib, "d3d9.lib")

#include "../../../dependencies/imgui/imgui_impl_dx9.h"
#include "../../../dependencies/imgui/imgui_impl_win32.h"
#include "../../../dependencies/minhook/MinHook.h"

#include "../../../menu/menu.hpp"
#include "../../../utils/utils.hpp"
#include "../../hooks.hpp"

static HWND     g_hGameWnd    = NULL;
static HWND     g_hHelperWnd  = NULL;
static IUnknown* g_pPrimaryIdentity = nullptr;
static IDirect3D9*        g_pD3D      = nullptr;
static IDirect3DDevice9*  g_pDevice   = nullptr;
static IDirect3DSurface9* g_pReadback = nullptr;
static UINT g_width  = 0;
static UINT g_height = 0;

static volatile bool g_bComposedThisFrame = false;
static bool g_bHasD3D7Device = false;

static std::add_pointer_t<HRESULT WINAPI(IDirectDrawSurface7*, IDirectDrawSurface7*, DWORD)> oFlip;
static std::add_pointer_t<HRESULT WINAPI(IDirectDrawSurface7*, LPRECT, IDirectDrawSurface7*, LPRECT, DWORD, LPDDBLTFX)> oBlt;
static std::add_pointer_t<HRESULT WINAPI(IDirectDrawSurface7*, DWORD, DWORD, IDirectDrawSurface7*, LPRECT, DWORD)> oBltFast;

static const GUID IID_IDirect3D7_UHX = {
    0xf5049e77, 0x4861, 0x11d2, {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}
};

static constexpr int D3D7_VTI_CreateDevice = 4;
static constexpr int D3D7DEV_VTI_EndScene       = 6;
static constexpr int D3D7DEV_VTI_GetRenderTarget = 9;

using D3D7CreateDevice_t    = HRESULT(WINAPI*)(IUnknown*, REFCLSID, IDirectDrawSurface7*, IUnknown**);
using D3D7EndScene_t        = HRESULT(WINAPI*)(IUnknown*);
using D3D7GetRenderTarget_t = HRESULT(WINAPI*)(IUnknown*, IDirectDrawSurface7**);

static D3D7CreateDevice_t oD3D7CreateDevice = nullptr;
static D3D7EndScene_t     oD3D7EndScene     = nullptr;

static void DebugLog(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
}

static void* UploadTextureRGBA_DDraw(const uint8_t* rgba, int w, int h) {
    if (!g_pDevice)
        return nullptr;

    IDirect3DTexture9* pTex = nullptr;
    if (FAILED(g_pDevice->CreateTexture((UINT)w, (UINT)h, 1, D3DUSAGE_DYNAMIC,
                                        D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &pTex, nullptr)))
        return nullptr;

    D3DLOCKED_RECT lr;
    if (FAILED(pTex->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD))) {
        pTex->Release( );
        return nullptr;
    }
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = rgba + (size_t)y * w * 4;
        uint8_t*       dst = reinterpret_cast<uint8_t*>(lr.pBits) + (size_t)y * lr.Pitch;
        for (int x = 0; x < w; ++x) {
            dst[x * 4 + 0] = src[x * 4 + 2]; // B
            dst[x * 4 + 1] = src[x * 4 + 1]; // G
            dst[x * 4 + 2] = src[x * 4 + 0]; // R
            dst[x * 4 + 3] = src[x * 4 + 3]; // A
        }
    }
    pTex->UnlockRect(0);
    return pTex;
}

static void DestroyD3D9Resources( ) {
    Menu::InvalidateDeviceTextures([](void* tex) {
        static_cast<IDirect3DTexture9*>(tex)->Release( );
    });

    if (ImGui::GetCurrentContext( ) && ImGui::GetIO( ).BackendRendererUserData)
        ImGui_ImplDX9_Shutdown( );
    if (g_pReadback) { g_pReadback->Release( ); g_pReadback = nullptr; }
    if (g_pDevice)   { g_pDevice->Release( );   g_pDevice   = nullptr; }
    if (g_pD3D)      { g_pD3D->Release( );      g_pD3D      = nullptr; }
}

static HWND CreateHelperWindow( ) {
    WNDCLASSEXA wc   = { };
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.lpszClassName = "UHX_DDraw_Helper";
    RegisterClassExA(&wc); // harmless if already registered
    return CreateWindowExA(0, "UHX_DDraw_Helper", nullptr, WS_POPUP,
                           0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
}

static bool TryCreateDevice(UINT w, UINT h, HWND devWnd) {
    D3DPRESENT_PARAMETERS pp    = { };
    pp.Windowed                 = TRUE;
    pp.SwapEffect               = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth          = w;
    pp.BackBufferHeight         = h;
    pp.BackBufferFormat         = D3DFMT_A8R8G8B8;
    pp.hDeviceWindow            = devWnd;
    pp.PresentationInterval     = D3DPRESENT_INTERVAL_IMMEDIATE;

    HRESULT hr = g_pD3D->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, devWnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE,
        &pp, &g_pDevice);
    if (FAILED(hr))
        hr = g_pD3D->CreateDevice(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, devWnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE,
            &pp, &g_pDevice);
    if (FAILED(hr))
        DebugLog("[UHX] DDraw: TryCreateDevice failed hr=0x%08X on hwnd=%p\n", (unsigned)hr, (void*)devWnd);
    return SUCCEEDED(hr);
}

static bool CreateOffscreenD3D9(UINT w, UINT h) {
    g_pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_pD3D) {
        OutputDebugStringA("[UHX] DDraw: Direct3DCreate9 failed\n");
        return false;
    }

    if (!TryCreateDevice(w, h, g_hGameWnd)) {
        DebugLog("[UHX] DDraw: game-window device failed, trying helper window\n");
        if (!g_hHelperWnd)
            g_hHelperWnd = CreateHelperWindow( );
        if (!g_hHelperWnd || !TryCreateDevice(w, h, g_hHelperWnd)) {
            DebugLog("[UHX] DDraw: CreateDevice failed on both windows\n");
            g_pD3D->Release( );
            g_pD3D = nullptr;
            return false;
        }
    }

    HRESULT hr = g_pDevice->CreateOffscreenPlainSurface(
        w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &g_pReadback, nullptr);
    if (FAILED(hr)) {
        DebugLog("[UHX] DDraw: CreateOffscreenPlainSurface failed hr=0x%08X\n", (unsigned)hr);
        g_pDevice->Release( ); g_pDevice = nullptr;
        g_pD3D->Release( );   g_pD3D   = nullptr;
        return false;
    }

    g_width  = w;
    g_height = h;
    DebugLog("[UHX] DDraw: Offscreen D3D9 ready %ux%u\n", w, h);
    return true;
}

static void CompositeImGuiIntoDDrawSurface(IDirectDrawSurface7* pDDSurface) {
    static std::atomic<bool> s_rendering{false};
    bool expected = false;
    if (!s_rendering.compare_exchange_strong(expected, true))
        return;
    struct Guard { ~Guard( ) { s_rendering.store(false); } } guard;

    if (U::GetRenderingBackend( ) != DIRECTDRAW)
        return;

    UINT surfW = 0, surfH = 0;
    {
        DDSURFACEDESC2 surfInfo = { };
        surfInfo.dwSize = sizeof(surfInfo);
        if (SUCCEEDED(pDDSurface->GetSurfaceDesc(&surfInfo))) {
            surfW = surfInfo.dwWidth;
            surfH = surfInfo.dwHeight;
        }
    }
    if (surfW == 0 || surfH == 0) {
        RECT rc = { };
        if (GetClientRect(g_hGameWnd, &rc) && rc.right > 0 && rc.bottom > 0) {
            surfW = (UINT)(rc.right  - rc.left);
            surfH = (UINT)(rc.bottom - rc.top);
        }
    }
    if (surfW == 0 || surfH == 0) {
        surfW = (UINT)GetSystemMetrics(SM_CXSCREEN);
        surfH = (UINT)GetSystemMetrics(SM_CYSCREEN);
    }
    if (surfW == 0 || surfH == 0) {
        OutputDebugStringA("[UHX] DDraw: composite - could not determine surface dimensions\n");
        return;
    }

    if (!g_pDevice || surfW != g_width || surfH != g_height) {
        DebugLog("[UHX] DDraw: (re)creating D3D9 offscreen %ux%u\n", surfW, surfH);
        DestroyD3D9Resources( );
        if (!CreateOffscreenD3D9(surfW, surfH)) {
            LOG("[!] DDraw: Failed to create offscreen D3D9 device.\n");
            return;
        }
    }

    if (!ImGui::GetIO( ).BackendRendererUserData) {
        OutputDebugStringA("[UHX] DDraw: initializing ImGui\n");
        if (ImGui::GetIO( ).BackendPlatformUserData)
            ImGui_ImplWin32_Shutdown( );
        ImGui_ImplWin32_Init(g_hGameWnd);
        ImGui_ImplDX9_Init(g_pDevice);
        Menu::RegisterTextureUploader(UploadTextureRGBA_DDraw);
        OutputDebugStringA("[UHX] DDraw: ImGui initialized\n");
    }

    if (H::bShuttingDown || !ImGui::GetCurrentContext( ))
        return;

    g_pDevice->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);

    ImGui_ImplDX9_NewFrame( );
    ImGui_ImplWin32_NewFrame( );
    ImGui::NewFrame( );
    Menu::Render( );
    ImGui::EndFrame( );

    if (g_pDevice->BeginScene( ) == D3D_OK) {
        ImGui::Render( );
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData( ));
        g_pDevice->EndScene( );
    }

    IDirect3DSurface9* pRT = nullptr;
    if (FAILED(g_pDevice->GetRenderTarget(0, &pRT)) || !pRT) {
        OutputDebugStringA("[UHX] DDraw: GetRenderTarget failed\n");
        return;
    }
    HRESULT hr = g_pDevice->GetRenderTargetData(pRT, g_pReadback);
    pRT->Release( );
    if (FAILED(hr)) {
        DebugLog("[UHX] DDraw: GetRenderTargetData failed hr=0x%08X\n", (unsigned)hr);
        return;
    }

    D3DLOCKED_RECT srcLR;
    if (FAILED(g_pReadback->LockRect(&srcLR, nullptr, D3DLOCK_READONLY))) {
        OutputDebugStringA("[UHX] DDraw: readback LockRect failed\n");
        return;
    }

    DDSURFACEDESC2 ddsd = { };
    ddsd.dwSize = sizeof(ddsd);
    hr = pDDSurface->Lock(nullptr, &ddsd, DDLOCK_WAIT | DDLOCK_NOSYSLOCK, nullptr);
    if (FAILED(hr)) {
        DebugLog("[UHX] DDraw: Lock(NOSYSLOCK) failed hr=0x%08X, retrying without\n", (unsigned)hr);
        hr = pDDSurface->Lock(nullptr, &ddsd, DDLOCK_WAIT, nullptr);
    }
    if (FAILED(hr)) {
        DebugLog("[UHX] DDraw: Surface Lock failed hr=0x%08X\n", (unsigned)hr);
        g_pReadback->UnlockRect( );
        return;
    }

    static bool s_formatLogged = false;
    if (!s_formatLogged) {
        s_formatLogged = true;
        DebugLog("[UHX] DDraw: surface bpp=%lu pitch=%ld size=%lux%lu "
                 "rMask=0x%08lX gMask=0x%08lX bMask=0x%08lX\n",
                 ddsd.ddpfPixelFormat.dwRGBBitCount,
                 ddsd.lPitch,
                 ddsd.dwWidth, ddsd.dwHeight,
                 ddsd.ddpfPixelFormat.dwRBitMask,
                 ddsd.ddpfPixelFormat.dwGBitMask,
                 ddsd.ddpfPixelFormat.dwBBitMask);
    }

    DWORD bpp   = ddsd.ddpfPixelFormat.dwRGBBitCount;
    DWORD rMask = ddsd.ddpfPixelFormat.dwRBitMask;

    UINT copyW = min(g_width,  (UINT)ddsd.dwWidth);
    UINT copyH = min(g_height, (UINT)ddsd.dwHeight);

    if (bpp == 32) {
       int rOff = (rMask == 0x000000FFu) ? 0 : 2; // byte offset of Red channel
        int bOff = (rMask == 0x000000FFu) ? 2 : 0; // byte offset of Blue channel

        for (UINT y = 0; y < copyH; ++y) {
            const uint8_t* srcRow = reinterpret_cast<const uint8_t*>(srcLR.pBits)
                                    + (size_t)y * srcLR.Pitch;
            uint8_t* dstRow = reinterpret_cast<uint8_t*>(ddsd.lpSurface)
                              + (size_t)y * ddsd.lPitch;
            for (UINT x = 0; x < copyW; ++x) {
                uint8_t sB = srcRow[x * 4 + 0];
                uint8_t sG = srcRow[x * 4 + 1];
                uint8_t sR = srcRow[x * 4 + 2];
                uint8_t sA = srcRow[x * 4 + 3];
                if (sA == 0)
                    continue;
                if (sA == 255) {
                    dstRow[x * 4 + bOff] = sB;
                    dstRow[x * 4 + 1]    = sG;
                    dstRow[x * 4 + rOff] = sR;
                } else {
                    uint32_t a = sA, ia = 255u - sA;
                    dstRow[x * 4 + bOff] = (uint8_t)((a * sB + ia * dstRow[x * 4 + bOff]) / 255u);
                    dstRow[x * 4 + 1]    = (uint8_t)((a * sG + ia * dstRow[x * 4 + 1   ]) / 255u);
                    dstRow[x * 4 + rOff] = (uint8_t)((a * sR + ia * dstRow[x * 4 + rOff]) / 255u);
                }
            }
        }
    } else if (bpp == 16) {
        bool isR5G6B5 = (rMask == 0xF800u);

        for (UINT y = 0; y < copyH; ++y) {
            const uint8_t* srcRow = reinterpret_cast<const uint8_t*>(srcLR.pBits)
                                    + (size_t)y * srcLR.Pitch;
            uint16_t* dstRow = reinterpret_cast<uint16_t*>(
                reinterpret_cast<uint8_t*>(ddsd.lpSurface) + (size_t)y * ddsd.lPitch);

            for (UINT x = 0; x < copyW; ++x) {
                uint8_t sB = srcRow[x * 4 + 0];
                uint8_t sG = srcRow[x * 4 + 1];
                uint8_t sR = srcRow[x * 4 + 2];
                uint8_t sA = srcRow[x * 4 + 3];
                if (sA == 0)
                    continue;

                if (sA == 255) {
                    if (isR5G6B5)
                        dstRow[x] = (uint16_t)(((sR >> 3) << 11) | ((sG >> 2) << 5) | (sB >> 3));
                    else
                        dstRow[x] = (uint16_t)(((sR >> 3) << 10) | ((sG >> 3) << 5) | (sB >> 3));
                } else {
                    // Decode existing destination pixel
                    uint16_t dp = dstRow[x];
                    uint8_t dR, dG, dB;
                    if (isR5G6B5) {
                        dR = (uint8_t)(((dp >> 11) & 0x1Fu) << 3);
                        dG = (uint8_t)(((dp >>  5) & 0x3Fu) << 2);
                        dB = (uint8_t)(((dp      ) & 0x1Fu) << 3);
                    } else {
                        dR = (uint8_t)(((dp >> 10) & 0x1Fu) << 3);
                        dG = (uint8_t)(((dp >>  5) & 0x1Fu) << 3);
                        dB = (uint8_t)(((dp      ) & 0x1Fu) << 3);
                    }
                    uint32_t a = sA, ia = 255u - sA;
                    uint8_t oR = (uint8_t)((a * sR + ia * dR) / 255u);
                    uint8_t oG = (uint8_t)((a * sG + ia * dG) / 255u);
                    uint8_t oB = (uint8_t)((a * sB + ia * dB) / 255u);
                    if (isR5G6B5)
                        dstRow[x] = (uint16_t)(((oR >> 3) << 11) | ((oG >> 2) << 5) | (oB >> 3));
                    else
                        dstRow[x] = (uint16_t)(((oR >> 3) << 10) | ((oG >> 3) << 5) | (oB >> 3));
                }
            }
        }
    } else {
        static bool s_unknownFmtLogged = false;
        if (!s_unknownFmtLogged) {
            s_unknownFmtLogged = true;
            DebugLog("[UHX] DDraw: unsupported surface bpp=%lu - cannot composite\n", bpp);
        }
    }

    pDDSurface->Unlock(nullptr);
    g_pReadback->UnlockRect( );
}

static bool IsPrimarySurface(IDirectDrawSurface7* pSurf) {
    if (!pSurf) return false;

    DDSURFACEDESC2 desc = { };
    desc.dwSize = sizeof(desc);
    if (SUCCEEDED(pSurf->GetSurfaceDesc(&desc)) &&
        (desc.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE))
        return true;

    if (g_pPrimaryIdentity) {
        IUnknown* pUnk = nullptr;
        if (SUCCEEDED(pSurf->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&pUnk)))) {
            bool match = (pUnk == g_pPrimaryIdentity);
            pUnk->Release( );
            if (match) return true;
        }
    }

    return false;
}

static HRESULT WINAPI hkFlip(IDirectDrawSurface7* pSurface,
                             IDirectDrawSurface7* pTargetSurface,
                             DWORD dwFlags) {
    static bool once = false;
    if (!once) { once = true; OutputDebugStringA("[UHX] DDraw: hkFlip first call\n"); }

    bool composedInEndScene = g_bComposedThisFrame;
    g_bComposedThisFrame = false;

    if (!composedInEndScene) {
        DDSCAPS2 caps = { };
        caps.dwCaps = DDSCAPS_BACKBUFFER;
        IDirectDrawSurface7* pBack = nullptr;
        if (SUCCEEDED(pSurface->GetAttachedSurface(&caps, &pBack)) && pBack) {
            CompositeImGuiIntoDDrawSurface(pBack);
            pBack->Release( );
        } else {
            CompositeImGuiIntoDDrawSurface(pSurface);
        }
    } else {
        static bool once2 = false;
        if (!once2) { once2 = true; OutputDebugStringA("[UHX] DDraw: hkFlip skip composite (D3D7 EndScene handled it)\n"); }
    }

    return oFlip(pSurface, pTargetSurface, dwFlags);
}

static HRESULT WINAPI hkBlt(IDirectDrawSurface7* pThis,
                            LPRECT pDestRect,
                            IDirectDrawSurface7* pSrcSurface,
                            LPRECT pSrcRect,
                            DWORD dwFlags,
                            LPDDBLTFX pFX) {
    static bool once = false;
    if (!once) { once = true; OutputDebugStringA("[UHX] DDraw: hkBlt first call\n"); }

    if (!g_bComposedThisFrame && pSrcSurface)
        CompositeImGuiIntoDDrawSurface(pSrcSurface);

    return oBlt(pThis, pDestRect, pSrcSurface, pSrcRect, dwFlags, pFX);
}

static HRESULT WINAPI hkBltFast(IDirectDrawSurface7* pThis,
                                DWORD dwX, DWORD dwY,
                                IDirectDrawSurface7* pSrcSurface,
                                LPRECT pSrcRect,
                                DWORD dwFlags) {
    static bool once = false;
    if (!once) { once = true; OutputDebugStringA("[UHX] DDraw: hkBltFast first call\n"); }

    if (!g_bComposedThisFrame && pSrcSurface)
        CompositeImGuiIntoDDrawSurface(pSrcSurface);

    return oBltFast(pThis, dwX, dwY, pSrcSurface, pSrcRect, dwFlags);
}

static HRESULT WINAPI hkD3D7EndScene(IUnknown* pDevice) {
    static bool once = false;
    if (!once) { once = true; OutputDebugStringA("[UHX] D3D7: hkD3D7EndScene first call\n"); }

    // Call the original EndScene first so the full frame is rendered before we composite.
    HRESULT hr = oD3D7EndScene(pDevice);

    if (SUCCEEDED(hr) && !g_bComposedThisFrame) {
        void** vt = *reinterpret_cast<void***>(pDevice);
        auto fnGetRT = reinterpret_cast<D3D7GetRenderTarget_t>(vt[D3D7DEV_VTI_GetRenderTarget]);

        IDirectDrawSurface7* pRT = nullptr;
        HRESULT hrRT = fnGetRT(pDevice, &pRT);
        if (FAILED(hrRT) || !pRT) {
            static bool rtFailed = false;
            if (!rtFailed) { rtFailed = true; DebugLog("[UHX] D3D7: GetRenderTarget failed hr=0x%08X\n", (unsigned)hrRT); }
        } else {
            static bool rtOnce = false;
            if (!rtOnce) { rtOnce = true; DebugLog("[UHX] D3D7: compositing via EndScene, pRT=%p\n", (void*)pRT); }
            CompositeImGuiIntoDDrawSurface(pRT);
            pRT->Release( );
            g_bComposedThisFrame = true;
        }
    }

    return hr;
}

static HRESULT WINAPI hkD3D7CreateDevice(IUnknown*           pD3D7,
                                          REFCLSID             rclsid,
                                          IDirectDrawSurface7* pSurface,
                                          IUnknown**           ppDevice) {
    OutputDebugStringA("[UHX] D3D7: hkD3D7CreateDevice fired\n");

    HRESULT hr = oD3D7CreateDevice(pD3D7, rclsid, pSurface, ppDevice);

    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        void** vt = *reinterpret_cast<void***>(*ppDevice);
        void*  fnEndScene = vt[D3D7DEV_VTI_EndScene];

        char buf[256];
        snprintf(buf, sizeof(buf),
                 "[UHX] D3D7: Device created ppDevice=%p EndScene=%p\n",
                 (void*)*ppDevice, fnEndScene);
        OutputDebugStringA(buf);

        static bool bEndSceneHooked = false;
        if (!bEndSceneHooked) {
            bEndSceneHooked = true;
            MH_STATUS st = MH_CreateHook(fnEndScene, &hkD3D7EndScene,
                                         reinterpret_cast<void**>(&oD3D7EndScene));
            snprintf(buf, sizeof(buf),
                     "[UHX] D3D7: MH_CreateHook(EndScene)=%d addr=%p\n",
                     (int)st, fnEndScene);
            OutputDebugStringA(buf);

            if (st == MH_OK || st == MH_ERROR_ALREADY_CREATED) {
                MH_EnableHook(fnEndScene);
                OutputDebugStringA("[UHX] D3D7: EndScene hook enabled\n");
                g_bHasD3D7Device = true;
            } else {
                OutputDebugStringA("[UHX] D3D7: EndScene hook FAILED\n");
            }
        }
    }

    return hr;
}

static void HookD3D7(IDirectDraw7* pDD) {
    HMODULE hD3DIM = GetModuleHandleA("d3dim700.dll");
    if (!hD3DIM) {
        OutputDebugStringA("[UHX] D3D7: d3dim700.dll not loaded, skipping D3D7 device hook\n");
        return;
    }
    OutputDebugStringA("[UHX] D3D7: d3dim700.dll present, hooking IDirect3D7::CreateDevice\n");

    IUnknown* pD3D7 = nullptr;
    HRESULT hr = pDD->QueryInterface(IID_IDirect3D7_UHX, reinterpret_cast<void**>(&pD3D7));
    if (FAILED(hr) || !pD3D7) {
        DebugLog("[UHX] D3D7: QueryInterface(IDirect3D7) failed hr=0x%08X\n", (unsigned)hr);
        return;
    }
    OutputDebugStringA("[UHX] D3D7: Got IDirect3D7 interface\n");

    // vtable[4] = IDirect3D7::CreateDevice
    void** vt = *reinterpret_cast<void***>(pD3D7);
    void*  fnCreateDevice = vt[D3D7_VTI_CreateDevice];

    char buf[192];
    snprintf(buf, sizeof(buf), "[UHX] D3D7: CreateDevice=%p\n", fnCreateDevice);
    OutputDebugStringA(buf);

    static MH_STATUS createDeviceStatus =
        MH_CreateHook(fnCreateDevice, &hkD3D7CreateDevice,
                      reinterpret_cast<void**>(&oD3D7CreateDevice));
    snprintf(buf, sizeof(buf), "[UHX] D3D7: MH_CreateHook(CreateDevice)=%d\n",
             (int)createDeviceStatus);
    OutputDebugStringA(buf);

    if (createDeviceStatus == MH_OK || createDeviceStatus == MH_ERROR_ALREADY_CREATED) {
        MH_EnableHook(fnCreateDevice);
        OutputDebugStringA("[UHX] D3D7: CreateDevice hooked – waiting for game to call it\n");
    } else {
        DebugLog("[UHX] D3D7: CreateDevice hook FAILED status=%d\n", (int)createDeviceStatus);
    }

    pD3D7->Release( );
}

namespace DDraw {
    void Hook(HWND hwnd) {
        OutputDebugStringA("[UHX] DDraw::Hook start\n");

        HMODULE hDDrawModule = GetModuleHandleA("ddraw.dll");
        if (!hDDrawModule) {
            OutputDebugStringA("[!] DDraw: ddraw.dll not loaded - skipping.\n");
            return;
        }

        typedef HRESULT(WINAPI* PFN_DirectDrawCreateEx)(GUID*, LPVOID*, REFIID, IUnknown*);
        auto pfnCreate = reinterpret_cast<PFN_DirectDrawCreateEx>(
            GetProcAddress(hDDrawModule, "DirectDrawCreateEx"));
        if (!pfnCreate) {
            OutputDebugStringA("[!] DDraw: DirectDrawCreateEx not found.\n");
            return;
        }

        IDirectDraw7* pDD = nullptr;
        if (FAILED(pfnCreate(NULL, reinterpret_cast<LPVOID*>(&pDD), IID_IDirectDraw7, NULL))) {
            OutputDebugStringA("[!] DDraw: DirectDrawCreateEx failed.\n");
            return;
        }

        pDD->SetCooperativeLevel(hwnd, DDSCL_NORMAL);

        DDSURFACEDESC2 sd = { };
        sd.dwSize = sizeof(sd);
        sd.dwFlags = DDSD_CAPS;
        sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;

        IDirectDrawSurface7* pSurface = nullptr;
        bool bFullscreenExclusive = false;
        if (FAILED(pDD->CreateSurface(&sd, &pSurface, NULL))) {
            OutputDebugStringA("[UHX] DDraw: Primary surface unavailable, trying offscreen\n");
            bFullscreenExclusive = true;
            sd.dwFlags        = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
            sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
            sd.dwWidth        = 1;
            sd.dwHeight       = 1;
            if (FAILED(pDD->CreateSurface(&sd, &pSurface, NULL))) {
                LOG("[!] DDraw: CreateSurface failed.\n");
                pDD->Release( );
                return;
            }
        } else {
            IUnknown* pUnk = nullptr;
            if (SUCCEEDED(pSurface->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&pUnk)))) {
                if (g_pPrimaryIdentity) g_pPrimaryIdentity->Release( );
                g_pPrimaryIdentity = pUnk; // kept alive – used for present-blt detection
                OutputDebugStringA("[UHX] DDraw: primary identity cached\n");
            }
        }

        void** pVTable = *reinterpret_cast<void***>(pSurface);
        void* fnBlt     = pVTable[5];
        void* fnBltFast = pVTable[7];
        void* fnFlip    = pVTable[11];

        char buf[192];
        snprintf(buf, sizeof(buf), "[UHX] DDraw: Blt=%p BltFast=%p Flip=%p\n", fnBlt, fnBltFast, fnFlip);
        OutputDebugStringA(buf);

        pSurface->Release( );

        g_hGameWnd = hwnd;

        Menu::InitializeContext(hwnd);

        static MH_STATUS bltStatus     = MH_CreateHook(reinterpret_cast<void**>(fnBlt),     &hkBlt,     reinterpret_cast<void**>(&oBlt));
        static MH_STATUS bltFastStatus = MH_CreateHook(reinterpret_cast<void**>(fnBltFast), &hkBltFast, reinterpret_cast<void**>(&oBltFast));
        static MH_STATUS flipStatus    = MH_CreateHook(reinterpret_cast<void**>(fnFlip),    &hkFlip,    reinterpret_cast<void**>(&oFlip));

        LOG("[+] DDraw: MH_CreateHook(Blt)     = %d\n", (int)bltStatus);
        LOG("[+] DDraw: MH_CreateHook(BltFast) = %d\n", (int)bltFastStatus);
        LOG("[+] DDraw: MH_CreateHook(Flip)    = %d\n", (int)flipStatus);

        MH_EnableHook(fnBlt);
        MH_EnableHook(fnBltFast);
        MH_EnableHook(fnFlip);

        HookD3D7(pDD);

        pDD->Release( );

        if (U::GetRenderingBackend( ) == NONE) {
            U::SetRenderingBackend(DIRECTDRAW);
            if (bFullscreenExclusive)
                OutputDebugStringA("[UHX] DDraw: fullscreen exclusive - claimed DIRECTDRAW\n");
            else
                OutputDebugStringA("[UHX] DDraw: windowed - claimed DIRECTDRAW\n");
        } else {
            char claimBuf[128];
            snprintf(claimBuf, sizeof(claimBuf),
                     "[UHX] DDraw: backend already claimed (%d) - not overriding\n",
                     (int)U::GetRenderingBackend( ));
            OutputDebugStringA(claimBuf);
        }

        OutputDebugStringA("[+] DDraw: Hooks installed.\n");
        OutputDebugStringA("[UHX] DDraw::Hook complete.\n");
    }

    void Unhook( ) {
        if (ImGui::GetCurrentContext( )) {
            if (ImGui::GetIO( ).BackendRendererUserData)
                ImGui_ImplDX9_Shutdown( );
            if (ImGui::GetIO( ).BackendPlatformUserData)
                ImGui_ImplWin32_Shutdown( );
            ImGui::DestroyContext( );
        }

        if (g_pReadback) { g_pReadback->Release( ); g_pReadback = nullptr; }
        if (g_pDevice)   { g_pDevice->Release( );   g_pDevice   = nullptr; }
        if (g_pD3D)      { g_pD3D->Release( );      g_pD3D      = nullptr; }

        if (g_hHelperWnd) {
            DestroyWindow(g_hHelperWnd);
            g_hHelperWnd = NULL;
        }

        g_bHasD3D7Device    = false;
        g_bComposedThisFrame = false;
    }
} // namespace DDraw

#else
#include <Windows.h>
namespace DDraw {
    void Hook(HWND hwnd) { LOG("[!] DirectDraw backend is not enabled!\n"); }
    void Unhook( ) { }
} // namespace DDraw
#endif
