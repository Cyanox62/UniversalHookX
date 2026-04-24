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
        CreateThread(nullptr, 0, PipeThread, nullptr, 0, nullptr);

        //U::SetRenderingBackend(DIRECTX12);

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
    Console::Alloc( );
    LOG("[+] Auto-detecting rendering backend...");
    //LOG("[+] Rendering backend: %s\n", U::RenderingBackendToStr( ));
    //if (U::GetRenderingBackend( ) == NONE) {
    //    LOG("[!] Looks like you forgot to set a backend. Will unload after pressing enter...");
    //    std::cin.get( );

    //    FreeLibraryAndExitThread(reinterpret_cast<HMODULE>(lpParam), 0);
    //    return 0;
    //}

    MH_Initialize( );
    H::Init( );

    //std::thread([]( ) {
    //    Sleep(2000);
    //    Menu::AddNotification("Achievement unlocked!", "You have unlocked a new achievement.", 5.0f);
    //}).detach( );

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
        if (ReadFile(pipe, &pkt, sizeof(pkt), &read, nullptr) && read == sizeof(pkt))
            Menu::AddNotification(pkt.title, pkt.message, pkt.duration);
        DisconnectNamedPipe(pipe);
    }
}
