#include "sonyconnect/session.hpp"
#include "sonyconnect/desktop.hpp"
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <winrt/base.h>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>
#include <utility>

namespace sc = sonyconnect;
namespace p = sc::protocol;
namespace d = sc::desktop;
namespace {
constexpr UINT changed_message = WM_APP + 1, tray_message = WM_APP + 2, hotkey_message = WM_APP + 3;
constexpr UINT discovery_sync_message = WM_APP + 4;
constexpr UINT combo_commit_message = WM_APP + 5;
constexpr wchar_t window_class[] = L"SonyConnect.Native.Window";
enum Id {
    tabs = 100,
    devices,
    discover,
    connect_device,
    refresh,
    disconnect_device,
    noise,
    ambient,
    sound_off,
    ambient_level,
    apply_ambient,
    voice_focus,
    eq_enabled,
    eq_preset,
    apply_eq_preset,
    eq_band = 200,
    apply_eq = 210,
    source_list = 220,
    switch_source,
    multipoint,
    fix_source,
    release_source,
    touch_key = 240,
    touch_preset,
    apply_touch,
    assistant,
    apply_assistant,
    cycle_noise,
    cycle_ambient,
    cycle_off,
    apply_cycle,
    guidance,
    volume_prompt,
    apply_volume_prompt,
    custom_action,
    custom_function,
    apply_custom,
    restore_linked,
    close_tray = 270,
    startup,
    local_volume,
    apply_local_volume,
    previous,
    play,
    pause,
    next,
    update_channel,
    check_update,
    release_page,
    save_hotkeys,
    log_mib,
    log_runs,
    apply_log,
    export_log,
    replay_log,
    hide_window,
    exit_app,
    hotkey_recognizer = 300,
    hotkey_key = 310,
    feature_choice = 400,
    feature_apply = 430,
    capture_hotkey = 500,
    eq_value = 520,
    ambient_value = 530,
    custom_action_label = 540,
    custom_function_label,
    cycle_hint
};
std::wstring wide(const std::string &text) {
    if (text.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                nullptr, 0);
    if (!n)
        return L"无法显示的 UTF-8 文本";
    std::wstring out(n, L' ');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}
std::wstring window_text(HWND handle) {
    std::wstring text(GetWindowTextLengthW(handle) + 1, L'\0');
    text.resize(GetWindowTextW(handle, text.data(), static_cast<int>(text.size())));
    return text;
}
std::filesystem::path executable_path() {
    std::wstring path(32768, L' ');
    DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(n);
    return path;
}
std::filesystem::path native_data() {
    PWSTR folder{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder)))
        throw std::runtime_error("LocalAppData unavailable");
    std::filesystem::path path(folder);
    CoTaskMemFree(folder);
    return path / L"SonyConnect" / L"Native";
}
template <class T> std::wstring value_text(const sc::Value<T> &value) {
    if (!value.known())
        return value.status == sc::ValueStatus::Stale ? L"已过期（请重新读取）" : L"未知／尚未读取";
    if constexpr (std::is_same_v<T, bool>)
        return *value.value ? L"开启" : L"关闭";
    else if constexpr (std::is_same_v<T, std::string>)
        return wide(*value.value);
    else
        return std::to_wstring(*value.value);
}
std::wstring percent_text(const sc::Value<int> &value) {
    return value_text(value) + (value.known() ? L"%" : L"");
}
std::wstring preset_name(p::Byte id) {
    switch (id) {
    case 0:
        return L"环境声音控制";
    case 0x10:
        return L"音量调节";
    case 0x20:
        return L"播放控制";
    case 0x21:
        return L"曲目控制";
    case 0x22:
        return L"播放控制（语音助手限制）";
    case 0x35:
        return L"环境声／Quick Access";
    case 0x43:
        return L"环境声／Quick Access（仅经典蓝牙）";
    case 0x30:
        return L"移动设备语音助手";
    case 0x31:
        return L"Google Assistant";
    case 0x32:
        return L"Amazon Alexa";
    case 0x33:
        return L"腾讯小微";
    case 0x36:
        return L"Quick Access";
    case 0x71:
        return L"自定义 1";
    case 0x72:
        return L"自定义 2";
    case 0xff:
        return L"无功能";
    default:
        return L"设备预设 " + std::to_wstring(id);
    }
}
std::wstring choice_name(p::Feature f, int v) {
    if (f == p::Feature::ConnectionPreference) {
        switch (v) {
        case 0:
            return L"优先音质";
        case 1:
            return L"优先稳定连接";
        }
    }
    if (f == p::Feature::Dsee)
        return v == 0 ? L"关闭" : v == 1 ? L"自动" : L"设备选项 " + std::to_wstring(v);
    if (f == p::Feature::SpeakToChat || f == p::Feature::WearDetectionPause ||
        f == p::Feature::Notifications) {
        if (v == 0)
            return L"开启";
        if (v == 1)
            return L"关闭";
        return L"设备选项 " + std::to_wstring(v);
    }
    if (f == p::Feature::Multipoint)
        return v ? L"开启" : L"关闭";
    if (f == p::Feature::AutoPowerOff) {
        switch (v) {
        case 0:
            return L"5 分钟";
        case 1:
            return L"30 分钟";
        case 2:
            return L"60 分钟";
        case 3:
            return L"180 分钟";
        case 4:
            return L"15 分钟";
        case 0x10:
            return L"摘下耳机后自动关机";
        case 0x11:
            return L"不自动关机";
        }
    }
    if (f == p::Feature::PowerOff)
        return L"关闭耳机";
    return L"设备选项 " + std::to_wstring(v);
}
std::wstring assistant_name(int v) {
    switch (v) {
    case 0x30:
        return L"移动设备语音助手";
    case 0x31:
        return L"Google Assistant";
    case 0x32:
        return L"Amazon Alexa";
    case 0x33:
        return L"腾讯小微";
    case 0x34:
        return L"Sony 语音助手";
    case 0x3f:
        return L"其他设备已启用";
    case 0xff:
        return L"不使用";
    default:
        return L"未知助手（" + std::to_wstring(v) + L"）";
    }
}
std::wstring action_name(int v) {
    switch (v) {
    case 0:
        return L"单击";
    case 1:
        return L"双击";
    case 2:
        return L"三击";
    case 3:
        return L"连续点击";
    case 0x10:
        return L"单击并按住";
    case 0x11:
        return L"双击并按住";
    case 0x21:
        return L"长按后启用";
    case 0x22:
        return L"按住期间启用";
    default:
        return L"未知手势（" + std::to_wstring(v) + L"）";
    }
}
std::wstring function_name(int v) {
    switch (v) {
    case 0:
        return L"无功能";
    case 1:
        return L"降噪／环境声／关闭";
    case 2:
        return L"降噪／环境声";
    case 3:
        return L"降噪／关闭";
    case 4:
        return L"环境声／关闭";
    case 0x10:
        return L"快速注意";
    case 0x11:
        return L"降噪优化";
    case 0x20:
        return L"播放／暂停";
    case 0x21:
        return L"下一首";
    case 0x22:
        return L"上一首";
    case 0x23:
        return L"增大音量";
    case 0x24:
        return L"减小音量";
    case 0x30:
        return L"移动设备语音助手";
    case 0x31:
        return L"读取通知";
    case 0x32:
        return L"Google Assistant";
    case 0x33:
        return L"停止 Google Assistant";
    case 0x34:
        return L"取消语音输入";
    case 0x35:
        return L"腾讯小微";
    case 0x36:
        return L"取消语音识别";
    case 0x37:
        return L"Alexa 语音输入";
    case 0x38:
        return L"取消 Alexa";
    case 0x39:
        return L"取消腾讯小微";
    default:
        return L"未知功能（" + std::to_wstring(v) + L"）";
    }
}
std::wstring touch_key_name(int v) {
    switch (v) {
    case 0:
        return L"左耳";
    case 1:
        return L"右耳";
    case 2:
        return L"自定义按键";
    case 3:
        return L"C 按键";
    case 4:
    case 5:
        return L"NC／AMB 按键";
    default:
        return L"未知按键（" + std::to_wstring(v) + L"）";
    }
}
constexpr std::array<std::pair<p::Feature, const wchar_t *>, 6> feature_rows{
    {{p::Feature::ConnectionPreference, L"蓝牙连接质量"},
     {p::Feature::Dsee, L"DSEE"},
     {p::Feature::SpeakToChat, L"智能免摘"},
     {p::Feature::WearDetectionPause, L"摘下耳机暂停播放"},
     {p::Feature::AutoPowerOff, L"自动关机"},
     {p::Feature::PowerOff, L"关机"}}};

