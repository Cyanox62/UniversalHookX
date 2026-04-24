#include "menu/menu.hpp"

extern "C" __declspec(dllexport) void AddNotification(const char* title, const char* message, float duration) {
    Menu::AddNotification(title, message, duration);
}
