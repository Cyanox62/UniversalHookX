#pragma once

#ifdef ENABLE_BACKEND_DX12
#include <dxgi.h>
#endif

namespace DX12 {
    void Hook(HWND hwnd);
    void Unhook( );
#ifdef ENABLE_BACKEND_DX12
    void RenderFrame(IDXGISwapChain* pSwapChain);
    void CleanupRenderTargets( );
#endif
} // namespace DX12
