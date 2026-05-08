#include <Windows.h>
#include <thread>
#include <dxgi.h>

#include "utils.hpp"

#include "../console/console.hpp"

#define RB2STR(x) case x: return #x

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

static RenderingBackend_t g_eRenderingBackend = NONE;

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
	void SetRenderingBackend(RenderingBackend_t eRenderingBackground) {
		g_eRenderingBackend = eRenderingBackground;
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
		// Timeout — windowless processes (e.g. Chromium/Electron GPU subprocesses)
		// would otherwise loop forever. Returning NULL is a valid outcome and the
		// caller is expected to handle it (skip WndProc, run windowless).
		constexpr int TIMEOUT_MS = 5000;
		constexpr int POLL_MS    = 200;

		HWND hwnd = nullptr;
		EnumWindows(::EnumWindowsCallback, reinterpret_cast<LPARAM>(&hwnd));

		int waited = 0;
		while (!hwnd && waited < TIMEOUT_MS) {
			std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
			waited += POLL_MS;
			EnumWindows(::EnumWindowsCallback, reinterpret_cast<LPARAM>(&hwnd));
		}

		if (hwnd) {
			char name[128];
			GetWindowTextA(hwnd, name, RTL_NUMBER_OF(name));
			LOG("[UHX] Got window with name: '%s'\n", name);
		} else {
			LOG("[UHX] No window found after %dms — running windowless (likely a GPU subprocess).\n", TIMEOUT_MS);
		}

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
