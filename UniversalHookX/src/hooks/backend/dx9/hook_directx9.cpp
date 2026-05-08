#include "../../../backend.hpp"
#include "../../../console/console.hpp"

#ifdef ENABLE_BACKEND_DX9
#include <Windows.h>

#include <d3d9.h>
#pragma comment(lib, "d3d9.lib")

#include <memory>

#include "../dx9/hook_directx9.hpp"
#include "../dx10/hook_directx10.hpp"
#include "../dx11/hook_directx11.hpp"
#include "../dx12/hook_directx12.hpp"
#include "../opengl/hook_opengl.hpp"
#include "../vulkan/hook_vulkan.hpp"

#include "../../../dependencies/imgui/imgui_impl_dx9.h"
#include "../../../dependencies/imgui/imgui_impl_win32.h"
#include "../../../dependencies/minhook/MinHook.h"

#include "../../hooks.hpp"
#include "../../../utils/utils.hpp"

#include "../../../menu/menu.hpp"

static LPDIRECT3D9 g_pD3D = NULL;
static LPDIRECT3DDEVICE9 g_pd3dDevice = NULL;
static IDirect3DDevice9* g_gameDevice = NULL;

static void* UploadTextureRGBA_DX9(const uint8_t* rgba, int w, int h) {
    if (!g_gameDevice) return nullptr;

    IDirect3DTexture9* pTex = nullptr;
    if (FAILED(g_gameDevice->CreateTexture((UINT)w, (UINT)h, 1, D3DUSAGE_DYNAMIC,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &pTex, nullptr)))
        return nullptr;

    D3DLOCKED_RECT lr;
    if (FAILED(pTex->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD))) {
        pTex->Release();
        return nullptr;
    }

    for (int y = 0; y < h; ++y) {
        const uint8_t* src = rgba + (size_t)y * w * 4;
        uint8_t*       dst = reinterpret_cast<uint8_t*>(lr.pBits) + (size_t)y * lr.Pitch;
        for (int x = 0; x < w; ++x) {
            dst[x*4+0] = src[x*4+2];
            dst[x*4+1] = src[x*4+1];
            dst[x*4+2] = src[x*4+0];
            dst[x*4+3] = src[x*4+3];
        }
    }
    pTex->UnlockRect(0);
    return pTex;
}

static void CleanupDeviceD3D9( );
static void RenderImGui_DX9(IDirect3DDevice9* pDevice);

static constexpr const char* TEMP_WND_CLASS = "__uhx_dx9_tmp";

static HWND CreateTempWindow( ) {
    WNDCLASSEXA wc = { sizeof(wc) };
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = TEMP_WND_CLASS;
    RegisterClassExA(&wc);
    return CreateWindowExA(0, TEMP_WND_CLASS, NULL, WS_POPUP, 0, 0, 1, 1,
                           NULL, NULL, wc.hInstance, NULL);
}

static bool CreateDeviceD3D9(HWND hWnd) {
    g_pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (g_pD3D == NULL) {
        LOG("[!] Direct3DCreate9() is failed.\n");
        return false;
    }

    D3DPRESENT_PARAMETERS d3dpp = { };
    d3dpp.Windowed         = TRUE;
    d3dpp.SwapEffect       = D3DSWAPEFFECT_DISCARD;
    d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;
    d3dpp.BackBufferWidth  = 1;
    d3dpp.BackBufferHeight = 1;

    HRESULT hr = g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES, &d3dpp, &g_pd3dDevice);
    if (FAILED(hr))
        hr = g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES, &d3dpp, &g_pd3dDevice);
    if (FAILED(hr)) {
        hr = g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, hWnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &d3dpp, &g_pd3dDevice);
    }

    if (FAILED(hr)) {
        LOG("[!] CreateDevice() failed. [rv: 0x%08X]\n", hr);
        return false;
    }

    return true;
}

static std::add_pointer_t<HRESULT WINAPI(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*)> oReset;
static HRESULT WINAPI hkReset(IDirect3DDevice9* pDevice,
                              D3DPRESENT_PARAMETERS* pPresentationParameters) {
    if (ImGui::GetCurrentContext( ) && ImGui::GetIO( ).BackendRendererUserData)
        ImGui_ImplDX9_InvalidateDeviceObjects( );

    return oReset(pDevice, pPresentationParameters);
}

