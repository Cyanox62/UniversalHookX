#pragma once

#include <Windows.h>
#include <cstdint>
#include <string>

namespace Menu {
    // Registered by each graphics backend so menu.cpp can upload images without
    // knowing which API is active. Signature: (RGBA pixels, width, height) -> ImTextureID.
    using TextureUploaderFn = void*(*)(const uint8_t* rgba, int w, int h);
    void RegisterTextureUploader(TextureUploaderFn fn);

    void AddNotification(const std::string& title, const std::string& message, float durationSeconds, const std::string& imageUrl = {});
    void InitializeContext(HWND hwnd);
    void Render( );

    inline bool bShowMenu = true;
} // namespace Menu
