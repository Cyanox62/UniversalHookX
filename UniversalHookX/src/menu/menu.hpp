#pragma once

#include <Windows.h>
#include <string>

namespace Menu {
    void AddNotification(const std::string& title, const std::string& message, float durationSeconds);
    void InitializeContext(HWND hwnd);
    void Render( );

    inline bool bShowMenu = true;
} // namespace Menu
