#include "menu.hpp"

#include "../dependencies/imgui/imgui.h"
#include "../dependencies/imgui/imgui_impl_win32.h"

#include <vector>
#include <string>
#include <chrono>
#include <mutex>

namespace ig = ImGui;

namespace Menu {
    void InitializeContext(HWND hwnd) {
        if (ig::GetCurrentContext( ))
            return;

        ImGui::CreateContext( );
        ImGui_ImplWin32_Init(hwnd);

        ImGuiIO& io = ImGui::GetIO( );
        io.IniFilename = io.LogFilename = nullptr;
    }

    struct Notification {
        std::string title;
        std::string message;
        std::chrono::steady_clock::time_point start;
        std::chrono::steady_clock::time_point expiry;
    };

    static std::vector<Notification> g_notifications;
    static std::mutex g_notificationsMutex;

    void AddNotification(const std::string& title, const std::string& message, float durationSeconds) {
        auto now = std::chrono::steady_clock::now( );
        std::lock_guard<std::mutex> lock(g_notificationsMutex);
        g_notifications.push_back({title, message, now, now + std::chrono::milliseconds((int)(durationSeconds * 1000))});
    }

    void RenderNotifications( ) {
        std::lock_guard<std::mutex> lock(g_notificationsMutex);
        auto now = std::chrono::steady_clock::now( );
        g_notifications.erase(
            std::remove_if(g_notifications.begin( ), g_notifications.end( ),
                           [&](const Notification& n) { return now >= n.expiry; }),
            g_notifications.end( ));

        if (g_notifications.empty( ))
            return;

        ImGuiIO& io = ImGui::GetIO( );
        const float padding = 12.0f;
        const float width = 340.0f;
        const float height = 70.0f;
        float startY = io.DisplaySize.y - padding;

        for (int i = (int)g_notifications.size( ) - 1; i >= 0; i--) {
            startY -= height + padding;

            ImGui::SetNextWindowPos({io.DisplaySize.x - width - padding, startY});
            ImGui::SetNextWindowSize({width, height});
            ImGui::SetNextWindowBgAlpha(0.90f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.10f, 0.90f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.83f, 0.68f, 0.21f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);

            ImGui::Begin(("##notif" + std::to_string(i)).c_str( ), nullptr,
                         ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_NoInputs |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus);

            ImDrawList* draw = ImGui::GetWindowDrawList( );
            ImVec2 wp = ImGui::GetWindowPos( );

            // --- Trophy body ---
            const float cx = wp.x + 35.0f;
            const float cy = wp.y + 28.0f;
            const ImU32 gold = IM_COL32(212, 175, 55, 255);
            const ImU32 dgold = IM_COL32(160, 120, 20, 255);

            // Cup body
            draw->AddRectFilled({cx - 12, cy - 14}, {cx + 12, cy + 6}, gold, 2.0f);
            // Cup rim
            draw->AddRectFilled({cx - 14, cy - 16}, {cx + 14, cy - 12}, gold);
            // Handles
            draw->AddCircle({cx - 14, cy - 8}, 5.0f, gold, 12, 2.5f);
            draw->AddCircle({cx + 14, cy - 8}, 5.0f, gold, 12, 2.5f);
            // Stem
            draw->AddRectFilled({cx - 3, cy + 6}, {cx + 3, cy + 14}, dgold);
            // Base
            draw->AddRectFilled({cx - 10, cy + 14}, {cx + 10, cy + 17}, gold);
            // Star inside cup
            draw->AddText({cx - 5, cy - 8}, IM_COL32(255, 255, 255, 200), "*");

            // --- Text ---
            ImGui::SetCursorPos({60.0f, 10.0f});
            ImGui::TextColored({1.0f, 0.84f, 0.21f, 1.0f}, "%s", g_notifications[i].title.c_str( ));
            ImGui::SetCursorPos({60.0f, 30.0f});
            ImGui::TextColored({0.85f, 0.85f, 0.85f, 1.0f}, "%s", g_notifications[i].message.c_str( ));

            // --- Progress bar (time remaining) ---
            auto total = std::chrono::duration<float>(g_notifications[i].expiry - g_notifications[i].start).count( );
            auto remaining = std::chrono::duration<float>(g_notifications[i].expiry - now).count( );
            float progress = remaining / total;

            ImGui::SetCursorPos({60.0f, 50.0f});
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.83f, 0.68f, 0.21f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
            ImGui::ProgressBar(progress, {width - 72.0f, 6.0f}, "");
            ImGui::PopStyleColor(2);

            ImGui::End( );
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
        }
    }

    void Render( ) {
        RenderNotifications( );
    }
}
