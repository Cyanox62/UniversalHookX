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

static HWND g_hGameWnd = NULL;
static IDirect3D9* g_pD3D = nullptr;
static IDirect3DDevice9* g_pDevice = nullptr;
static IDirect3DSurface9* g_pReadback = nullptr;
static UINT g_width = 0;
static UINT g_height = 0;

static std::add_pointer_t<HRESULT WINAPI(IDirectDrawSurface7*, IDirectDrawSurface7*, DWORD)> oFlip;
static std::add_pointer_t<HRESULT WINAPI(IDirectDrawSurface7*, LPRECT, IDirectDrawSurface7*, LPRECT, DWORD, LPDDBLTFX)> oBlt;

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
        uint8_t* dst = reinterpret_cast<uint8_t*>(lr.pBits) + (size_t)y * lr.Pitch;
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

static bool CreateOffscreenD3D9(UINT w, UINT h) {
    g_pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_pD3D) {
        OutputDebugStringA("[UHX] DDraw: Direct3DCreate9 failed\n");
        return false;
    }

    D3DPRESENT_PARAMETERS pp = { };
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = w;
    pp.BackBufferHeight = h;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.hDeviceWindow = g_hGameWnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    HRESULT hr = g_pD3D->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hGameWnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES | D3DCREATE_MULTITHREADED,
        &pp, &g_pDevice);

    if (FAILED(hr))
        hr = g_pD3D->CreateDevice(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hGameWnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES | D3DCREATE_MULTITHREADED,
            &pp, &g_pDevice);

    if (FAILED(hr)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[UHX] DDraw: CreateDevice failed hr=0x%08X\n", (unsigned)hr);
        OutputDebugStringA(buf);
        g_pD3D->Release( );
        g_pD3D = nullptr;
        return false;
    }

    hr = g_pDevice->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8,
                                                D3DPOOL_SYSTEMMEM, &g_pReadback, nullptr);
    if (FAILED(hr)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[UHX] DDraw: CreateOffscreenPlainSurface failed hr=0x%08X\n", (unsigned)hr);
        OutputDebugStringA(buf);
        g_pDevice->Release( );
        g_pDevice = nullptr;
        g_pD3D->Release( );
        g_pD3D = nullptr;
        return false;
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "[UHX] DDraw: Offscreen D3D9 ready %ux%u\n", w, h);
    OutputDebugStringA(buf);
    return true;
}

