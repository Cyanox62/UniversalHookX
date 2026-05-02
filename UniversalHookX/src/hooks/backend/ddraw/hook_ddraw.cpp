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

static HWND g_hGameWnd    = NULL;
static HWND g_hHelperWnd  = NULL; // hidden window used when game owns adapter exclusively
static IDirect3D9*        g_pD3D      = nullptr;
static IDirect3DDevice9*  g_pDevice   = nullptr;
static IDirect3DSurface9* g_pReadback = nullptr;
static UINT g_width  = 0;
static UINT g_height = 0;

static std::add_pointer_t<HRESULT WINAPI(IDirectDrawSurface7*, IDirectDrawSurface7*, DWORD)> oFlip;
static std::add_pointer_t<HRESULT WINAPI(IDirectDrawSurface7*, LPRECT, IDirectDrawSurface7*, LPRECT, DWORD, LPDDBLTFX)> oBlt;

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
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = src[x * 4 + 3];
        }
    }
    pTex->UnlockRect(0);
    return pTex;
}

static void DestroyD3D9Resources( ) {
    if (ImGui::GetCurrentContext( ) && ImGui::GetIO( ).BackendRendererUserData)
        ImGui_ImplDX9_Shutdown( );
    if (g_pReadback) { g_pReadback->Release( ); g_pReadback = nullptr; }
    if (g_pDevice)   { g_pDevice->Release( );   g_pDevice   = nullptr; }
    if (g_pD3D)      { g_pD3D->Release( );      g_pD3D      = nullptr; }
}

// Creates a tiny hidden window to use as the D3D9 device window when the game
// holds exclusive fullscreen (which blocks device creation on the game's HWND).
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
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES | D3DCREATE_MULTITHREADED,
        &pp, &g_pDevice);
    if (FAILED(hr))
        hr = g_pD3D->CreateDevice(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, devWnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES | D3DCREATE_MULTITHREADED,
            &pp, &g_pDevice);
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

    if (U::GetRenderingBackend( ) != NONE && U::GetRenderingBackend( ) != DIRECTDRAW)
        return;

    // Determine the actual surface dimensions so the D3D9 device matches exactly.
    DDSURFACEDESC2 surfInfo = { };
    surfInfo.dwSize = sizeof(surfInfo);
    if (FAILED(pDDSurface->GetSurfaceDesc(&surfInfo)))
        return;
    UINT surfW = surfInfo.dwWidth;
    UINT surfH = surfInfo.dwHeight;
    if (surfW == 0 || surfH == 0)
        return;

    // (Re)create D3D9 if it doesn't exist or the surface size changed.
    if (!g_pDevice || surfW != g_width || surfH != g_height) {
        DestroyD3D9Resources( );
        if (!CreateOffscreenD3D9(surfW, surfH)) {
            LOG("[!] DDraw: Failed to create offscreen D3D9 device.\n");
            return;
        }
    }

    if (U::GetRenderingBackend( ) == NONE) {
        OutputDebugStringA("[UHX] DDraw hook fired  claiming backend\n");
        U::SetRenderingBackend(DIRECTDRAW);
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
    hr = pDDSurface->Lock(nullptr, &ddsd, DDLOCK_WAIT, nullptr);
    if (FAILED(hr)) {
        DebugLog("[UHX] DDraw: Surface Lock failed hr=0x%08X\n", (unsigned)hr);
        g_pReadback->UnlockRect( );
        return;
    }

    static bool s_formatLogged = false;
    if (!s_formatLogged) {
        s_formatLogged = true;
        DebugLog("[UHX] DDraw: surface bpp=%lu pitch=%lu size=%lux%lu "
                 "rMask=0x%08lX gMask=0x%08lX bMask=0x%08lX\n",
                 ddsd.ddpfPixelFormat.dwRGBBitCount, ddsd.lPitch,
                 ddsd.dwWidth, ddsd.dwHeight,
                 ddsd.ddpfPixelFormat.dwRBitMask,
                 ddsd.ddpfPixelFormat.dwGBitMask,
                 ddsd.ddpfPixelFormat.dwBBitMask);
    }

    UINT copyW = min((UINT)ddsd.dwWidth, g_width);
    UINT copyH = min((UINT)ddsd.dwHeight, g_height);

    for (UINT y = 0; y < copyH; ++y) {
        const uint8_t* srcRow = reinterpret_cast<const uint8_t*>(srcLR.pBits) + (size_t)y * srcLR.Pitch;
        uint8_t*       dstRow = reinterpret_cast<uint8_t*>(ddsd.lpSurface)    + (size_t)y * ddsd.lPitch;
        for (UINT x = 0; x < copyW; ++x) {
            uint8_t sB = srcRow[x * 4 + 0];
            uint8_t sG = srcRow[x * 4 + 1];
            uint8_t sR = srcRow[x * 4 + 2];
            uint8_t sA = srcRow[x * 4 + 3];
            if (sA == 0)
                continue;
            if (sA == 255) {
                dstRow[x * 4 + 0] = sB;
                dstRow[x * 4 + 1] = sG;
                dstRow[x * 4 + 2] = sR;
            } else {
                uint32_t a = sA, ia = 255u - sA;
                dstRow[x * 4 + 0] = (uint8_t)((a * sB + ia * dstRow[x * 4 + 0]) / 255u);
                dstRow[x * 4 + 1] = (uint8_t)((a * sG + ia * dstRow[x * 4 + 1]) / 255u);
                dstRow[x * 4 + 2] = (uint8_t)((a * sR + ia * dstRow[x * 4 + 2]) / 255u);
            }
        }
    }

    pDDSurface->Unlock(nullptr);
    g_pReadback->UnlockRect( );
}

