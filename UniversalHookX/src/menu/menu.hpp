#pragma once

#include <Windows.h>
#include <cstdint>
#include <string>

namespace Menu {
    // Registered by each graphics backend so menu.cpp can upload images without
    // knowing which API is active. Signature: (RGBA pixels, width, height) -> ImTextureID.
    using TextureUploaderFn = void*(*)(const uint8_t* rgba, int w, int h);
    void RegisterTextureUploader(TextureUploaderFn fn);

    // Call before destroying the graphics device. Releases every cached texture via
    // releaser, clears the cache, and resets pending notification textures so they
    // get re-uploaded with the next device. Pixel data is retained for re-upload.
    void InvalidateDeviceTextures(void (*releaser)(void* tex));
    void EagerInit( );
    void AddNotification(const std::string& title, const std::string& message, float durationSeconds, const std::string& imageUrl = {});
    void InitializeContext(HWND hwnd);
    void Render( );

    inline bool bShowMenu = true;
} // namespace Menu
