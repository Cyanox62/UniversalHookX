#include <Windows.h>
#include <thread>
#include <atomic>
#include <dxgi.h>

#include "utils.hpp"

#include "../console/console.hpp"

#define RB2STR(x) case x: return #x

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

static RenderingBackend_t g_eRenderingBackend = NONE;
static std::atomic<bool> g_backendNotified{ false };

static DWORD WINAPI NotifyLauncherThread(LPVOID) {
    char pipeName[64];
    snprintf(pipeName, sizeof(pipeName), "\\\\.\\pipe\\MistOverlayStatus_%lu", GetCurrentProcessId());

    for (int i = 0; i < 10; i++) {
        HANDLE pipe = CreateFileA(pipeName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            BYTE confirm = 1;
            DWORD written = 0;
            WriteFile(pipe, &confirm, 1, &written, nullptr);
            CloseHandle(pipe);
            return 0;
        }
        Sleep(500);
    }
    return 0;
}

static BOOL CALLBACK EnumWindowsCallback(HWND handle, LPARAM lParam) {
	const auto isMainWindow = [ handle ]( ) {
		return GetWindow(handle, GW_OWNER) == nullptr && IsWindowVisible(handle);
	};

	DWORD pID = 0;
	GetWindowThreadProcessId(handle, &pID);

	if (GetCurrentProcessId( ) != pID || !isMainWindow( ) || handle == GetConsoleWindow( ))
		return TRUE;

	*reinterpret_cast<HWND*>(lParam) = handle;

	return FALSE;
}

static DWORD WINAPI _UnloadDLL(LPVOID lpParam) {
	FreeLibraryAndExitThread(Utils::GetCurrentImageBase( ), 0);
	return 0;
}

namespace Utils {
	void NotifyLauncher() {
		if (!g_backendNotified.exchange(true)) {
			HANDLE h = CreateThread(nullptr, 0, NotifyLauncherThread, nullptr, 0, nullptr);
			if (h) CloseHandle(h);
		}
	}

	void SetRenderingBackend(RenderingBackend_t eRenderingBackground) {
		if (g_eRenderingBackend != NONE) return;
		g_eRenderingBackend = eRenderingBackground;
		NotifyLauncher();
	}

	RenderingBackend_t GetRenderingBackend( ) {
		return g_eRenderingBackend;
	}

	const char* RenderingBackendToStr( ) {
		RenderingBackend_t eRenderingBackend = GetRenderingBackend( );

		switch (eRenderingBackend) {
			RB2STR(DIRECTX9);
			RB2STR(DIRECTX10);
			RB2STR(DIRECTX11);
			RB2STR(DIRECTX12);

			RB2STR(OPENGL);
			RB2STR(VULKAN);

			RB2STR(DIRECTDRAW);
		}

		return "NONE/UNKNOWN";
	}

	HWND GetProcessWindow( ) {
		HWND hwnd = nullptr;
		EnumWindows(::EnumWindowsCallback, reinterpret_cast<LPARAM>(&hwnd));

		while (!hwnd) {
			EnumWindows(::EnumWindowsCallback, reinterpret_cast<LPARAM>(&hwnd));
			LOG("[!] Waiting for window to appear.\n");
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
		}

		char name[128];
		GetWindowTextA(hwnd, name, RTL_NUMBER_OF(name));
		LOG("[+] Got window with name: '%s'\n", name);

		return hwnd;
	}

	void UnloadDLL( ) {
		HANDLE hThread = CreateThread(NULL, 0, _UnloadDLL, NULL, 0, NULL);
		if (hThread != NULL) CloseHandle(hThread);
	}

	HMODULE GetCurrentImageBase( ) {
		return (HINSTANCE)(&__ImageBase);
	}

	int GetCorrectDXGIFormat(int eCurrentFormat) {
		switch (eCurrentFormat) {
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
		}

		return eCurrentFormat;
	}
}