class Window {
  public:
    HWND hwnd{};
    bool offline{}, ui_test{}, start_hidden{}, quitting{}, busy{};
    explicit Window(bool off, bool test, bool hidden) : offline(off), ui_test(test), start_hidden(hidden) {}
    ~Window() {
        if (hotkeys)
            hotkeys->pause(true);
        hotkeys.reset();
        {
            std::lock_guard lock(queue_mutex);
            stop = true;
            queue.clear();
        }
        queue_cv.notify_all();
        if (controller)
            controller->disconnect();
        if (worker.joinable())
            worker.join();
        if (controller) {
            controller->set_trace({});
            controller.reset();
        }
        diagnostics.reset();
        if (tray_added)
            Shell_NotifyIconW(NIM_DELETE, &tray);
        if (font)
            DeleteObject(font);
    }
    void create(HINSTANCE instance) {
        WNDCLASSEXW cls{sizeof(cls)};
        cls.lpfnWndProc = proc;
        cls.hInstance = instance;
        cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
        cls.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        cls.lpszClassName = window_class;
        RegisterClassExW(&cls);
        hwnd = CreateWindowExW(0, window_class, L"SonyConnect · C++ 功能验证", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 1000, 800, nullptr, nullptr, instance, this);
        if (!hwnd)
            throw std::runtime_error("Cannot create main window");
        RECT sized{0, 0, px(960), px(750)};
        AdjustWindowRectExForDpi(&sized, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
        SetWindowPos(hwnd, nullptr, 0, 0, sized.right - sized.left, sized.bottom - sized.top,
                     SWP_NOMOVE | SWP_NOZORDER);
        if (!offline) {
            root = native_data();
            std::filesystem::create_directories(root);
            close_to_tray =
                GetPrivateProfileIntW(L"Desktop", L"CloseToTray", 0, (root / L"desktop.ini").c_str()) != 0;
            controller = std::make_unique<sc::DeviceController>();
            d::DiagnosticOptions log_options{
                static_cast<unsigned>(
                    GetPrivateProfileIntW(L"Diagnostics", L"SegmentMiB", 2, (root / L"desktop.ini").c_str())),
                static_cast<unsigned>(GetPrivateProfileIntW(L"Diagnostics", L"RetainedRuns", 10,
                                                            (root / L"desktop.ini").c_str()))};
            log_options.validate();
            diagnostics = std::make_unique<d::DiagnosticLog>(root / L"Logs", log_options);
            SetDlgItemInt(hwnd, log_mib, log_options.segment_limit_mib, FALSE);
            SetDlgItemInt(hwnd, log_runs, log_options.retained_runs, FALSE);
            controller->set_trace([this](const sc::ProtocolTrace &trace) {
                d::DiagnosticEvent e;
                e.event = trace.event;
                e.origin = "Device";
                e.generation = trace.link;
                e.receive_order = trace.receive_order;
                e.type = trace.type;
                e.sequence = trace.sequence;
                e.command = trace.command;
                diagnostics->record(std::move(e));
            });
            hotkeys = std::make_unique<d::GlobalHotkeys>(root / L"hotkeys.json", [this](d::SoundMode mode) {
                PostMessageW(hwnd, hotkey_message, static_cast<WPARAM>(mode), 0);
            });
            try {
                hotkeys->start();
            } catch (const std::exception &e) {
                set_feedback(L"热键初始化失败：" + wide(e.what()));
            }
            bindings = hotkeys->bindings();
            for (int i = 0; i < 3; ++i) {
                set_text(hotkey_key + i, bindings[i].gesture.label().c_str());
            }
            worker = std::thread([this] { worker_loop(); });
            tray.cbSize = sizeof(tray);
            tray.hWnd = hwnd;
            tray.uID = 1;
            tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
            tray.uCallbackMessage = tray_message;
            tray.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
            wcscpy_s(tray.szTip, L"SonyConnect C++");
            tray_added = Shell_NotifyIconW(NIM_ADD, &tray) != FALSE;
        }
        set_checked(close_tray, close_to_tray ? BST_CHECKED : BST_UNCHECKED);
        set_checked(startup, !offline && startup_enabled() ? BST_CHECKED : BST_UNCHECKED);
        render();
        SetTimer(hwnd, 1, 500, nullptr);
        if (!start_hidden || !tray_added)
            ShowWindow(hwnd, ui_test ? SW_SHOWNOACTIVATE : SW_SHOW);
    }
    bool self_test(const std::filesystem::path &report) {
        bool passed = true;
        std::vector<std::pair<std::string, bool>> checks;
        auto check_result = [&](std::string name, bool success) {
            checks.emplace_back(std::move(name), success);
            passed = passed && success;
        };
        unsigned control_count{};
        EnumChildWindows(
            hwnd,
            [](HWND, LPARAM param) -> BOOL {
                ++*reinterpret_cast<unsigned *>(param);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&control_count));
        check_result("offline-actions-disabled", controller == nullptr && hotkeys == nullptr &&
                                                     !IsWindowEnabled(GetDlgItem(hwnd, noise)) &&
                                                     !IsWindowEnabled(GetDlgItem(hwnd, check_update)));
        // Constructed view state tests never instantiate a transport or dispatch commands.
        auto known = [](auto input) {
            sc::Value<std::decay_t<decltype(input)>> value;
            value.status = sc::ValueStatus::Known;
            value.value = input;
            return value;
        };
        offline = false;
        state.ready = state.connected = true;
        state.link = 1;
        state.revision = 1;
        state.model = "WF-1000XM5";
        state.sound.writable = true;
        state.sound.mode = known(p::SoundMode::Ambient);
        state.sound.voice_focus = known(true);
        state.sound.ambient_level = known(8);
        state.sound.ranges = {{0, {1, 20, 1}}, {1, {1, 10, 1}}};
        state.equalizer.writable = state.equalizer.five_band_layout = true;
        state.equalizer.preset = known(0x30);
        state.equalizer.presets = {{0, "Off"}, {0x30, "Bright"}, {0xa0, "Custom"}};
        state.equalizer.bands = known(std::vector<int>{1, 2, 3, 4, 5});
        state.equalizer.clear_bass = known(0);
        state.touch.cycle = known(3);
        state.touch.guidance = known(0);
        state.touch.prompt_volume = known(-2);
        state.features[p::Feature::SpeakToChat].value = known(0);
        state.features[p::Feature::SpeakToChat].choices = {0, 1};
        render();
        found = {{0x1001, "WH-1000XM5", "test-wh", true, true, false},
                 {0x1002, "WF-1000XM5", "test-wf", true, true, false}};
        sync_discovered_devices();
        select_data(GetDlgItem(hwnd, devices), 1);
        auto rebuilt_before = discovery_rebuilds;
        SetFocus(GetDlgItem(hwnd, devices));
        SendDlgItemMessageW(hwnd, devices, CB_SHOWDROPDOWN, TRUE, 0);
        check_result("discovery-dropdown-opened",
                     SendDlgItemMessageW(hwnd, devices, CB_GETDROPPEDSTATE, 0, 0));
        for (int i = 0; i < 20; ++i)
            message(changed_message, 0, 0);
        check_result("background-refresh-keeps-discovery-popup-and-selection",
                     discovery_rebuilds == rebuilt_before && selection(devices) == 1 &&
                         SendDlgItemMessageW(hwnd, devices, CB_GETDROPPEDSTATE, 0, 0));
        std::swap(found[0], found[1]);
        message(changed_message, 0, 0);
        check_result("changed-discovery-is-deferred-while-popup-open",
                     discovery_rebuilds == rebuilt_before && selection(devices) == 1 &&
                         displayed_found[1].address == 0x1002 &&
                         SendDlgItemMessageW(hwnd, devices, CB_GETDROPPEDSTATE, 0, 0));
        SendDlgItemMessageW(hwnd, devices, CB_SHOWDROPDOWN, FALSE, 0);
        message(discovery_sync_message, 0, 0);
        check_result("discovery-selection-follows-device-address-after-reorder",
                     discovery_rebuilds == rebuilt_before + 1 && selection(devices) == 0 &&
                         displayed_found[0].address == 0x1002);
        found.erase(found.begin());
        sync_discovered_devices();
        render();
        check_result("removed-selection-does-not-connect-a-different-device",
                     selection(devices) == -1 && !IsWindowEnabled(GetDlgItem(hwnd, connect_device)));
        found.clear();
        sync_discovered_devices();
        check_result("empty-discovery-clears-list",
                     SendDlgItemMessageW(hwnd, devices, CB_GETCOUNT, 0, 0) == 0);
        check_result("voice-range-and-readout",
                     SendDlgItemMessageW(hwnd, ambient_level, TBM_GETRANGEMAX, 0, 0) == 10 &&
                         SendDlgItemMessageW(hwnd, ambient_level, TBM_GETPOS, 0, 0) == 8);
        check_result("guidance-zero-means-enabled", selection(guidance) == 0);
        check_result("prompt-volume-signed-range",
                     SendDlgItemMessageW(hwnd, volume_prompt, TBM_GETRANGEMIN, 0, 0) == -2 &&
                         SendDlgItemMessageW(hwnd, volume_prompt, TBM_GETRANGEMAX, 0, 0) == 2 &&
                         SendDlgItemMessageW(hwnd, volume_prompt, TBM_GETPOS, 0, 0) == -2);
        check_result("cycle-wire-value-three", cycle_selection() == 3);
        set_checked(cycle_ambient, BST_CHECKED);
        SendDlgItemMessageW(hwnd, eq_band, TBM_SETPOS, TRUE, 7);
        ++state.revision; // An unrelated battery notification must preserve drafts.
        render();
        check_result("notification-preserves-user-drafts",
                     cycle_selection() == 1 && SendDlgItemMessageW(hwnd, eq_band, TBM_GETPOS, 0, 0) == 7);
        state.equalizer.preset = known(0);
        ++state.revision;
        render();
        check_result("eq-off-disables-edit-and-remembers-preset",
                     !IsWindowEnabled(GetDlgItem(hwnd, eq_band)) && last_enabled_eq == 0x30);
        state.equalizer.preset = known(0x30);
        state.equalizer.presets.pop_back();
        ++state.revision;
        render();
        check_result("missing-custom-prevents-gain-write", !IsWindowEnabled(GetDlgItem(hwnd, eq_band)));
        state.equalizer.preset = known(0xa1);
        state.equalizer.presets.push_back({0xa1, "自定义 1"});
        ++state.revision;
        render();
        check_result("user-slot-edit-without-manual-candidate", IsWindowEnabled(GetDlgItem(hwnd, eq_band)));
        check_result("instant-controls-have-no-confirm-buttons",
                     !GetDlgItem(hwnd, apply_ambient) && !GetDlgItem(hwnd, eq_enabled) &&
                         !GetDlgItem(hwnd, apply_eq_preset) && !GetDlgItem(hwnd, apply_eq));
        auto dispatch_count = ui_dispatches;
        SendDlgItemMessageW(hwnd, eq_band, TBM_SETPOS, TRUE, 9);
        message(WM_HSCROLL, TB_THUMBTRACK, reinterpret_cast<LPARAM>(GetDlgItem(hwnd, eq_band)));
        check_result("eq-drag-only-updates-number",
                     ui_dispatches == dispatch_count && window_text(GetDlgItem(hwnd, eq_value)) == L"+9");
        message(WM_HSCROLL, TB_ENDTRACK, reinterpret_cast<LPARAM>(GetDlgItem(hwnd, eq_band)));
        check_result("eq-release-dispatches-once", ui_dispatches == dispatch_count + 1);
        message(changed_message, 1, 0);
        check_result("failed-immediate-write-restores-readback",
                     SendDlgItemMessageW(hwnd, eq_band, TBM_GETPOS, 0, 0) == 1);
        dispatch_count = ui_dispatches;
        select_data(GetDlgItem(hwnd, eq_preset), 0);
        command(eq_preset, CBN_SELCHANGE);
        check_result("eq-preset-change-dispatches-immediately", ui_dispatches == dispatch_count + 1);
        message(changed_message, 1, 0);
        dispatch_count = ui_dispatches;
        SendDlgItemMessageW(hwnd, ambient_level, TBM_SETPOS, TRUE, 9);
        message(WM_HSCROLL, TB_ENDTRACK, reinterpret_cast<LPARAM>(GetDlgItem(hwnd, ambient_level)));
        check_result("ambient-release-dispatches-immediately", ui_dispatches == dispatch_count + 1);
        message(changed_message, 1, 0);
        background_work = true;
        dispatch_count = ui_dispatches;
        SendDlgItemMessageW(hwnd, ambient_level, TBM_SETPOS, TRUE, 9);
        apply_slider(ambient_level);
        busy = true; // A foreground operation now waits behind the existing read task.
        message(changed_message, 0, 0);
        check_result("background-completion-preserves-pending-immediate-write",
                     ui_dispatches == dispatch_count + 1 && busy && immediate_control == ambient_level);
        message(changed_message, 1, 0);
        state.touch.keys = {{7, 0, 0x71, {{0x71, {}, {}, {{1, 2, {2, 3}}}}}}};
        state.touch.assignments = known(p::Bytes{0x71});
        state.touch.details = known(std::vector<p::TouchPresetState>{{0x72, {{1, 2}}}, {0x71, {{1, 3}}}});
        ++state.revision;
        render();
        check_result("touch-readback-keyed-by-preset", selection(custom_function) == 3);
        command(hotkey_key, EN_SETFOCUS);
        MSG capture_message{};
        capture_message.message = WM_KEYDOWN;
        capture_message.wParam = VK_CONTROL;
        capture_message.lParam = 1ll << 24; // Right control, same normalization as physical input.
        pre_translate(capture_message);
        capture_message.wParam = VK_F2;
        capture_message.lParam = 0;
        pre_translate(capture_message);
        check_result("hotkey-right-side-capture",
                     bindings[0].gesture.recognizer == d::Recognizer::right_ctrl &&
                         bindings[0].gesture.key == VK_F2 &&
                         window_text(GetDlgItem(hwnd, hotkey_key)) == bindings[0].gesture.label());
        check_result("hotkeys-have-only-input-fields",
                     !GetDlgItem(hwnd, hotkey_recognizer) && !GetDlgItem(hwnd, capture_hotkey));
        capture_message.lParam = 1ll << 30;
        check_result("completed-hotkey-does-not-restart-on-repeat",
                     !pre_translate(capture_message) && capturing < 0);
        command(hotkey_key, EN_SETFOCUS);
        capture_message.wParam = VK_ESCAPE;
        capture_message.lParam = 0;
        pre_translate(capture_message);
        capture_message.wParam = VK_TAB;
        check_result("canceled-hotkey-allows-tab-navigation",
                     !pre_translate(capture_message) && capturing < 0);
        hold = true;
        show_page(1);
        check_result("changing-page-cancels-fixed-source-hold", !hold);
        state.sources.available = state.sources.switch_writable = state.sources.multipoint_writable = true;
        state.sources.multipoint = known(true);
        state.sources.active_source = known(std::string("pc"));
        state.sources.entries = {{"pc", "Test computer", 1, {}},
                                 {"phone", "Test phone", 2, {}},
                                 {"old", "Registered tablet", 0, {}}};
        for (int i = 0; i < 5; ++i) {
            auto &feature = state.features[feature_rows[i].first];
            feature.writable = true;
            feature.value = known(i == 4 ? 0x11 : i == 0 ? 1 : 0);
            feature.choices = i == 4 ? std::vector<int>{0x10, 0x11} : std::vector<int>{0, 1};
        }
        state.touch.keys = {
            {0, 0, 0x35, {{0x35, {}, {}, {{0, 4, {1, 2, 3, 4}}}}, {0x20, {}, {}, {}}, {0xff, {}, {}, {}}}},
            {1, 0, 0x20, {{0, {}, {}, {}}, {0x20, {}, {}, {}}, {0xff, {}, {}, {}}}}};
        state.touch.assignments = known(p::Bytes{0x35, 0x20});
        state.touch.details = known(std::vector<p::TouchPresetState>{{0x35, {{0, 4}}}});
        state.touch.voice_assistant = known(0xff);
        state.touch.voice_assistant_candidates = {0x30, 0x33, 0xff};
        state.touch.writable = state.touch.cycle_writable = state.touch.guidance_writable =
            state.touch.volume_writable = state.touch.assistant_writable = true;
        ++state.revision;
        render();
        auto source_text = window_text(source_readout);
        check_result("source-panel-preserves-registered-records",
                     source_text.find(L"Test computer · 已连接 · 当前音源") != std::wstring::npos &&
                         source_text.find(L"Test phone · 已连接") != std::wstring::npos &&
                         source_text.find(L"Registered tablet") != std::wstring::npos &&
                         SendDlgItemMessageW(hwnd, source_list, CB_GETCOUNT, 0, 0) == 2);
        check_result("csharp-immediate-settings-have-no-apply-buttons",
                     !GetDlgItem(hwnd, apply_touch) && !GetDlgItem(hwnd, apply_custom) &&
                         !GetDlgItem(hwnd, apply_cycle) && !GetDlgItem(hwnd, apply_volume_prompt) &&
                         !GetDlgItem(hwnd, apply_assistant) && !GetDlgItem(hwnd, apply_local_volume) &&
                         !GetDlgItem(hwnd, switch_source) && !GetDlgItem(hwnd, feature_apply));
        check_result("wf-cycle-is-not-a-duplicate-gesture-editor",
                     radio_groups.at(custom_action).choices.empty() &&
                         radio_groups.at(custom_function).choices.empty());
        dispatch_count = ui_dispatches;
        set_checked(cycle_noise, BST_UNCHECKED);
        set_checked(cycle_ambient, BST_UNCHECKED);
        set_checked(cycle_off, BST_CHECKED);
        command(cycle_noise, BN_CLICKED);
        check_result("single-cycle-mode-is-rejected-and-restored",
                     ui_dispatches == dispatch_count && cycle_selection() == 3);
        set_checked(cycle_noise, BST_CHECKED);
        set_checked(cycle_ambient, BST_CHECKED);
        set_checked(cycle_off, BST_CHECKED);
        command(cycle_ambient, BN_CLICKED);
        check_result("three-cycle-modes-submit-immediately", ui_dispatches == dispatch_count + 1);
        message(changed_message, 1, 0);
        dispatch_count = ui_dispatches;
        set_checked(cycle_noise, BST_CHECKED);
        set_checked(cycle_ambient, BST_CHECKED);
        set_checked(cycle_off, BST_UNCHECKED);
        command(cycle_off, BN_CLICKED);
        check_result("two-cycle-modes-submit-immediately", ui_dispatches == dispatch_count + 1);
        message(changed_message, 1, 0);
        dispatch_count = ui_dispatches;
        auto &assignment = radio_groups.at(touch_preset);
        command(assignment.base + 1, BN_CLICKED);
        check_result("touch-assignment-selection-submits-immediately", ui_dispatches == dispatch_count + 1);
        message(changed_message, 1, 0);
        dispatch_count = ui_dispatches;
        auto &guidance_options = radio_groups.at(guidance);
        command(guidance_options.base + 1, BN_CLICKED);
        check_result("guidance-selection-submits-immediately", ui_dispatches == dispatch_count + 1);
        message(changed_message, 1, 0);
        dispatch_count = ui_dispatches;
        SendDlgItemMessageW(hwnd, volume_prompt, TBM_SETPOS, TRUE, 1);
        message(WM_HSCROLL, TB_ENDTRACK, reinterpret_cast<LPARAM>(GetDlgItem(hwnd, volume_prompt)));
        check_result("prompt-volume-release-submits-immediately", ui_dispatches == dispatch_count + 1);
        message(changed_message, 1, 0);
        dispatch_count = ui_dispatches;
        select_data(GetDlgItem(hwnd, source_list), 1);
        command(source_list, CBN_SELCHANGE);
        check_result("source-selection-submits-immediately", ui_dispatches == dispatch_count + 1);
        message(changed_message, 1, 0);
        dispatch_count = ui_dispatches;
        auto &assistants = radio_groups.at(assistant);
        command(assistants.base, BN_CLICKED);
        check_result("assistant-selection-submits-immediately", ui_dispatches == dispatch_count + 1);
        message(changed_message, 1, 0);
        dispatch_count = ui_dispatches;
        SendDlgItemMessageW(hwnd, ambient_level, TBM_SETPOS, TRUE, 9);
        message(WM_HSCROLL, TB_THUMBTRACK, reinterpret_cast<LPARAM>(GetDlgItem(hwnd, ambient_level)));
        check_result("ambient-drag-is-coalesced-before-dispatch",
                     pending_slider && pending_slider->value == 9 && ui_dispatches == dispatch_count);
        pending_slider->due = 0;
        message(WM_TIMER, 3, 0);
        check_result("ambient-drag-dispatches-after-debounce", ui_dispatches == dispatch_count + 1);
        busy = true;
        SendDlgItemMessageW(hwnd, ambient_level, TBM_SETPOS, TRUE, 10);
        message(WM_HSCROLL, TB_ENDTRACK, reinterpret_cast<LPARAM>(GetDlgItem(hwnd, ambient_level)));
        check_result("last-slider-value-is-kept-during-inflight-write",
                     pending_slider && pending_slider->value == 10);
        message(changed_message, 1, 0);
        check_result("last-slider-value-dispatches-after-inflight-write",
                     !pending_slider && ui_dispatches == dispatch_count + 2);
        message(changed_message, 1, 0);
        dispatch_count = ui_dispatches;
        SendDlgItemMessageW(hwnd, ambient_level, TBM_SETPOS, TRUE, 10);
        apply_slider(ambient_level, false);
        ++state.link;
        pending_slider->due = 0;
        flush_slider();
        check_result("stale-slider-intent-is-canceled-and-readback-restored",
                     !pending_slider && ui_dispatches == dispatch_count &&
                         SendDlgItemMessageW(hwnd, ambient_level, TBM_GETPOS, 0, 0) == 8);
        state.sources.switch_writable = false;
        render();
        check_result("immediate-source-selector-requires-write-capability",
                     !IsWindowEnabled(GetDlgItem(hwnd, source_list)));
        state.sources.switch_writable = true;
        // Exercise actual native popups with timers and notifications; no transport is created.
        for (auto [id, target_page, name] :
             {std::tuple{eq_preset, 1, "eq"}, std::tuple{source_list, 2, "source"},
              std::tuple{update_channel, 6, "update"}}) {
            show_page(target_page);
            render();
            auto mutations_before = ui_mutations;
            for (int i = 0; i < 20; ++i)
                render();
            check_result(std::string(name) + "-idle-refresh-does-not-mutate-controls",
                         mutations_before == ui_mutations);
            SetFocus(GetDlgItem(hwnd, id));
            SendDlgItemMessageW(hwnd, id, CB_SHOWDROPDOWN, TRUE, 0);
            auto popup_selection = selection(id);
            for (int i = 0; i < 20; ++i) {
                ++state.revision; // Simulate unrelated device state arrivals.
                message(changed_message, 0, 0);
                message(WM_TIMER, 1, 0);
            }
            check_result(std::string(name) + "-popup-survives-state-and-timer-refresh",
                         selection(id) == popup_selection &&
                             SendDlgItemMessageW(hwnd, id, CB_GETDROPPEDSTATE, 0, 0));
            SendDlgItemMessageW(hwnd, id, CB_SHOWDROPDOWN, FALSE, 0);
            message(discovery_sync_message, id, 0);
        }
        show_page(1);
        dispatch_count = ui_dispatches;
        SendDlgItemMessageW(hwnd, eq_preset, CB_SHOWDROPDOWN, TRUE, 0);
        select_data(GetDlgItem(hwnd, eq_preset), 0);
        command(eq_preset, CBN_SELCHANGE);
        command(eq_preset, CBN_SELENDOK);
        message(combo_commit_message, eq_preset, 0);
        check_result("open-eq-selection-does-not-start-a-write",
                     ui_dispatches == dispatch_count && combo_intents.contains(eq_preset) &&
                         SendDlgItemMessageW(hwnd, eq_preset, CB_GETDROPPEDSTATE, 0, 0));
        state.equalizer.presets.push_back({0xa2, "自定义 2"});
        ++state.revision;
        render();
        check_result("eq-candidates-change-is-deferred-until-popup-closes",
                     SendDlgItemMessageW(hwnd, eq_preset, CB_GETCOUNT, 0, 0) == 3 &&
                         SendDlgItemMessageW(hwnd, eq_preset, CB_GETDROPPEDSTATE, 0, 0));
        SendDlgItemMessageW(hwnd, eq_preset, WM_KEYDOWN, VK_RETURN, 0);
        message(combo_commit_message, eq_preset, 0);
        message(combo_commit_message, eq_preset, 0);
        message(discovery_sync_message, eq_preset, 0);
        check_result("accepted-eq-selection-commits-once-after-popup-closes",
                     ui_dispatches == dispatch_count + 1 &&
                         SendDlgItemMessageW(hwnd, eq_preset, CB_GETCOUNT, 0, 0) == 4);
        message(changed_message, 1, 0);
        dispatch_count = ui_dispatches;
        SendDlgItemMessageW(hwnd, eq_preset, CB_SHOWDROPDOWN, TRUE, 0);
        select_data(GetDlgItem(hwnd, eq_preset), 0);
        command(eq_preset, CBN_SELCHANGE);
        command(eq_preset, CBN_SELENDCANCEL);
        SendDlgItemMessageW(hwnd, eq_preset, CB_SHOWDROPDOWN, FALSE, 0);
        command(eq_preset, CBN_SELCHANGE); // Some notification orders select after close.
        message(discovery_sync_message, eq_preset, 0);
        message(combo_commit_message, eq_preset, 0);
        check_result("canceled-eq-popup-does-not-write-and-restores-readback",
                     ui_dispatches == dispatch_count && selection(eq_preset) == 0xa1);
        show_page(2);
        dispatch_count = ui_dispatches;
        SendDlgItemMessageW(hwnd, source_list, CB_SHOWDROPDOWN, TRUE, 0);
        select_data(GetDlgItem(hwnd, source_list), source_tokens.at("phone"));
        command(source_list, CBN_SELCHANGE);
        std::swap(state.sources.entries[0], state.sources.entries[1]);
        ++state.revision;
        render();
        command(source_list, CBN_SELENDOK);
        check_result("source-popup-selection-is-bound-to-address-after-reorder",
                     combo_intents.at(source_list).source == "phone" && ui_dispatches == dispatch_count &&
                         SendDlgItemMessageW(hwnd, source_list, CB_GETDROPPEDSTATE, 0, 0));
        SendDlgItemMessageW(hwnd, source_list, WM_KEYDOWN, VK_RETURN, 0);
        message(combo_commit_message, source_list, 0);
        message(discovery_sync_message, source_list, 0);
        check_result("reordered-source-selection-commits-once", ui_dispatches == dispatch_count + 1);
        message(changed_message, 1, 0);
        dispatch_count = ui_dispatches;
        SendDlgItemMessageW(hwnd, source_list, CB_SHOWDROPDOWN, TRUE, 0);
        select_data(GetDlgItem(hwnd, source_list), source_tokens.at("phone"));
        command(source_list, CBN_SELENDOK);
        state.sources.entries[0].connection_slot = 0;
        ++state.revision;
        render();
        SendDlgItemMessageW(hwnd, source_list, WM_KEYDOWN, VK_RETURN, 0);
        message(combo_commit_message, source_list, 0);
        message(discovery_sync_message, source_list, 0);
        check_result("removed-source-selection-is-rejected-before-dispatch",
                     ui_dispatches == dispatch_count &&
                         SendDlgItemMessageW(hwnd, source_list, CB_GETCOUNT, 0, 0) == 1);
        state.sources.entries[0].connection_slot = 2;
        ++state.revision;
        render();
        dispatch_count = ui_dispatches;
        SendDlgItemMessageW(hwnd, source_list, CB_SHOWDROPDOWN, TRUE, 0);
        select_data(GetDlgItem(hwnd, source_list), source_tokens.at("phone"));
        command(source_list, CBN_SELENDOK);
        SendDlgItemMessageW(hwnd, source_list, WM_KEYDOWN, VK_RETURN, 0);
        ++state.link;
        message(combo_commit_message, source_list, 0);
        message(discovery_sync_message, source_list, 0);
        check_result("source-selection-from-an-old-link-is-canceled-before-dispatch",
                     ui_dispatches == dispatch_count && selection(source_list) == source_tokens.at("pc"));
        SendDlgItemMessageW(hwnd, source_list, CB_SHOWDROPDOWN, TRUE, 0);
        state.ready = false;
        ++state.revision;
        render();
        check_result("real-disconnection-disables-and-closes-source-popup",
                     !IsWindowEnabled(GetDlgItem(hwnd, source_list)) &&
                         !SendDlgItemMessageW(hwnd, source_list, CB_GETDROPPEDSTATE, 0, 0));
        message(discovery_sync_message, source_list, 0);
        check_result("disconnected-source-panel-does-not-claim-current-connection",
                     window_text(source_readout).find(L"上次设备记录") != std::wstring::npos &&
                         window_text(source_readout).find(L"已连接") == std::wstring::npos);
        state.ready = true;
        ++state.revision;
        render();
        auto &quality = radio_groups.at(feature_choice);
        command(quality.base, BN_CLICKED);
        unsigned checked{};
        for (auto child : quality.buttons)
            checked += SendMessageW(child, BM_GETCHECK, 0, 0) == BST_CHECKED;
        check_result("feature-radio-options-are-exclusive", selection(feature_choice) == 0 && checked == 1);
        show_page(3);
        SetFocus(quality.buttons.front());
        MSG arrow{};
        arrow.hwnd = quality.buttons.front();
        arrow.message = WM_KEYDOWN;
        arrow.wParam = VK_RIGHT;
        if (!IsDialogMessageW(hwnd, &arrow))
            DispatchMessageW(&arrow);
        checked = 0;
        for (auto child : quality.buttons)
            checked += (GetWindowLongPtrW(child, GWL_STYLE) & WS_TABSTOP) != 0;
        check_result("radio-keyboard-route-and-one-tabstop",
                     selection(feature_choice) == 1 && checked == 1 &&
                         (GetWindowLongPtrW(quality.panel, GWL_EXSTYLE) & WS_EX_CONTROLPARENT));
        check_result("verified-chinese-option-labels",
                     choice_name(p::Feature::AutoPowerOff, 0x11) == L"不自动关机" &&
                         assistant_name(0x33) == L"腾讯小微" && assistant_name(0xff) == L"不使用" &&
                         function_name(4) == L"环境声／关闭" && action_name(0) == L"单击");
        std::vector<std::pair<std::wstring, int>> many_choices;
        for (int i = 0; i < 64; ++i)
            many_choices.emplace_back(function_name(i), i);
        sync_radios(custom_function, many_choices, 63);
        auto &functions = radio_groups.at(custom_function);
        SendMessageW(functions.panel, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, 900), 0);
        check_result("radio-overflow-keeps-all-candidates-accessible", functions.buttons.size() == 64 &&
                                                                           selection(custom_function) == 63 &&
                                                                           functions.scroll > 0);
        sync_radios(custom_function, {}, -1);
        ++state.revision;
        render();
        bindings = d::default_hotkeys();
        for (int i = 0; i < 3; ++i)
            set_text(hotkey_key + i, bindings[i].gesture.label().c_str());
        for (int i = 0; i < 6; ++i) {
            TabCtrl_SetCurSel(tab, i);
            show_page(i);
            UpdateWindow(hwnd);
            capture_png(report.parent_path() / (L"native-populated-" + std::to_wstring(i) + L".png"));
        }
        offline = true;
        state = {};
        ++state.revision;
        sync_cache.clear();
        rendered_revision = ~0ull;
        render();
        bindings = d::default_hotkeys();
        for (int i = 0; i < 3; ++i) {
            set_text(hotkey_key + i, bindings[i].gesture.label().c_str());
        }
        set_feedback(L"离线自检完成；未连接耳机或调用更新网络。");
        for (int i = 0; i < 7; ++i) {
            TabCtrl_SetCurSel(tab, i);
            show_page(i);
            check_result("page-" + std::to_string(i), page == i);
            UpdateWindow(hwnd);
            capture_png(report.parent_path() / (L"native-ui-" + std::to_wstring(i) + L".png"));
        }
        show_page(0);
        TabCtrl_SetCurSel(tab, 0);
        UpdateWindow(hwnd);
        auto screenshot = report.parent_path() / L"native-ui.png";
        capture_png(screenshot);
        std::ofstream out(report);
        out << "{\"Passed\":" << (passed ? "true" : "false")
            << ",\"HardwareVerified\":false,\"BluetoothApisCalled\":false,\"ExternalNetworkCalled\":false,"
               "\"Controls\":"
            << control_count << ",\"Pages\":7,\"NativeExecutable\":true,\"Scenarios\":[";
        for (std::size_t i = 0; i < checks.size(); ++i) {
            if (i)
                out << ',';
            out << "{\"Name\":\"" << checks[i].first
                << "\",\"Passed\":" << (checks[i].second ? "true" : "false") << '}';
        }
        out << "]}";
        return passed && out.good();
    }
    bool pre_translate(const MSG &message) {
        if (capturing < 0)
            return false;
        if (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN ||
            message.message == WM_KEYUP || message.message == WM_SYSKEYUP) {
            bool down = message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN;
            int key = static_cast<int>(message.wParam);
            if (key == VK_CONTROL)
                key = (message.lParam & (1ll << 24)) ? VK_RCONTROL : VK_LCONTROL;
            else if (key == VK_MENU)
                key = (message.lParam & (1ll << 24)) ? VK_RMENU : VK_LMENU;
            else if (key == VK_SHIFT)
                key = static_cast<int>(MapVirtualKeyW((message.lParam >> 16) & 0xff, MAPVK_VSC_TO_VK_EX));
            auto result = capture.feed(key, down, (message.lParam & (1ll << 30)) != 0 && down);
            set_text(hotkey_key + capturing, result.preview.c_str());
            if (result.finished) {
                int row = capturing;
                if (result.gesture) {
                    bindings[row].gesture = *result.gesture;
                }
                capture.end();
                capturing = -1;
                set_text(hotkey_key + row, bindings[row].gesture.label().c_str());
                SetFocus(GetDlgItem(hwnd, save_hotkeys));
                set_feedback(result.message.empty() ? L"热键已录入，点击保存后生效。" : result.message);
            }
            return true;
        }
        return false;
    }

