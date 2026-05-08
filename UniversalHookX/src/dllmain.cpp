#include <Windows.h>
#include <iostream>
#include <thread>
#include <atomic>

#include "console/console.hpp"

#include "hooks/hooks.hpp"
#include "hooks/backend/electron/hook_electron.hpp"
#include "utils/utils.hpp"
#include "menu/menu.hpp"

#include "dependencies/minhook/MinHook.h"

DWORD WINAPI OnProcessAttach(LPVOID lpParam);
DWORD WINAPI OnProcessDetach(LPVOID lpParam);
static DWORD WINAPI PipeThread(LPVOID);

// Guard so the init logic runs at most once per process even if both DllMain
// and the manual-map shellcode try to spawn it.
static std::atomic<bool> g_initStarted { false };

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        // Use OutputDebugStringA directly here — no CRT calls — so this fires
        // even for manually-mapped DLLs where CRT init may be incomplete.
        OutputDebugStringA("[UHX] DllMain DLL_PROCESS_ATTACH entered\n");

        DisableThreadLibraryCalls(hinstDLL);

        HANDLE hHandle = CreateThread(NULL, 0, OnProcessAttach, hinstDLL, 0, NULL);
        if (hHandle != NULL) {
            CloseHandle(hHandle);
            OutputDebugStringA("[UHX] DllMain CreateThread(OnProcessAttach) ok\n");
        } else {
            OutputDebugStringA("[UHX] DllMain CreateThread(OnProcessAttach) FAILED\n");
        }
    } else if (fdwReason == DLL_PROCESS_DETACH && !lpReserved) {
        OnProcessDetach(NULL);
    }

    return TRUE;
}

DWORD WINAPI OnProcessAttach(LPVOID lpParam) {
    // Plain OutputDebugStringA before any CRT call — proves the thread started.
    OutputDebugStringA("[UHX] OnProcessAttach entered\n");

    bool expected = false;
    if (!g_initStarted.compare_exchange_strong(expected, true)) {
        OutputDebugStringA("[UHX] OnProcessAttach: already initialized — skipping.\n");
        return 0;
    }

    const DWORD pid = GetCurrentProcessId( );
    {
        // Use wsprintfA (user32) + WideCharToMultiByte (kernel32) here so this
        // block has zero CRT dependency. In a manually-mapped DLL the thread
        // spawned by DllMain can run while _DllMainCRTStartup is still setting
        // up static CRT state, making snprintf unsafe this early.
        LPWSTR cmdW = GetCommandLineW( );
        char cmdA[512];
        cmdA[0] = '\0';
        if (cmdW)
            WideCharToMultiByte(CP_UTF8, 0, cmdW, -1, cmdA, (int)sizeof(cmdA) - 1, nullptr, nullptr);
        char logBuf[640];
        wsprintfA(logBuf, "[UHX] Injected! PID=%lu CmdLine=%.480s\n", pid, cmdA);
        OutputDebugStringA(logBuf);
    }
    OutputDebugStringA("[UHX] OnProcessAttach: past CmdLine log, entering EagerInit\n");

    Menu::EagerInit( );
    OutputDebugStringA("[UHX] OnProcessAttach: EagerInit done, calling MH_Initialize\n");

    if (MH_Initialize( ) != MH_OK) {
        OutputDebugStringA("[UHX] MH_Initialize() FAILED — aborting.\n");
        return 1;
    }
    OutputDebugStringA("[UHX] OnProcessAttach: MH_Initialize done, calling H::Init\n");

    // Spawn PipeThread before H::Init so the named pipe is accepting connections
    // while hook installation runs. This prevents a race where Mist sends a
    // notification during H::Init and falls back to the desktop window because
    // no server was listening yet.
    // Browser process: skip if a GPU subprocess was found — the GPU process
    // will own the pipe (using the browser PID) and is the actual renderer.
    const bool skipPipe = !Electron::IsGPUProcess( ) && Electron::GetGPUSubprocessPID( ) != 0;
    if (!skipPipe) {
        HANDLE hPipe = CreateThread(nullptr, 0, PipeThread, nullptr, 0, nullptr);
        if (hPipe) CloseHandle(hPipe);
    } else {
        OutputDebugStringA("[UHX] OnProcessAttach: browser process with GPU subprocess — skipping PipeThread (GPU process owns pipe)\n");
    }

    H::Init( );
    OutputDebugStringA("[UHX] OnProcessAttach: H::Init done\n");

    return 0;
}

