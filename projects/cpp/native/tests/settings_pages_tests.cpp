#include "ui_test_geometry.hpp"
#include "sonyconnect/more_window.hpp"
#include <commctrl.h>
#include <windowsx.h>
#include <winrt/base.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace ui = sonyconnect::ui;
namespace {
void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
template<class T> sonyconnect::Value<T> known(T v) {
    sonyconnect::Value<T> out;
    out.status = sonyconnect::ValueStatus::Known;
    out.value = std::move(v);
    return out;
}
std::wstring text(HWND h) {
    std::wstring out(GetWindowTextLengthW(h) + 1, L' ');
    out.resize(GetWindowTextW(h, out.data(), static_cast<int>(out.size())));
    return out;
}
HWND control(ui::MoreWindow &w, int id) {
    auto h = GetDlgItem(w.handle(), id);
    check(h != nullptr, "Missing page input");
    return h;
}
void click(ui::MoreWindow &w, int id) {
    auto h = control(w, id);
    check(IsWindowVisible(h) && IsWindowEnabled(h), "Page input must be available");
    SendMessageW(h, BM_CLICK, 0, 0);
}
void pump(ui::MoreWindow &w, unsigned ms = 30) {
    const auto deadline = GetTickCount64() + ms;
    do {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            if (msg.message != WM_QUIT && !w.pre_translate(msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        if (GetTickCount64() < deadline)
            MsgWaitForMultipleObjectsEx(0, nullptr, 8, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < deadline);
}
unsigned mutations{};
LRESULT CALLBACK observe(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_SETTEXT || m == WM_ENABLE || m == WM_WINDOWPOSCHANGING) ++mutations;
    return DefSubclassProc(h, m, w, l);
}
struct Action { ui::MoreAction action; int value, detail; };
}
int wmain(int argc, wchar_t **argv) {
    const std::filesystem::path output = argc > 1 ? argv[1] : L"settings-pages-tests";
    std::filesystem::create_directories(output);
    std::vector<std::string> passed;
    std::string failure;
    const bool generic_only = argc > 2 && std::wstring(argv[2]) == L"--generic-only";
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        INITCOMMONCONTROLSEX init{sizeof(init), ICC_WIN95_CLASSES};
        InitCommonControlsEx(&init);
        ui::MainWindow owner([](auto, int, bool, std::uint64_t) {});
        owner.create(GetModuleHandleW(nullptr), 96);
        auto model = ui::more_preview();
        model.preview = false;
        model.device.model = "WF-1000XM5";
        model.device.touch.assistant_writable = true;
        model.device.touch.voice_assistant = known(0x30);
        model.assistant_name = L"移动设备语音助手";
        model.assistant_choices = {{0x30, L"移动设备语音助手"}, {0xff, L"不使用"}};
        model.device.safe_listening.enabled = known(true);
        model.device.safe_listening.writable = true;
        model.device.ready = true;
        model.device.link = 7;
        std::vector<Action> actions;
        auto dispatch = [&](auto a, int v, int d, bool) { actions.push_back({a, v, d}); };
        if (!generic_only) {
        ui::MoreWindow personal(dispatch, ui::SettingsPage::personalization);
        personal.create(GetModuleHandleW(nullptr), owner.handle(), 96);
        personal.update(model);
        personal.show();
        pump(personal);
        RECT bounds{};
        GetWindowRect(personal.handle(), &bounds);
        check(bounds.right - bounds.left == ui_test::pixel(416), "Personalization must use Pen compact width");
        for (int i = 0; i < 4; ++i) {
            click(personal, 1060 + i);
            check(GetDlgItem(personal.handle(), 1100), "Style menu must open immediately");
            click(personal, 1060 + i);
            check(!GetDlgItem(personal.handle(), 1100), "Reclick must close style menu immediately");
            click(personal, 1060 + i);
            click(personal, 1101);
            check(actions.back().action == ui::MoreAction::slider_appearance && actions.back().detail == i &&
                  actions.back().value == 1, "Styles must address four independent slots");
            model.slider_styles[i] = ui::MainSliderStyle::elastic;
            personal.update(model);
        }
        passed.push_back("four_independent_style_menus_toggle_and_dispatch");
        personal.capture(output / L"01-personal-elastic.png");
        model.slider_styles[0] = model.slider_styles[2] = ui::MainSliderStyle::wake;
        personal.update(model);
        personal.capture(output / L"02-personal-mixed.png");
        click(personal, 1060);
        for (int i = 0; i < 25; ++i) {
            ++model.device.revision;
            model.device.battery.left = known(70 + i % 10);
            personal.update(model);
        }
        check(GetDlgItem(personal.handle(), 1100), "Background device updates must preserve local style menu");
        personal.capture(output / L"03-personal-menu.png");
        SendMessageW(personal.handle(), WM_KEYDOWN, VK_ESCAPE, 0);
        check(!GetDlgItem(personal.handle(), 1100) && IsWindowVisible(personal.handle()),
              "Escape must close popup before closing page");
        model.device.ready = false;
        personal.update(model);
        check(IsWindowEnabled(control(personal, 1060)), "Local appearance must work without a headphone");
        passed.push_back("personalization_survives_device_updates_and_disconnection");
        personal.hide();

        model.device.ready = true;
        ui::MoreWindow advanced(dispatch, ui::SettingsPage::advanced);
        advanced.create(GetModuleHandleW(nullptr), owner.handle(), 96);
        advanced.update(model);
        advanced.show();
        pump(advanced);
        check(text(control(advanced, 1070)).find(L"快速注意") != std::wstring::npos,
              "Ambient assignment must describe fixed Quick Attention");
        check(text(control(advanced, 1071)).find(L"移动设备语音助手") != std::wstring::npos &&
              text(control(advanced, 1071)).find(L"提高音量") != std::wstring::npos,
              "Playback assignment must describe assistant and physical right volume");
        check(!IsWindowEnabled(control(advanced, 1066)), "Missing backup must disable restore");
        check(!GetDlgItem(advanced.handle(), 1067), "Quick Access must not have a standalone switch");
        advanced.capture(output / L"04-advanced.png");
        passed.push_back("wf1000xm5_read_only_gestures_and_mobile_quick_access");
        for (int id : {1064, 1065, 1066, 1070, 1071}) SetWindowSubclass(control(advanced, id), observe, 1, 0);
        pump(advanced, 250);
        mutations = 0;
        for (int i = 0; i < 40; ++i) {
            ++model.device.revision;
            model.device.battery.right = known(i);
            advanced.update(model);
        }
        check(mutations == 0, "Unrelated updates must not rewrite advanced controls");
        passed.push_back("advanced_background_refresh_causes_no_control_mutations");
        click(advanced, 1065);
        auto menu = GetDlgItem(advanced.handle(), 1100);
        advanced.update(model);
        check(GetDlgItem(advanced.handle(), 1100) == menu, "Assistant popup identity must survive refresh");
        click(advanced, 1101);
        check(actions.back().action == ui::MoreAction::voice_assistant && actions.back().value == 0xff,
              "Assistant menu must dispatch actual advertised candidate");
        click(advanced, 1064);
        check(actions.back().action == ui::MoreAction::safe_listening && actions.back().value == 0,
              "Safe listening must dispatch requested boolean");
        model.busy = true;
        advanced.update(model);
        check(!IsWindowEnabled(control(advanced, 1064)) && !IsWindowEnabled(control(advanced, 1065)),
              "Busy operations must prevent duplicate device submissions");
        model.busy = false;
        model.feedback = L"回读错误，安全聆听保持原值";
        model.feedback_error = true;
        advanced.update(model);
        check(text(control(advanced, 1064)).find(L"开启") != std::wstring::npos,
              "Failure must restore confirmed toggle state");
        advanced.capture(output / L"05-advanced-error.png");
        passed.push_back("advanced_routes_confirmed_candidates_and_restores_failed_values");
        model.feedback_error = false;
        model.linked_restore_available = true;
        (*model.device.touch.assignments.value)[0] = 0xff;
        advanced.update(model);
        check(IsWindowEnabled(control(advanced, 1066)), "Actual backup must enable restore");
        check(text(control(advanced, 1070)).find(L"禁用") != std::wstring::npos,
              "Unassigned ear must describe disabled four-tap volume");
        click(advanced, 1066);
        check(actions.back().action == ui::MoreAction::restore_linked, "Restore must request confirmation through adapter");
        click(advanced, 1065);
        ++model.device.link;
        model.device.ready = false;
        model.device.touch.assignments = {};
        advanced.update(model);
        check(!GetDlgItem(advanced.handle(), 1100) && !IsWindowEnabled(control(advanced, 1065)),
              "New connection must cancel stale assistant choices");
        check(text(control(advanced, 1070)).find(L"等待") != std::wstring::npos,
              "Unknown assignment must not be projected as off");
        advanced.capture(output / L"06-advanced-disconnected.png");
        passed.push_back("backup_availability_and_stale_connection_guards");
        advanced.hide();
        for (auto page : {ui::SettingsPage::personalization, ui::SettingsPage::advanced}) {
            ui::MoreWindow scaled(dispatch, page);
            scaled.create(GetModuleHandleW(nullptr), owner.handle(), 192);
            scaled.update(model);
            scaled.show();
            pump(scaled);
            GetWindowRect(scaled.handle(), &bounds);
            check(bounds.right - bounds.left == ui_test::pixel(page == ui::SettingsPage::personalization ? 416.f : 680.f, 192),
                  "Settings page must preserve DIP width at 200 percent");
            SendMessageW(scaled.handle(), WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA * 30)), 0);
            pump(scaled);
            check(IsWindowVisible(control(scaled, 1000)), "Scrolled page must retain close input");
            check(IsWindowVisible(control(scaled, page == ui::SettingsPage::personalization ? 1063 : 1066)),
                  "Bottom setting must remain reachable in restricted workarea");
            scaled.capture(output / (page == ui::SettingsPage::personalization ? L"07-personal-200.png" : L"08-advanced-200.png"));
            scaled.hide();
        }
        passed.push_back("high_dpi_pages_scroll_with_reachable_header_and_last_setting");
        }
        model.device.model = "Fixture-OtherModel";
        model.device.ready = true;
        model.device.touch.assignments = known(sonyconnect::protocol::Bytes{0, 0x20});
        model.device.touch.details = known(std::vector<sonyconnect::protocol::TouchPresetState>{{0, {{0x22, 0x20}}}});
        model.custom_values = {0, 0x22, 0x20};
        model.custom_context = 0x22;
        model.custom_choices = {{{{0, L"左耳"}, {1, L"右耳"}}, {{0x22, L"按住期间启用"}},
                                  {{0x20, L"播放 / 暂停"}, {0x24, L"减小音量"}}}};
        ui::MoreWindow generic(dispatch, ui::SettingsPage::advanced);
        generic.create(GetModuleHandleW(nullptr), owner.handle(), 96);
        generic.update(model);
        generic.show();
        pump(generic);
        check(IsWindowVisible(control(generic, 1080)) && IsWindowEnabled(control(generic, 1082)) &&
                  !IsWindowVisible(control(generic, 1070)),
              "Other models must preserve actual advertised customizable controls");
        click(generic, 1082);
        click(generic, 1101);
        check(actions.back().action == ui::MoreAction::custom_function && actions.back().value == 0x24 &&
                  actions.back().detail == 0x22,
              "Custom function must retain its actual key, preset and action context");
        passed.push_back("other_models_keep_reported_custom_function_editor");
        click(generic, 1082);
        const auto before_stale = actions.size();
        const auto old_context_option = control(generic, 1101);
        model.custom_context = 0x10022;
        generic.update(model);
        check(!GetDlgItem(generic.handle(), 1100), "Changed key context must immediately close obsolete custom choices");
        SendMessageW(generic.handle(), WM_COMMAND, MAKEWPARAM(1101, BN_CLICKED), reinterpret_cast<LPARAM>(old_context_option));
        check(actions.size() == before_stale, "Context changed while popup open must reject stale custom function");
        passed.push_back("custom_function_menu_rejects_changed_key_context");
        model.custom_choices[2].clear();
        model.custom_values[2] = -1;
        generic.update(model);
        check(!IsWindowEnabled(control(generic, 1082)), "Missing advertised functions must disable editor");
        generic.capture(output / L"09-other-model-capabilities.png");
        passed.push_back("missing_custom_capability_disables_without_invented_options");
        model.device.safe_listening.enabled = {};
        model.device.touch.voice_assistant = {};
        generic.update(model);
        check(!IsWindowEnabled(control(generic, 1064)) && !IsWindowEnabled(control(generic, 1065)),
              "Unknown confirmed settings must wait for readback before toggling");
        passed.push_back("unknown_safe_listening_and_assistant_wait_for_readback");
        generic.hide();
    } catch (const std::exception &e) { failure = e.what(); }
    std::ofstream report(output / L"settings-pages-test-report.json");
    report << "{\"HardwareVerified\":false,\"BluetoothApisCalled\":false,\"passed\":" << passed.size() << ",\"scenarios\":[";
    for (std::size_t i = 0; i < passed.size(); ++i) report << (i ? "," : "") << "\"" << passed[i] << "\"";
    report << "],\"failure\":\"" << failure << "\"}";
    std::cout << passed.size() << " settings page scenarios passed\n";
    if (!failure.empty()) std::cerr << failure << '\n';
    return failure.empty() ? 0 : 1;
}
