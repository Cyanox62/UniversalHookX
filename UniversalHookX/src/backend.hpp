#pragma once

//
// Comment whatever backend you don't need.
//	[!] Don't enable DX12 on Windows 7.
//

#define ENABLE_BACKEND_DX9
#define ENABLE_BACKEND_DX10
#define ENABLE_BACKEND_DX11
#define ENABLE_BACKEND_DX12
#define ENABLE_BACKEND_OPENGL
#ifdef _WIN64
#define ENABLE_BACKEND_VULKAN
#else
#define ENABLE_BACKEND_DX8
#endif
#define ENABLE_BACKEND_DDRAW
#define ENABLE_BACKEND_ELECTRON
