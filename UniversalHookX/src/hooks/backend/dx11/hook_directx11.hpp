#pragma once

#ifdef ENABLE_BACKEND_DX11
#include <dxgi.h>
#endif

namespace DX11 {
	void Hook(HWND hwnd);
	void Unhook( );
#ifdef ENABLE_BACKEND_DX11
	void RenderFrame(IDXGISwapChain* pSwapChain);
#endif
}