// Exported entry point used by the manual-map shellcode as a fallback when
// DllMain → CreateThread(OnProcessAttach) doesn't get the init thread running
// in the GPU subprocess. Calling this is safe to do redundantly: the atomic
// guard inside OnProcessAttach makes it idempotent.
extern "C" __declspec(dllexport) DWORD WINAPI UHXOverlayInit(LPVOID lpParam) {
    OutputDebugStringA("[UHX] UHXOverlayInit called (manual-map fallback)\n");
    HANDLE h = CreateThread(NULL, 0, OnProcessAttach, NULL, 0, NULL);
    if (h) {
        CloseHandle(h);
        OutputDebugStringA("[UHX] UHXOverlayInit: spawn ok\n");
    } else {
        OutputDebugStringA("[UHX] UHXOverlayInit: CreateThread failed — running inline\n");
        OnProcessAttach(NULL);
    }
    return 1;
}

DWORD WINAPI OnProcessDetach(LPVOID lpParam) {
    H::Free( );
    MH_Uninitialize( );

    Console::Free( );

    return 0;
}

struct NotifyPacket {
    char title[128];
    char message[256];
    float duration;
    char imageUrl[512];  // optional: empty string means no image
};

// Returns the PID of the process that created this process (i.e. the browser
// process when we are in a Chromium GPU subprocess).
static DWORD GetParentPID( ) {
    typedef NTSTATUS(NTAPI* PFN)(HANDLE, DWORD, PVOID, ULONG, PULONG);
    auto pfn = reinterpret_cast<PFN>(GetProcAddress(GetModuleHandleA("ntdll.dll"),
                                                    "NtQueryInformationProcess"));
    if (!pfn) return 0;
    struct { PVOID r1; PVOID peb; PVOID r2[2]; ULONG_PTR self; ULONG_PTR parent; } pbi = {};
    ULONG n = 0;
    return (pfn(GetCurrentProcess( ), 0, &pbi, (ULONG)sizeof(pbi), &n) == 0)
               ? static_cast<DWORD>(pbi.parent) : 0;
}

static DWORD WINAPI PipeThread(LPVOID) {
    // GPU subprocess: use the browser process PID as the pipe name so Mist
    // (which injects into and addresses the browser process) can reach us.
    DWORD pipePid = GetCurrentProcessId( );
    if (Electron::IsGPUProcess( )) {
        DWORD parent = GetParentPID( );
        if (parent) pipePid = parent;
    }

    char pipeName[64];
    snprintf(pipeName, sizeof(pipeName), "\\\\.\\pipe\\MistOverlay_%lu", pipePid);

    HANDLE pipe = CreateNamedPipeA(pipeName,
                                   PIPE_ACCESS_INBOUND,
                                   PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                   1, 0, sizeof(NotifyPacket), 0, nullptr);

    if (pipe == INVALID_HANDLE_VALUE)
        return 1;

    while (true) {
        if (!ConnectNamedPipe(pipe, nullptr)) {
            Sleep(100);
            continue;
        }
        NotifyPacket pkt{ };
        DWORD read = 0;
        if (ReadFile(pipe, &pkt, sizeof(pkt), &read, nullptr) && read == sizeof(pkt)) {
            pkt.imageUrl[sizeof(pkt.imageUrl) - 1] = '\0';
            Menu::AddNotification(pkt.title, pkt.message, pkt.duration, pkt.imageUrl);
        }
        DisconnectNamedPipe(pipe);
    }
}
