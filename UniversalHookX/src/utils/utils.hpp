#pragma once

enum RenderingBackend_t {
	NONE = 0,

	DIRECTX8,
	DIRECTX9,
	DIRECTX10,
	DIRECTX11,
	DIRECTX12,

	OPENGL,
	VULKAN,

	DIRECTDRAW,
};

struct CritSec {
    CRITICAL_SECTION cs;
    CritSec( ) { InitializeCriticalSection(&cs); }
    ~CritSec( ) { DeleteCriticalSection(&cs); }
    void lock( ) { EnterCriticalSection(&cs); }
    void unlock( ) { LeaveCriticalSection(&cs); }
};
struct CritSecGuard {
    CritSec& m;
    CritSecGuard(CritSec& m) : m(m) { m.lock( ); }
    ~CritSecGuard( ) { m.unlock( ); }
};

namespace Utils {
	void SetRenderingBackend(RenderingBackend_t eRenderingBackend);
	RenderingBackend_t GetRenderingBackend( );
	const char* RenderingBackendToStr( );

	HWND GetProcessWindow( );
	void UnloadDLL( );
	
	HMODULE GetCurrentImageBase( );

	int GetCorrectDXGIFormat(int eCurrentFormat);
}

namespace U = Utils;