static std::add_pointer_t<HRESULT WINAPI(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*)> oResetEx;
static HRESULT WINAPI hkResetEx(IDirect3DDevice9* pDevice,
                                D3DPRESENT_PARAMETERS* pPresentationParameters,
                                D3DDISPLAYMODEEX* pFullscreenDisplayMode) {
    if (ImGui::GetCurrentContext( ) && ImGui::GetIO( ).BackendRendererUserData)
        ImGui_ImplDX9_InvalidateDeviceObjects( );

    return oResetEx(pDevice, pPresentationParameters, pFullscreenDisplayMode);
}

static std::add_pointer_t<HRESULT WINAPI(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*)> oPresent;
static HRESULT WINAPI hkPresent(IDirect3DDevice9* pDevice,
                                const RECT* pSourceRect,
                                const RECT* pDestRect,
                                HWND hDestWindowOverride,
                                const RGNDATA* pDirtyRegion) {
    RenderImGui_DX9(pDevice);

    return oPresent(pDevice, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
}

static std::add_pointer_t<HRESULT WINAPI(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD)> oPresentEx;
static HRESULT WINAPI hkPresentEx(IDirect3DDevice9* pDevice,
                                  const RECT* pSourceRect,
                                  const RECT* pDestRect,
                                  HWND hDestWindowOverride,
                                  const RGNDATA* pDirtyRegion,
                                  DWORD dwFlags) {
    RenderImGui_DX9(pDevice);

    return oPresentEx(pDevice, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
}

static std::add_pointer_t<HRESULT WINAPI(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD)> oSwapChainPresent;
static HRESULT WINAPI hkSwapChainPresent(IDirect3DSwapChain9* pSwapChain,
                                         const RECT* pSourceRect,
                                         const RECT* pDestRect,
                                         HWND hDestWindowOverride,
                                         const RGNDATA* pDirtyRegion,
                                         DWORD dwFlags) {
    IDirect3DDevice9* pDevice = nullptr;
    if (SUCCEEDED(pSwapChain->GetDevice(&pDevice)) && pDevice) {
        RenderImGui_DX9(pDevice);
        pDevice->Release( );
    }
    return oSwapChainPresent(pSwapChain, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
}

namespace DX9 {
    void Hook(HWND hwnd) {
        HWND hTempWnd = CreateTempWindow( );
        if (!hTempWnd) {
            LOG("[!] DX9: Failed to create temp window.\n");
            return;
        }

        if (!CreateDeviceD3D9(hTempWnd)) {
            LOG("[!] CreateDeviceD3D9() failed.\n");
            DestroyWindow(hTempWnd);
            UnregisterClassA(TEMP_WND_CLASS, GetModuleHandleA(NULL));
            return;
        }

        LOG("[+] DirectX9: g_pD3D: 0x%p\n", g_pD3D);
        LOG("[+] DirectX9: g_pd3dDevice: 0x%p\n", g_pd3dDevice);

        if (g_pd3dDevice) {
            Menu::InitializeContext(hwnd);

            void** pVTable = *reinterpret_cast<void***>(g_pd3dDevice);

            void* fnReset     = pVTable[16];
            void* fnPresent   = pVTable[17];
            void* fnPresentEx = pVTable[121];
            void* fnResetEx   = pVTable[132];

            // Swap chain Present, used by Adobe AIR
            void* fnSwapChainPresent = nullptr;
            IDirect3DSwapChain9* pSwapChain = nullptr;
            if (SUCCEEDED(g_pd3dDevice->GetSwapChain(0, &pSwapChain)) && pSwapChain) {
                void** pSCVTable = *reinterpret_cast<void***>(pSwapChain);
                fnSwapChainPresent = pSCVTable[3];
                pSwapChain->Release( );
                LOG("[+] DirectX9: IDirect3DSwapChain9::Present at 0x%p\n", fnSwapChainPresent);
            }

            CleanupDeviceD3D9( );
            DestroyWindow(hTempWnd);
            UnregisterClassA(TEMP_WND_CLASS, GetModuleHandleA(NULL));

            static MH_STATUS resetStatus    = MH_CreateHook(reinterpret_cast<void**>(fnReset),     &hkReset,     reinterpret_cast<void**>(&oReset));
            static MH_STATUS resetExStatus  = MH_CreateHook(reinterpret_cast<void**>(fnResetEx),   &hkResetEx,   reinterpret_cast<void**>(&oResetEx));
            static MH_STATUS presentStatus  = MH_CreateHook(reinterpret_cast<void**>(fnPresent),   &hkPresent,   reinterpret_cast<void**>(&oPresent));
            static MH_STATUS presentExStatus= MH_CreateHook(reinterpret_cast<void**>(fnPresentEx), &hkPresentEx, reinterpret_cast<void**>(&oPresentEx));

            MH_EnableHook(fnReset);
            MH_EnableHook(fnResetEx);
            MH_EnableHook(fnPresent);
            MH_EnableHook(fnPresentEx);

            if (fnSwapChainPresent) {
                static MH_STATUS scPresentStatus = MH_CreateHook(
                    reinterpret_cast<void**>(fnSwapChainPresent),
                    &hkSwapChainPresent,
                    reinterpret_cast<void**>(&oSwapChainPresent));
                MH_EnableHook(fnSwapChainPresent);
            }
        } else {
            DestroyWindow(hTempWnd);
            UnregisterClassA(TEMP_WND_CLASS, GetModuleHandleA(NULL));
        }
    }

    void Unhook( ) {
        if (ImGui::GetCurrentContext( )) {
            if (ImGui::GetIO( ).BackendRendererUserData)
                ImGui_ImplDX9_Shutdown( );

            if (ImGui::GetIO( ).BackendPlatformUserData)
                ImGui_ImplWin32_Shutdown( );

            ImGui::DestroyContext( );
        }
    }
} // namespace DX9

static void CleanupDeviceD3D9( ) {
    if (g_pD3D) {
        g_pD3D->Release( );
        g_pD3D = NULL;
    }
    if (g_pd3dDevice) {
        g_pd3dDevice->Release( );
        g_pd3dDevice = NULL;
    }
}

static void RenderImGui_DX9(IDirect3DDevice9* pDevice) {
    static std::atomic<bool> s_rendering { false };
    bool expected = false;
    if (!s_rendering.compare_exchange_strong(expected, true))
        return;
    struct Guard { ~Guard( ) { s_rendering.store(false); } } g;

    if (U::GetRenderingBackend( ) != NONE && U::GetRenderingBackend( ) != DIRECTX9)
        return;

    if (U::GetRenderingBackend( ) == NONE) {
        LOG("[UHX] DX9 Present fired — claiming backend\n");
        U::SetRenderingBackend(DIRECTX9);
    }

    if (!ImGui::GetIO( ).BackendRendererUserData) {
        g_gameDevice = pDevice;
        D3DDEVICE_CREATION_PARAMETERS dcp;
        if (SUCCEEDED(pDevice->GetCreationParameters(&dcp))) {
            if (ImGui::GetIO( ).BackendPlatformUserData)
                ImGui_ImplWin32_Shutdown( );
            ImGui_ImplWin32_Init(dcp.hFocusWindow);
        }
        ImGui_ImplDX9_Init(pDevice);
        Menu::RegisterTextureUploader(UploadTextureRGBA_DX9);
    }

    if (!H::bShuttingDown && ImGui::GetCurrentContext( )) {
        // Fixes menu being too 'white' on games likes 'CS:S'.
        DWORD SRGBWriteEnable;
        pDevice->GetRenderState(D3DRS_SRGBWRITEENABLE, &SRGBWriteEnable);
        pDevice->SetRenderState(D3DRS_SRGBWRITEENABLE, false);

        ImGui_ImplDX9_NewFrame( );
        ImGui_ImplWin32_NewFrame( );
        ImGui::NewFrame( );

        Menu::Render( );

        ImGui::EndFrame( );
        if (pDevice->BeginScene( ) == D3D_OK) {
            ImGui::Render( );
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData( ));
            pDevice->EndScene( );
        }

        pDevice->SetRenderState(D3DRS_SRGBWRITEENABLE, SRGBWriteEnable);
    }
}
#else
#include <Windows.h>
namespace DX9 {
    void Hook(HWND hwnd) { LOG("[!] DirectX9 backend is not enabled!\n"); }
    void Unhook( ) { }
} // namespace DX9
#endif
