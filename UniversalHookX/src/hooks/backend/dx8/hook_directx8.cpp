#include "../../../backend.hpp"
#include "../../../console/console.hpp"

#ifdef ENABLE_BACKEND_DX8
#include <Windows.h>
#include <d3d8.h>
#pragma comment(lib, "d3d8.lib")

#include <atomic>

#include "../../../menu/menu.hpp"
#include "../../../utils/utils.hpp"
#include "../../hooks.hpp"

#include "../../../dependencies/imgui/imgui_impl_dx8.h"
#include "../../../dependencies/imgui/imgui_impl_win32.h"
#include "../../../dependencies/minhook/MinHook.h"
#include <Psapi.h>

static IDirect3DDevice8* g_gameDevice = nullptr;

static void* UploadTextureRGBA_DX8(const uint8_t* rgba, int w, int h) {
    if (!g_gameDevice)
        return nullptr;

    IDirect3DTexture8* pTex = nullptr;
    if (FAILED(g_gameDevice->CreateTexture((UINT)w, (UINT)h, 1, 0,
                                           D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &pTex)))
        return nullptr;

    D3DLOCKED_RECT lr;
    if (FAILED(pTex->LockRect(0, &lr, nullptr, 0))) {
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

static void RenderImGui_DX8(IDirect3DDevice8* pDevice) {
    static std::atomic<bool> s_rendering{false};
    bool expected = false;
    if (!s_rendering.compare_exchange_strong(expected, true))
        return;
    struct Guard {
        ~Guard( ) { s_rendering.store(false); }
    } g;

    if (U::GetRenderingBackend( ) != NONE && U::GetRenderingBackend( ) != DIRECTX8)
        return;

    if (U::GetRenderingBackend( ) == NONE) {
        LOG("[UHX] DX8 Present fired — claiming backend\n");
        U::SetRenderingBackend(DIRECTX8);
    }

    if (!ImGui::GetIO( ).BackendRendererUserData) {
        g_gameDevice = pDevice;
        D3DDEVICE_CREATION_PARAMETERS dcp;
        if (SUCCEEDED(pDevice->GetCreationParameters(&dcp))) {
            if (ImGui::GetIO( ).BackendPlatformUserData)
                ImGui_ImplWin32_Shutdown( );
            ImGui_ImplWin32_Init(dcp.hFocusWindow);
        }
        ImGui_ImplDX8_Init(pDevice);
        Menu::RegisterTextureUploader(UploadTextureRGBA_DX8);
    }

    if (!H::bShuttingDown && ImGui::GetCurrentContext( )) {
        ImGui_ImplDX8_NewFrame( );
        ImGui_ImplWin32_NewFrame( );
        ImGui::NewFrame( );

        Menu::Render( );

        ImGui::EndFrame( );

        if (pDevice->BeginScene( ) == D3D_OK) {
            ImGui::Render( );
            ImGui_ImplDX8_RenderDrawData(ImGui::GetDrawData( ));
            pDevice->EndScene( );
        }
    }
}

static std::add_pointer_t<HRESULT WINAPI(IDirect3DDevice8*, D3DPRESENT_PARAMETERS*)> oReset;
static HRESULT WINAPI hkReset(IDirect3DDevice8* pDevice, D3DPRESENT_PARAMETERS* pPresentationParameters) {
    if (ImGui::GetCurrentContext( ) && ImGui::GetIO( ).BackendRendererUserData)
        ImGui_ImplDX8_InvalidateDeviceObjects( );
    return oReset(pDevice, pPresentationParameters);
}

static std::add_pointer_t<HRESULT WINAPI(IDirect3DDevice8*, const RECT*, const RECT*, HWND, const RGNDATA*)> oPresent;
static HRESULT WINAPI hkPresent(IDirect3DDevice8* pDevice, const RECT* src, const RECT* dst, HWND window, const RGNDATA* dirty) {
    RenderImGui_DX8(pDevice);
    return oPresent(pDevice, src, dst, window, dirty);
}

uintptr_t FindVTableByFunction(HMODULE module, uintptr_t fnAddress) {
    MODULEINFO mi = {0};
    if (!module || !GetModuleInformation(GetCurrentProcess( ), module, &mi, sizeof(mi)))
        return 0;

    uintptr_t start = (uintptr_t)mi.lpBaseOfDll;
    uintptr_t end = start + mi.SizeOfImage;

    for (uintptr_t i = start + sizeof(uintptr_t); i < end - sizeof(uintptr_t); i += sizeof(uintptr_t)) {
        if (*reinterpret_cast<uintptr_t*>(i) == fnAddress) {
            uintptr_t possibleReset = *reinterpret_cast<uintptr_t*>(i - sizeof(uintptr_t));
            if (possibleReset > start && possibleReset < end)
                return i - sizeof(uintptr_t);
        }
    }
    return 0;
}

uintptr_t FindSignature(HMODULE moduleHandle, const char* signature, const char* mask) {
    MODULEINFO moduleInfo = {0};

    if (!moduleHandle)
        return 0;

    if (!GetModuleInformation(GetCurrentProcess( ), moduleHandle, &moduleInfo, sizeof(moduleInfo)))
        return 0;

    uintptr_t moduleStart = (uintptr_t)moduleInfo.lpBaseOfDll;
    uintptr_t moduleSize = (uintptr_t)moduleInfo.SizeOfImage;
    size_t sigLen = strlen(mask);

    for (uintptr_t i = 0; i < moduleSize - sigLen; i++) {
        bool found = true;

        for (size_t j = 0; j < sigLen; j++) {
            if (mask[j] == 'x' && *(char*)(moduleStart + i + j) != signature[j]) {
                found = false;
                break;
            }
        }

        if (found) {
            return moduleStart + i;
        }
    }

    return 0;
}

namespace DX8 {
    void Hook(HWND hwnd) {
        const char* pattern = "\x8B\x86\x00\x00\x00\x00\x85\xC0\x75\x00\xBE\x6C\x08\x00\x88\xEB\x00\xFF\x75\x00";
        const char* mask = "xx????xxx?xxx?xx?xx?";

        HMODULE hD3D8 = GetModuleHandleA("d3d8.dll");

        if (!hD3D8 || !GetProcAddress(hD3D8, "Direct3DCreate8")) {
            LOG("[UHX] DX8: Direct3DCreate8 not loaded\n");
            return;
        }

        uintptr_t raw = FindSignature(hD3D8, pattern, mask);
        if (!raw) {
            LOG("[UHX] DX8: Failed to find Present signature.\n");
            return;
        }

        if (raw < 0x22) {
            LOG("[UHX] DX8: Signature match too close to zero to apply offset.\n");
            return;
        }

        uintptr_t match = raw - 0x22;

        MODULEINFO mi = {0};
        GetModuleInformation(GetCurrentProcess( ), hD3D8, &mi, sizeof(mi));
        uintptr_t modStart = (uintptr_t)mi.lpBaseOfDll;
        uintptr_t modEnd   = modStart + mi.SizeOfImage;

        if (match < modStart || match >= modEnd) {
            LOG("[UHX] DX8: Adjusted address 0x%p is outside d3d8.dll bounds.\n", (void*)match);
            return;
        }

        void* fnPresent = reinterpret_cast<void*>(match);
        LOG("[UHX] DX8: Derived Present at 0x%p\n", fnPresent);

        unsigned char prologue = *reinterpret_cast<unsigned char*>(fnPresent);
        if (prologue != 0x55 && prologue != 0x8B && prologue != 0x56) {
            LOG("[UHX] DX8: Sig match failed prologue check (0x%02X)\n", prologue);
            return;
        }

        uintptr_t resetVTableEntry = FindVTableByFunction(hD3D8, match);
        if (!resetVTableEntry) {
            LOG("[UHX] DX8: Could not derive Reset from VTable.\n");
            return;
        }

        void* fnReset = (void*)*reinterpret_cast<uintptr_t*>(resetVTableEntry);
        LOG("[UHX] DX8: Derived Reset at 0x%p\n", fnReset);

        Menu::InitializeContext(hwnd);

        if (MH_CreateHook(reinterpret_cast<void*>(fnReset), &hkReset, reinterpret_cast<void**>(&oReset)) == MH_OK) {
            MH_EnableHook(reinterpret_cast<void*>(fnReset));
            LOG("[UHX] DX8: Hooked Reset.\n");
        }

        if (MH_CreateHook(reinterpret_cast<void*>(fnPresent), &hkPresent, reinterpret_cast<void**>(&oPresent)) == MH_OK) {
            MH_EnableHook(reinterpret_cast<void*>(fnPresent));
            LOG("[UHX] DX8: Hooked Present.\n");
        }
    }

    void Unhook( ) {
        if (ImGui::GetCurrentContext( )) {
            if (ImGui::GetIO( ).BackendRendererUserData)
                ImGui_ImplDX8_Shutdown( );
            if (ImGui::GetIO( ).BackendPlatformUserData)
                ImGui_ImplWin32_Shutdown( );
            ImGui::DestroyContext( );
        }
    }
} // namespace DX8
#else
#include <Windows.h>
namespace DX8 {
    void Hook(HWND hwnd) { LOG("[!] DirectX8 backend is not enabled!\n"); }
    void Unhook( ) { }
} // namespace DX8
#endif
