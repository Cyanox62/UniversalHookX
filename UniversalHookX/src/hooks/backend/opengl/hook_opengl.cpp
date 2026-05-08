#include "../../../backend.hpp"
#include "../../../console/console.hpp"

#ifdef ENABLE_BACKEND_OPENGL
#include <Windows.h>
#include <GL/gl.h>
#pragma comment(lib, "opengl32.lib")

#include <atomic>
#include <memory>

#include "hook_opengl.hpp"

#include "../../../dependencies/imgui/imgui_impl_opengl3.h"
#include "../../../dependencies/imgui/imgui_impl_win32.h"
#include "../../../dependencies/minhook/MinHook.h"

#include "../../hooks.hpp"
#include "../../../utils/utils.hpp"

#include "../../../menu/menu.hpp"

static void* UploadTextureRGBA_GL(const uint8_t* rgba, int w, int h) {
    GLint prevTex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);

    GLuint texId = 0;
    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    return reinterpret_cast<void*>(static_cast<intptr_t>(texId));
}

static std::add_pointer_t<BOOL WINAPI(HDC)> oWglSwapBuffers;
static BOOL WINAPI hkWglSwapBuffers(HDC Hdc) {
    static std::atomic<bool> s_rendering { false };
    bool expected = false;
    if (!s_rendering.compare_exchange_strong(expected, true))
        return oWglSwapBuffers(Hdc);
    struct Guard { ~Guard( ) { s_rendering.store(false); } } g;

    if (U::GetRenderingBackend( ) != NONE && U::GetRenderingBackend( ) != OPENGL)
        return oWglSwapBuffers(Hdc);

    if (U::GetRenderingBackend( ) == NONE) {
        LOG("[UHX] OpenGL SwapBuffers fired — claiming backend\n");
        U::SetRenderingBackend(OPENGL);
    }

    if (!H::bShuttingDown && ImGui::GetCurrentContext( )) {
        if (!ImGui::GetIO( ).BackendRendererUserData) {
            HWND hwnd = WindowFromDC(Hdc);
            if (hwnd && ImGui::GetIO( ).BackendPlatformUserData)
                ImGui_ImplWin32_Shutdown( );
            if (hwnd)
                ImGui_ImplWin32_Init(hwnd);
            ImGui_ImplOpenGL3_Init( );
            Menu::RegisterTextureUploader(UploadTextureRGBA_GL);
        }

        ImGui_ImplOpenGL3_NewFrame( );
        ImGui_ImplWin32_NewFrame( );
        ImGui::NewFrame( );

        Menu::Render( );

        ImGui::Render( );
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData( ));
    }

    return oWglSwapBuffers(Hdc);
}

namespace GL {
    void Hook(HWND hwnd) {
        HMODULE openGL32 = GetModuleHandleA("opengl32.dll");
        if (openGL32) {
            LOG("[+] OpenGL32: ImageBase: 0x%p\n", openGL32);

            void* fnWglSwapBuffers = reinterpret_cast<void*>(GetProcAddress(openGL32, "wglSwapBuffers"));
            if (fnWglSwapBuffers) {
                Menu::InitializeContext(hwnd);

                // Hook
                LOG("[+] OpenGL32: fnWglSwapBuffers: 0x%p\n", fnWglSwapBuffers);

                static MH_STATUS wsbStatus = MH_CreateHook(reinterpret_cast<void**>(fnWglSwapBuffers), &hkWglSwapBuffers, reinterpret_cast<void**>(&oWglSwapBuffers));

                MH_EnableHook(fnWglSwapBuffers);
            }
        }
    }

    void Unhook( ) {
        if (ImGui::GetCurrentContext( )) {
            if (ImGui::GetIO( ).BackendRendererUserData)
                ImGui_ImplOpenGL3_Shutdown( );

            if (ImGui::GetIO( ).BackendPlatformUserData)
                ImGui_ImplWin32_Shutdown( );

            ImGui::DestroyContext( );
        }
    }
} // namespace GL
#else
#include <Windows.h>
namespace GL {
    void Hook(HWND hwnd) { LOG("[!] OpenGL backend is not enabled!\n"); }
    void Unhook( ) { }
} // namespace GL
#endif
