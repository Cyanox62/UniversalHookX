#include <cstdio>
#include <mutex>
#include <thread>

#include "hooks.hpp"

#include "backend/ddraw/hook_ddraw.hpp"
#include "backend/dx10/hook_directx10.hpp"
#include "backend/dx11/hook_directx11.hpp"
#include "backend/dx12/hook_directx12.hpp"
#include "backend/dx9/hook_directx9.hpp"
#include "backend/dx8/hook_directx8.hpp"

#include "backend/opengl/hook_opengl.hpp"
#include "backend/vulkan/hook_vulkan.hpp"

#include "../console/console.hpp"
#include "../menu/menu.hpp"
#include "../utils/utils.hpp"

#include "../dependencies/minhook/MinHook.h"

static HWND g_hWindow = NULL;
static CritSec* g_mReinitHooksGuard = nullptr;

static void CallHookSafe(void (*fn)(HWND), HWND hwnd, const char* name) {
    __try {
        fn(hwnd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[UHX] Exception caught in %s::Hook() — skipping backend.\n", name);
        OutputDebugStringA(buf);
    }
}

static DWORD WINAPI ReinitializeGraphicalHooks(LPVOID lpParam) {
    if (!g_mReinitHooksGuard)
        return 0;

    CritSecGuard guard{*g_mReinitHooksGuard};

    LOG("[UHX] Hooks will reinitialize!\n");

    HWND hNewWindow = U::GetProcessWindow( );
    while (hNewWindow == reinterpret_cast<HWND>(lpParam)) {
        hNewWindow = U::GetProcessWindow( );
    }

    H::bShuttingDown = true;

    H::Free( );
    H::Init( );

    H::bShuttingDown = false;
    Menu::bShowMenu = true;

    return 0;
}

static WNDPROC oWndProc;
static LRESULT WINAPI WndProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_KEYDOWN) {
        if (wParam == VK_INSERT) {
            Menu::bShowMenu = !Menu::bShowMenu;
            return 0;
        } else if (wParam == VK_HOME) {
            HANDLE hHandle = CreateThread(NULL, 0, ReinitializeGraphicalHooks, NULL, 0, NULL);
            if (hHandle != NULL)
                CloseHandle(hHandle);
            return 0;
        } else if (wParam == VK_END) {
            H::bShuttingDown = true;
            U::UnloadDLL( );
            return 0;
        }
    } else if (uMsg == WM_DESTROY) {
        HANDLE hHandle = CreateThread(NULL, 0, ReinitializeGraphicalHooks, hWnd, 0, NULL);
        if (hHandle != NULL)
            CloseHandle(hHandle);
    }

    LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    if (Menu::bShowMenu) {
        ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam);
    }

    if (!oWndProc)
        return DefWindowProcW(hWnd, uMsg, wParam, lParam);

    return CallWindowProc(oWndProc, hWnd, uMsg, wParam, lParam);
}

namespace Hooks {
    void Init( ) {
        if (!g_mReinitHooksGuard)
            g_mReinitHooksGuard = new CritSec( );

        g_hWindow = U::GetProcessWindow( );

        CallHookSafe(VK::Hook,    g_hWindow, "VK");
        CallHookSafe(DX8::Hook,   g_hWindow, "DX8");
        CallHookSafe(DX9::Hook,   g_hWindow, "DX9");
        //CallHookSafe(DX10::Hook, g_hWindow, "DX10");
        CallHookSafe(DX11::Hook,  g_hWindow, "DX11");
        CallHookSafe(DX12::Hook,  g_hWindow, "DX12");
        CallHookSafe(GL::Hook,    g_hWindow, "GL");
        CallHookSafe(DDraw::Hook, g_hWindow, "DDraw");

        oWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtr(g_hWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));
        if (!oWndProc)
            LOG("[UHX] SetWindowLongPtr failed (GWLP_WNDPROC).\n");
    }

    void Free( ) {
        if (oWndProc && g_hWindow) {
            SetWindowLongPtr(g_hWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(oWndProc));
            oWndProc = nullptr;
        }

        MH_DisableHook(MH_ALL_HOOKS);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        RenderingBackend_t eRenderingBackend = U::GetRenderingBackend( );
        switch (eRenderingBackend) {
            case DIRECTX8:
                DX8::Unhook( );
                break;
            case DIRECTX9:
                DX9::Unhook( );
                break;
            case DIRECTX10:
                DX10::Unhook( );
                break;
            case DIRECTX11:
                DX11::Unhook( );
                break;
            case DIRECTX12:
                DX12::Unhook( );
                break;
            case OPENGL:
                GL::Unhook( );
                break;
            case VULKAN:
                VK::Unhook( );
                break;
            case DIRECTDRAW:
                DDraw::Unhook( );
                break;
            default:
                break;
        }
    }
} // namespace Hooks
