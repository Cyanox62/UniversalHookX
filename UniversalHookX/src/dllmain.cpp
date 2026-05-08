#include <Windows.h>
#include <iostream>
#include <thread>

#include "console/console.hpp"

#include "hooks/hooks.hpp"
#include "utils/utils.hpp"
#include "menu/menu.hpp"

#include "dependencies/minhook/MinHook.h"

DWORD WINAPI OnProcessAttach(LPVOID lpParam);
DWORD WINAPI OnProcessDetach(LPVOID lpParam);
static DWORD WINAPI PipeThread(LPVOID);

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);

        HANDLE hHandle = CreateThread(NULL, 0, OnProcessAttach, hinstDLL, 0, NULL);
        if (hHandle != NULL) {
            CloseHandle(hHandle);
        }
    } else if (fdwReason == DLL_PROCESS_DETACH && !lpReserved) {
        OnProcessDetach(NULL);
    }

    return TRUE;
}

DWORD WINAPI OnProcessAttach(LPVOID lpParam) {
    LOG("[UHX] Injected!\n");
    LOG("[UHX] Auto-detecting rendering backend...\n");
    Menu::EagerInit( );

    if (MH_Initialize( ) != MH_OK) {
        LOG("[UHX] MH_Initialize() failed — aborting hook setup.\n");
        return 1;
    }
    H::Init( );

    HANDLE hPipe = CreateThread(nullptr, 0, PipeThread, nullptr, 0, nullptr);
    if (hPipe)
        CloseHandle(hPipe);

    return 0;
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

static DWORD WINAPI PipeThread(LPVOID) {
    char pipeName[64];
    snprintf(pipeName, sizeof(pipeName), "\\\\.\\pipe\\MistOverlay_%lu", GetCurrentProcessId( ));

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
