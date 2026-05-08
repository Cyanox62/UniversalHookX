#pragma once
#include <cstdio>
#include <Windows.h>

// All log output goes to OutputDebugStringA (visible in DebugView / VS Output window).
#define LOG(fmt, ...) do { \
    char _uhx_log_buf_[1024]; \
    snprintf(_uhx_log_buf_, sizeof(_uhx_log_buf_), fmt, ##__VA_ARGS__); \
    OutputDebugStringA(_uhx_log_buf_); \
} while (0)

namespace Console {
    void Alloc( );
    void Free( );
}
