#pragma once
#include <windows.h>
#include <string>
namespace sonyconnect::ui {
class LicensesWindow {
  public:
    ~LicensesWindow();
    void create(HINSTANCE instance, HWND owner);
    void show();
    void hide();
    HWND handle() const { return hwnd_; }
    bool pre_translate(MSG &message);
    static std::wstring full_text();
  private:
    HWND hwnd_{};
    static INT_PTR CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
};
} // namespace sonyconnect::ui
