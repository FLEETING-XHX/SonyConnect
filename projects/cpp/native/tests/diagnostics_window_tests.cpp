#include "ui_test_geometry.hpp"
#include "sonyconnect/more_window.hpp"
#include <commctrl.h>
#include <windowsx.h>
#include <winrt/base.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace ui = sonyconnect::ui;
namespace d = sonyconnect::desktop;
namespace {
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
HWND child(ui::MoreWindow &w, int id) {
    auto h = GetDlgItem(w.handle(), id); require(h != nullptr, "Missing diagnostic control"); return h;
}
std::wstring text(HWND h) {
    std::wstring value(GetWindowTextLengthW(h) + 1, L'\0');
    value.resize(GetWindowTextW(h, value.data(), static_cast<int>(value.size()))); return value;
}
void click(ui::MoreWindow &w, int id) {
    auto h = child(w, id); require(IsWindowEnabled(h), "Diagnostic action must be enabled");
    SendMessageW(h, BM_CLICK, 0, 0);
}
void pump(ui::MoreWindow &w, unsigned ms = 30) {
    auto until = GetTickCount64() + ms;
    do {
        MSG m{};
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) continue;
            if (!w.pre_translate(m)) { TranslateMessage(&m); DispatchMessageW(&m); }
        }
        if (GetTickCount64() < until) MsgWaitForMultipleObjectsEx(0, nullptr, 8, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < until);
}
unsigned mutations{};
LRESULT CALLBACK observe(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_SETTEXT || m == WM_ENABLE || m == WM_WINDOWPOSCHANGING) ++mutations;
    return DefSubclassProc(h, m, w, l);
}
struct Action { ui::MoreAction action; int value, detail; };
}
int wmain(int argc, wchar_t **argv) {
    const std::filesystem::path output = argc > 1 ? argv[1] : L"diagnostics-window-tests";
    std::filesystem::create_directories(output);
    std::vector<std::string> passed;
    std::string failure;
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_WIN95_CLASSES}; InitCommonControlsEx(&common);
        ui::MainWindow owner([](auto, int, bool, std::uint64_t) {});
        owner.create(GetModuleHandleW(nullptr), 96);
        std::vector<Action> actions;
        const auto dispatch = [&](auto a, int v, int detail, bool) { actions.push_back({a, v, detail}); };
        ui::MoreWindow window(dispatch, ui::SettingsPage::diagnostics);
        window.create(GetModuleHandleW(nullptr), owner.handle(), 96);
        ui::MoreModel model;
        model.diagnostics_available = true;
        model.diagnostic_status = L"诊断记录已启用";
        model.update_message = L"尚未检查更新";
        window.update(model); window.show(); pump(window);
        RECT r{}; GetWindowRect(window.handle(), &r);
        require(r.right - r.left == ui_test::pixel(680) && r.bottom - r.top == ui_test::pixel(592), "Pen42 must retain 680 by 592 DIP geometry");
        require(IsWindowEnabled(child(window, 903)) && IsWindowEnabled(child(window, 905)) && !model.device.ready,
                "Local diagnostic and update actions must not require a headphone");
        window.capture(output / L"01-diagnostics.png");
        passed.push_back("pen42_geometry_and_no_headphone_local_actions");

        SetWindowTextW(child(window, 900), L"8"); SetWindowTextW(child(window, 901), L"100");
        click(window, 902);
        require(actions.back().action == ui::MoreAction::diagnostic_limits && actions.back().value == 8 && actions.back().detail == 100,
                "Validated numeric input must route both exact limits");
        model.log_mib = 8; model.log_runs = 100; window.update(model);
        auto count = actions.size();
        SetWindowTextW(child(window, 900), L"0"); SetWindowTextW(child(window, 901), L"101"); click(window, 902);
        require(actions.size() == count && IsWindowVisible(child(window, 1090)) && text(child(window, 1090)).find(L"1–8") != std::wstring::npos,
                "Invalid limits must show inline error without a service call");
        window.capture(output / L"02-invalid-limits.png");
        passed.push_back("numeric_ranges_are_validated_before_dispatch");
        SetWindowTextW(child(window, 900), L"7"); SetWindowTextW(child(window, 901), L"99");
        SetFocus(child(window, 900)); SendMessageW(child(window, 900), EM_SETSEL, 0, -1);
        for (auto id : {900, 901, 902, 903, 904, 905, 906, 907}) SetWindowSubclass(child(window, id), observe, 1, 0);
        pump(window); mutations = 0;
        for (int i = 0; i < 40; ++i) { ++model.device.revision; model.device.link = i; window.update(model); }
        DWORD start{}, end{}; SendMessageW(child(window, 900), EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
        require(mutations == 0 && text(child(window, 900)) == L"7" && text(child(window, 901)) == L"99" && start == 0 && end == 1,
                "Background device updates must preserve numeric draft, selection and input identity");
        MSG enter{}; enter.hwnd = child(window, 900); enter.message = WM_KEYDOWN; enter.wParam = VK_RETURN;
        require(window.pre_translate(enter) && actions.back().value == 7 && actions.back().detail == 99,
                "Enter from native numeric input must apply once through pretranslation");
        SendMessageW(child(window, 900), EM_SETSEL, 0, -1);
        SendMessageW(child(window, 900), WM_CHAR, L'6', 0);
        require(text(child(window, 900)) == L"6", "Native typing must update the rendered numeric draft");
        require(window.pre_translate(enter) && actions.back().value == 6, "Typed value must be used by Enter apply");
        passed.push_back("numeric_drafts_and_selection_survive_refresh_and_enter_applies");
        click(window, 905); ++model.device.link; window.update(model); click(window, 1101);
        require(actions.back().action == ui::MoreAction::update_channel && actions.back().value == 1,
                "Update channel menu must remain local across device changes");
        model.update_channel = 1; model.update_checking = true; window.update(model);
        require(!IsWindowEnabled(child(window, 905)) && text(child(window, 906)) == L"取消检查", "Checking must lock channel and expose cancellation");
        click(window, 906); require(actions.back().action == ui::MoreAction::check_update, "Cancel must route update action");
        model.update_canceling = true; window.update(model);
        require(!IsWindowEnabled(child(window, 906)), "Canceling must prevent repeated cancellation submissions");
        model.update_checking = model.update_canceling = false;
        model.update_error = true; model.update_message = L"GitHub 匿名 API 请求已限流，请切换 Gitee。"; window.update(model);
        window.capture(output / L"03-update-error.png");
        passed.push_back("channel_menu_check_cancel_and_error_feedback");
        for (const auto [id, action] : {std::pair{903, ui::MoreAction::diagnostic_export},
                                       std::pair{904, ui::MoreAction::diagnostic_replay}, std::pair{907, ui::MoreAction::release_page}}) {
            click(window, id); require(actions.back().action == action, "Diagnostic buttons must preserve each existing service route");
        }
        model.busy = true; window.update(model);
        require(!IsWindowEnabled(child(window, 900)) && !IsWindowEnabled(child(window, 903)), "Busy file work must prevent duplicate limits and exports");
        model.busy = false; window.update(model); window.hide();
        passed.push_back("service_routes_and_duplicate_work_guard");

        ui::MoreWindow replay(dispatch, ui::SettingsPage::replay);
        replay.create(GetModuleHandleW(nullptr), owner.handle(), 96);
        replay.update(model); replay.show(); pump(replay);
        replay.capture(output / L"04-empty-replay.png");
        auto entries = std::make_shared<std::vector<d::DiagnosticReplayEntry>>(100000);
        for (std::size_t i = 0; i < entries->size(); ++i) {
            (*entries)[i].timestamp = "2026-10-18T01:02:03.004Z";
            (*entries)[i].entry.event = "event-" + std::to_string(i);
            (*entries)[i].entry.origin = i % 2 ? "Simulation" : "Device";
            (*entries)[i].entry.status = "fixture status";
        }
        (*entries)[2].entry.event = "command-failed"; (*entries)[2].entry.error_code = "Timeout";
        model.replay_entries = entries; model.replay_name = L"诊断文件 · 测试时间线.scdiag";
        replay.update(model); pump(replay);
        auto list = child(replay, 908); SetFocus(list);
        replay.capture(output / L"05-replay.png");
        MSG last{}; last.hwnd = list; last.message = WM_KEYDOWN; last.wParam = VK_END;
        require(replay.pre_translate(last) && text(list).find(L"event-99999") != std::wstring::npos,
                "Virtual timeline must reach the final event without creating per-event windows");
        int children{}; EnumChildWindows(replay.handle(), [](HWND, LPARAM p)->BOOL { ++*reinterpret_cast<int *>(p); return TRUE; }, reinterpret_cast<LPARAM>(&children));
        require(children < 10, "Large timeline must keep a bounded native child count");
        auto full = ui::replay_text((*entries)[2]);
        require(full.error && full.detail.find(L"Timeout") != std::wstring::npos && full.detail.find(L"2026-10-18") != std::wstring::npos,
                "Replay details must preserve actual time and error fields without inventing device state");
        d::DiagnosticReplayEntry normal;
        normal.entry.event = "confirmed"; normal.entry.error_code = "None";
        require(!ui::replay_text(normal).error, "Explicit no-error code must not color a confirmed event as failed");
        replay.capture(output / L"06-replay-end.png");
        passed.push_back("hundred_thousand_events_are_virtualized_with_complete_details");
        SetWindowSubclass(list, observe, 1, 0); mutations = 0;
        const auto selection = text(list);
        for (int i = 0; i < 40; ++i) { ++model.device.revision; replay.update(model); }
        require(mutations == 0 && text(list) == selection, "Live headphone refresh must not reset offline timeline selection");
        model.diagnostic_error = true; model.diagnostic_feedback = L"文件读取失败，保留当前时间线。"; replay.update(model);
        require(text(list) == selection && IsWindowVisible(child(replay, 1090)), "Failed file replacement must retain the visible selected event");
        replay.capture(output / L"07-replay-error.png");
        passed.push_back("replay_selection_survives_live_updates_and_failed_import");
        RECT list_bounds{}; GetClientRect(list, &list_bounds);
        SendMessageW(list, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(list_bounds.right - 6, 30));
        require(GetCapture() == list, "Timeline scrollbar must capture actual input"); replay.hide();
        require(GetCapture() != list, "Hiding replay must release scrollbar capture");
        passed.push_back("timeline_scrollbar_capture_is_canceled_on_hide");

        for (auto dpi : {120u, 144u, 192u}) for (auto page : {ui::SettingsPage::diagnostics, ui::SettingsPage::replay}) {
            ui::MoreWindow scaled(dispatch, page); scaled.create(GetModuleHandleW(nullptr), owner.handle(), dpi);
            scaled.update(model); scaled.show(); pump(scaled);
            GetWindowRect(scaled.handle(), &r);
            require(r.right - r.left == ui_test::pixel(680, dpi), "Diagnostic pages must preserve DIP width across normal DPI");
            SendMessageW(scaled.handle(), WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-30 * WHEEL_DELTA)), 0); pump(scaled);
            require(IsWindowVisible(child(scaled, 1000)), "High DPI diagnostic pages must preserve fixed close input");
            scaled.capture(output / (std::to_wstring(static_cast<int>(page)) + L"-dpi-" + std::to_wstring(dpi) + L".png")); scaled.hide();
        }
        passed.push_back("diagnostics_and_replay_125_150_200_percent_scroll_and_close");
    } catch (const winrt::hresult_error &e) { failure = winrt::to_string(e.message()); }
      catch (const std::exception &e) { failure = e.what(); }
    std::ofstream report(output / L"diagnostics-window-report.json");
    report << "{\"HardwareVerified\":false,\"NetworkCalled\":false,\"LegacyWindowShown\":false,\"passed\":" << passed.size() << ",\"scenarios\":[";
    for (std::size_t i = 0; i < passed.size(); ++i) report << (i ? "," : "") << "\"" << passed[i] << "\"";
    report << "],\"failure\":\"" << failure << "\"}";
    std::cout << passed.size() << " diagnostic UI scenarios passed; " << failure << '\n';
    return failure.empty() ? 0 : 1;
}
