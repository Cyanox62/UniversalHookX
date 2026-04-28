#pragma once
#include <Windows.h>
#include <atomic>

namespace Hooks {
	void Init( );
	void Free( );

	inline std::atomic<bool> bShuttingDown { false };
}

namespace H = Hooks;
