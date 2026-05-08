#pragma once
#include <Windows.h>

namespace Electron {
    bool IsGPUProcess();
    void Hook();
    void Unhook();
    DWORD GetGPUSubprocessPID(); // 0 if not yet found / not applicable
}
