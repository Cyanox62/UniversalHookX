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
#include "backend/electron/hook_electron.hpp"

#include "backend/opengl/hook_opengl.hpp"
#include "backend/vulkan/hook_vulkan.hpp"

#include "../console/console.hpp"
#include "../menu/menu.hpp"
#include "../utils/utils.hpp"

#include "../dependencies/minhook/MinHook.h"

static HWND g_hWindow = NULL;
static CritSec* g_mReinitHooksGuard = nullptr;

// Calls a Hook(HWND) function inside a structured exception handler so that a
// crash in one backend cannot propagate and kill the host process.
static void CallHookSafe(void (*fn)(HWND), HWND hwnd, const char* name) {
    __try {
        fn(hwnd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[UHX] Exception caught in %s::Hook() — skipping backend.\n", name);
        OutputDebugStringA(buf);
    }
}

// Variant for hooks whose Hook() takes no HWND (Electron subprocess monitor).
static void CallHookSafeNoArg(void (*fn)( ), const char* name) {
    __try {
        fn( );
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

        const DWORD pid = GetCurrentProcessId( );
        const bool isGPU = Electron::IsGPUProcess( );
        OutputDebugStringA("[UHX] H::Init: entered (before first LOG)\n");
        LOG("[UHX] H::Init start PID=%lu IsGPUProcess=%d\n", pid, isGPU ? 1 : 0);
        OutputDebugStringA("[UHX] H::Init: past first LOG (snprintf OK)\n");

        // GPU subprocesses are windowless — skip the 5-second polling loop.
        // All rendering hooks pull the HWND from the real swapchain at present time.
        if (isGPU) {
            g_hWindow = NULL;
            OutputDebugStringA("[UHX] H::Init: GPU process — windowless, skipping GetProcessWindow\n");
        } else {
            g_hWindow = U::GetProcessWindow( );
        }

        // In a Chromium/Electron GPU subprocess (windowless) we still install
        // render hooks — DX11/DX12 hooks pull HWND from the swapchain at
        // present time. We just skip the WndProc hook below.
        // Use OutputDebugStringA (not LOG) for the hook-progress markers so
        // they are visible even if snprintf is broken in the GPU subprocess.
        OutputDebugStringA("[UHX] H::Init: calling VK::Hook\n");
        CallHookSafe(VK::Hook,    g_hWindow, "VK");
        OutputDebugStringA("[UHX] H::Init: calling DX8::Hook\n");
        CallHookSafe(DX8::Hook,   g_hWindow, "DX8");
        OutputDebugStringA("[UHX] H::Init: calling DX9::Hook\n");
        CallHookSafe(DX9::Hook,   g_hWindow, "DX9");
        //CallHookSafe(DX10::Hook, g_hWindow, "DX10");
        OutputDebugStringA("[UHX] H::Init: calling DX11::Hook\n");
        CallHookSafe(DX11::Hook,  g_hWindow, "DX11");
        OutputDebugStringA("[UHX] H::Init: calling DX12::Hook\n");
        CallHookSafe(DX12::Hook,  g_hWindow, "DX12");
        OutputDebugStringA("[UHX] H::Init: calling GL::Hook\n");
        CallHookSafe(GL::Hook,    g_hWindow, "GL");
        OutputDebugStringA("[UHX] H::Init: calling DDraw::Hook\n");
        CallHookSafe(DDraw::Hook, g_hWindow, "DDraw");
        OutputDebugStringA("[UHX] H::Init: calling Electron::Hook\n");
        CallHookSafeNoArg(Electron::Hook, "Electron");

        if (g_hWindow) {
            oWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtr(g_hWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));
            if (!oWndProc)
                LOG("[UHX] SetWindowLongPtr(GWLP_WNDPROC) failed PID=%lu\n", pid);
        } else {
            LOG("[UHX] H::Init: skipping WndProc install (windowless process).\n");
        }

        LOG("[UHX] H::Init: all hooks done\n");
        LOG("[UHX] H::Init done PID=%lu\n", pid);
    }

    void Free( ) {
        if (oWndProc && g_hWindow) {
            SetWindowLongPtr(g_hWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(oWndProc));
            oWndProc = nullptr;
        }

        MH_DisableHook(MH_ALL_HOOKS);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        Electron::Unhook( );

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