static void CompositeImGuiIntoDDrawSurface(IDirectDrawSurface7* pDDSurface) {
    static std::atomic<bool> s_rendering{false};
    bool expected = false;
    if (!s_rendering.compare_exchange_strong(expected, true))
        return;
    struct Guard {
        ~Guard( ) { s_rendering.store(false); }
    } g;

    if (U::GetRenderingBackend( ) != NONE && U::GetRenderingBackend( ) != DIRECTDRAW)
        return;

    if (!g_pDevice)
        return;

    if (U::GetRenderingBackend( ) == NONE) {
        OutputDebugStringA("[UHX] DDraw hook fired — claiming backend\n");
        LOG("[+] DDraw hook fired — claiming backend\n");
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
        char buf[128];
        snprintf(buf, sizeof(buf), "[UHX] DDraw: GetRenderTargetData failed hr=0x%08X\n", (unsigned)hr);
        OutputDebugStringA(buf);
        return;
    }

    D3DLOCKED_RECT srcLR;
    if (FAILED(g_pReadback->LockRect(&srcLR, nullptr, D3DLOCK_READONLY))) {
        OutputDebugStringA("[UHX] DDraw: readback LockRect failed\n");
        return;
    }

    DDSURFACEDESC2 ddsd = { };
    ddsd.dwSize = sizeof(ddsd);
    hr = pDDSurface->Lock(nullptr, &ddsd, DDLOCK_WAIT | DDLOCK_WRITEONLY, nullptr);
    if (FAILED(hr)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[UHX] DDraw: Surface Lock failed hr=0x%08X\n", (unsigned)hr);
        OutputDebugStringA(buf);
        g_pReadback->UnlockRect( );
        return;
    }

    static bool s_formatLogged = false;
    if (!s_formatLogged) {
        s_formatLogged = true;
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "[UHX] DDraw: surface bpp=%lu pitch=%lu size=%lux%lu rMask=0x%08lX gMask=0x%08lX bMask=0x%08lX\n",
                 ddsd.ddpfPixelFormat.dwRGBBitCount, ddsd.lPitch,
                 ddsd.dwWidth, ddsd.dwHeight,
                 ddsd.ddpfPixelFormat.dwRBitMask,
                 ddsd.ddpfPixelFormat.dwGBitMask,
                 ddsd.ddpfPixelFormat.dwBBitMask);
        OutputDebugStringA(buf);
    }

    UINT surfW = min(ddsd.dwWidth, g_width);
    UINT surfH = min(ddsd.dwHeight, g_height);
    DWORD bpp = ddsd.ddpfPixelFormat.dwRGBBitCount;

    const uint8_t* pSrc = reinterpret_cast<const uint8_t*>(srcLR.pBits);
    uint8_t* pDst = reinterpret_cast<uint8_t*>(ddsd.lpSurface);

    if (bpp == 32) {
        for (UINT y = 0; y < surfH; ++y) {
            const uint32_t* srcRow = reinterpret_cast<const uint32_t*>(pSrc + (size_t)y * srcLR.Pitch);
            uint32_t* dstRow = reinterpret_cast<uint32_t*>(pDst + (size_t)y * ddsd.lPitch);
            for (UINT x = 0; x < surfW; ++x) {
                uint32_t sp = srcRow[x]; // A8R8G8B8
                uint8_t a = (sp >> 24) & 0xFF;
                if (a == 0)
                    continue;
                if (a == 255) {
                    dstRow[x] = sp;
                    continue;
                }
                uint32_t dp = dstRow[x];
                uint8_t sr = (sp >> 16) & 0xFF, sg = (sp >> 8) & 0xFF, sb = sp & 0xFF;
                uint8_t dr = (dp >> 16) & 0xFF, dg = (dp >> 8) & 0xFF, db = dp & 0xFF;
                uint8_t or_ = (uint8_t)((sr * a + dr * (255 - a)) / 255);
                uint8_t og = (uint8_t)((sg * a + dg * (255 - a)) / 255);
                uint8_t ob = (uint8_t)((sb * a + db * (255 - a)) / 255);
                dstRow[x] = 0xFF000000u | ((uint32_t)or_ << 16) | ((uint32_t)og << 8) | ob;
            }
        }
    } else if (bpp == 16) {
        DWORD rMask = ddsd.ddpfPixelFormat.dwRBitMask;
        DWORD gMask = ddsd.ddpfPixelFormat.dwGBitMask;
        DWORD bMask = ddsd.ddpfPixelFormat.dwBBitMask;

        auto lowestBit = [](DWORD m) -> int { if (!m) return 0; int s=0; while(!(m&1)){m>>=1;++s;} return s; };
        auto countBits = [](DWORD m) -> int { int b=0; while(m){b+=(m&1);m>>=1;} return b; };

        int rShift = lowestBit(rMask), rBits = countBits(rMask);
        int gShift = lowestBit(gMask), gBits = countBits(gMask);
        int bShift = lowestBit(bMask), bBits = countBits(bMask);

        for (UINT y = 0; y < surfH; ++y) {
            const uint32_t* srcRow = reinterpret_cast<const uint32_t*>(pSrc + (size_t)y * srcLR.Pitch);
            uint16_t* dstRow = reinterpret_cast<uint16_t*>(pDst + (size_t)y * ddsd.lPitch);
            for (UINT x = 0; x < surfW; ++x) {
                uint32_t sp = srcRow[x];
                uint8_t a = (sp >> 24) & 0xFF;
                if (a == 0)
                    continue;

                uint8_t sr = (sp >> 16) & 0xFF;
                uint8_t sg = (sp >> 8) & 0xFF;
                uint8_t sb = sp & 0xFF;

                if (a < 255) {
                    uint16_t dp = dstRow[x];
                    uint8_t dr = (uint8_t)(((dp & rMask) >> rShift) << (8 - rBits));
                    uint8_t dg = (uint8_t)(((dp & gMask) >> gShift) << (8 - gBits));
                    uint8_t db = (uint8_t)(((dp & bMask) >> bShift) << (8 - bBits));
                    sr = (uint8_t)((sr * a + dr * (255 - a)) / 255);
                    sg = (uint8_t)((sg * a + dg * (255 - a)) / 255);
                    sb = (uint8_t)((sb * a + db * (255 - a)) / 255);
                }

                dstRow[x] = (uint16_t)(((uint16_t)(sr >> (8 - rBits)) << rShift) |
                                       ((uint16_t)(sg >> (8 - gBits)) << gShift) |
                                       ((uint16_t)(sb >> (8 - bBits)) << bShift));
            }
        }
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "[UHX] DDraw: unsupported bpp=%lu — cannot composite\n", bpp);
        OutputDebugStringA(buf);
    }

    pDDSurface->Unlock(nullptr);
    g_pReadback->UnlockRect( );
}

static HRESULT WINAPI hkFlip(IDirectDrawSurface7* pSurface,
                             IDirectDrawSurface7* pTargetSurface,
                             DWORD dwFlags) {
    static bool once = false;
    if (!once) {
        once = true;
        OutputDebugStringA("[UHX] DDraw: hkFlip fired\n");
    }
    CompositeImGuiIntoDDrawSurface(pSurface);
    return oFlip(pSurface, pTargetSurface, dwFlags);
}

