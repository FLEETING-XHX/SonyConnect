#include "ui_test_geometry.hpp"
#include "sonyconnect/more_window.hpp"
#include "sonyconnect/ui_layout.hpp"
#include <commctrl.h>
#include <windowsx.h>
#include <winrt/base.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace ui = sonyconnect::ui;
namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
RECT bounds(HWND h) { RECT r{}; GetWindowRect(h, &r); return r; }
HWND child(HWND h, int id) {
    auto result = GetDlgItem(h, id);
    require(result != nullptr, "Missing acceptance input");
    return result;
}
void pump(unsigned ms = 30) {
    const auto until = GetTickCount64() + ms;
    do {
        MSG m{};
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) continue;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (GetTickCount64() < until) MsgWaitForMultipleObjectsEx(0, nullptr, 8, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < until);
}
HWND tooltip(HWND owner) {
    struct Find { HWND owner, result{}; } find{owner};
    EnumThreadWindows(GetCurrentThreadId(), [](HWND h, LPARAM data) -> BOOL {
        auto &f = *reinterpret_cast<Find *>(data);
        wchar_t name[80]{};
        GetClassNameW(h, name, 80);
        if (GetWindow(h, GW_OWNER) == f.owner && std::wstring(name) == TOOLTIPS_CLASSW) f.result = h;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&find));
    require(find.result != nullptr, "Full text tooltip must belong to its page");
    return find.result;
}
std::wstring full_text(HWND owner, HWND tool) {
    NMTTDISPINFOW info{};
    info.hdr.hwndFrom = tooltip(owner);
    info.hdr.idFrom = reinterpret_cast<UINT_PTR>(tool);
    info.hdr.code = TTN_GETDISPINFOW;
    SendMessageW(owner, WM_NOTIFY, 0, reinterpret_cast<LPARAM>(&info));
    return info.lpszText ? info.lpszText : L"";
}
void click(HWND h, int id) { SendMessageW(child(h, id), BM_CLICK, 0, 0); }
void key(ui::MoreWindow &w, WPARAM key) {
    MSG m{};
    m.hwnd = GetFocus(); m.message = WM_KEYDOWN; m.wParam = key;
    if (!w.pre_translate(m)) SendMessageW(m.hwnd, m.message, m.wParam, 0);
}
struct MainEvent { ui::MainAction action; int value; bool final; };
LRESULT CALLBACK count_timers(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
    if (m == WM_TIMER) ++*reinterpret_cast<unsigned *>(data);
    return DefSubclassProc(h, m, w, l);
}
}
int wmain(int argc, wchar_t **argv) {
    const std::filesystem::path output = argc > 1 ? argv[1] : L"ui-acceptance-tests";
    std::filesystem::create_directories(output);
    std::vector<std::string> checks;
    std::string failure;
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        INITCOMMONCONTROLSEX init{sizeof(init), ICC_WIN95_CLASSES}; InitCommonControlsEx(&init);
        for (auto work : {ui::DesktopRect{0, 40, 1920, 1080}, ui::DesktopRect{48, 0, 1920, 1040},
                          ui::DesktopRect{-2560, -1440, 0, -40}, ui::DesktopRect{1920, -300, 4480, 1100}}) {
            for (int dpi : {120, 144, 168, 192}) {
                const int width = MulDiv(680, dpi, 96), height = std::min(MulDiv(772, dpi, 96), work.height() - 16);
                const auto r = ui::anchor_settings(work, {work.left + 10, work.top + 10, work.left + 470, work.top + 700}, width, height, 16);
                require(r.left >= work.left && r.right <= work.right && r.top >= work.top && r.bottom <= work.bottom,
                        "Negative coordinates / taskbar workareas must contain anchored page");
                const auto moved = ui::fit_desktop(work, work.right + 100, work.top - 100, width, height);
                require(moved.right == work.right && moved.top == work.top, "Screen change must recover offscreen page");
            }
        }
        checks.push_back("workareas_negative_coordinates_taskbar_edges_125_to_200_percent");
        for (const auto dpi : {120u, 144u, 168u, 192u}) {
            for (const int available : {640, 1024, 1334, 1920}) {
                const auto scale = ui::fit_window_scale(dpi, 680, available);
                require(std::lround(680 * scale) <= available && scale <= dpi / 96.f,
                        "Constrained width must fit without increasing the native scale");
                if (680 * (dpi / 96.f) <= available)
                    require(scale == dpi / 96.f, "Normal displays must preserve their requested DPI");
            }
        }
        checks.push_back("constrained_width_preserves_normal_dpi_and_fits_small_workareas");
        for (float height : {180.f, 410.f, 772.f}) {
            for (float y : {60.f, height / 2, height - 50}) {
                const auto p = ui::place_popup(350, y, 260, 36, 416, height, 12);
                require(p.left >= 8 && p.left + p.width <= 408 && p.top >= 48 && p.top + p.rows * 32 <= height - 8,
                        "Popup rows and edges must remain inside its viewport");
            }
        }
        const auto up = ui::place_popup(252, 304, 140, 36, 416, 410, 2);
        require(up.above && up.top + up.rows * 32 < 304, "Bottom choice must expand above its anchor");
        checks.push_back("popup_direction_reduces_rows_without_covering_anchor");

        std::vector<MainEvent> events;
        unsigned main_timers{};
        ui::MainWindow main([&](auto a, int v, bool f, std::uint64_t) { events.push_back({a, v, f}); });
        main.create(GetModuleHandleW(nullptr), 120);
        SetWindowSubclass(main.handle(), count_timers, 1, reinterpret_cast<DWORD_PTR>(&main_timers));
        auto model = ui::main_preview(L"ambient"); model.preview = false;
        model.local.media.title = L"这是一段用于查看完整名称的长歌曲标题 - SC-24 Long media title with spaces and Unicode 音乐";
        main.update(model); main.show(); pump(350);
        require(SetWindowPos(main.handle(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE),
                "Input fixture must be unobscured");
        require(full_text(main.handle(), child(main.handle(), 33)).find(model.local.media.title) != std::wstring::npos,
                "Full media tooltip must preserve complete Unicode title");
        require(full_text(main.handle(), child(main.handle(), 10)).find(L"WF-1000XM5") != std::wstring::npos,
                "Device name must be available without ellipsis");
        main.capture(output / L"01-main-125.png");
        checks.push_back("main_125_percent_and_full_media_device_names");

        auto volume = child(main.handle(), 30);
        SetWindowPos(main.handle(), HWND_TOPMOST, 100, 100, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        pump(30);
        const auto r = bounds(volume);
        const POINT hit_point{(r.left+r.right)/2, (r.top+r.bottom)/2};
        const auto hit = WindowFromPoint(hit_point);
        wchar_t hit_name[80]{}; GetClassNameW(hit, hit_name, 80);
        std::wcout << L"Wheel input: point=" << hit_point.x << L"," << hit_point.y << L" hit=" << hit_name
                   << L" belongs=" << (hit == main.handle() || IsChild(main.handle(), hit))
                   << L" enabled=" << IsWindowEnabled(volume) << L" visible=" << IsWindowVisible(volume)
                   << L" ownerEnabled=" << IsWindowEnabled(main.handle()) << L" iconic=" << IsIconic(main.handle())
                   << L" topmost=" << ((GetWindowLongPtrW(main.handle(), GWL_EXSTYLE) & WS_EX_TOPMOST) != 0)
                   << L" value=" << SendMessageW(volume, TBM_GETPOS, 0, 0) << L'\n';
        events.clear();
        SendMessageW(main.handle(), WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM((r.left+r.right)/2, (r.top+r.bottom)/2));
        require(std::any_of(events.begin(), events.end(), [](auto &e) { return e.action == ui::MainAction::volume && !e.final; }),
                "Fixture must stage an actual wheel value before cancellation");
        main.hide(); pump(240); main.show(); pump(240);
        require(std::any_of(events.begin(), events.end(), [](auto &e) { return e.action == ui::MainAction::cancel_edit; }) &&
                std::none_of(events.begin(), events.end(), [](auto &e) { return e.action == ui::MainAction::volume && e.final; }),
                "Hidden window must not replay an unsent wheel edit on reopening");
        checks.push_back("main_hide_cancels_wheel_without_delayed_write_after_reopen");
        events.clear();
        SendMessageW(volume, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(ui_test::pixel(12), ui_test::pixel(12)));
        require(GetCapture() == volume, "Fixture must capture actual slider");
        SendMessageW(main.handle(), WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
        require(GetCapture() != volume, "Suspend must cancel active input capture");
        SendMessageW(main.handle(), WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0); pump(30);
        require(std::none_of(events.begin(), events.end(), [](auto &e) { return e.action == ui::MainAction::volume && e.final; }),
                "Suspend/resume must not commit canceled slider");
        checks.push_back("main_suspend_resume_cancels_drag_without_device_operations");
        click(main.handle(), 14);
        model.device.sound.mode.value = sonyconnect::protocol::SoundMode::NoiseCanceling;
        ++model.device.revision; main.update(model);
        main.hide(); main.show();
        require(!IsWindowEnabled(child(main.handle(), 15)), "Reopening must preserve the remaining 200ms mode cooldown");
        pump(220);
        require(IsWindowEnabled(child(main.handle(), 15)), "Preserved mode cooldown must unlock after its deadline");
        checks.push_back("main_cooldown_survives_hide_and_resumes_without_stuck_input");
        main.hide();
        pump(30); const auto hidden_timers = main_timers;
        ++model.device.revision; model.local.volume.percent = 67; main.update(model); pump(220);
        require(main_timers == hidden_timers, "Hidden main must not restart animation or wheel timers on state refresh");
        checks.push_back("main_hidden_refresh_leaves_no_ui_timer_running");

        for (auto page : {ui::SettingsPage::more, ui::SettingsPage::personalization, ui::SettingsPage::advanced}) {
            std::vector<ui::MoreAction> actions;
            unsigned page_timers{};
            ui::MoreWindow window([&](auto a, int, int, bool final) { if (final) actions.push_back(a); }, page);
            window.create(GetModuleHandleW(nullptr), main.handle(), 120);
            SetWindowSubclass(window.handle(), count_timers, 1, reinterpret_cast<DWORD_PTR>(&page_timers));
            auto value = ui::more_preview(); value.preview = false;
            window.update(value); window.show(); pump();
            auto box = bounds(window.handle());
            require(box.right - box.left == ui_test::pixel(page == ui::SettingsPage::personalization ? 416.f : 680.f, 120),
                    "All settings pages must honor 125 percent DIP width");
            window.capture(output / (std::to_wstring(static_cast<int>(page)) + L"-page-125.png"));
            const int anchor = page == ui::SettingsPage::more ? 1001 : page == ui::SettingsPage::personalization ? 1063 : 1065;
            if (page == ui::SettingsPage::advanced) {
                value.device.touch.assistant_writable = true;
                value.device.touch.voice_assistant.status = sonyconnect::ValueStatus::Known;
                value.device.touch.voice_assistant.value = 0xff;
                value.assistant_choices = {{0xff, L"不使用"}, {0x30, L"移动设备语音助手"}};
                window.update(value);
            }
            click(window.handle(), anchor);
            require(GetDlgItem(window.handle(), 1100) != nullptr, "Choice must open");
            key(window, VK_END);
            require(GetFocus() && GetDlgCtrlID(GetFocus()) >= 1100, "End must keep keyboard focus in popup");
            key(window, VK_HOME); key(window, VK_NEXT); key(window, VK_PRIOR); key(window, VK_ESCAPE);
            require(!GetDlgItem(window.handle(), 1100) && IsWindowVisible(window.handle()) && GetFocus() == child(window.handle(), anchor),
                    "Escape must close menu and restore its anchor focus before closing page");
            click(window.handle(), anchor);
            const auto anchor_box = bounds(child(window.handle(), anchor));
            if (page == ui::SettingsPage::personalization)
                require(bounds(child(window.handle(), 1101)).bottom <= anchor_box.top, "Last personal choice must open upward");
            const auto tooltip_window = tooltip(window.handle());
            require(SendMessageW(tooltip_window, TTM_GETTOOLCOUNT, 0, 0) > 0, "Native full-name tools must be registered");
            for (int i = 0; i < 8; ++i) { ++value.device.revision; window.update(value); }
            require(GetDlgItem(window.handle(), 1100), "Unrelated state refresh must preserve active popup");
            if (page != ui::SettingsPage::personalization) {
                value.device.ready = false;
                window.update(value);
                require(!GetDlgItem(window.handle(), 1100), "Unknown/offline capability must cancel old device choices");
                value.device.ready = true; window.update(value); click(window.handle(), anchor);
            }
            SendMessageW(window.handle(), WM_ACTIVATE, WA_INACTIVE, 0);
            require(!GetDlgItem(window.handle(), 1100) && actions.empty(), "Focus loss must cancel menu without selecting");
            click(window.handle(), anchor);
            SendMessageW(window.handle(), WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
            require(!GetDlgItem(window.handle(), 1100), "Suspend must dismiss menu");
            SendMessageW(window.handle(), WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
            click(window.handle(), anchor);
            require(GetDlgItem(window.handle(), 1100), "Menu must work after resume");
            SendMessageW(window.handle(), WM_DISPLAYCHANGE, 0, 0);
            require(!GetDlgItem(window.handle(), 1100), "Display change must discard old popup geometry");
            click(window.handle(), anchor); window.hide(); pump(180); window.show(); pump();
            require(!GetDlgItem(window.handle(), 1100) && IsWindowVisible(window.handle()) && actions.empty(),
                    "Hide/reopen must not replay popup selection");
            checks.push_back("page_" + std::to_string(static_cast<int>(page)) + "_125_keyboard_focus_popup_suspend_display_reopen");
            if (page == ui::SettingsPage::more) {
                auto prompt = child(window.handle(), 1041);
                SendMessageW(prompt, WM_KEYDOWN, VK_RIGHT, 0);
                window.hide(); window.show();
                require(!IsWindowEnabled(prompt), "Reopening must preserve the remaining 150ms prompt cooldown");
                pump(170);
                require(IsWindowEnabled(prompt), "Prompt cooldown must resume and unlock after reopening");
                checks.push_back("prompt_cooldown_survives_hide_and_resumes_without_stuck_input");
            }
            window.hide();
            pump(30); const auto stopped = page_timers;
            for (int i = 0; i < 8; ++i) { ++value.device.revision; value.device.battery.left.value = i; window.update(value); }
            pump(180);
            require(page_timers == stopped, "Hidden settings must not retain timers after background refresh");
        }
        pump(30);
        const auto users_before = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
        const auto gdi_before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        for (int i = 0; i < 12; ++i) {
            ui::MoreWindow temporary([](auto, int, int, bool) {}, static_cast<ui::SettingsPage>(i % 3));
            temporary.create(GetModuleHandleW(nullptr), main.handle(), 120);
            temporary.update(ui::more_preview());
            temporary.hide();
        }
        pump(30);
        require(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) <= users_before + 2 &&
                GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= gdi_before + 2,
                "Repeated page destruction must release native inputs, tooltips and drawing resources");
        checks.push_back("repeated_settings_destruction_releases_user_and_gdi_resources");
        {
            // Force a logical workarea narrower than the two-column page on
            // this monitor, without changing the user's display configuration.
            ui::MoreWindow narrow([](auto, int, int, bool) {});
            narrow.create(GetModuleHandleW(nullptr), main.handle(), 480);
            narrow.update(ui::more_preview()); narrow.show(); pump();
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfoW(MonitorFromWindow(narrow.handle(), MONITOR_DEFAULTTONEAREST), &monitor);
            const auto narrow_bounds = bounds(narrow.handle());
            require(narrow_bounds.left >= monitor.rcWork.left && narrow_bounds.right <= monitor.rcWork.right &&
                    narrow_bounds.top >= monitor.rcWork.top && narrow_bounds.bottom <= monitor.rcWork.bottom,
                    "Narrow logical workarea must contain the complete settings window");
            SendMessageW(narrow.handle(), WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-30 * WHEEL_DELTA)), 0);
            pump();
            require(IsWindowVisible(child(narrow.handle(), 1000)) && IsWindowVisible(child(narrow.handle(), 1054)),
                    "Width fitting must preserve fixed header and reachable footer");
            narrow.capture(output / L"narrow-workarea.png"); narrow.hide();
        }
        checks.push_back("narrow_workarea_actual_window_fit_and_scroll_reaches_footer");
        checks.push_back("fixtures_construct_no_controller_no_network_no_legacy_ui");
    } catch (const winrt::hresult_error &e) { failure = winrt::to_string(e.message()); }
      catch (const std::exception &e) { failure = e.what(); }
    std::ofstream report(output / L"acceptance-report.json");
    report << "{\"HardwareVerified\":false,\"PhysicalMultiMonitorVerified\":false,\"Passed\":" << (failure.empty() ? "true" : "false") << ",\"checks\":[";
    for (std::size_t i = 0; i < checks.size(); ++i) report << (i ? "," : "") << "\"" << checks[i] << "\"";
    report << "],\"failure\":\"" << failure << "\"}";
    std::cout << checks.size() << " UI acceptance scenarios passed; " << failure << '\n';
    return failure.empty() ? 0 : 1;
}
