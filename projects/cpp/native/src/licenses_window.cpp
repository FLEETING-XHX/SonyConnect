#include "sonyconnect/licenses_window.hpp"
#include "sonyconnect/embedded_assets.hpp"
#include <winrt/base.h>
#include <algorithm>
#include <stdexcept>

namespace sonyconnect::ui {
namespace {
void layout(HWND hwnd) {
    RECT area{}; GetClientRect(hwnd, &area);
    const int unit = MulDiv(12, GetDpiForWindow(hwnd), 96);
    const int button_height = MulDiv(32, GetDpiForWindow(hwnd), 96);
    const int top = unit * 4;
    const int bottom = area.bottom - unit - button_height;
    MoveWindow(GetDlgItem(hwnd, 403), unit, unit, std::max(1L, area.right - 2 * unit), top - 2 * unit, TRUE);
    MoveWindow(GetDlgItem(hwnd, 401), unit, top, std::max(1L, area.right - 2 * unit),
               std::max(1, bottom - top - unit), TRUE);
    const int button_width = unit * 9;
    MoveWindow(GetDlgItem(hwnd, 402), area.right - 2 * button_width - 2 * unit, bottom, button_width, button_height, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDOK), area.right - button_width - unit, bottom, button_width, button_height, TRUE);
}
bool copy_all(HWND hwnd) {
    const auto edit = GetDlgItem(hwnd, 401);
    const int length = GetWindowTextLengthW(edit);
    const auto memory = GlobalAlloc(GMEM_MOVEABLE, (static_cast<size_t>(length) + 1) * sizeof(wchar_t));
    if (!memory) return false;
    const auto text = static_cast<wchar_t *>(GlobalLock(memory));
    if (!text) { GlobalFree(memory); return false; }
    GetWindowTextW(edit, text, length + 1);
    GlobalUnlock(memory);
    if (!OpenClipboard(hwnd)) { GlobalFree(memory); return false; }
    const bool ok = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
    CloseClipboard();
    if (!ok) GlobalFree(memory);
    return ok;
}
}
std::wstring LicensesWindow::full_text() {
    std::wstring text;
    for (const auto &notice : desktop::embedded_notices()) {
        text += L"========== " + std::wstring(winrt::to_hstring(notice.name)) + L" ==========\r\n\r\n";
        const std::wstring original(winrt::to_hstring(notice.text));
        for (size_t i = 0; i < original.size(); ++i) {
            if (original[i] == L'\n' && (i == 0 || original[i - 1] != L'\r')) text += L'\r';
            text += original[i];
        }
        text += L"\r\n\r\n";
    }
    return text;
}
void LicensesWindow::create(HINSTANCE instance, HWND owner) {
    const auto text = full_text(); // Fail visibly if any resource is missing.
    hwnd_ = CreateDialogParamW(instance, MAKEINTRESOURCEW(400), owner, procedure, 0);
    if (!hwnd_) throw std::runtime_error("Cannot create third-party notices window");
    SendDlgItemMessageW(hwnd_, 401, EM_SETLIMITTEXT, text.size() + 1, 0);
    SetDlgItemTextW(hwnd_, 401, text.c_str());
    layout(hwnd_);
}
LicensesWindow::~LicensesWindow() { if (IsWindow(hwnd_)) DestroyWindow(hwnd_); }
void LicensesWindow::show() { ShowWindow(hwnd_, SW_SHOWNORMAL); SetForegroundWindow(hwnd_); SetFocus(GetDlgItem(hwnd_, 401)); }
void LicensesWindow::hide() { ShowWindow(hwnd_, SW_HIDE); }
bool LicensesWindow::pre_translate(MSG &message) {
    if (message.hwnd == GetDlgItem(hwnd_, 401) && message.message == WM_KEYDOWN &&
        message.wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        SendMessageW(message.hwnd, EM_SETSEL, 0, -1); return true;
    }
    return IsDialogMessageW(hwnd_, &message) != FALSE;
}
INT_PTR CALLBACK LicensesWindow::procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_INITDIALOG: return TRUE;
    case WM_SIZE: layout(hwnd); return TRUE;
    case WM_GETMINMAXINFO: {
        auto info = reinterpret_cast<MINMAXINFO *>(l);
        info->ptMinTrackSize = {MulDiv(480, GetDpiForWindow(hwnd), 96), MulDiv(320, GetDpiForWindow(hwnd), 96)};
        return TRUE;
    }
    case WM_DPICHANGED: {
        const auto area = reinterpret_cast<RECT *>(l);
        SetWindowPos(hwnd, nullptr, area->left, area->top, area->right - area->left, area->bottom - area->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) { ShowWindow(hwnd, SW_HIDE); return TRUE; }
        if (LOWORD(w) == 402 && HIWORD(w) == BN_CLICKED) {
            if (!copy_all(hwnd)) MessageBoxW(hwnd, L"无法复制，请稍后重试。", L"第三方许可", MB_ICONERROR);
            return TRUE;
        }
        break;
    case WM_CLOSE: ShowWindow(hwnd, SW_HIDE); return TRUE;
    }
    return FALSE;
}
} // namespace sonyconnect::ui
