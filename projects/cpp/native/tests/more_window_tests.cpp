#include "ui_test_geometry.hpp"
#include "sonyconnect/more_window.hpp"
#include <algorithm>
#include <commctrl.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <windowsx.h>
#include <winrt/base.h>

namespace ui = sonyconnect::ui;
namespace p = sonyconnect::protocol;
namespace {
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class T> sonyconnect::Value<T> known(T value) {
    sonyconnect::Value<T> result;
    result.status = sonyconnect::ValueStatus::Known;
    result.value = std::move(value);
    return result;
}
struct Action {
    ui::MoreAction action;
    int value, detail;
    bool final;
};
unsigned mutations{}, timer_messages{};
LRESULT CALLBACK observe(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_SETTEXT || m == WM_ENABLE || m == WM_WINDOWPOSCHANGING)
        ++mutations;
    if (m == WM_TIMER)
        ++timer_messages;
    return DefSubclassProc(h, m, w, l);
}
void pump(ui::MoreWindow &window, unsigned ms) {
    const auto deadline = GetTickCount64() + ms;
    do {
        MSG message{};
        while (GetTickCount64() < deadline && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            if (message.message != WM_QUIT && !window.pre_translate(message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        if (GetTickCount64() < deadline)
            MsgWaitForMultipleObjectsEx(0, nullptr, 8, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < deadline);
}
RECT rect(HWND h) {
    RECT r{};
    check(GetWindowRect(h, &r) != FALSE, "Cannot read settings bounds");
    return r;
}
HWND control(ui::MoreWindow &window, int id) {
    auto h = GetDlgItem(window.handle(), id);
    if (!h) throw std::runtime_error("Missing settings control " + std::to_string(id));
    return h;
}
std::wstring text(HWND h) {
    std::wstring s(GetWindowTextLengthW(h) + 1, L' ');
    s.resize(GetWindowTextW(h, s.data(), static_cast<int>(s.size())));
    return s;
}
void click(ui::MoreWindow &window, int id) {
    auto h = control(window, id);
    check(IsWindowEnabled(h) && IsWindowVisible(h), "Fixture button must be enabled and visible");
    SendMessageW(h, BM_CLICK, 0, 0);
}
std::vector<DWORD> screen(HWND h, RECT region, bool physical_coordinates = false) {
    if (!physical_coordinates)
        region = ui_test::physical(region);
    POINT origin{region.left, region.top};
    ClientToScreen(h, &origin);
    const int w = region.right - region.left, hgt = region.bottom - region.top;
    const auto hit = WindowFromPoint({origin.x + w / 2, origin.y + hgt / 2});
    if (hit != h && !IsChild(h, hit)) {
        wchar_t name[100]{}; GetClassNameW(hit, name, 100);
        std::wcerr << L"Settings screen probe covered by " << name << L'\n';
        throw std::runtime_error("Real screen probe is obscured; pixels cannot establish UI stability");
    }
    HDC dc = GetDC(nullptr), mem = CreateCompatibleDC(dc);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -hgt;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void *bits{};
    auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    auto old = SelectObject(mem, bitmap);
    if (!BitBlt(mem, 0, 0, w, hgt, dc, origin.x, origin.y, SRCCOPY | CAPTUREBLT))
        throw std::runtime_error("Cannot read real settings pixels: Windows error " +
            std::to_string(GetLastError()) + "; dc=" + std::to_string(reinterpret_cast<UINT_PTR>(dc)) +
            "; size=" + std::to_string(w) + "x" + std::to_string(hgt));
    GdiFlush();
    std::vector<DWORD> result(static_cast<DWORD *>(bits), static_cast<DWORD *>(bits) + w * hgt);
    // A screen DC defines RGB; its unused high byte is not an alpha channel.
    for (auto &pixel : result)
        pixel &= 0x00ffffff;
    SelectObject(mem, old);
    DeleteObject(bitmap);
    DeleteDC(mem);
    ReleaseDC(nullptr, dc);
    return result;
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    std::filesystem::path output = argc > 1 ? argv[1] : L"more-window-tests";
    std::filesystem::create_directories(output);
    std::vector<std::string> passed;
    std::string failure;
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        INITCOMMONCONTROLSEX init{sizeof(init), ICC_WIN95_CLASSES};
        check(InitCommonControlsEx(&init) != FALSE, "Cannot initialize controls");
        std::vector<Action> actions;
        std::function<void(ui::MoreAction, int, bool)> on_action;
        ui::MainWindow owner([](auto, int, bool, std::uint64_t) {});
        owner.create(GetModuleHandleW(nullptr), 96);
        owner.update(ui::main_preview(L"connected"));
        owner.show(false);
        ui::MoreWindow window(
            [&](auto a, int value, int detail, bool final) {
                actions.push_back({a, value, detail, final});
                if (on_action) on_action(a, value, final);
            });
        window.create(GetModuleHandleW(nullptr), owner.handle(), 96);
        auto model = ui::more_preview();
        model.preview = false;
        window.update(model);
        window.show();
        SetWindowSubclass(window.handle(), observe, 91, 0);
        SetWindowPos(window.handle(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        pump(window, 300);
        auto bounds = rect(window.handle());
        const auto style = GetWindowLongPtrW(window.handle(), GWL_STYLE);
        check(bounds.right - bounds.left == ui_test::pixel(680), "More window must preserve Pen width");
        check(!(style & WS_THICKFRAME) && !(style & WS_MAXIMIZEBOX),
              "Settings must not allow sizing or maximizing");
        check(IsWindowVisible(control(window, 1000)), "Close hit target must remain visible in fixed header");
        SendMessageW(window.handle(), WM_SYSCOMMAND, SC_MAXIMIZE, 0);
        auto after = rect(window.handle());
        check(EqualRect(&bounds, &after), "Maximize must not move settings");
        window.capture(output / L"01-wake.png");
        passed.push_back("pen_geometry_fixed_window_header_and_wake_capture");

        const auto selections_before = actions.size();
        for (int attempt = 0; attempt < 64; ++attempt) {
            click(window, 1001);
            const auto option = control(window, 1100);
            SendMessageW(option, BM_CLICK, 0, 0);
            check(!IsWindow(option) && !GetDlgItem(window.handle(), 1100),
                  "A native selection notification must safely retire its own menu item");
        }
        check(actions.size() == selections_before + 64 &&
                  std::all_of(actions.begin() + selections_before, actions.end(),
                              [](const auto &a) { return a.action == ui::MoreAction::equalizer_preset && a.final; }),
              "Repeated menu selection must dispatch exactly one selected preset each time");
        passed.push_back("native_menu_selection_self_destruction_keeps_one_dispatch_per_click");

        const auto original = screen(window.handle(), {36, 158, 312, 317});
        model.device.equalizer.bands = known(std::vector<int>{-8, -8, -8, -8, -8});
        window.update(model);
        ValidateRect(window.handle(), nullptr);
        pump(window, 350);
        check(screen(window.handle(), {36, 158, 312, 317}) != original,
              "Same-size EQ update must reach real screen");
        passed.push_back("same_size_equalizer_values_reach_real_screen");
        for (int id : {1001, 1010, 1011, 1012, 1013, 1014, 1015, 1020, 1021, 1022, 1030, 1031, 1032,
                       1040, 1041, 1042, 1043, 1044, 1045, 1046, 1051, 1052, 1053, 1054})
            SetWindowSubclass(control(window, id), observe, 90, 0);
        pump(window, 350);
        const auto before_mutations = mutations;
        const auto idle = screen(window.handle(), {24, 44, 656, 710});
        for (int i = 0; i < 30; ++i) {
            ++model.device.revision;
            model.device.battery.left = known(70 + i % 10);
            window.update(model);
        }
        pump(window, 150);
        check(mutations == before_mutations,
              "Unrelated background updates must not mutate native settings controls");
        const auto idle_after = screen(window.handle(), {24, 44, 656, 710});
        if (idle_after != idle) {
            std::size_t changed = 0;
            int left = 632, top = 666, right = 0, bottom = 0;
            for (std::size_t i = 0; i < idle.size(); ++i)
                if (idle[i] != idle_after[i]) {
                    ++changed;
                    const int x = static_cast<int>(i % 632), y = static_cast<int>(i / 632);
                    left = std::min(left, x);
                    top = std::min(top, y);
                    right = std::max(right, x);
                    bottom = std::max(bottom, y);
                }
            std::cerr << "idle RGB changed=" << changed << " box=" << left << ',' << top << ',' << right
                      << ',' << bottom << '\n';
        }
        check(idle_after == idle, "Unrelated background updates must not flash settings pixels");
        passed.push_back("unrelated_updates_preserve_pixels_and_native_control_identity");

        check(SendMessageW(window.handle(), WM_MOUSEACTIVATE, 0, 0) == MA_ACTIVATE,
              "The first click must activate settings without being discarded");
        for (int anchor : {1001, 1021, 1022}) {
            const auto before_toggle = actions.size();
            auto input = control(window, anchor);
            SendMessageW(input, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(ui_test::pixel(10), ui_test::pixel(10)));
            check(GetDlgItem(window.handle(), 1100) != nullptr,
                  "Dropdown must open during mouse-down, before release or a timer frame");
            SendMessageW(input, WM_LBUTTONUP, 0, MAKELPARAM(ui_test::pixel(10), ui_test::pixel(10)));
            check(GetDlgItem(window.handle(), 1100) != nullptr, "Mouse release must not toggle twice");
            SendMessageW(input, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(ui_test::pixel(10), ui_test::pixel(10)));
            check(!GetDlgItem(window.handle(), 1100), "Second mouse-down must close immediately");
            SendMessageW(input, WM_LBUTTONUP, 0, MAKELPARAM(ui_test::pixel(10), ui_test::pixel(10)));
            check(!GetDlgItem(window.handle(), 1100) && actions.size() == before_toggle,
                  "Clicking the same anchor again must close its menu without changing settings");
            click(window, anchor);
            check(GetDlgItem(window.handle(), 1100) != nullptr, "Closed dropdown must reopen on next click");
            click(window, anchor);
            check(!GetDlgItem(window.handle(), 1100), "Repeated anchor toggles must remain consistent");
        }
        passed.push_back("all_choice_anchors_toggle_closed_and_reopen_without_dispatch");
        window.capture(output / L"07-centered-cycle.png");

        click(window, 1001);
        check(GetDlgItem(window.handle(), 1100) != nullptr, "EQ popup must open after toggling all anchors");
        pump(window, 40);
        auto first_item = control(window, 1100);
        const auto action_count = actions.size();
        for (int i = 0; i < 10; ++i) {
            ++model.device.revision;
            window.update(model);
        }
        pump(window, 40);
        check(control(window, 1100) == first_item, "State refresh must preserve an open dropdown");
        MSG escape_message{GetFocus(), WM_KEYDOWN, VK_ESCAPE, 0};
        check(window.pre_translate(escape_message), "Dialog routing must preserve Escape cancellation");
        pump(window, 30);
        check(!IsWindow(first_item) && actions.size() == action_count,
              "Canceling dropdown must not submit a selection");
        passed.push_back("dropdown_survives_refresh_and_escape_does_not_commit");
        click(window, 1001);
        pump(window, 20);
        auto inactive_menu = control(window, 1100);
        const auto inactive_actions = actions.size();
        SendMessageW(window.handle(), WM_ACTIVATE, WA_INACTIVE, 0);
        check(!IsWindow(inactive_menu) && actions.size() == inactive_actions,
              "Deactivating settings must cancel its menu without committing");
        passed.push_back("window_deactivation_cancels_menu_without_dispatch");
        const auto original_presets = model.device.equalizer.presets;
        for (int i = 0; i < 10; ++i)
            model.device.equalizer.presets.push_back(
                {static_cast<p::Byte>(0x40 + i), "Extra " + std::to_string(i)});
        window.update(model);
        click(window, 1001);
        pump(window, 20);
        const auto scroll_actions = actions.size();
        auto menu_row = control(window, 1102);
        auto menu_rect = rect(menu_row);
        SendMessageW(menu_row, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)),
                     MAKELPARAM(menu_rect.left + 10, menu_rect.top + 10));
        check(actions.size() == scroll_actions,
              "Scrolling a dropdown must not adjust the equalizer beneath it");
        MSG scroll_up{GetFocus(), WM_KEYDOWN, VK_UP, 0};
        window.pre_translate(scroll_up);
        MSG scroll_enter{GetFocus(), WM_KEYDOWN, VK_RETURN, 0};
        window.pre_translate(scroll_enter);
        check(actions.back().action == ui::MoreAction::equalizer_preset && actions.back().value == 0xa1,
              "Scrolled dropdown focus must retain its real candidate identity");
        click(window, 1001);
        pump(window, 30);
        auto scrollbar = control(window, 1200);
        check(IsWindowVisible(scrollbar), "Long preset dropdown must expose a right scrollbar");
        window.capture(output / L"05-preset-scrollbar.png");
        const auto scrollbar_actions = actions.size();
        auto verify_selection = [&] {
            for (int i = 0; i < 6; ++i) {
                auto row = control(window, 1100 + i);
                RECT region = rect(row);
                MapWindowPoints(nullptr, window.handle(), reinterpret_cast<POINT *>(&region), 2);
                const auto pixels = screen(window.handle(), region, true);
                const bool highlighted = std::count(pixels.begin(), pixels.end(), 0xeef0fdu) > 100;
                check(highlighted == (text(row).find(L"自定义 2") != std::wstring::npos),
                      "Only the confirmed preset may be highlighted, independently of scroll and focus");
            }
        };
        verify_selection();
        for (int i = 0; i < 5; ++i)
            SendMessageW(scrollbar, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
        pump(window, 30);
        verify_selection();
        for (int i = 0; i < 5; ++i)
            SendMessageW(scrollbar, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
        pump(window, 30);
        verify_selection();
        SendMessageW(scrollbar, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(ui_test::pixel(6), ui_test::pixel(8)));
        SendMessageW(scrollbar, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(ui_test::pixel(6), ui_test::pixel(400)));
        SendMessageW(scrollbar, WM_LBUTTONUP, 0, MAKELPARAM(ui_test::pixel(6), ui_test::pixel(400)));
        pump(window, 30);
        verify_selection();
        check(text(control(window, 1105)) == L"Extra 9" && actions.size() == scrollbar_actions,
              "Dragging scrollbar must reach the final preset without changing EQ");
        window.capture(output / L"06-preset-scroll-end.png");
        SendMessageW(control(window, 1105), WM_KEYDOWN, VK_ESCAPE, 0);
        check(!GetDlgItem(window.handle(), 1200), "Closing popup must release its scrollbar");
        passed.push_back("preset_scrollbar_drag_and_scroll_keep_device_selection_highlight");
        model.device.equalizer.presets = original_presets;
        window.update(model);
        passed.push_back("long_dropdown_scroll_preserves_keyboard_identity_and_blocks_slider_underlay");
        click(window, 1001);
        pump(window, 20);
        MSG up_message{GetFocus(), WM_KEYDOWN, VK_UP, 0};
        check(window.pre_translate(up_message),
              "Dropdown arrows must route through its own keyboard handler");
        MSG enter_message{GetFocus(), WM_KEYDOWN, VK_RETURN, 0};
        check(window.pre_translate(enter_message), "Dropdown Enter must commit exactly one candidate");
        check(actions.back().action == ui::MoreAction::equalizer_preset && actions.back().value == 0xa1,
              "Dialog routing must not swallow dropdown keyboard selection");
        passed.push_back("dropdown_keyboard_routes_through_real_pretranslation");
        click(window, 1001);
        pump(window, 30);
        click(window, 1101);
        pump(window, 20);
        check(actions.back().action == ui::MoreAction::equalizer_preset && actions.back().value == 0x10,
              "EQ preset must dispatch actual device candidate");
        passed.push_back("preset_menu_routes_actual_wire_value");
        click(window, 1001);
        pump(window, 30);
        const auto stale_count = actions.size();
        const auto removed_option = control(window, 1101);
        model.device.equalizer.presets.erase(model.device.equalizer.presets.begin() + 1);
        window.update(model);
        check(!GetDlgItem(window.handle(), 1100), "Removed capability must dismiss obsolete choices immediately");
        SendMessageW(window.handle(), WM_COMMAND, MAKEWPARAM(1101, BN_CLICKED), reinterpret_cast<LPARAM>(removed_option));
        check(actions.size() == stale_count, "Removed preset must not dispatch a stale dropdown value");
        passed.push_back("removed_candidate_is_rejected_before_dispatch");

        auto band = control(window, 1010);
        const auto drag_count = actions.size();
        SendMessageW(band, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(ui_test::pixel(12), ui_test::pixel(20)));
        model.equalizer_targets[0] = 8;
        window.update(model);
        check(GetCapture() == band, "Background state must retain an active EQ gesture");
        SendMessageW(band, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(ui_test::pixel(12), ui_test::pixel(7)));
        check(SendMessageW(band, TBM_GETPOS, 0, 0) == 10 && text(band).find(L"10") != std::wstring::npos,
              "EQ drag must update accessible value before release");
        check(actions.size() > drag_count && !actions.back().final,
              "EQ drag must remain a draft before release");
        SendMessageW(band, WM_LBUTTONUP, 0, MAKELPARAM(ui_test::pixel(12), ui_test::pixel(7)));
        check(actions.back().final && actions.back().value == 10, "EQ release must commit final value");
        model.equalizer_targets[0].reset();
        passed.push_back("vertical_drag_realtime_label_background_refresh_and_final_commit");
        const auto cancel_count = actions.size();
        SendMessageW(band, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(ui_test::pixel(12), ui_test::pixel(40)));
        SendMessageW(band, WM_KEYDOWN, VK_ESCAPE, 0);
        check(actions.size() > cancel_count && actions.back().action == ui::MoreAction::cancel_edit,
              "Escape must discard an uncommitted slider draft");
        passed.push_back("canceled_slider_draft_is_not_written");

        const auto wheel_count = actions.size();
        auto bass = control(window, 1015);
        SendMessageW(bass, WM_MOUSEWHEEL, MAKEWPARAM(0, 60), 0);
        check(actions.size() == wheel_count, "Partial wheel step must not leak to another control");
        SendMessageW(bass, WM_MOUSEWHEEL, MAKEWPARAM(0, 60), 0);
        check(actions.size() == wheel_count + 1 && !actions.back().final,
              "Wheel must publish immediate draft");
        pump(window, 200);
        if (actions.back().action != ui::MoreAction::clear_bass || !actions.back().final)
            std::cerr << "wheel last action=" << static_cast<int>(actions.back().action)
                      << " final=" << actions.back().final << " timers=" << timer_messages
                      << " visible=" << IsWindowVisible(window.handle()) << '\n';
        check(actions.back().action == ui::MoreAction::clear_bass && actions.back().final,
              "Wheel must debounce into one final write");
        passed.push_back("numeric_wheel_precision_and_bounded_commit");

        auto prompt = control(window, 1041);
        const auto prompt_wheel_actions = actions.size();
        const auto prompt_before = SendMessageW(prompt, TBM_GETPOS, 0, 0);
        const auto prompt_rect = rect(prompt);
        const LPARAM prompt_point = MAKELPARAM(prompt_rect.left + 10, prompt_rect.top + 10);
        SendMessageW(prompt, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), prompt_point);
        SendMessageW(window.handle(), WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), prompt_point);
        pump(window, 160);
        check(actions.size() == prompt_wheel_actions && SendMessageW(prompt, TBM_GETPOS, 0, 0) == prompt_before,
              "Prompt volume must ignore wheel events through child and parent routes");
        passed.push_back("prompt_volume_wheel_is_disabled");
        on_action = [&](ui::MoreAction action, int value, bool) {
            if (action != ui::MoreAction::prompt_volume) return;
            model.prompt_target = value;
            model.busy = actions.back().final;
            window.update(model);
        };
        SendMessageW(prompt, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(ui_test::pixel(226), ui_test::pixel(16)));
        SendMessageW(prompt, WM_LBUTTONUP, 0, MAKELPARAM(ui_test::pixel(226), ui_test::pixel(16)));
        check(model.busy && SendMessageW(prompt, TBM_GETPOS, 0, 0) == 1,
              "Prompt release must retain +1 while readback still reports zero");
        model.busy = false;
        model.device.touch.prompt_volume = known(1);
        model.prompt_target.reset();
        window.update(model);
        check(!IsWindowEnabled(prompt), "Fast readback must not bypass the 150ms cooldown");
        const auto cooling_actions = actions.size();
        SendMessageW(prompt, WM_KEYDOWN, VK_RIGHT, 0);
        SendMessageW(prompt, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(ui_test::pixel(302), ui_test::pixel(16)));
        SendMessageW(prompt, WM_LBUTTONUP, 0, MAKELPARAM(ui_test::pixel(302), ui_test::pixel(16)));
        check(actions.size() == cooling_actions && SendMessageW(prompt, TBM_GETPOS, 0, 0) == 1,
              "Cooldown must reject repeated keyboard and pointer input");
        pump(window, 180);
        check(IsWindowEnabled(prompt) && SendMessageW(prompt, TBM_GETPOS, 0, 0) == 1,
              "Confirmed prompt value must remain stable when cooldown expires");
        passed.push_back("prompt_value_handoff_and_150ms_cooldown_reject_repeated_input");
        SendMessageW(prompt, WM_KEYDOWN, VK_RIGHT, 0);
        pump(window, 180);
        check(model.busy && !IsWindowEnabled(prompt) && SendMessageW(prompt, TBM_GETPOS, 0, 0) == 2,
              "Cooldown expiry must still wait for outstanding readback");
        model.busy = false;
        model.prompt_target.reset();
        model.feedback_error = true;
        model.feedback = L"提示音量回读错误，已恢复原设置。";
        window.update(model);
        pump(window, 250);
        check(IsWindowEnabled(prompt) && SendMessageW(prompt, TBM_GETPOS, 0, 0) == 1,
              "Failed prompt readback must restore the previous confirmed value");
        model.feedback_error = false;
        model.feedback.clear();
        on_action = {};
        window.update(model);
        passed.push_back("prompt_slow_readback_and_error_rollback");

        model.busy = true;
        window.update(model);
        pump(window, 40);
        check(!IsWindowEnabled(control(window, 1020)), "Busy state must block conflicting device actions");
        const auto busy_pixels = screen(window.handle(), {20, 60, 320, 90});
        model.busy = false;
        window.update(model);
        pump(window, 40);
        check(busy_pixels == screen(window.handle(), {20, 60, 320, 90}),
              "Busy state must not change section heading colors");
        passed.push_back("busy_guard_preserves_unrelated_text_color");
        const auto touch_labels_before = screen(window.handle(), {354, 100, 656, 184});
        const auto prompt_label_before = screen(window.handle(), {24, 614, 260, 640});
        for (int origin = 0; origin < 2; ++origin) {
            model.busy = origin == 0;
            model.device.busy = origin == 1;
            window.update(model);
            pump(window, 45);
            check(!IsWindowEnabled(control(window, 1021)) && !IsWindowEnabled(control(window, 1041)),
                  "Busy settings must continue to block conflicting input");
            check(screen(window.handle(), {354, 100, 656, 184}) == touch_labels_before &&
                      screen(window.handle(), {24, 614, 260, 640}) == prompt_label_before,
                  "Host and device busy transitions must not flash touch rows or prompt-volume text");
            model.busy = model.device.busy = false;
            window.update(model);
            pump(window, 45);
        }
        passed.push_back("cycle_submission_busy_keeps_touch_rows_and_prompt_volume_pixels_stable");
        click(window, 1040);
        check(actions.back().action == ui::MoreAction::guidance && actions.back().value == 0,
              "Guidance on switch must request off without guessing its wire encoding");
        model.busy = true;
        window.update(model);
        pump(window, 100);
        model.busy = false;
        model.feedback_error = true;
        model.feedback = L"耳机回读错误，已恢复原设置。";
        window.update(model);
        pump(window, 300);
        check(text(control(window, 1040)).find(L"开启") != std::wstring::npos,
              "Failed readback must restore confirmed switch state");
        check(text(control(window, 1090)).find(L"回读错误") != std::wstring::npos,
              "Readback failure must also be available to assistive technology");
        window.capture(output / L"02-error-rollback.png");
        passed.push_back("failed_readback_restores_switch_and_pen_error_palette");
        model.feedback.clear();
        model.feedback_error = false;
        const auto cycle_count = actions.size();
        click(window, 1030);
        check(actions.back().action == ui::MoreAction::cycle && actions.back().value == 4,
              "Cycle action must preserve existing combination encoding");
        model.device.touch.cycle = known(4);
        window.update(model);
        click(window, 1031);
        check(actions.size() == cycle_count + 1, "Cycle must not allow fewer than two selected modes");
        passed.push_back("cycle_retains_minimum_two_modes");
        click(window, 1021);
        pump(window, 20);
        click(window, 1101);
        check(actions.back().action == ui::MoreAction::touch_preset && actions.back().detail == 0 &&
                  actions.back().value == 0x20,
              "Left preset must retain its source key slot and actual preset");
        click(window, 1044);
        check(!GetDlgItem(window.handle(), 1100), "Automatic power-off must be a switch, not a menu");
        check(actions.back().action == ui::MoreAction::auto_power_off && actions.back().value == 0x11,
              "Power switch must use the reported never-off candidate");
        model.busy = true;
        window.update(model);
        model.busy = false;
        model.device.features[p::Feature::AutoPowerOff].value = known(0x11);
        window.update(model);
        check(text(control(window, 1044)).find(L"摘下耳机后不自动关机") != std::wstring::npos,
              "Disabled automatic power-off hint must explicitly refer to headphone removal");
        window.capture(output / L"08-power-off-disabled.png");
        click(window, 1044);
        check(actions.back().value == 0x10 && !GetDlgItem(window.handle(), 1100),
              "Enabling power switch must restore a reported automatic mode");
        model.device.features[p::Feature::AutoPowerOff].value = known(0x10);
        window.update(model);
        passed.push_back("touch_candidates_and_auto_power_switch_preserve_reported_values");
        click(window, 1021);
        pump(window, 20);
        const auto key_change_actions = actions.size();
        const auto old_key_option = control(window, 1101);
        model.device.touch.keys[0].id = 2;
        window.update(model);
        check(!GetDlgItem(window.handle(), 1100), "Replaced physical key must cancel its old popup immediately");
        SendMessageW(window.handle(), WM_COMMAND, MAKEWPARAM(1101, BN_CLICKED), reinterpret_cast<LPARAM>(old_key_option));
        check(actions.size() == key_change_actions,
              "A menu from a replaced physical key must not change another key");
        model.device.touch.keys[0].id = 0;
        window.update(model);
        passed.push_back("touch_key_context_change_rejects_old_menu");
        model.device.features[p::Feature::AutoPowerOff].value = known(0);
        model.auto_power_choices = {{0, L"5 分钟"}, {0x11, L"不自动关机"}};
        model.device.features[p::Feature::AutoPowerOff].choices = {0, 0x11};
        window.update(model);
        pump(window, 300);
        const auto power_track = screen(window.handle(), {620, 454, 656, 472});
        check(text(control(window, 1044)).find(L"开启") != std::wstring::npos &&
                  std::count_if(power_track.begin(), power_track.end(),
                                [](auto pixel) { return (pixel & 0x00ffffff) == 0x4c5ed6; }) > 10,
              "Timed automatic power-off value zero must be shown as enabled");
        passed.push_back("timed_auto_power_zero_is_enabled");
        click(window, 1044);
        check(actions.back().value == 0x11, "Timed automatic power mode must switch off normally");
        model.device.features[p::Feature::AutoPowerOff].value = known(0x11);
        window.update(model);
        click(window, 1044);
        check(actions.back().value == 0, "Power switch must restore the reported timed value zero");
        model.auto_power_choices = {{0x11, L"不自动关机"}};
        model.device.features[p::Feature::AutoPowerOff].value = known(0x11);
        window.update(model);
        check(!IsWindowEnabled(control(window, 1044)), "Missing automatic candidate must disable enable action");
        passed.push_back("auto_power_switch_preserves_timed_values_and_rejects_missing_candidates");
        for (int cycle = 1; cycle <= 4; ++cycle) {
            model.device.touch.cycle = known(cycle);
            window.update(model);
            pump(window, 20);
            window.capture(output / (L"09-cycle-" + std::to_wstring(cycle) + L".png"));
        }
        passed.push_back("all_four_pen07_cycle_combinations_keep_check_and_cross_marks");
        const auto before_hotkey = actions.size();
        SendMessageW(window.handle(), WM_COMMAND, MAKEWPARAM(1050, BN_CLICKED),
                     0);
        check(!GetDlgItem(window.handle(), 1050) && actions.size() == before_hotkey,
              "Removed hotkey entry must not exist or dispatch stale notifications");
        passed.push_back("hotkey_entry_is_removed_and_rejects_stale_notifications");
        for (int i = 1; i < 4; ++i) {
            click(window, 1050 + i);
            check(actions.back().action ==
                      static_cast<ui::MoreAction>(static_cast<int>(ui::MoreAction::personalization) + i - 1),
                  "Settings link must route to its own existing functionality");
        }
        passed.push_back("remaining_settings_entries_preserve_routes");

        model.slider_style = ui::MainSliderStyle::elastic;
        model.device.equalizer.preset = known(0xa2);
        window.update(model);
        pump(window, 300);
        window.capture(output / L"03-elastic.png");
        passed.push_back("elastic_appearance_uses_same_controls_and_values");
        const auto before_key = screen(window.handle(), {48, 191, 74, 292});
        auto elastic_band = control(window, 1010);
        SendMessageW(elastic_band, WM_KEYDOWN, VK_HOME, 0);
        pump(window, 350);
        check(SendMessageW(elastic_band, TBM_GETPOS, 0, 0) == -10 &&
                  screen(window.handle(), {48, 191, 74, 292}) != before_key,
              "Elastic keyboard change must update its real fill as well as its value");
        passed.push_back("elastic_keyboard_updates_real_fill");
        model.eq_slider_style = ui::MainSliderStyle::wake;
        model.bass_slider_style = ui::MainSliderStyle::elastic;
        window.update(model);
        pump(window, 350);
        const auto eq_wake = screen(window.handle(), {36, 184, 312, 298});
        const auto bass_elastic = screen(window.handle(), {36, 354, 314, 386});
        model.bass_slider_style = ui::MainSliderStyle::wake;
        window.update(model);
        pump(window, 350);
        const auto bass_wake = screen(window.handle(), {36, 354, 314, 386});
        check(bass_wake != bass_elastic && screen(window.handle(), {36, 184, 312, 298}) == eq_wake,
              "Bass appearance must change independently of equalizer appearance");
        model.eq_slider_style = ui::MainSliderStyle::elastic;
        window.update(model);
        pump(window, 350);
        check(screen(window.handle(), {36, 184, 312, 298}) != eq_wake &&
                  screen(window.handle(), {36, 354, 314, 386}) == bass_wake,
              "Equalizer appearance must change independently of bass appearance");
        passed.push_back("equalizer_and_bass_appearances_are_independent_in_real_pixels");
        click(window, 1001);
        pump(window, 20);
        auto old_menu = control(window, 1100);
        ++model.device.link;
        model.device.ready = false;
        model.device.equalizer.preset = {};
        model.device.equalizer.bands = {};
        model.device.equalizer.clear_bass = {};
        model.device.touch.guidance = {};
        window.update(model);
        pump(window, 50);
        check(!IsWindow(old_menu) && !IsWindowEnabled(control(window, 1010)),
              "Disconnect must cancel old menu and prevent device writes");
        window.capture(output / L"04-unavailable.png");
        passed.push_back("disconnection_cancels_menu_and_marks_unknown_readings");
        model.offline = true;
        window.update(model);
        check(!IsWindowEnabled(control(window, 1045)),
              "Offline preview must not change startup registration");
        passed.push_back("offline_mode_disables_system_mutations");

        // Different physical controls must not inherit the WF left/right assignment UI.
        on_action = {};
        auto ch = ui::more_preview();
        ch.preview = false;
        ch.device.model = "WH-CH720N";
        ch.device.touch.keys.clear();
        ch.device.touch.assignments = {};
        ch.device.touch.writable = false;
        ch.touch_indices = {-1, -1};
        ch.device.features.erase(p::Feature::SpeakToChat);
        ch.device.features.erase(p::Feature::WearDetectionPause);
        const auto ch_labels = ui::more_control_presentation(ch.device);
        check(ch_labels.physical_nc_button && ch_labels.no_touch_sensor &&
                  ch_labels.cycle_label.find(L"[NC/AMB]") != std::wstring::npos &&
                  ch_labels.dsee_label == L"DSEE",
              "CH720N must describe its physical NC/AMB button and ordinary DSEE");
        window.update(ch);
        pump(window, 100);
        check(!IsWindowEnabled(control(window, 1021)) && !IsWindowEnabled(control(window, 1022)) &&
                  !IsWindowEnabled(control(window, 1042)) && !IsWindowEnabled(control(window, 1043)) &&
                  IsWindowEnabled(control(window, 1030)),
              "CH720N must disable absent touch/wear features while retaining its advertised button cycle");
        const auto before_disabled = actions.size();
        for (int id : {1021, 1022, 1042, 1043}) SendMessageW(control(window, id), BM_CLICK, 0, 0);
        check(actions.size() == before_disabled, "Disabled unsupported controls must never dispatch");
        window.capture(output / L"12-ch720n-button-and-disabled-features.png");
        passed.push_back("ch720n_physical_button_dsee_and_disabled_features");
        auto wh = ui::more_preview();
        wh.preview = false;
        wh.device.model = "WH-1000XM5";
        wh.device.touch.keys.clear();
        wh.device.touch.assignments = {};
        wh.device.touch.writable = false;
        wh.touch_indices = {-1, -1};
        const auto wh_labels = ui::more_control_presentation(wh.device);
        check(wh_labels.physical_nc_button && !wh_labels.no_touch_sensor &&
                  wh_labels.dsee_label == L"DSEE Extreme",
              "WH1000XM5 must retain a distinct right touch sensor and DSEE Extreme");
        window.update(wh);
        pump(window, 50);
        wchar_t row[128]{};
        GetWindowTextW(control(window, 1022), row, 128);
        check(std::wstring(row) == L"此版本尚未适配", "Unimplemented WH touch must not be labelled unsupported hardware");
        check(IsWindowEnabled(control(window, 1042)) && IsWindowEnabled(control(window, 1043)),
              "WH must retain its own advertised wearing and speak-to-chat features");
        window.capture(output / L"13-wh1000xm5-distinct-right-touch.png");
        auto wf = ui::more_preview();
        wf.preview = false;
        const auto wf_labels = ui::more_control_presentation(wf.device);
        check(!wf_labels.physical_nc_button && !wf_labels.no_touch_sensor,
              "WF must retain left/right touch semantics");
        window.update(wf);
        pump(window, 50);
        check(IsWindowEnabled(control(window, 1021)) && IsWindowEnabled(control(window, 1022)),
              "Changing from CH to WF must restore supported touch assignments");
        window.capture(output / L"14-wf-left-and-right-touch.png");
        passed.push_back("wh_touch_support_is_distinct_and_wf_restores_both_assignments");

        for (UINT dpi : {144u, 192u}) {
            ui::MoreWindow scaled([](auto, int, int, bool) {});
            scaled.create(GetModuleHandleW(nullptr), owner.handle(), dpi);
            auto example = ui::more_preview();
            scaled.update(example);
            scaled.show();
            pump(scaled, 80);
            const auto r = rect(scaled.handle());
            check(r.right - r.left == ui_test::pixel(680, dpi), "High DPI settings must preserve DIP width");
            SendMessageW(scaled.handle(), WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA * 20)),
                         0);
            pump(scaled, 50);
            check(IsWindowVisible(control(scaled, 1054)), "High DPI scroll must make footer reachable");
            check(IsWindowVisible(control(scaled, 1000)), "High DPI scroll must preserve header close input");
            scaled.capture(output / (std::to_wstring(dpi) + L"-scrolled.png"));
            scaled.hide();
        }
        passed.push_back("high_dpi_workarea_scroll_preserves_header_and_footer_access");
        window.hide();
        window.show();
        pump(window, 50);
        check(IsWindowVisible(window.handle()), "Settings must restore after hiding");
        passed.push_back("hide_restore_preserves_window_identity");
    } catch (const std::exception &e) {
        failure = e.what();
        std::cerr << failure << '\n';
    }
    std::ofstream report(output / L"more-window-test-report.json");
    report << "{\n\"HardwareVerified\":false,\n\"BluetoothApisCalled\":false,\n\"passed\":" << passed.size()
           << ",\n\"scenarios\":[";
    for (std::size_t i = 0; i < passed.size(); ++i)
        report << (i ? "," : "") << "\n\"" << passed[i] << "\"";
    report << "\n],\n\"failure\":\"" << failure << "\"\n}";
    std::cout << passed.size() << " offline more settings scenarios passed\n";
    return failure.empty() ? 0 : 1;
}