static HRESULT WINAPI hkBlt(IDirectDrawSurface7* pThis,
                            LPRECT pDestRect,
                            IDirectDrawSurface7* pSrcSurface,
                            LPRECT pSrcRect,
                            DWORD dwFlags,
                            LPDDBLTFX pFX) {
    static bool once = false;
    if (!once) {
        once = true;
        OutputDebugStringA("[UHX] DDraw: hkBlt fired\n");
    }
    IDirectDrawSurface7* target = pSrcSurface ? pSrcSurface : pThis;
    CompositeImGuiIntoDDrawSurface(target);
    return oBlt(pThis, pDestRect, pSrcSurface, pSrcRect, dwFlags, pFX);
}

// ---------------------------------------------------------------------------
namespace DDraw {
    void Hook(HWND hwnd) {
        OutputDebugStringA("[UHX] DDraw::Hook start\n");

        HMODULE hDDrawModule = GetModuleHandleA("ddraw.dll");
        if (!hDDrawModule) {
            LOG("[!] DDraw: ddraw.dll not loaded — skipping.\n");
            return;
        }

        typedef HRESULT(WINAPI * PFN_DirectDrawCreateEx)(GUID*, LPVOID*, REFIID, IUnknown*);
        auto pfnCreate = reinterpret_cast<PFN_DirectDrawCreateEx>(
            GetProcAddress(hDDrawModule, "DirectDrawCreateEx"));
        if (!pfnCreate) {
            LOG("[!] DDraw: DirectDrawCreateEx not found.\n");
            return;
        }

        IDirectDraw7* pDD = nullptr;
        if (FAILED(pfnCreate(NULL, reinterpret_cast<LPVOID*>(&pDD), IID_IDirectDraw7, NULL))) {
            LOG("[!] DDraw: DirectDrawCreateEx failed.\n");
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
            sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
            sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
            sd.dwWidth = 1;
            sd.dwHeight = 1;
            if (FAILED(pDD->CreateSurface(&sd, &pSurface, NULL))) {
                LOG("[!] DDraw: CreateSurface failed.\n");
                pDD->Release( );
                return;
            }
        }

        DDSURFACEDESC2 surfDesc = { };
        surfDesc.dwSize = sizeof(surfDesc);
        if (SUCCEEDED(pSurface->GetSurfaceDesc(&surfDesc)) &&
            surfDesc.dwWidth > 0 && surfDesc.dwHeight > 0) {
            g_width = surfDesc.dwWidth;
            g_height = surfDesc.dwHeight;
        }
        if (g_width == 0 || g_height == 0) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            g_width = (UINT)(rc.right - rc.left);
            g_height = (UINT)(rc.bottom - rc.top);
        }
        if (g_width == 0 || g_height == 0) {
            g_width = (UINT)GetSystemMetrics(SM_CXSCREEN);
            g_height = (UINT)GetSystemMetrics(SM_CYSCREEN);
        }

        char buf[128];
        snprintf(buf, sizeof(buf), "[UHX] DDraw: dims %ux%u\n", g_width, g_height);
        OutputDebugStringA(buf);

        void** pVTable = *reinterpret_cast<void***>(pSurface);
        void* fnBlt = pVTable[5];
        void* fnFlip = pVTable[11];

        snprintf(buf, sizeof(buf), "[UHX] DDraw: Blt=%p Flip=%p\n", fnBlt, fnFlip);
        OutputDebugStringA(buf);

        pSurface->Release( );
        pDD->Release( );

        g_hGameWnd = hwnd;

        if (!CreateOffscreenD3D9(g_width, g_height)) {
            LOG("[!] DDraw: Failed to create offscreen D3D9 device.\n");
            return;
        }

        Menu::InitializeContext(hwnd);

        static MH_STATUS bltStatus = MH_CreateHook(reinterpret_cast<void**>(fnBlt), &hkBlt, reinterpret_cast<void**>(&oBlt));
        static MH_STATUS flipStatus = MH_CreateHook(reinterpret_cast<void**>(fnFlip), &hkFlip, reinterpret_cast<void**>(&oFlip));

        LOG("[+] DDraw: MH_CreateHook(Blt)  = %d\n", (int)bltStatus);
        LOG("[+] DDraw: MH_CreateHook(Flip) = %d\n", (int)flipStatus);

        MH_EnableHook(fnBlt);
        MH_EnableHook(fnFlip);

        LOG("[+] DDraw: Hooks installed.\n");
        OutputDebugStringA("[UHX] DDraw::Hook complete\n");
    }

    void Unhook( ) {
        if (ImGui::GetCurrentContext( )) {
            if (ImGui::GetIO( ).BackendRendererUserData)
                ImGui_ImplDX9_Shutdown( );
            if (ImGui::GetIO( ).BackendPlatformUserData)
                ImGui_ImplWin32_Shutdown( );
            ImGui::DestroyContext( );
        }

        if (g_pReadback) {
            g_pReadback->Release( );
            g_pReadback = nullptr;
        }
        if (g_pDevice) {
            g_pDevice->Release( );
            g_pDevice = nullptr;
        }
        if (g_pD3D) {
            g_pD3D->Release( );
            g_pD3D = nullptr;
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