  private:
    struct Control {
        HWND handle;
        int page, x, y, w, h;
    };
    std::vector<Control> controls;
    HWND tab{}, status{}, readout{}, local_readout{}, touch_readout{}, codec_readout{}, diag_readout{},
        source_readout{};
    struct TouchConfirmation {
        std::uint64_t link{};
        p::Byte key{}, preset{};
    };
    std::optional<TouchConfirmation> touch_confirmation;
    struct SliderIntent {
        int id{}, value{};
        std::uint64_t link{};
        ULONGLONG due{};
        std::wstring device, endpoint;
    };
    std::optional<SliderIntent> pending_slider;
    int editing_slider{-1};
    struct RadioGroup {
        int page{}, x{}, y{}, width{}, columns{}, base{}, height{}, scroll{};
        HWND panel{};
        int current{-1};
        bool enabled{};
        std::vector<std::pair<std::wstring, int>> choices;
        std::vector<HWND> buttons;
    };
    std::map<int, RadioGroup> radio_groups;
    std::array<HWND, 6> feature_status{};
    HFONT font{};
    UINT dpi{96};
    int page{};
    bool background_work{};
    bool close_to_tray{}, tray_added{}, hold{}, hold_key{};
    ULONGLONG hold_started{};
    std::uint64_t hold_link{};
    NOTIFYICONDATAW tray{};
    std::filesystem::path root;
    std::unique_ptr<sc::DeviceController> controller;
    std::unique_ptr<d::DiagnosticLog> diagnostics;
    std::unique_ptr<d::GlobalHotkeys> hotkeys;
    std::unique_ptr<d::LocalPlaybackBackend> playback;
    std::thread worker;
    std::mutex queue_mutex, data_mutex;
    std::condition_variable queue_cv;
    std::deque<std::function<void()>> queue;
    bool stop{};
    std::vector<sc::DeviceDescriptor> found;
    std::vector<sc::DeviceDescriptor> displayed_found;
    unsigned discovery_rebuilds{};
    std::map<HWND, std::array<int, 4>> layout_cache;
    unsigned ui_mutations{};
    struct ComboIntent {
        int id{}, value{};
        std::uint64_t link{};
        std::string source;
    };
    std::map<int, ComboIntent> combo_intents;
    std::set<int> changed_open_combos;
    std::set<int> canceled_combos;
    std::map<std::string, int> source_tokens;
    std::map<int, std::string> source_addresses;
    sc::DeviceSnapshot state;
    d::LocalPlaybackSnapshot local;
    std::wstring feedback = L"就绪";
    std::uint64_t rendered_revision = ~0ull;
    unsigned tick{};
    d::HotkeyBindings bindings = d::default_hotkeys();
    d::HotkeyCapture capture;
    int capturing{-1};
    std::map<int, std::wstring> sync_cache;
    std::optional<int> last_enabled_eq;
    std::uint64_t last_eq_link{};
    std::optional<int> immediate_control;
    unsigned ui_dispatches{};
    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto self = reinterpret_cast<Window *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            self = static_cast<Window *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            self->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(h, m, w, l);
        try {
            return self->message(m, w, l);
        } catch (const winrt::hresult_error &e) {
            self->set_feedback(L"Windows 操作失败：" + std::wstring(e.message()));
            return 0;
        } catch (const std::exception &e) {
            self->set_feedback(L"操作失败：" + wide(e.what()));
            return 0;
        } catch (...) {
            self->set_feedback(L"操作失败：未知异常。");
            return 0;
        }
    }
    int px(int n) const { return MulDiv(n, static_cast<int>(dpi), 96); }
    HWND add(const wchar_t *cls, const wchar_t *text, int id, int pg, int x, int y, int w, int h,
             DWORD style = 0) {
        DWORD base = WS_CHILD | (pg < 0 || pg == page ? WS_VISIBLE : 0);
        if (wcscmp(cls, L"STATIC") != 0)
            base |= WS_TABSTOP;
        HWND child =
            CreateWindowExW(wcscmp(cls, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0, cls, text, base | style, px(x),
                            px(y), px(w), px(h), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                            GetModuleHandleW(nullptr), nullptr);
        if (!child)
            throw std::runtime_error("Cannot create child control");
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        controls.push_back({child, pg, x, y, w, h});
        return child;
    }
    HWND label(const wchar_t *text, int pg, int x, int y, int w = 900, int h = 30) {
        return add(L"STATIC", text, 0, pg, x, y, w, h);
    }
    HWND button(const wchar_t *text, int id, int pg, int x, int y, int w = 120) {
        return add(L"BUTTON", text, id, pg, x, y, w, 36, BS_PUSHBUTTON);
    }
    HWND check(const wchar_t *text, int id, int pg, int x, int y, int w = 170) {
        return add(L"BUTTON", text, id, pg, x, y, w, 34, BS_AUTOCHECKBOX);
    }
    HWND combo(int id, int pg, int x, int y, int w) {
        return add(WC_COMBOBOXW, L"", id, pg, x, y, w, 240, CBS_DROPDOWNLIST | WS_VSCROLL);
    }
    HWND slider(int id, int pg, int x, int y, int w, int min, int max) {
        auto h = add(TRACKBAR_CLASSW, L"", id, pg, x, y, w, 38, TBS_AUTOTICKS);
        SendMessageW(h, TBM_SETRANGE, TRUE, MAKELPARAM(min, max));
        SetWindowSubclass(h, slider_proc, id, reinterpret_cast<DWORD_PTR>(this));
        return h;
    }
    static LRESULT CALLBACK slider_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
        auto self = reinterpret_cast<Window *>(data);
        bool navigation = w == VK_LEFT || w == VK_RIGHT || w == VK_UP || w == VK_DOWN || w == VK_PRIOR ||
                          w == VK_NEXT || w == VK_HOME || w == VK_END;
        if ((m == WM_LBUTTONDOWN || (m == WM_KEYDOWN && navigation)) && IsWindowEnabled(h))
            self->editing_slider = static_cast<int>(id);
        auto result = DefSubclassProc(h, m, w, l);
        if ((m == WM_LBUTTONUP || m == WM_CAPTURECHANGED || m == WM_KILLFOCUS ||
             (m == WM_KEYUP && navigation)) &&
            self->editing_slider == static_cast<int>(id)) {
            self->editing_slider = -1;
            self->apply_slider(static_cast<int>(id));
        }
        return result;
    }
    void radio_group(int id, int pg, int x, int y, int width, int columns, int height = 29) {
        int base = 1000 + static_cast<int>(radio_groups.size()) * 256;
        RadioGroup group;
        group.page = pg;
        group.x = x;
        group.y = y;
        group.width = width;
        group.columns = columns;
        group.base = base;
        group.height = height;
        group.panel = add(L"STATIC", L"", id, pg, x, y, width, height, WS_VSCROLL);
        SetWindowLongPtrW(group.panel, GWL_EXSTYLE,
                          GetWindowLongPtrW(group.panel, GWL_EXSTYLE) | WS_EX_CONTROLPARENT);
        radio_groups[id] = std::move(group);
        SetWindowSubclass(radio_groups.at(id).panel, radio_scroll_proc, id,
                          reinterpret_cast<DWORD_PTR>(this));
    }
    static LRESULT CALLBACK radio_scroll_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id,
                                              DWORD_PTR data) {
        auto self = reinterpret_cast<Window *>(data);
        auto &group = self->radio_groups.at(static_cast<int>(id));
        if (m == WM_COMMAND) {
            SendMessageW(self->hwnd, m, w, l);
            return 0;
        }
        if (m == WM_VSCROLL || m == WM_MOUSEWHEEL) {
            int position = group.scroll;
            if (m == WM_MOUSEWHEEL)
                position -= GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * 29;
            else
                switch (LOWORD(w)) {
                case SB_LINEUP:
                    position -= 29;
                    break;
                case SB_LINEDOWN:
                    position += 29;
                    break;
                case SB_PAGEUP:
                    position -= group.height;
                    break;
                case SB_PAGEDOWN:
                    position += group.height;
                    break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION:
                    position = HIWORD(w);
                    break;
                }
            int rows = (static_cast<int>(group.choices.size()) + group.columns - 1) / group.columns;
            group.scroll = std::clamp(position, 0, std::max(0, rows * 29 - group.height));
            SetScrollPos(h, SB_VERT, group.scroll, TRUE);
            for (std::size_t i = 0; i < group.buttons.size(); ++i)
                SetWindowPos(
                    group.buttons[i], nullptr,
                    self->px(static_cast<int>(i % group.columns) * (group.width - 20) / group.columns),
                    self->px(static_cast<int>(i / group.columns) * 29 - group.scroll), 0, 0,
                    SWP_NOSIZE | SWP_NOZORDER);
            return 0;
        }
        return DefSubclassProc(h, m, w, l);
    }
    static LRESULT CALLBACK radio_key_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR group_id,
                                           DWORD_PTR data) {
        auto self = reinterpret_cast<Window *>(data);
        if (m == WM_GETDLGCODE)
            return DefSubclassProc(h, m, w, l) | DLGC_WANTARROWS;
        if (m == WM_KEYDOWN && (w == VK_UP || w == VK_DOWN || w == VK_LEFT || w == VK_RIGHT)) {
            auto &group = self->radio_groups.at(static_cast<int>(group_id));
            auto it = std::find(group.buttons.begin(), group.buttons.end(), h);
            if (it != group.buttons.end() && !group.buttons.empty()) {
                int index = static_cast<int>(it - group.buttons.begin());
                int direction = w == VK_UP || w == VK_LEFT ? -1 : 1;
                index = (index + direction + static_cast<int>(group.buttons.size())) %
                        static_cast<int>(group.buttons.size());
                auto next = group.buttons[index];
                SetFocus(next);
                SendMessageW(self->hwnd, WM_COMMAND, MAKEWPARAM(group.base + index, BN_CLICKED),
                             reinterpret_cast<LPARAM>(next));
                int row_top = index / group.columns * 29;
                if (row_top < group.scroll || row_top + 29 > group.scroll + group.height)
                    SendMessageW(group.panel, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, row_top), 0);
            }
            return 0;
        }
        if (m == WM_MOUSEWHEEL)
            return SendMessageW(GetParent(h), m, w, l);
        return DefSubclassProc(h, m, w, l);
    }
    void sync_radios(int id, const std::vector<std::pair<std::wstring, int>> &choices, int current) {
        auto &group = radio_groups.at(id);
        bool changed = group.choices != choices;
        if (changed) {
            for (auto child : group.buttons) {
                layout_cache.erase(child);
                std::erase_if(controls, [child](auto &c) { return c.handle == child; });
                DestroyWindow(child);
            }
            group.buttons.clear();
            group.choices = choices;
            group.scroll = 0;
            int column_width = (group.width - 20) / group.columns;
            for (std::size_t i = 0; i < choices.size(); ++i) {
                auto child = add(L"BUTTON", choices[i].first.c_str(), group.base + static_cast<int>(i),
                                 group.page, static_cast<int>(i % group.columns) * column_width,
                                 static_cast<int>(i / group.columns) * 29, column_width - 8, 28,
                                 BS_RADIOBUTTON | (i == 0 ? WS_GROUP : 0));
                SetParent(child, group.panel);
                SetWindowSubclass(child, radio_key_proc, id, reinterpret_cast<DWORD_PTR>(this));
                SetWindowPos(child, nullptr, px(static_cast<int>(i % group.columns) * column_width),
                             px(static_cast<int>(i / group.columns) * 29), 0, 0, SWP_NOSIZE | SWP_NOZORDER);
                set_enabled(child, group.enabled);
                group.buttons.push_back(child);
            }
            int rows = (static_cast<int>(choices.size()) + group.columns - 1) / group.columns;
            SCROLLINFO info{sizeof(info),
                            SIF_RANGE | SIF_PAGE | SIF_POS,
                            0,
                            std::max(0, rows * 29 - 1),
                            static_cast<UINT>(group.height),
                            0};
            SetScrollInfo(group.panel, SB_VERT, &info, TRUE);
        }
        if (changed || current != group.current) {
            group.current = current;
            for (std::size_t i = 0; i < group.buttons.size(); ++i)
                set_checked(group.buttons[i], choices[i].second == current ? BST_CHECKED : BST_UNCHECKED);
        }
        update_radio_tabstop(group);
    }
    void update_radio_tabstop(RadioGroup &group) {
        bool any = std::any_of(group.buttons.begin(), group.buttons.end(), [](HWND child) {
            return SendMessageW(child, BM_GETCHECK, 0, 0) == BST_CHECKED;
        });
        for (std::size_t i = 0; i < group.buttons.size(); ++i) {
            auto child = group.buttons[i];
            auto style = GetWindowLongPtrW(child, GWL_STYLE) & ~WS_TABSTOP;
            if (SendMessageW(child, BM_GETCHECK, 0, 0) == BST_CHECKED || (!any && i == 0))
                style |= WS_TABSTOP;
            if (GetWindowLongPtrW(child, GWL_STYLE) != style) {
                SetWindowLongPtrW(child, GWL_STYLE, style);
                ++ui_mutations;
            }
        }
    }
    void select_radio(int id, int value) {
        auto &group = radio_groups.at(id);
        for (std::size_t i = 0; i < group.buttons.size(); ++i)
            set_checked(group.buttons[i], group.choices[i].second == value ? BST_CHECKED : BST_UNCHECKED);
        update_radio_tabstop(group);
    }
    void sync_cycle(bool force = false) {
        auto signature =
            std::to_wstring(state.link) + L":" +
            (state.touch.cycle.known() ? std::to_wstring(*state.touch.cycle.value) : L"unknown") +
            (state.touch.cycle_writable ? L":write" : L":read");
        if (!force && sync_cache[apply_cycle] == signature)
            return;
        sync_cache[apply_cycle] = signature;
        auto value = state.touch.cycle.known() ? *state.touch.cycle.value : 0;
        set_checked(cycle_noise, value == 1 || value == 2 || value == 3 ? BST_CHECKED : BST_UNCHECKED);
        set_checked(cycle_ambient, value == 1 || value == 2 || value == 4 ? BST_CHECKED : BST_UNCHECKED);
        set_checked(cycle_off, value == 1 || value == 3 || value == 4 ? BST_CHECKED : BST_UNCHECKED);
    }
    int cycle_selection() const {
        bool n = IsDlgButtonChecked(hwnd, cycle_noise) == BST_CHECKED,
             a = IsDlgButtonChecked(hwnd, cycle_ambient) == BST_CHECKED,
             o = IsDlgButtonChecked(hwnd, cycle_off) == BST_CHECKED;
        return n && a && o ? 1 : n && a ? 2 : n && o ? 3 : a && o ? 4 : 0;
    }
    void submit_cycle() {
        auto value = cycle_selection();
        if (!value) {
            sync_cycle(true);
            set_feedback(L"至少选择两个模式，已恢复上次确认的组合。");
            return;
        }
        if (state.touch.cycle.known() && *state.touch.cycle.value == value)
            return;
        immediate_control = cycle_noise;
        enqueue([this, value] { result(controller->set(p::Feature::AmbientCycle, value)); });
    }
    void submit_touch() {
        auto k = selection(touch_key), value = selection(touch_preset);
        if (k < 0 || static_cast<std::size_t>(k) >= state.touch.keys.size() || value < 0)
            return;
        if (state.touch.assignments.known() &&
            static_cast<std::size_t>(k) < state.touch.assignments.value->size() &&
            (*state.touch.assignments.value)[k] == value)
            return;
        TouchConfirmation intent{state.link, state.touch.keys[k].id, static_cast<p::Byte>(value)};
        immediate_control = touch_preset;
        enqueue([this, intent] {
            auto r = controller->snapshot().link != intent.link
                         ? sc::Result{sc::ResultCode::Canceled, "设备会话已变化，请重新选择。"}
                         : controller->set_touch(intent.key, intent.preset);
            if (r.code == sc::ResultCode::ConfirmationRequired) {
                std::lock_guard lock(data_mutex);
                touch_confirmation = intent;
            }
            result(r);
        });
    }
    void submit_custom() {
        auto a = selection(custom_action), f = selection(custom_function);
        auto preset = selected_touch_preset();
        if (!preset || a < 0 || f < 0 || !state.touch.details.known())
            return;
        auto details = *state.touch.details.value;
        bool replaced = false;
        for (auto &candidate : details)
            if (candidate.preset == preset->id)
                for (auto &action : candidate.actions)
                    if (action.action == a && action.function != f) {
                        action.function = static_cast<p::Byte>(f);
                        replaced = true;
                    }
        if (!replaced)
            return;
        immediate_control = custom_function;
        enqueue([this, details] { result(controller->set_touch_details(details)); });
    }
    void set_text(HWND handle, const std::wstring &value) {
        if (handle && window_text(handle) != value) {
            ::SetWindowTextW(handle, value.c_str());
            ++ui_mutations;
        }
    }
    void set_text(int id, const std::wstring &value) { set_text(GetDlgItem(hwnd, id), value); }
    void set_enabled(HWND handle, bool value) {
        if (handle && (IsWindowEnabled(handle) != FALSE) != value) {
            EnableWindow(handle, value);
            ++ui_mutations;
        }
    }
    void set_visible(HWND handle, bool value) {
        if (handle && ((GetWindowLongPtrW(handle, GWL_STYLE) & WS_VISIBLE) != 0) != value) {
            ShowWindow(handle, value ? SW_SHOW : SW_HIDE);
            ++ui_mutations;
        }
    }
    void set_checked(HWND handle, int value) {
        if (handle && SendMessageW(handle, BM_GETCHECK, 0, 0) != value) {
            SendMessageW(handle, BM_SETCHECK, value, 0);
            ++ui_mutations;
        }
    }
    void set_checked(int id, int value) { set_checked(GetDlgItem(hwnd, id), value); }
    void position(HWND handle, int x, int y, int width, int height) {
        std::array<int, 4> requested{x, y, width, height};
        if (auto it = layout_cache.find(handle); it != layout_cache.end() && it->second == requested)
            return;
        layout_cache[handle] = requested;
        SetWindowPos(handle, nullptr, x, y, width, height, SWP_NOZORDER);
        ++ui_mutations;
    }
    void item(HWND h, const std::wstring &text, LPARAM data) {
        auto n = SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        SendMessageW(h, CB_SETITEMDATA, n, data);
    }
    void sync_discovered_devices() {
        std::vector<sc::DeviceDescriptor> latest;
        {
            std::lock_guard lock(data_mutex);
            latest = found;
        }
        bool unchanged = latest.size() == displayed_found.size() &&
                         std::equal(latest.begin(), latest.end(), displayed_found.begin(),
                                    [](const auto &a, const auto &b) {
                                        return a.address == b.address && a.name == b.name;
                                    });
        if (unchanged) {
            displayed_found = std::move(latest);
            return;
        }
        auto cb = GetDlgItem(hwnd, devices);
        if (SendMessageW(cb, CB_GETDROPPEDSTATE, 0, 0))
            return; // Preserve both the open popup and its current item mapping.
        std::optional<std::uint64_t> selected_address;
        auto selected = selection(devices);
        if (selected >= 0 && static_cast<std::size_t>(selected) < displayed_found.size())
            selected_address = displayed_found[selected].address;
        bool visible = (GetWindowLongPtrW(cb, GWL_STYLE) & WS_VISIBLE) != 0;
        if (visible)
            SendMessageW(cb, WM_SETREDRAW, FALSE, 0);
        SendMessageW(cb, CB_RESETCONTENT, 0, 0);
        displayed_found = std::move(latest);
        int next = selected_address ? -1 : displayed_found.empty() ? -1 : 0;
        for (std::size_t i = 0; i < displayed_found.size(); ++i) {
            item(cb, wide(displayed_found[i].name), static_cast<LPARAM>(i));
            if (selected_address == displayed_found[i].address)
                next = static_cast<int>(i);
        }
        SendMessageW(cb, CB_SETCURSEL, next, 0);
        if (visible) {
            SendMessageW(cb, WM_SETREDRAW, TRUE, 0);
            RedrawWindow(cb, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
        }
        ++discovery_rebuilds;
    }
    LPARAM selection(int id) const {
        if (auto it = radio_groups.find(id); it != radio_groups.end()) {
            const auto &group = it->second;
            for (std::size_t i = 0; i < group.buttons.size(); ++i)
                if (SendMessageW(group.buttons[i], BM_GETCHECK, 0, 0) == BST_CHECKED)
                    return group.choices[i].second;
            return -1;
        }
        auto h = GetDlgItem(hwnd, id);
        auto n = SendMessageW(h, CB_GETCURSEL, 0, 0);
        return n == CB_ERR ? -1 : SendMessageW(h, CB_GETITEMDATA, n, 0);
    }
    void select_data(HWND h, LPARAM data) {
        auto count = SendMessageW(h, CB_GETCOUNT, 0, 0);
        for (LRESULT i = 0; i < count; ++i)
            if (SendMessageW(h, CB_GETITEMDATA, i, 0) == data) {
                SendMessageW(h, CB_SETCURSEL, i, 0);
                return;
            }
        SendMessageW(h, CB_SETCURSEL, count ? 0 : -1, 0);
    }
    void sync_combo(int id, const std::vector<std::pair<std::wstring, int>> &choices, int current) {
        std::wstring signature = std::to_wstring(current);
        for (auto &choice : choices)
            signature += L"|" + std::to_wstring(choice.second) + L":" + choice.first;
        if (sync_cache[id] == signature)
            return;
        auto cb = GetDlgItem(hwnd, id);
        if (SendMessageW(cb, CB_GETDROPPEDSTATE, 0, 0))
            return;
        sync_cache[id] = signature;
        SendMessageW(cb, CB_RESETCONTENT, 0, 0);
        for (auto &choice : choices)
            item(cb, choice.first, choice.second);
        select_data(cb, current);
        ++ui_mutations;
    }
    void queue_combo_intent(int id) {
        auto value = static_cast<int>(selection(id));
        if (value < 0)
            return;
        ComboIntent intent{id, value, state.link, {}};
        if (id == source_list) {
            auto it = source_addresses.find(value);
            if (it == source_addresses.end())
                return;
            intent.source = it->second;
        }
        combo_intents[id] = std::move(intent);
        PostMessageW(hwnd, combo_commit_message, id, 0);
    }
    void commit_combo_intent(int id) {
        auto it = combo_intents.find(id);
        if (it == combo_intents.end())
            return;
        // Native selection notifications can arrive before the popup has closed.
        if (SendDlgItemMessageW(hwnd, id, CB_GETDROPPEDSTATE, 0, 0))
            return;
        auto intent = it->second;
        combo_intents.erase(it);
        if (controller)
            state = controller->snapshot();
        if (busy || offline || !state.ready || intent.link != state.link) {
            sync_cache.erase(id);
            rendered_revision = ~0ull;
            render();
            return;
        }
        if (id == eq_preset) {
            if (!state.equalizer.writable ||
                (state.equalizer.preset.known() && *state.equalizer.preset.value == intent.value))
                return;
            immediate_control = id;
            enqueue(
                [this, intent] { result(controller->set_equalizer(static_cast<p::Byte>(intent.value))); });
        } else if (id == source_list) {
            bool available = std::any_of(
                state.sources.entries.begin(), state.sources.entries.end(),
                [&](const auto &entry) { return entry.id == intent.source && entry.connection_slot != 0; });
            if (!state.sources.switch_writable || !available ||
                (state.sources.active_source.known() && *state.sources.active_source.value == intent.source))
                return;
            immediate_control = id;
            enqueue([this, intent] { result(controller->switch_source(intent.source)); });
        }
    }
    void sync_slider(int id, std::optional<int> value) {
        if (id == editing_slider || (pending_slider && pending_slider->id == id) || immediate_control == id)
            return;
        if (!value)
            return;
        auto signature = std::to_wstring(*value);
        if (sync_cache[id] == signature)
            return;
        sync_cache[id] = signature;
        if (SendDlgItemMessageW(hwnd, id, TBM_GETPOS, 0, 0) != *value) {
            SendDlgItemMessageW(hwnd, id, TBM_SETPOS, TRUE, *value);
            ++ui_mutations;
        }
    }
    void update_slider_labels() {
        for (int i = 0; i < 6; ++i) {
            auto value = static_cast<int>(SendDlgItemMessageW(hwnd, eq_band + i, TBM_GETPOS, 0, 0));
            auto text = (value > 0 ? L"+" : L"") + std::to_wstring(value);
            bool known = i == 5 ? state.equalizer.clear_bass.known()
                                : state.equalizer.bands.known() &&
                                      static_cast<std::size_t>(i) < state.equalizer.bands.value->size();
            set_text(eq_value + i, known ? text.c_str() : L"—");
        }
        auto level = static_cast<int>(SendDlgItemMessageW(hwnd, ambient_level, TBM_GETPOS, 0, 0));
        set_text(ambient_value,
                 state.sound.ambient_level.known() ? std::to_wstring(level).c_str() : L"尚未读取");
    }
    void apply_slider(int id, bool final = true) {
        update_slider_labels();
        if (!IsWindowEnabled(GetDlgItem(hwnd, id)))
            return;
        auto value = static_cast<int>(SendDlgItemMessageW(hwnd, id, TBM_GETPOS, 0, 0));
        SliderIntent intent{id, value, state.link,
                            final ? 0 : GetTickCount64() + (id == local_volume ? 80 : 120)};
        if (id == local_volume) {
            std::lock_guard lock(data_mutex);
            intent.device = state.device ? state.device->aep_id : L"";
            intent.endpoint = local.volume.endpoint_id;
        }
        pending_slider = std::move(intent);
        if (final)
            flush_slider();
        else
            SetTimer(hwnd, 3, id == local_volume ? 80 : 120, nullptr);
    }
    void flush_slider() {
        if (!pending_slider || busy)
            return;
        if (pending_slider->due && GetTickCount64() < pending_slider->due)
            return;
        KillTimer(hwnd, 3);
        auto intent = *pending_slider;
        pending_slider.reset();
        if (offline || intent.link != state.link || (intent.id != local_volume && !state.ready)) {
            restore_slider(intent.id);
            return;
        }
        auto id = intent.id, value = intent.value;
        if (id == ambient_level) {
            if (!state.sound.writable || !state.sound.mode.known() ||
                *state.sound.mode.value != p::SoundMode::Ambient)
                return restore_slider(id);
            if (!state.sound.ambient_level.known() || *state.sound.ambient_level.value != value) {
                immediate_control = id;
                enqueue([this, value] { result(controller->set_sound({}, value)); });
            }
        } else if (id >= eq_band && id < eq_band + 6) {
            int index = id - eq_band;
            bool changed =
                index == 5 ? !state.equalizer.clear_bass.known() || *state.equalizer.clear_bass.value != value
                           : !state.equalizer.bands.known() ||
                                 static_cast<std::size_t>(index) >= state.equalizer.bands.value->size() ||
                                 (*state.equalizer.bands.value)[index] != value;
            if (changed) {
                immediate_control = id;
                enqueue([this, index, value] {
                    result(index == 5 ? controller->set_equalizer({}, {}, {}, value)
                                      : controller->set_equalizer({}, index, value));
                });
            }
        } else if (id == volume_prompt) {
            if (!state.touch.volume_writable) {
                restore_slider(id);
                return;
            }
            if (!state.touch.prompt_volume.known() || *state.touch.prompt_volume.value != value) {
                immediate_control = id;
                enqueue([this, value] { result(controller->set(p::Feature::PromptVolume, value)); });
            }
        } else if (id == local_volume) {
            {
                std::lock_guard lock(data_mutex);
                if (local.volume.endpoint_id == intent.endpoint &&
                    static_cast<int>(local.volume.percent) == intent.value)
                    return;
            }
            immediate_control = id;
            enqueue([this, intent] {
                desktop_result(playback->set_volume(intent.device, intent.endpoint, intent.value));
                refresh_local();
            });
        }
    }
    void restore_slider(int id) {
        if (editing_slider == id)
            editing_slider = -1;
        sync_cache.erase(id);
        rendered_revision = ~0ull;
        render();
    }
    void enabled(int id, bool value) {
        if (auto it = radio_groups.find(id); it != radio_groups.end()) {
            it->second.enabled = value && !busy;
            for (auto child : it->second.buttons)
                set_enabled(child, value && !busy);
        } else {
            bool live_slider = id == editing_slider && immediate_control == id && !offline;
            if (live_slider) {
                value = id == ambient_level
                            ? state.ready && state.connected && state.sound.writable &&
                                  state.sound.mode.known() && *state.sound.mode.value == p::SoundMode::Ambient
                        : id == volume_prompt ? state.ready && state.connected && state.touch.volume_writable
                                              : value;
            }
            set_enabled(GetDlgItem(hwnd, id), value && (!busy || live_slider));
        }
    }
    void build_controls() {
        dpi = GetDpiForWindow(hwnd);
        font =
            CreateFontW(-px(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        label(L"SonyConnect · 原生 C++ 功能验证", -1, 24, 12, 600, 32);
        button(L"隐藏到托盘", hide_window, -1, 680, 10, 130);
        button(L"退出", exit_app, -1, 830, 10, 90);
        label(offline ? L"离线界面检查 · 未调用蓝牙或更新网络"
                      : L"C++ 实机验收待完成 · 控制通道请与手机 Sony App 交替使用",
              -1, 24, 50, 900, 30);
        tab = add(WC_TABCONTROLW, L"", tabs, -1, 20, 87, 930, 36);
        const wchar_t *names[] = {L"连接与声音", L"均衡器",     L"双设备与固定", L"扩展设置",
                                  L"触摸与提示", L"本机与热键", L"诊断与更新"};
        for (int i = 0; i < 7; ++i) {
            TCITEMW ti{};
            ti.mask = TCIF_TEXT;
            ti.pszText = const_cast<wchar_t *>(names[i]);
            TabCtrl_InsertItem(tab, i, &ti);
        }
        label(L"选择当前连接的耳机", 0, 28, 136, 180);
        combo(devices, 0, 210, 130, 700);
        button(L"发现设备", discover, 0, 28, 174);
        button(L"连接", connect_device, 0, 158, 174);
        button(L"重新读取", refresh, 0, 288, 174);
        button(L"断开", disconnect_device, 0, 418, 174);
        readout = label(L"尚未连接", 0, 28, 220, 880, 220);
        button(L"降噪", noise, 0, 28, 450);
        button(L"通透", ambient, 0, 158, 450);
        button(L"关闭", sound_off, 0, 288, 450);
        label(L"通透等级", 0, 28, 505, 170);
        slider(ambient_level, 0, 190, 496, 440, 1, 20);
        label(L"", 0, 660, 505, 120);
        add(L"STATIC", L"—", ambient_value, 0, 660, 505, 150, 30);
        check(L"关注语音", voice_focus, 0, 28, 548);
        label(L"设置提交后等待耳机回读确认；未知读数不推测为关闭或零。", 0, 28, 594, 870, 45);
        label(L"均衡器预设", 1, 28, 139, 180);
        combo(eq_preset, 1, 210, 130, 680);
        const wchar_t *band_names[] = {L"400 Hz", L"1 kHz", L"2.5 kHz", L"6.3 kHz", L"16 kHz", L"CLEAR BASS"};
        for (int i = 0; i < 6; ++i) {
            label(band_names[i], 1, 28, 190 + i * 58, 180);
            slider(eq_band + i, 1, 210, 178 + i * 58, 470, -10, 10);
            add(L"STATIC", L"0", eq_value + i, 1, 720, 190 + i * 58, 100, 30);
            label(L"−10", 1, 207, 212 + i * 58, 44, 22);
            label(L"0", 1, 439, 212 + i * 58, 44, 22);
            label(L"+10", 1, 646, 212 + i * 58, 55, 22);
        }
        label(L"选择预设即应用；滑块松开即提交。自定义 1／2 保留各自预设，内置预设修改后转为手动。", 1, 28,
              566, 880, 64);
        check(L"同时连接两台设备", multipoint, 2, 28, 132, 260);
        combo(source_list, 2, 28, 186, 680);
        label(L"选择音源即请求切换，显示状态以耳机回读为准。", 2, 28, 236, 880, 40);
        source_readout = add(L"EDIT", L"尚未读取连接设备", 0, 2, 28, 287, 880, 145,
                             ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL);
        auto fix = button(L"按住 1 秒固定当前音源", fix_source, 2, 28, 450, 350);
        SetWindowSubclass(fix, hold_proc, 1, reinterpret_cast<DWORD_PTR>(this));
        button(L"解除固定", release_source, 2, 410, 450, 170);
        label(L"提前松开、移出、Esc、切换页面或断开会取消；通话／语音助手可能拒绝固定。", 2, 28, 499, 880,
              60);
        label(L"设备注册记录保存在耳机中。未连接的已注册设备会继续列出。", 2, 28, 579, 880, 60);
        for (int i = 0; i < 6; ++i) {
            label(feature_rows[i].second, 3, 28, 136 + i * 80, 210);
            if (i < 5)
                radio_group(feature_choice + i, 3, 245, 130 + i * 80, 490, 2, i == 4 ? 58 : 29);
            if (i == 5)
                button(L"关闭耳机", feature_apply + i, 3, 750, 130 + i * 80, 140);
            feature_status[i] = label(L"尚未读取", 3, 245, (i == 4 ? 512 : 165 + i * 80), 480, 27);
        }
        codec_readout = label(L"当前编码：尚未读取", 3, 28, 598, 880, 65);
        label(L"触摸按键", 4, 28, 136, 120);
        radio_group(touch_key, 4, 160, 130, 530, 3);
        label(L"触摸功能", 4, 28, 180, 120);
        radio_group(touch_preset, 4, 160, 175, 750, 2, 87);
        touch_readout = label(L"分配前核对关联语音助手。", 4, 28, 266, 880, 30);
        add(L"STATIC", L"高级触摸动作", custom_action_label, 4, 28, 305, 120, 30);
        radio_group(custom_action, 4, 160, 300, 740, 4);
        add(L"STATIC", L"动作功能", custom_function_label, 4, 28, 350, 120, 30);
        radio_group(custom_function, 4, 160, 345, 550, 2, 72);
        check(L"降噪", cycle_noise, 4, 28, 426, 160);
        check(L"通透", cycle_ambient, 4, 220, 426, 140);
        check(L"关闭", cycle_off, 4, 380, 426, 140);
        add(L"STATIC", L"至少选择两个模式；勾选后立即提交。", cycle_hint, 4, 28, 461, 840, 26);
        label(L"语音指导和通知", 4, 28, 491, 170);
        radio_group(guidance, 4, 210, 484, 480, 2);
        label(L"提示音量", 4, 28, 532, 120);
        slider(volume_prompt, 4, 160, 520, 480, -2, 2);
        label(L"语音助手", 4, 28, 575, 120);
        radio_group(assistant, 4, 160, 568, 550, 2, 58);
        button(L"恢复关联备份", restore_linked, 4, 28, 630, 220);
        label(L"助手和触摸分配可能联动；关闭语音指导后，提示音量不可修改。", 4, 280, 636, 620, 36);
        check(L"关闭窗口时隐藏到托盘", close_tray, 5, 28, 130, 290);
        check(L"开机启动（当前用户）", startup, 5, 370, 130, 330);
        local_readout = label(L"本机端点与媒体：尚未读取", 5, 28, 181, 880, 86);
        slider(local_volume, 5, 28, 266, 620, 0, 100);
        button(L"上一首", previous, 5, 28, 312);
        button(L"播放", play, 5, 158, 312);
        button(L"暂停", pause, 5, 288, 312);
        button(L"下一首", next, 5, 418, 312);
        label(L"热键：点击输入框，按住左／右 Ctrl、Alt、Shift、Caps Lock 或 Tab，再按一个键。", 5, 28, 370,
              900, 42);
        const wchar_t *modes[] = {L"降噪", L"通透", L"关闭"};
        for (int i = 0; i < 3; ++i) {
            label(modes[i], 5, 28, 425 + i * 49, 110);
            add(L"EDIT", bindings[i].gesture.label().c_str(), hotkey_key + i, 5, 160, 415 + i * 49, 530, 36,
                ES_READONLY | ES_AUTOHSCROLL);
        }
        button(L"保存热键", save_hotkeys, 5, 720, 464, 170);
        label(L"只保留按键状态；热键不记录输入内容。键码设置会校验冲突和可用性。", 5, 28, 590, 900, 40);
        label(L"单段日志 MiB（1–8）", 6, 28, 138, 240);
        add(L"EDIT", L"2", log_mib, 6, 280, 130, 80, 34, ES_NUMBER);
        label(L"保留运行数（1–100）", 6, 410, 138, 260);
        add(L"EDIT", L"10", log_runs, 6, 670, 130, 90, 34, ES_NUMBER);
        button(L"应用限制", apply_log, 6, 780, 130, 130);
        diag_readout = label(L"诊断未启用", 6, 28, 184, 880, 78);
        button(L"导出脱敏诊断包", export_log, 6, 28, 265, 240);
        button(L"回放诊断包", replay_log, 6, 300, 265, 220);
        label(L"回放只显示脱敏时间线，不连接耳机、不发送控制指令。", 6, 28, 316, 880, 40);
        label(L"软件更新", 6, 28, 390, 160);
        auto ch = combo(update_channel, 6, 220, 382, 360);
        item(ch, L"GitHub 更新", 0);
        item(ch, L"Gitee 更新", 1);
        select_data(ch, 0);
        button(L"检查更新", check_update, 6, 620, 382, 130);
        button(L"发布页面", release_page, 6, 780, 382, 130);
        label(L"仅手动检测，不自动下载或安装；发布情况和网络错误会如实显示。", 6, 28, 440, 880, 66);
        status = label(L"就绪", -1, 24, 674, 920, 70);
    }
    void layout() {
        RECT r{};
        GetClientRect(hwnd, &r);
        for (auto &c : controls) {
            int w = c.w;
            if (c.handle == tab || c.handle == status || c.handle == readout || c.handle == local_readout ||
                c.handle == touch_readout || c.handle == codec_readout || c.handle == diag_readout)
                w = std::max(c.w, MulDiv(r.right, 96, dpi) - c.x - 30);
            int y = c.handle == status ? std::max(c.y, MulDiv(r.bottom, 96, dpi) - 76) : c.y;
            if (c.page == 4 && GetParent(c.handle) == hwnd && c.y >= 426 &&
                radio_groups.contains(custom_action) && radio_groups.at(custom_action).choices.empty())
                y -= 126;
            for (auto &[id, group] : radio_groups)
                if (GetParent(c.handle) == group.panel)
                    y -= group.scroll;
            position(c.handle, px(c.x), px(y), px(w), px(c.h));
        }
    }
    void show_page(int selected) {
        cancel_hold();
        if (editing_slider >= 0) {
            auto id = editing_slider;
            editing_slider = -1;
            apply_slider(id);
        }
        if (capturing >= 0) {
            set_text(hotkey_key + capturing, bindings[capturing].gesture.label().c_str());
            capturing = -1;
            capture.end();
        }
        page = selected;
        for (auto &c : controls)
            if (c.page >= 0)
                set_visible(c.handle, c.page == selected);
        if (hotkeys)
            hotkeys->pause(selected == 5);
        render();
    }
    void set_feedback(std::wstring text) {
        std::lock_guard lock(data_mutex);
        feedback = std::move(text);
        set_text(status, feedback.c_str());
    }
    void enqueue(std::function<void()> work, bool foreground = true) {
        if (offline) {
            set_feedback(L"离线检查模式，不执行设备／系统变更。");
            return;
        }
        if (busy || (!foreground && background_work)) {
            set_feedback(L"已有操作在等待确认。");
            return;
        }
        if (ui_test) {
            ++ui_dispatches; // Verify UI routing without executing the captured operation.
            return;
        }
        if (foreground)
            busy = true;
        else
            background_work = true;
        if (foreground) {
            render();
            set_feedback(L"正在执行，等待实际结果…");
        }
        {
            std::lock_guard lock(queue_mutex);
            queue.push_back([this, work = std::move(work), foreground] {
                try {
                    work();
                } catch (const winrt::hresult_error &e) {
                    std::lock_guard lock(data_mutex);
                    feedback = L"Windows 操作失败：" + std::wstring(e.message());
                } catch (const std::exception &e) {
                    std::lock_guard lock(data_mutex);
                    feedback = L"操作失败：" + wide(e.what());
                } catch (...) {
                    std::lock_guard lock(data_mutex);
                    feedback = L"操作失败：未知异常。";
                }
                PostMessageW(hwnd, changed_message, foreground ? 1 : 0, 0);
            });
        }
        queue_cv.notify_one();
    }
    void worker_loop() {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            for (;;) {
                std::function<void()> action;
                {
                    std::unique_lock lock(queue_mutex);
                    queue_cv.wait(lock, [this] { return stop || !queue.empty(); });
                    if (stop)
                        break;
                    action = std::move(queue.front());
                    queue.pop_front();
                }
                action();
            }
            playback.reset();
            winrt::uninit_apartment();
        } catch (const winrt::hresult_error &e) {
            std::lock_guard lock(data_mutex);
            feedback = L"系统服务初始化失败：" + std::wstring(e.message());
            PostMessageW(hwnd, changed_message, 1, 0);
        }
    }
    void result(const sc::Result &r) {
        std::lock_guard lock(data_mutex);
        feedback = wide(r.message.empty() ? (r.confirmed() ? "耳机回读确认" : "操作未确认") : r.message);
    }
    void desktop_result(const d::DesktopResult &r) {
        std::lock_guard lock(data_mutex);
        feedback = r.message;
    }
    void refresh_local() {
        if (!playback)
            playback = std::make_unique<d::LocalPlaybackBackend>();
        auto current = controller->snapshot();
        auto snap = playback->read(current.device ? current.device->aep_id : L"");
        std::lock_guard lock(data_mutex);
        local = std::move(snap);
    }
    void render() {
        if (controller)
            state = controller->snapshot();
        bool ready = !offline && state.ready && !state.busy;
        enabled(discover, !offline);
        auto selected_device = selection(devices);
        enabled(connect_device, !offline && selected_device >= 0 &&
                                    static_cast<std::size_t>(selected_device) < displayed_found.size());
        enabled(refresh, ready);
        enabled(disconnect_device, state.connected);
        std::wstringstream out;
        out << L"连接：" << wide(state.status.empty() ? (offline ? "离线" : "未连接") : state.status)
            << L"\r\n型号：" << wide(state.model) << L"    固件：" << wide(state.firmware) << L"\r\n电量：左 "
            << percent_text(state.battery.left) << L" ／右 " << percent_text(state.battery.right) << L" ／盒 "
            << percent_text(state.battery.case_level) << L"\r\n单体电量："
            << percent_text(state.battery.single) << L"\r\n声音模式："
            << (state.sound.mode.known() ? (*state.sound.mode.value == p::SoundMode::NoiseCanceling ? L"降噪"
                                            : *state.sound.mode.value == p::SoundMode::Ambient      ? L"通透"
                                                                                                    : L"关闭")
                                         : L"未知")
            << L"    通透等级：" << value_text(state.sound.ambient_level) << L"\r\n关注语音："
            << value_text(state.sound.voice_focus) << L"\r\n协议及能力："
            << (state.family == p::Family::V2 ? L"MDR V2" : L"尚未确认") << L"    C++ 版尚未实机验收";
        set_text(readout, out.str().c_str());
        for (int id : {noise, ambient, sound_off})
            enabled(id, ready && state.sound.writable);
        bool is_ambient = state.sound.mode.known() && *state.sound.mode.value == p::SoundMode::Ambient;
        enabled(ambient_level, ready && is_ambient && state.sound.writable);
        enabled(apply_ambient, ready && is_ambient && state.sound.writable);
        enabled(voice_focus, ready && is_ambient && state.sound.writable);
        set_checked(voice_focus, state.sound.voice_focus.known() && *state.sound.voice_focus.value
                                     ? BST_CHECKED
                                     : BST_UNCHECKED);
        auto eq_on = state.equalizer.preset.known() && *state.equalizer.preset.value != 0;
        enabled(eq_preset, ready && state.equalizer.writable);
        auto target = state.equalizer.preset.value.value_or(0);
        if (target != 0xa1 && target != 0xa2)
            target = 0xa0;
        bool has_custom = std::any_of(state.equalizer.presets.begin(), state.equalizer.presets.end(),
                                      [target](auto &preset) { return preset.id == target; });
        for (int i = 0; i < 6; ++i) {
            enabled(eq_band + i, ready && eq_on && has_custom && state.equalizer.writable &&
                                     state.equalizer.five_band_layout);
        }
        if (state.link != last_eq_link) {
            last_eq_link = state.link;
            last_enabled_eq.reset();
            sync_cache.clear();
        }
        if (eq_on)
            last_enabled_eq = *state.equalizer.preset.value;
        set_checked(multipoint, state.sources.multipoint.known() && *state.sources.multipoint.value
                                    ? BST_CHECKED
                                    : BST_UNCHECKED);
        enabled(multipoint, ready && state.sources.multipoint_writable);
        enabled(switch_source, ready && state.sources.switch_writable);
        enabled(source_list, ready && state.sources.switch_writable);
        enabled(fix_source, ready && state.touch.fixation_writable && state.sources.active_source.known() &&
                                !(state.touch.playback_fixed.known() && *state.touch.playback_fixed.value));
        enabled(release_source, ready && state.touch.fixation_writable &&
                                    state.touch.playback_fixed.known() && *state.touch.playback_fixed.value);
        for (int i = 0; i < 6; ++i) {
            auto it = state.features.find(feature_rows[i].first);
            bool allow = ready && it != state.features.end() && it->second.writable;
            enabled(feature_choice + i, allow);
            enabled(feature_apply + i, allow);
            set_text(feature_status[i],
                     it == state.features.end()
                         ? L"尚未读取"
                         : (it->second.value.known()
                                ? (L"当前：" + choice_name(it->first, *it->second.value.value)).c_str()
                                : value_text(it->second.value).c_str()));
        }
        set_text(codec_readout, (L"当前编码：" + value_text(state.codec) +
                                 L"\r\n偏好不能强制 Windows 编码；DSEE 自动不代表始终生效。")
                                    .c_str());
        enabled(touch_key, ready && !state.touch.keys.empty());
        enabled(touch_preset, ready && state.touch.writable);
        enabled(apply_touch, ready && state.touch.writable);
        enabled(custom_action, ready && state.touch.writable);
        enabled(custom_function, ready && state.touch.writable && state.touch.details.known());
        enabled(apply_custom, ready && state.touch.writable && state.touch.details.known());
        for (int id : {cycle_noise, cycle_ambient, cycle_off})
            enabled(id, ready && state.touch.cycle_writable);
        enabled(guidance, ready && state.touch.guidance_writable);
        enabled(volume_prompt, ready && state.touch.volume_writable);
        enabled(apply_volume_prompt, ready && state.touch.volume_writable);
        enabled(assistant, ready && state.touch.assistant_writable);
        enabled(apply_assistant, ready && state.touch.assistant_writable);
        enabled(restore_linked, ready && state.touch.linked_backup_assignments.has_value() &&
                                    state.touch.linked_backup_assistant.has_value());
        if (state.revision != rendered_revision) {
            rendered_revision = state.revision;
            populate_state();
        }
        bool show_advanced = page == 4 && !radio_groups.at(custom_action).choices.empty();
        for (int id : {custom_action_label, custom_function_label, custom_action, custom_function})
            set_visible(GetDlgItem(hwnd, id), show_advanced);
        layout();
        update_slider_labels();
        d::LocalPlaybackSnapshot local_copy;
        {
            std::lock_guard lock(data_mutex);
            local_copy = local;
        }
        set_text(local_readout, (L"本机音量：" +
                                 (local_copy.volume.endpoint_id.empty()
                                      ? L"未知／尚未读取"
                                      : local_copy.volume.endpoint_name + L" · " +
                                            std::to_wstring(static_cast<int>(local_copy.volume.percent)) +
                                            L"% · 静音：" + (local_copy.volume.muted ? L"是" : L"否")) +
                                 L" · " + local_copy.volume.message + L"\r\n本机媒体：" +
                                 (local_copy.media.session_id.empty()
                                      ? L"无可用会话"
                                      : local_copy.media.title + L" · " + local_copy.media.application_id +
                                            L" · " + (local_copy.media.playing ? L"播放中" : L"未播放")) +
                                 L" · " + local_copy.media.message)
                                    .c_str());
        if (local_copy.volume.can_write)
            sync_slider(local_volume, static_cast<int>(local_copy.volume.percent));
        enabled(local_volume, !offline && local_copy.volume.can_write);
        enabled(apply_local_volume, !offline && local_copy.volume.can_write);
        enabled(play, !offline && local_copy.media.can_play);
        enabled(pause, !offline && local_copy.media.can_pause);
        enabled(previous, !offline && local_copy.media.can_previous);
        enabled(next, !offline && local_copy.media.can_next);
        for (int id : {close_tray, startup, save_hotkeys, apply_log, export_log, replay_log, check_update,
                       release_page})
            enabled(id, !offline);
        if (diagnostics)
            set_text(diag_readout, diagnostics->status().c_str());
        enabled(hide_window, tray_added);
        if (hold && (!ready || state.link != hold_link))
            cancel_hold();
    }
    void populate_state() {
        auto range = state.sound.ranges.find(
            state.sound.voice_focus.known() && *state.sound.voice_focus.value ? 1 : 0);
        if (range != state.sound.ranges.end()) {
            auto r = range->second;
            if (SendDlgItemMessageW(hwnd, ambient_level, TBM_GETRANGEMIN, 0, 0) != r.minimum ||
                SendDlgItemMessageW(hwnd, ambient_level, TBM_GETRANGEMAX, 0, 0) != r.maximum) {
                SendDlgItemMessageW(hwnd, ambient_level, TBM_SETRANGE, TRUE,
                                    MAKELPARAM(r.minimum, r.maximum));
                ++ui_mutations;
            }
        }
        if (state.sound.ambient_level.known())
            sync_slider(ambient_level, state.sound.ambient_level.value);
        std::vector<std::pair<std::wstring, int>> choices;
        for (const auto &q : state.equalizer.presets)
            choices.emplace_back(wide(q.name), q.id);
        sync_combo(eq_preset, choices, state.equalizer.preset.value.value_or(-1));
        if (state.equalizer.bands.known())
            for (std::size_t i = 0; i < std::min<std::size_t>(5, state.equalizer.bands.value->size()); ++i)
                sync_slider(eq_band + static_cast<int>(i), (*state.equalizer.bands.value)[i]);
        if (state.equalizer.clear_bass.known())
            sync_slider(eq_band + 5, state.equalizer.clear_bass.value);
        choices.clear();
        int active = -1;
        for (std::size_t i = 0; i < state.sources.entries.size(); ++i) {
            if (state.sources.entries[i].connection_slot == 0)
                continue;
            bool current = state.sources.active_source.known() &&
                           *state.sources.active_source.value == state.sources.entries[i].id;
            auto [token, inserted] = source_tokens.try_emplace(state.sources.entries[i].id,
                                                               static_cast<int>(source_tokens.size()));
            if (inserted)
                source_addresses[token->second] = state.sources.entries[i].id;
            if (current)
                active = token->second;
            choices.emplace_back(wide(state.sources.entries[i].name) + (current ? L" · 当前播放" : L""),
                                 token->second);
        }
        sync_combo(source_list, choices, active);
        std::wstringstream sources;
        sources << (state.ready && state.sources.available ? L"当前连接设备"
                                                           : L"上次设备记录（连接状态待确认）")
                << L"\r\n";
        bool any_connected = false;
        for (const auto &entry : state.sources.entries)
            if (entry.connection_slot != 0) {
                any_connected = true;
                sources << L"  " << wide(entry.name)
                        << (state.ready && state.sources.available ? L" · 已连接" : L" · 上次连接")
                        << (state.ready && state.sources.active_source.known() &&
                                    *state.sources.active_source.value == entry.id
                                ? L" · 当前音源"
                                : L"")
                        << L"\r\n";
            }
        if (!any_connected)
            sources << (state.ready && state.sources.available ? L"  暂无已连接的音源设备" : L"  尚未确认")
                    << L"\r\n";
        sources << L"已注册、未连接的设备\r\n";
        bool any_registered = false;
        for (const auto &entry : state.sources.entries)
            if (entry.connection_slot == 0) {
                any_registered = true;
                sources << L"  " << wide(entry.name) << L"\r\n";
            }
        if (!any_registered)
            sources << L"  无记录";
        set_text(source_readout, sources.str().c_str());
        for (int i = 0; i < 6; ++i) {
            choices.clear();
            auto it = state.features.find(feature_rows[i].first);
            if (it != state.features.end())
                for (int v : it->second.choices)
                    choices.emplace_back(choice_name(it->first, v), v);
            if (i < 5)
                sync_radios(feature_choice + i, choices,
                            it != state.features.end() && it->second.value.known() ? *it->second.value.value
                                                                                   : -1);
        }
        choices.clear();
        for (std::size_t i = 0; i < state.touch.keys.size(); ++i)
            choices.emplace_back(touch_key_name(state.touch.keys[i].id), static_cast<int>(i));
        sync_radios(touch_key, choices, choices.empty() ? -1 : 0);
        populate_touch();
        choices.clear();
        for (auto v : state.touch.voice_assistant_candidates)
            choices.emplace_back(assistant_name(v), v);
        sync_radios(assistant, choices,
                    state.touch.voice_assistant.known() ? *state.touch.voice_assistant.value : -1);
        sync_radios(guidance, {{L"开启", 0}, {L"关闭", 1}},
                    state.touch.guidance.known() ? *state.touch.guidance.value : -1);
        set_text(touch_readout,
                 (L"当前助手：" +
                  (state.touch.voice_assistant.known() ? assistant_name(*state.touch.voice_assistant.value)
                                                       : value_text(state.touch.voice_assistant)) +
                  L"；修改后以耳机回读为准。")
                     .c_str());
        sync_cycle();
        if (state.touch.prompt_volume.known())
            sync_slider(volume_prompt, state.touch.prompt_volume.value);
    }
    void populate_touch() {
        std::vector<std::pair<std::wstring, int>> choices;
        int selected = -1;
        auto k = selection(touch_key);
        if (k >= 0 && static_cast<std::size_t>(k) < state.touch.keys.size()) {
            for (auto &pr : state.touch.keys[k].presets)
                choices.emplace_back(preset_name(pr.id), pr.id);
            if (state.touch.assignments.known() &&
                static_cast<std::size_t>(k) < state.touch.assignments.value->size())
                selected = (*state.touch.assignments.value)[k];
        }
        sync_radios(touch_preset, choices, selected);
        populate_actions();
    }
    const p::TouchPreset *selected_touch_preset() const {
        auto k = selection(touch_key), pr = selection(touch_preset);
        if (k < 0 || static_cast<std::size_t>(k) >= state.touch.keys.size())
            return nullptr;
        for (auto &item : state.touch.keys[k].presets)
            if (item.id == pr)
                return &item;
        return nullptr;
    }
    void populate_actions() {
        std::vector<std::pair<std::wstring, int>> choices;
        auto pr = selected_touch_preset();
        if (pr)
            for (auto &a : pr->customizable)
                if (!(a.action == 0 && !a.functions.empty() &&
                      std::all_of(a.functions.begin(), a.functions.end(),
                                  [](auto f) { return f >= 1 && f <= 4; })))
                    choices.emplace_back(action_name(a.action), a.action);
        sync_radios(custom_action, choices, choices.empty() ? -1 : choices.front().second);
        populate_functions();
    }
    void populate_functions() {
        std::vector<std::pair<std::wstring, int>> choices;
        int current = -1;
        auto pr = selected_touch_preset();
        if (pr)
            for (auto &a : pr->customizable)
                if (a.action == selection(custom_action)) {
                    for (auto f : a.functions)
                        choices.emplace_back(function_name(f), f);
                    if (state.touch.details.known())
                        for (auto &detail : *state.touch.details.value)
                            if (detail.preset == pr->id)
                                for (auto &actual : detail.actions)
                                    if (actual.action == a.action)
                                        current = actual.function;
                }
        sync_radios(custom_function, choices, current);
        bool show_advanced = !radio_groups.at(custom_action).choices.empty() && page == 4;
        for (int id : {custom_action_label, custom_function_label, custom_action, custom_function})
            set_visible(GetDlgItem(hwnd, id), show_advanced);
    }
    bool startup_enabled() const {
        HKEY key{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                          KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
            return false;
        DWORD size{};
        auto rc = RegQueryValueExW(key, L"SonyConnect.Native", nullptr, nullptr, nullptr, &size);
        RegCloseKey(key);
        return rc == ERROR_SUCCESS && size > 2;
    }
    void set_startup(bool value) {
        HKEY key{};
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                            nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
            throw std::runtime_error("Cannot open startup setting");
        std::wstring command = L"\"" + executable_path().wstring() + L"\" --start-hidden";
        auto rc = value ? RegSetValueExW(key, L"SonyConnect.Native", 0, REG_SZ,
                                         reinterpret_cast<const BYTE *>(command.c_str()),
                                         static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)))
                        : RegDeleteValueW(key, L"SonyConnect.Native");
        RegCloseKey(key);
        if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND)
            throw std::runtime_error("Cannot save startup setting");
    }
    std::filesystem::path dialog_path(bool save, const wchar_t *filter, const wchar_t *extension) {
        std::array<wchar_t, 32768> file{};
        OPENFILENAMEW ofn{sizeof(ofn)};
        ofn.hwndOwner = hwnd;
        ofn.lpstrFile = file.data();
        ofn.nMaxFile = static_cast<DWORD>(file.size());
        ofn.lpstrFilter = filter;
        ofn.lpstrDefExt = extension;
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
        return (save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn)) ? std::filesystem::path(file.data())
                                                                        : std::filesystem::path{};
    }
    void cancel_hold() {
        if (!hold)
            return;
        hold = false;
        KillTimer(hwnd, 2);
        if (GetCapture() == GetDlgItem(hwnd, fix_source))
            ReleaseCapture();
        set_text(GetDlgItem(hwnd, fix_source), L"按住 1 秒固定当前音源");
    }
    static LRESULT CALLBACK hold_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
        auto self = reinterpret_cast<Window *>(ref);
        if (m == WM_LBUTTONDOWN ||
            (m == WM_KEYDOWN && (w == VK_SPACE || w == VK_RETURN) && !(l & (1ll << 30)))) {
            if (IsWindowEnabled(h)) {
                self->hold = true;
                self->hold_key = m == WM_KEYDOWN;
                self->hold_started = GetTickCount64();
                self->hold_link = self->state.link;
                if (!self->hold_key)
                    SetCapture(h);
                SetTimer(self->hwnd, 2, 50, nullptr);
                self->set_text(h, L"正在等待 1 秒…");
            }
            return 0;
        }
        if (m == WM_LBUTTONUP || m == WM_KEYUP || m == WM_KILLFOCUS || m == WM_CAPTURECHANGED ||
            m == WM_CANCELMODE)
            self->cancel_hold();
        if (m == WM_MOUSEMOVE && self->hold && !self->hold_key) {
            RECT r{};
            GetClientRect(h, &r);
            POINT pt{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
            if (!PtInRect(&r, pt))
                self->cancel_hold();
        }
        return DefSubclassProc(h, m, w, l);
    }
    void command(int id, int notification) {
        bool combo = id == devices || id == eq_preset || id == source_list || id == update_channel;
        bool parameter_combo = id == eq_preset || id == source_list;
        if (combo) {
            if (notification == CBN_DROPDOWN) {
                changed_open_combos.erase(id);
                canceled_combos.erase(id);
                return;
            }
            if (notification == CBN_SELENDOK && parameter_combo) {
                queue_combo_intent(id);
                changed_open_combos.erase(id);
                return;
            }
            if (notification == CBN_SELENDCANCEL) {
                canceled_combos.insert(id);
                changed_open_combos.erase(id);
                combo_intents.erase(id);
                sync_cache.erase(id);
            }
            if (notification == CBN_CLOSEUP || notification == CBN_SELENDCANCEL ||
                (notification == CBN_SELENDOK && id == devices)) {
                PostMessageW(hwnd, discovery_sync_message, id, 0);
                return;
            }
        }
        if (id == devices && notification == CBN_SELCHANGE) {
            render();
            return;
        }
        if (id == exit_app) {
            quitting = true;
            DestroyWindow(hwnd);
            return;
        }
        if (id == hide_window) {
            if (hotkeys)
                hotkeys->pause(false);
            ShowWindow(hwnd, SW_HIDE);
            return;
        }
        if (id >= hotkey_key && id < hotkey_key + 3) {
            if (notification == EN_SETFOCUS) {
                if (capturing >= 0)
                    set_text(hotkey_key + capturing, bindings[capturing].gesture.label().c_str());
                capturing = id - hotkey_key;
                capture.begin();
                set_text(id, L"请按组合键…");
                set_feedback(L"按住一个识别键，再按另一个键；Esc 取消。保存热键后生效。");
            } else if (notification == EN_KILLFOCUS && capturing == id - hotkey_key) {
                set_text(id, bindings[capturing].gesture.label().c_str());
                capturing = -1;
                capture.end();
            }
            return;
        }
        for (auto &[group_id, group] : radio_groups) {
            if (notification == BN_CLICKED && id >= group.base &&
                id < group.base + static_cast<int>(group.choices.size())) {
                int value = group.choices[id - group.base].second;
                if (!group.enabled || busy)
                    return;
                select_radio(group_id, value);
                if (group_id == touch_key)
                    populate_touch();
                else if (group_id == touch_preset) {
                    populate_actions();
                    submit_touch();
                } else if (group_id == custom_action)
                    populate_functions();
                else if (group_id == custom_function)
                    submit_custom();
                else if (group_id == guidance) {
                    immediate_control = guidance;
                    enqueue([this, value] { result(controller->set(p::Feature::Notifications, value)); });
                } else if (group_id == assistant) {
                    if (state.touch.voice_assistant.known() && *state.touch.voice_assistant.value == value)
                        return;
                    immediate_control = assistant;
                    enqueue([this, value] {
                        result(controller->set_voice_assistant(static_cast<p::Byte>(value)));
                    });
                } else if (group_id >= feature_choice && group_id < feature_choice + 5) {
                    auto feature = feature_rows[group_id - feature_choice].first;
                    auto it = state.features.find(feature);
                    if (it != state.features.end() && it->second.value.known() &&
                        *it->second.value.value == value)
                        return;
                    immediate_control = group_id;
                    enqueue([this, feature, value] { result(controller->set(feature, value)); });
                }
                return;
            }
        }
        if (id == eq_preset && notification == CBN_SELCHANGE) {
            if (canceled_combos.contains(id))
                return;
            if (SendDlgItemMessageW(hwnd, id, CB_GETDROPPEDSTATE, 0, 0))
                changed_open_combos.insert(id);
            else {
                queue_combo_intent(id);
                commit_combo_intent(id);
            }
            return;
        }
        if (id == source_list && notification == CBN_SELCHANGE) {
            if (canceled_combos.contains(id))
                return;
            if (SendDlgItemMessageW(hwnd, id, CB_GETDROPPEDSTATE, 0, 0))
                changed_open_combos.insert(id);
            else {
                queue_combo_intent(id);
                commit_combo_intent(id);
            }
            return;
        }
        if (id == touch_key && notification == CBN_SELCHANGE) {
            populate_touch();
            return;
        }
        if (id == touch_preset && notification == CBN_SELCHANGE) {
            populate_actions();
            return;
        }
        if (id == custom_action && notification == CBN_SELCHANGE) {
            populate_functions();
            return;
        }
        if (notification != BN_CLICKED)
            return;
        if (id == cycle_noise || id == cycle_ambient || id == cycle_off) {
            if (!busy && IsWindowEnabled(GetDlgItem(hwnd, id)))
                submit_cycle();
            return;
        }
        if (id == discover) {
            enqueue([this] {
                auto r = controller->discover();
                {
                    std::lock_guard lock(data_mutex);
                    found = std::move(r.devices);
                }
                result(r.result);
            });
            return;
        }
        if (id == connect_device) {
            sync_discovered_devices();
            auto n = selection(devices);
            if (n >= 0 && static_cast<std::size_t>(n) < displayed_found.size()) {
                auto device = displayed_found[n];
                enqueue([this, device]() mutable {
                    try {
                        device.aep_id = d::bluetooth_aep_id(device.address);
                    } catch (const std::exception &) {
                        device.aep_id.clear();
                    }
                    result(controller->connect(device));
                    refresh_local();
                });
            }
            return;
        }
        if (id == refresh) {
            enqueue([this] {
                result(controller->refresh());
                refresh_local();
            });
            return;
        }
        if (id == disconnect_device) {
            cancel_hold();
            enqueue([this] {
                controller->disconnect();
                result({sc::ResultCode::Success, "控制通道已断开"});
                refresh_local();
            });
            return;
        }
        if (id == noise || id == ambient || id == sound_off) {
            auto mode = id == noise     ? p::SoundMode::NoiseCanceling
                        : id == ambient ? p::SoundMode::Ambient
                                        : p::SoundMode::Off;
            enqueue([this, mode] { result(controller->set_sound(mode)); });
            return;
        }
        if (id == apply_ambient) {
            auto v = static_cast<int>(SendDlgItemMessageW(hwnd, ambient_level, TBM_GETPOS, 0, 0));
            enqueue([this, v] { result(controller->set_sound({}, v)); });
            return;
        }
        if (id == voice_focus) {
            auto v = IsDlgButtonChecked(hwnd, voice_focus) == BST_CHECKED;
            enqueue([this, v] { result(controller->set_sound({}, {}, v)); });
            return;
        }
        if (id >= apply_eq && id < apply_eq + 6) {
            int i = id - apply_eq,
                v = static_cast<int>(SendDlgItemMessageW(hwnd, eq_band + i, TBM_GETPOS, 0, 0));
            enqueue([this, i, v] {
                result(i == 5 ? controller->set_equalizer({}, {}, {}, v)
                              : controller->set_equalizer({}, i, v));
            });
            return;
        }
        if (id == multipoint) {
            auto v = IsDlgButtonChecked(hwnd, multipoint) == BST_CHECKED;
            enqueue([this, v] { result(controller->set(p::Feature::Multipoint, v ? 1 : 0)); });
            return;
        }
        if (id == switch_source) {
            auto n = selection(source_list);
            if (n >= 0 && static_cast<std::size_t>(n) < state.sources.entries.size()) {
                auto address = state.sources.entries[n].id;
                enqueue([this, address] { result(controller->switch_source(address)); });
            }
            return;
        }
        if (id == release_source) {
            enqueue([this] { result(controller->set(p::Feature::FixedPlayback, 0)); });
            return;
        }
        if (id >= feature_apply && id < feature_apply + 6) {
            int n = id - feature_apply;
            auto v = n == 5 ? 1 : selection(feature_choice + n);
            if (v >= 0) {
                auto feature = feature_rows[n].first;
                if (feature == p::Feature::PowerOff &&
                    MessageBoxW(hwnd, L"发送关机命令后耳机将断开；是否继续？", L"关闭耳机",
                                MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
                    return;
                enqueue([this, feature, v] { result(controller->set(feature, static_cast<int>(v))); });
            }
            return;
        }
        if (id == apply_touch) {
            auto k = selection(touch_key), v = selection(touch_preset);
            if (k >= 0 && static_cast<std::size_t>(k) < state.touch.keys.size() && v >= 0) {
                auto key = state.touch.keys[k].id;
                if (MessageBoxW(hwnd, L"分配可能联动修改语音助手。将保存关联读数，并在设备确认后更新显示。",
                                L"触摸分配", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
                    return;
                enqueue(
                    [this, key, v] { result(controller->set_touch(key, static_cast<p::Byte>(v), true)); });
            }
            return;
        }
        if (id == apply_custom) {
            auto a = selection(custom_action), f = selection(custom_function);
            auto preset = selected_touch_preset();
            if (preset && a >= 0 && f >= 0 && state.touch.details.known()) {
                auto detail = *state.touch.details.value;
                bool replaced = false;
                for (auto &candidate : detail)
                    if (candidate.preset == preset->id)
                        for (auto &action : candidate.actions)
                            if (action.action == a) {
                                action.function = static_cast<p::Byte>(f);
                                replaced = true;
                            }
                if (!replaced) {
                    set_feedback(L"尚未读到该预设的动作参数，请先应用预设并重新读取。");
                    return;
                }
                enqueue([this, detail] { result(controller->set_touch_details(detail)); });
            }
            return;
        }
        if (id == apply_cycle) {
            auto c = static_cast<int>(selection(cycle_noise));
            if (c < 1 || c > 4) {
                set_feedback(L"请选择循环模式组合。");
                return;
            }
            enqueue([this, c] { result(controller->set(p::Feature::AmbientCycle, c)); });
            return;
        }
        if (id == guidance) {
            auto value = static_cast<int>(selection(guidance));
            if (value >= 0)
                enqueue([this, value] { result(controller->set(p::Feature::Notifications, value)); });
            return;
        }
        if (id == apply_volume_prompt) {
            auto v = static_cast<int>(SendDlgItemMessageW(hwnd, volume_prompt, TBM_GETPOS, 0, 0));
            enqueue([this, v] { result(controller->set(p::Feature::PromptVolume, v)); });
            return;
        }
        if (id == apply_assistant) {
            auto v = selection(assistant);
            if (v >= 0 && MessageBoxW(hwnd, L"语音助手可能与触摸分配联动。确认修改？", L"语音助手",
                                      MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
                enqueue([this, v] { result(controller->set_voice_assistant(static_cast<p::Byte>(v))); });
            return;
        }
        if (id == restore_linked) {
            enqueue([this] { result(controller->restore_touch_linked_backup()); });
            return;
        }
        if (id == close_tray) {
            close_to_tray = IsDlgButtonChecked(hwnd, close_tray) == BST_CHECKED;
            auto path = root / L"desktop.ini";
            if (std::filesystem::exists(path))
                std::filesystem::copy_file(path, root / L"desktop.ini.bak",
                                           std::filesystem::copy_options::overwrite_existing);
            if (!WritePrivateProfileStringW(L"Desktop", L"CloseToTray", close_to_tray ? L"1" : L"0",
                                            path.c_str()))
                throw std::runtime_error("Cannot save desktop settings");
            set_feedback(L"关闭窗口行为已保存。");
            return;
        }
        if (id == startup) {
            set_startup(IsDlgButtonChecked(hwnd, startup) == BST_CHECKED);
            set_feedback(L"原生版开机启动设置已保存。");
            return;
        }
        if (id == apply_local_volume) {
            auto percent = static_cast<double>(SendDlgItemMessageW(hwnd, local_volume, TBM_GETPOS, 0, 0));
            std::wstring endpoint;
            {
                std::lock_guard lock(data_mutex);
                endpoint = local.volume.endpoint_id;
            }
            auto device = state.device ? state.device->aep_id : L"";
            enqueue([this, device, endpoint, percent] {
                desktop_result(playback->set_volume(device, endpoint, percent));
                refresh_local();
            });
            return;
        }
        if (id == previous || id == play || id == pause || id == next) {
            auto action = id == previous ? d::MediaAction::previous
                          : id == play   ? d::MediaAction::play
                          : id == pause  ? d::MediaAction::pause
                                         : d::MediaAction::next;
            std::wstring expected;
            {
                std::lock_guard lock(data_mutex);
                expected = local.media.session_id;
            }
            enqueue([this, expected, action] {
                desktop_result(playback->execute_media(expected, action));
                refresh_local();
            });
            return;
        }
        if (id == save_hotkeys) {
            d::validate_hotkeys(bindings);
            hotkeys->save(bindings);
            set_feedback(hotkeys->status());
            return;
        }
        if (id == apply_log) {
            d::DiagnosticOptions options{GetDlgItemInt(hwnd, log_mib, nullptr, FALSE),
                                         GetDlgItemInt(hwnd, log_runs, nullptr, FALSE)};
            options.validate();
            auto path = root / L"desktop.ini";
            if (std::filesystem::exists(path))
                std::filesystem::copy_file(path, root / L"desktop.ini.bak",
                                           std::filesystem::copy_options::overwrite_existing);
            if (!WritePrivateProfileStringW(L"Diagnostics", L"SegmentMiB",
                                            std::to_wstring(options.segment_limit_mib).c_str(),
                                            path.c_str()) ||
                !WritePrivateProfileStringW(L"Diagnostics", L"RetainedRuns",
                                            std::to_wstring(options.retained_runs).c_str(), path.c_str()))
                throw std::runtime_error("Cannot save diagnostic options");
            diagnostics->apply_options(options);
            set_feedback(diagnostics->status());
            return;
        }
        if (id == export_log || id == replay_log) {
            auto path = dialog_path(id == export_log, L"SonyConnect 诊断包\0*.scdiag\0\0", L"scdiag");
            if (path.empty())
                return;
            enqueue([this, path, id] {
                if (id == export_log) {
                    diagnostics->export_archive(path);
                    std::lock_guard lock(data_mutex);
                    feedback = L"脱敏诊断包已导出：" + path.wstring();
                } else {
                    auto entries = d::DiagnosticLog::read_replay(path);
                    std::ostringstream text;
                    text << "SonyConnect read-only replay: " << entries.size() << " events\r\n";
                    for (auto &entry : entries)
                        text << entry.timestamp << " " << entry.entry.origin << " " << entry.entry.event
                             << " " << entry.entry.status << " " << entry.entry.error_code << "\r\n";
                    auto output = root / L"last-replay.txt";
                    std::ofstream file(output, std::ios::binary);
                    file << "\xef\xbb\xbf" << text.str();
                    file.close();
                    auto opened =
                        ShellExecuteW(nullptr, L"open", L"notepad.exe",
                                      (L"\"" + output.wstring() + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
                    std::lock_guard lock(data_mutex);
                    feedback = reinterpret_cast<INT_PTR>(opened) > 32
                                   ? L"只读时间线已打开，不发送设备指令。"
                                   : L"时间线已保存，但打开查看器失败：" + output.wstring();
                }
            });
            return;
        }
        if (id == check_update || id == release_page) {
            auto source =
                d::UpdateSource::load(executable_path().parent_path() / L"config" / L"update-source.json");
            auto ch = selection(update_channel) == 1 ? d::UpdateChannel::gitee : d::UpdateChannel::github;
            if (id == release_page) {
                auto url = source.release_page(ch);
                ShellExecuteW(hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return;
            }
            enqueue([this, source, ch] {
                auto r = d::SoftwareUpdateService(source, SONYCONNECT_VERSION).check(ch);
                std::lock_guard lock(data_mutex);
                feedback = r.message;
            });
            return;
        }
    }
    void capture_png(const std::filesystem::path &path) {
        RECT r{};
        GetWindowRect(hwnd, &r);
        int w = r.right - r.left, h = r.bottom - r.top;
        HDC screen = GetDC(hwnd), memory = CreateCompatibleDC(screen);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void *pixels{};
        HBITMAP bitmap = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &pixels, nullptr, 0);
        auto previous_bitmap = SelectObject(memory, bitmap);
        PrintWindow(hwnd, memory, 2);
        for (std::size_t i = 3; i < static_cast<std::size_t>(w) * h * 4; i += 4)
            static_cast<BYTE *>(pixels)[i] = 255;
        IWICImagingFactory *factory{};
        IWICStream *stream{};
        IWICBitmapEncoder *encoder{};
        IWICBitmapFrameEncode *frame{};
        IPropertyBag2 *bag{};
        HRESULT hr =
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr))
            hr = factory->CreateStream(&stream);
        if (SUCCEEDED(hr))
            hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        if (SUCCEEDED(hr))
            hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (SUCCEEDED(hr))
            hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr))
            hr = encoder->CreateNewFrame(&frame, &bag);
        if (SUCCEEDED(hr))
            hr = frame->Initialize(bag);
        if (SUCCEEDED(hr))
            hr = frame->SetSize(w, h);
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(hr))
            hr = frame->SetPixelFormat(&format);
        if (SUCCEEDED(hr) && format != GUID_WICPixelFormat32bppBGRA)
            hr = E_FAIL;
        if (SUCCEEDED(hr))
            hr = frame->WritePixels(h, w * 4, w * h * 4, static_cast<BYTE *>(pixels));
        if (SUCCEEDED(hr))
            hr = frame->Commit();
        if (SUCCEEDED(hr))
            hr = encoder->Commit();
        if (bag)
            bag->Release();
        if (frame)
            frame->Release();
        if (encoder)
            encoder->Release();
        if (stream)
            stream->Release();
        if (factory)
            factory->Release();
        SelectObject(memory, previous_bitmap);
        DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(hwnd, screen);
        if (FAILED(hr))
            throw std::runtime_error("Cannot capture UI PNG");
    }
    LRESULT message(UINT m, WPARAM w, LPARAM l) {
        switch (m) {
        case WM_CREATE:
            build_controls();
            return 0;
        case WM_SIZE:
            layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto mi = reinterpret_cast<MINMAXINFO *>(l);
            mi->ptMinTrackSize = {px(980), px(800)};
            return 0;
        }
        case WM_DPICHANGED: {
            dpi = HIWORD(w);
            auto r = reinterpret_cast<RECT *>(l);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER);
            if (font)
                DeleteObject(font);
            font = CreateFontW(-px(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                               L"Microsoft YaHei UI");
            for (auto &c : controls)
                SendMessageW(c.handle, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            layout();
            return 0;
        }
        case WM_NOTIFY:
            if (reinterpret_cast<NMHDR *>(l)->idFrom == tabs &&
                reinterpret_cast<NMHDR *>(l)->code == TCN_SELCHANGE)
                show_page(TabCtrl_GetCurSel(tab));
            return 0;
        case WM_COMMAND:
            command(LOWORD(w), HIWORD(w));
            return 0;
        case WM_HSCROLL: {
            auto id = GetDlgCtrlID(reinterpret_cast<HWND>(l));
            update_slider_labels();
            if (LOWORD(w) == TB_ENDTRACK) {
                editing_slider = -1;
                apply_slider(id);
            } else if (id == ambient_level || id == local_volume) {
                editing_slider = id;
                apply_slider(id, false);
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (w == VK_ESCAPE)
                cancel_hold();
            return 0;
        case WM_TIMER:
            if (w == 3) {
                flush_slider();
            } else if (w == 2 && hold && GetTickCount64() - hold_started >= 1000) {
                cancel_hold();
                enqueue([this] { result(controller->set(p::Feature::FixedPlayback, 1)); });
            } else if (w == 1) {
                render();
                if (!offline && !busy && ++tick % 6 == 0)
                    enqueue(
                        [this] {
                            if (auto available = d::bluetooth_available())
                                controller->notify_bluetooth_available(*available);
                            refresh_local();
                        },
                        false);
            }
            return 0;
        case WM_POWERBROADCAST:
            if (controller) {
                if (w == PBT_APMSUSPEND) {
                    cancel_hold();
                    controller->suspend();
                    set_feedback(L"系统睡眠，已释放控制通道。");
                    render();
                } else if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) {
                    set_feedback(wide(controller->resume().message));
                    render();
                }
            }
            return TRUE;
        case WM_DEVICECHANGE:
            if (!offline && !busy)
                enqueue(
                    [this] {
                        if (auto available = d::bluetooth_available())
                            controller->notify_bluetooth_available(*available);
                    },
                    false);
            return 0;
        case discovery_sync_message:
            canceled_combos.erase(static_cast<int>(w));
            if (auto id = static_cast<int>(w); id == eq_preset || id == source_list) {
                if (changed_open_combos.erase(id)) {
                    queue_combo_intent(id);
                }
                commit_combo_intent(id);
            }
            sync_discovered_devices();
            rendered_revision = ~0ull;
            render();
            return 0;
        case combo_commit_message:
            commit_combo_intent(static_cast<int>(w));
            return 0;
        case changed_message: {
            if (w)
                busy = false;
            else
                background_work = false;
            if (w && immediate_control) {
                auto id = *immediate_control;
                sync_cache.erase(id);
                if (id == cycle_noise)
                    sync_cache.erase(apply_cycle);
                if (id == eq_preset || (id >= eq_band && id < eq_band + 6)) {
                    sync_cache.erase(eq_preset);
                    for (int i = 0; i < 6; ++i)
                        sync_cache.erase(eq_band + i);
                }
                if (auto it = radio_groups.find(id); it != radio_groups.end())
                    it->second.current = -2;
                immediate_control.reset();
                rendered_revision = ~0ull;
            }
            sync_discovered_devices();
            {
                std::lock_guard lock(data_mutex);
                set_text(status, feedback.c_str());
            }
            render();
            std::optional<TouchConfirmation> confirmation;
            if (w) {
                std::lock_guard lock(data_mutex);
                confirmation = std::exchange(touch_confirmation, {});
            }
            if (confirmation) {
                auto intent = *confirmation;
                if (state.link != intent.link || !state.ready) {
                    set_feedback(L"设备会话已变化，请重新选择分配。");
                } else if (MessageBoxW(
                               hwnd,
                               L"关闭右耳触摸分配会关闭关联语音助手。确认修改并保留本次连接内的原值备份？",
                               L"右耳触摸分配", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    immediate_control = touch_preset;
                    enqueue([this, intent] {
                        auto r = controller->refresh();
                        if (r.ok() && controller->snapshot().link == intent.link)
                            r = controller->set_touch(intent.key, intent.preset, true);
                        else if (r.ok())
                            r = {sc::ResultCode::Canceled, "设备会话已变化，未发送确认。"};
                        result(r);
                    });
                } else {
                    set_feedback(L"已取消关联修改，保留原分配。");
                }
            }
            flush_slider();
            return 0;
        }
        case hotkey_message: {
            if (!busy) {
                auto mode = static_cast<d::SoundMode>(w);
                enqueue([this, mode] {
                    result(controller->set_sound(mode == d::SoundMode::noise_canceling
                                                     ? p::SoundMode::NoiseCanceling
                                                 : mode == d::SoundMode::ambient ? p::SoundMode::Ambient
                                                                                 : p::SoundMode::Off));
                });
            }
            return 0;
        }
        case tray_message:
            if (l == WM_LBUTTONDBLCLK) {
                ShowWindow(hwnd, SW_RESTORE);
                if (hotkeys)
                    hotkeys->pause(page == 5);
                SetForegroundWindow(hwnd);
            } else if (l == WM_RBUTTONUP) {
                auto menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, 1, L"打开 SonyConnect");
                AppendMenuW(menu, MF_STRING, 2, L"本机设置");
                AppendMenuW(menu, MF_STRING, 3, L"退出");
                POINT pt{};
                GetCursorPos(&pt);
                SetForegroundWindow(hwnd);
                auto action =
                    TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(menu);
                if (action == 3) {
                    quitting = true;
                    DestroyWindow(hwnd);
                } else if (action) {
                    ShowWindow(hwnd, SW_RESTORE);
                    if (hotkeys)
                        hotkeys->pause(page == 5);
                    if (action == 2) {
                        TabCtrl_SetCurSel(tab, 5);
                        show_page(5);
                    }
                }
            }
            return 0;
        case WM_COPYDATA:
            ShowWindow(hwnd, SW_RESTORE);
            if (hotkeys)
                hotkeys->pause(page == 5);
            SetForegroundWindow(hwnd);
            return TRUE;
        case WM_CLOSE:
            cancel_hold();
            if (close_to_tray && tray_added && !quitting) {
                if (hotkeys)
                    hotkeys->pause(false);
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            KillTimer(hwnd, 2);
            KillTimer(hwnd, 3);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, m, w, l);
        }
    }
};
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int count{};
    LPWSTR *args = CommandLineToArgvW(GetCommandLineW(), &count);
    bool offline = false, test = false, hidden = false;
    std::filesystem::path report;
    for (int i = 1; i < count; ++i) {
        std::wstring arg = args[i];
        if (arg == L"--offline")
            offline = true;
        else if (arg == L"--ui-self-test") {
            test = true;
            offline = true;
        } else if (arg == L"--start-hidden")
            hidden = true;
        else if (arg == L"--report" && i + 1 < count)
            report = args[++i];
        else {
            LocalFree(args);
            MessageBoxW(nullptr,
                        L"未知参数。支持 --offline、--ui-self-test --report <path>、--start-hidden。",
                        L"SonyConnect", MB_ICONERROR);
            return 2;
        }
    }
    LocalFree(args);
    if (test && report.empty())
        return 2;
    HANDLE mutex{};
    if (!offline) {
        mutex = CreateMutexW(nullptr, FALSE, L"Local\\SonyConnect.Native.SingleInstance");
        if (!mutex)
            return 2;
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            auto existing = FindWindowW(window_class, nullptr);
            if (existing) {
                COPYDATASTRUCT data{};
                SendMessageTimeoutW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                                    SMTO_ABORTIFHUNG, 3000, nullptr);
            }
            CloseHandle(mutex);
            return 0;
        }
    }
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    INITCOMMONCONTROLSEX cc{sizeof(cc), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&cc);
    int code{};
    try {
        Window app(offline, test, hidden);
        app.create(instance);
        if (test) {
            std::filesystem::create_directories(report.parent_path());
            code = app.self_test(report) ? 0 : 1;
            DestroyWindow(app.hwnd);
        } else {
            MSG msg{};
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                if (!app.pre_translate(msg) && !IsDialogMessageW(app.hwnd, &msg)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
        }
    } catch (const winrt::hresult_error &e) {
        if (test) {
            std::ofstream out(report);
            out << "{\"Passed\":false,\"HardwareVerified\":false}";
        } else
            MessageBoxW(nullptr, e.message().c_str(), L"SonyConnect 初始化失败", MB_ICONERROR);
        code = 2;
    } catch (const std::exception &e) {
        if (test) {
            std::ofstream out(report);
            out << "{\"Passed\":false,\"HardwareVerified\":false}";
        } else
            MessageBoxW(nullptr, wide(e.what()).c_str(), L"SonyConnect 初始化失败", MB_ICONERROR);
        code = 2;
    }
    winrt::uninit_apartment();
    if (mutex)
        CloseHandle(mutex);
    return code;
}