static HRESULT WINAPI hkFlip(IDirectDrawSurface7* pSurface,
                             IDirectDrawSurface7* pTargetSurface,
                             DWORD dwFlags) {
    static bool once = false;
    if (!once) { once = true; OutputDebugStringA("[UHX] DDraw: hkFlip fired\n"); }

    DDSCAPS2 caps = { };
    caps.dwCaps = DDSCAPS_BACKBUFFER;
    IDirectDrawSurface7* pBack = nullptr;
    if (SUCCEEDED(pSurface->GetAttachedSurface(&caps, &pBack)) && pBack) {
        CompositeImGuiIntoDDrawSurface(pBack);
        pBack->Release( );
    } else {
        // No attached back buffer (windowed primary or single-surface), draw on front.
        CompositeImGuiIntoDDrawSurface(pSurface);
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
    if (!once) { once = true; OutputDebugStringA("[UHX] DDraw: hkBlt fired\n"); }

    IDirectDrawSurface7* target = pSrcSurface ? pSrcSurface : pThis;
    CompositeImGuiIntoDDrawSurface(target);
    return oBlt(pThis, pDestRect, pSrcSurface, pSrcRect, dwFlags, pFX);
}

namespace DDraw {
    void Hook(HWND hwnd) {
        OutputDebugStringA("[UHX] DDraw::Hook start\n");

        HMODULE hDDrawModule = GetModuleHandleA("ddraw.dll");
        if (!hDDrawModule) {
            OutputDebugStringA("[!] DDraw: ddraw.dll not loaded — skipping.\n");
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
        if (FAILED(pDD->CreateSurface(&sd, &pSurface, NULL))) {
            OutputDebugStringA("[UHX] DDraw: Primary surface unavailable, trying offscreen\n");
            sd.dwFlags        = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
            sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
            sd.dwWidth        = 1;
            sd.dwHeight       = 1;
            if (FAILED(pDD->CreateSurface(&sd, &pSurface, NULL))) {
                LOG("[!] DDraw: CreateSurface failed.\n");
                pDD->Release( );
                return;
            }
        }

        void** pVTable = *reinterpret_cast<void***>(pSurface);
        void* fnBlt  = pVTable[5];
        void* fnFlip = pVTable[11];

        char buf[128];
        snprintf(buf, sizeof(buf), "[UHX] DDraw: Blt=%p Flip=%p\n", fnBlt, fnFlip);
        OutputDebugStringA(buf);

        pSurface->Release( );
        pDD->Release( );

        g_hGameWnd = hwnd;

        Menu::InitializeContext(hwnd);

        static MH_STATUS bltStatus  = MH_CreateHook(reinterpret_cast<void**>(fnBlt),  &hkBlt,  reinterpret_cast<void**>(&oBlt));
        static MH_STATUS flipStatus = MH_CreateHook(reinterpret_cast<void**>(fnFlip), &hkFlip, reinterpret_cast<void**>(&oFlip));

        LOG("[+] DDraw: MH_CreateHook(Blt)  = %d\n", (int)bltStatus);
        LOG("[+] DDraw: MH_CreateHook(Flip) = %d\n", (int)flipStatus);

        MH_EnableHook(fnBlt);
        MH_EnableHook(fnFlip);

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
    }
} // namespace DDraw

#else
#include <Windows.h>
namespace DDraw {
    void Hook(HWND hwnd) { LOG("[!] DirectDraw backend is not enabled!\n"); }
    void Unhook( ) { }
} // namespace DDraw
#endif
