#pragma once
#include <windows.h>
#include <filesystem>
#include <string>

namespace sonyconnect::ui {
// Stable class name for single-instance activation; the host has no UI controls.
inline constexpr wchar_t app_host_class[] = L"SonyConnect.Native.Window";
enum class AppCheck { none, sliders, updates, diagnostics };
struct AppOptions {
    bool offline{}, start_hidden{};
    AppCheck check{AppCheck::none};
    std::wstring preview;
    UINT preview_dpi{};
    std::filesystem::path capture, report;
};
int run_application(HINSTANCE, const AppOptions &);
} // namespace sonyconnect::ui
