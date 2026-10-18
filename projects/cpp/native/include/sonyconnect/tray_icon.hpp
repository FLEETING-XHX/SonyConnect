#pragma once
#include <windows.h>
#include <shellapi.h>
#include <functional>
#include <stdexcept>

namespace sonyconnect::desktop {
class TrayIcon {
  public:
    using Notify = std::function<BOOL(DWORD, NOTIFYICONDATAW *)>;
    TrayIcon(HWND owner, UINT callback, HICON icon, Notify notify = Shell_NotifyIconW)
        : notify_(std::move(notify)), restart_(RegisterWindowMessageW(L"TaskbarCreated")) {
        if (!restart_) throw std::runtime_error("Cannot register taskbar restart message");
        data_.cbSize = sizeof(data_);
        data_.hWnd = owner;
        data_.uID = 1;
        data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        data_.uCallbackMessage = callback;
        data_.hIcon = icon;
        wcscpy_s(data_.szTip, L"SonyConnect");
    }
    ~TrayIcon() { if (added_) notify_(NIM_DELETE, &data_); }
    TrayIcon(const TrayIcon &) = delete;
    TrayIcon &operator=(const TrayIcon &) = delete;
    bool restore() { added_ = notify_(NIM_ADD, &data_) != FALSE; return added_; }
    bool added() const { return added_; }
    bool taskbar_created(UINT message) const { return message == restart_; }
  private:
    Notify notify_;
    NOTIFYICONDATAW data_{};
    UINT restart_{};
    bool added_{};
};
} // namespace sonyconnect::desktop
