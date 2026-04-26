#include "menu.hpp"

#include "../dependencies/imgui/imgui.h"
#include "../dependencies/imgui/imgui_impl_win32.h"

#include <vector>
#include <string>
#include <chrono>
#include <mutex>
#include "../dependencies/imgui/imgui_internal.h"

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
        float durationSeconds;
        std::chrono::steady_clock::time_point start;
        std::chrono::steady_clock::time_point expiry;
        bool started = false;
    };

    static std::vector<Notification> g_notifications;
    static std::mutex g_notificationsMutex;

    void AddNotification(const std::string& title, const std::string& message, float duration) {
        std::lock_guard<std::mutex> lock(g_notificationsMutex);
        g_notifications.push_back({title, message, duration});
    }

    // Returns text truncated so it fits within wrap_width on at most 2 lines, with "..." if cut.
    static std::string TruncateToTwoLines(const std::string& text, float wrap_width) {
        const float line_h = ImGui::GetTextLineHeight( );
        const float max_h  = line_h * 2.0f + 1.0f;

        if (ImGui::CalcTextSize(text.c_str( ), nullptr, false, wrap_width).y <= max_h)
            return text;

        int lo = 0, hi = (int)text.size( );
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            std::string candidate = text.substr(0, mid) + "...";
            if (ImGui::CalcTextSize(candidate.c_str( ), nullptr, false, wrap_width).y <= max_h)
                lo = mid;
            else
                hi = mid - 1;
        }
        return text.substr(0, lo) + "...";
    }

    void RenderNotifications( ) {
        std::lock_guard<std::mutex> lock(g_notificationsMutex);
        auto now = std::chrono::steady_clock::now( );

        // Drop the front notification once it has fully expired.
        if (!g_notifications.empty( )) {
            Notification& front = g_notifications.front( );
            if (front.started && now >= front.expiry)
                g_notifications.erase(g_notifications.begin( ));
        }

        if (g_notifications.empty( ))
            return;

        Notification& n = g_notifications.front( );

        // Start the display timer the first time this notification is rendered,
        // so queued notifications don't silently expire while waiting.
        if (!n.started) {
            n.start  = now;
            n.expiry = now + std::chrono::milliseconds((int)(n.durationSeconds));
            n.started = true;
        }

        const float elapsed   = std::chrono::duration<float>(now - n.start).count( );
        const float remaining = std::chrono::duration<float>(n.expiry - now).count( );

        const float FADE_IN_TIME  = 0.18f;
        const float FADE_OUT_TIME = 0.18f;
        const float alpha_in  = (elapsed   < FADE_IN_TIME)  ? ImClamp(elapsed   / FADE_IN_TIME,  0.0f, 1.0f) : 1.0f;
        const float alpha_out = (remaining < FADE_OUT_TIME) ? ImClamp(remaining / FADE_OUT_TIME, 0.0f, 1.0f) : 1.0f;
        const float alpha = alpha_in * alpha_out;

        ImGuiIO& io = ImGui::GetIO( );
        const float padding = 12.0f;
        const float width   = 340.0f;
        const float height  = 82.0f;

        ImGui::SetNextWindowPos({io.DisplaySize.x - width - padding, io.DisplaySize.y - height - padding});
        ImGui::SetNextWindowSize({width, height});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.10f, 0.90f * alpha));
        ImGui::PushStyleColor(ImGuiCol_Border,   ImVec4(0.83f, 0.68f, 0.21f, alpha));

        ImGui::Begin("##notif", nullptr,
                     ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoInputs      |
                     ImGuiWindowFlags_NoMove         |
                     ImGuiWindowFlags_NoSavedSettings|
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImDrawList* draw = ImGui::GetWindowDrawList( );
        ImVec2 wp = ImGui::GetWindowPos( );

        // Trophy icon, vertically centred in the card.
        const float cx    = wp.x + 35.0f;
        const float cy    = wp.y + height * 0.5f;
        const ImU32 gold  = IM_COL32(212, 175, 55,  (int)(255 * alpha));
        const ImU32 dgold = IM_COL32(160, 120, 20,  (int)(255 * alpha));
        const ImU32 white = IM_COL32(255, 255, 255, (int)(200 * alpha));

        draw->AddRectFilled({cx - 12, cy - 14}, {cx + 12, cy + 6},  gold, 2.0f);
        draw->AddRectFilled({cx - 14, cy - 16}, {cx + 14, cy - 12}, gold);
        draw->AddCircle({cx - 14, cy - 8}, 5.0f, gold, 12, 2.5f);
        draw->AddCircle({cx + 14, cy - 8}, 5.0f, gold, 12, 2.5f);
        draw->AddRectFilled({cx - 3, cy + 6},  {cx + 3,  cy + 14}, dgold);
        draw->AddRectFilled({cx - 10, cy + 14},{cx + 10, cy + 17}, gold);
        draw->AddText({cx - 5, cy - 8}, white, "*");

        // Title.
        ImGui::SetCursorPos({60.0f, 12.0f});
        ImGui::TextColored({1.0f, 0.84f, 0.21f, alpha}, "%s", n.title.c_str( ));

        // Description — at most 2 lines, truncated with "..." if longer.
        const float text_w = width - 60.0f - 12.0f;
        const std::string body = TruncateToTwoLines(n.message, text_w);
        ImGui::SetCursorPos({60.0f, 32.0f});
        ImGui::PushTextWrapPos(60.0f + text_w);
        ImGui::TextColored({0.85f, 0.85f, 0.85f, alpha}, "%s", body.c_str( ));
        ImGui::PopTextWrapPos( );

        ImGui::End( );
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }

    void Render( ) {
        RenderNotifications( );
    }
}
