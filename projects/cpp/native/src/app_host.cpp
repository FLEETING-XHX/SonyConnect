#include "sonyconnect/app_host.hpp"
#include "sonyconnect/session.hpp"
#include "sonyconnect/desktop.hpp"
#include "sonyconnect/main_window.hpp"
#include "sonyconnect/more_window.hpp"
#include "sonyconnect/settings_flow.hpp"
#include "sonyconnect/ui_motion.hpp"
#include "sonyconnect/tray_icon.hpp"
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winrt/base.h>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cmath>
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
constexpr UINT changed_message = WM_APP + 1, tray_message = WM_APP + 2;
constexpr UINT main_close_message = WM_APP + 6;
constexpr UINT update_done_message = WM_APP + 7;
constexpr UINT diagnostic_loaded_message = WM_APP + 8;
constexpr auto window_class = sc::ui::app_host_class;
constexpr std::array<const wchar_t *, 4> appearance_keys{
    L"VolumeSliderStyle", L"AmbientSliderStyle", L"EqSliderStyle", L"BassSliderStyle"};
using SliderAppearances = std::array<sc::ui::MainSliderStyle, 4>;
SliderAppearances read_appearances(const std::filesystem::path &path) {
    SliderAppearances styles;
    const int legacy = GetPrivateProfileIntW(L"UI", L"SliderStyle", 0, path.c_str());
    for (std::size_t i = 0; i < styles.size(); ++i)
        styles[i] = GetPrivateProfileIntW(L"UI", appearance_keys[i], legacy, path.c_str()) == 1
                        ? sc::ui::MainSliderStyle::elastic : sc::ui::MainSliderStyle::wake;
    return styles;
}
void save_appearance(const std::filesystem::path &path, int slot, int value) {
    if (slot < 0 || slot > 3 || value < 0 || value > 1) throw std::invalid_argument("Invalid slider appearance");
    if (std::filesystem::exists(path)) {
        auto backup = path;
        backup += L".bak";
        std::filesystem::copy_file(path, backup, std::filesystem::copy_options::overwrite_existing);
    }
    if (!WritePrivateProfileStringW(L"UI", appearance_keys[slot], value ? L"1" : L"0", path.c_str()))
        throw std::runtime_error("无法保存滑条样式。");
}
enum AdapterField { ambient_level = 109, eq_band = 200, volume_prompt = 250, local_volume = 272 };
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
std::wstring sound_pressure_text(const sc::DeviceSnapshot &state) {
    const auto &sl = state.safe_listening;
    if (!state.connected || !state.ready)
        return L"当前音压：—（未连接）";
    if (!sl.type)
        return L"当前音压：—（设备未报告支持的能力）";
    if (!sl.available)
        return L"当前音压：—（设备暂不可用）";
    if (!sl.minimum_interval_seconds || *sl.minimum_interval_seconds == 0)
        return L"当前音压：—（耳机更新间隔尚未确认）";
    if (!sl.enabled.known())
        return L"当前音压：—（安全聆听状态待确认）";
    if (!*sl.enabled.value)
        return L"当前音压：—（安全聆听已关闭）";
    if (!sl.reason.empty())
        return L"当前音压：—（" + wide(sl.reason) + L"）";
    const auto expiry = std::chrono::seconds(std::max(10u, sl.minimum_interval_seconds.value_or(5) * 2));
    if (sl.updated_at && std::chrono::steady_clock::now() - *sl.updated_at > expiry)
        return L"当前音压：—（读数已过期）";
    if (!sl.cause.known())
        return L"当前音压：—（等待耳机读数）";
    switch (*sl.cause.value) {
    case 0: return L"当前音压：—（未播放）";
    case 1: return L"当前音压：—（通话中）";
    case 2: return L"当前音压：—（未佩戴）";
    default:
        if (sl.decibels.known())
            return L"当前音压：" + std::to_wstring(*sl.decibels.value) + L" dB";
    }
    return L"当前音压：—（未知设备状态）";
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
class ApplicationHost {
  public:
    HWND hwnd{};
    bool offline{}, ui_test{}, start_hidden{}, quitting{}, busy{};
    explicit ApplicationHost(bool off, bool test, bool hidden) : offline(off), ui_test(test), start_hidden(hidden) {}
    std::wstring preview_main;
    UINT preview_dpi{};
    void capture_main(const std::filesystem::path &path) {
        if (main_view) main_view->capture(path);
    }
    ~ApplicationHost() {
        desktop_closing = true;
        update_cancel = true;
        update_closing = true;
        if (update_worker.joinable()) update_worker.join();
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
    }
    void create(HINSTANCE instance) {
        WNDCLASSEXW cls{sizeof(cls)};
        cls.lpfnWndProc = proc;
        cls.hInstance = instance;
        cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
        cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
        cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        cls.lpszClassName = window_class;
        RegisterClassExW(&cls);
        hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, window_class, L"SonyConnect background host", WS_POPUP,
                               0, 0, 0, 0, nullptr, nullptr, instance, this);
        if (!hwnd) throw std::runtime_error("Cannot create application message host");
        if (!offline) {
            root = native_data();
            std::filesystem::create_directories(root);
            close_to_tray =
                GetPrivateProfileIntW(L"Desktop", L"CloseToTray", 0, (root / L"desktop.ini").c_str()) != 0;
            slider_styles = read_appearances(root / L"desktop.ini");
            selected_update_channel = GetPrivateProfileIntW(L"UI", L"UpdateChannel", 0, (root / L"desktop.ini").c_str()) == 1
                                          ? d::UpdateChannel::gitee : d::UpdateChannel::github;
            main_slider_style = slider_styles[0];
            controller = std::make_unique<sc::DeviceController>();
            d::DiagnosticOptions log_options{
                static_cast<unsigned>(
                    GetPrivateProfileIntW(L"Diagnostics", L"SegmentMiB", 2, (root / L"desktop.ini").c_str())),
                static_cast<unsigned>(GetPrivateProfileIntW(L"Diagnostics", L"RetainedRuns", 10,
                                                            (root / L"desktop.ini").c_str()))};
            log_options.validate();
            diagnostic_options = log_options;
            diagnostics = std::make_unique<d::DiagnosticLog>(root / L"Logs", log_options);
            controller->set_trace([this](const sc::ProtocolTrace &trace) {
                d::DiagnosticEvent e;
                e.event = trace.event;
                e.status = trace.failure_reason;
                e.origin = "Device";
                e.generation = trace.link;
                e.receive_order = trace.receive_order;
                e.type = trace.type;
                e.sequence = trace.sequence;
                e.command = trace.command;
                diagnostics->record(std::move(e));
            });
            worker = std::thread([this] { worker_loop(); });
            tray = std::make_unique<d::TrayIcon>(hwnd, tray_message, LoadIconW(instance, MAKEINTRESOURCEW(101)));
            tray->restore();
        }
        startup_preference = !offline && startup_enabled();
        SetTimer(hwnd, 1, 500, nullptr);
        if (!ui_test) {
            main_view = std::make_unique<sc::ui::MainWindow>(
                [this](sc::ui::MainAction action, int value, bool final, std::uint64_t address) {
                    main_action(action, value, final, address);
                });
            main_view->create(instance, preview_dpi);
            SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(main_view->handle()));
            update_main();
            if (!start_hidden || !tray || !tray->added()) main_view->show(!offline);
            if (!offline) discover_main();
        }
    }
    bool diagnostic_self_test(const std::filesystem::path &report) {
        bool passed = true;
        std::vector<std::pair<std::string, bool>> checks;
        const auto check = [&](const char *name, bool ok) { checks.emplace_back(name, ok); passed &= ok; };
        root = std::filesystem::absolute(report.parent_path() / (L"fixture-" + std::to_wstring(GetCurrentProcessId())));
        std::filesystem::create_directories(root);
        offline = false;
        diagnostics = std::make_unique<d::DiagnosticLog>(root / L"Logs");
        auto instance = GetModuleHandleW(nullptr);
        main_view = std::make_unique<sc::ui::MainWindow>([](auto, int, bool, std::uint64_t) {});
        main_view->create(instance, 96);
        more_view = std::make_unique<sc::ui::MoreWindow>([](auto, int, int, bool) {});
        more_view->create(instance, main_view->handle(), 96);
        more_action(sc::ui::MoreAction::diagnostics, 0, 0, true);
        check("formal_diagnostics_route_keeps_legacy_window_hidden", diagnostics_view && !IsWindowVisible(hwnd) && !IsWindowVisible(diagnostics_view->handle()));
        WritePrivateProfileStringW(L"Other", L"Keep", L"73", (root / L"desktop.ini").c_str());
        more_action(sc::ui::MoreAction::diagnostic_limits, 8, 100, true);
        check("limits_reuse_service_persist_backup_and_preserve_other_config", diagnostic_options.segment_limit_mib == 8 &&
              diagnostic_options.retained_runs == 100 && GetPrivateProfileIntW(L"Other", L"Keep", 0, (root / L"desktop.ini").c_str()) == 73 &&
              std::filesystem::exists(root / L"desktop.ini.bak"));
        more_action(sc::ui::MoreAction::diagnostic_limits, 0, 101, true);
        check("invalid_limits_preserve_confirmed_options", diagnostic_error && diagnostic_options.segment_limit_mib == 8 && diagnostic_options.retained_runs == 100);
        d::DiagnosticEvent event;
        event.event = "fixture-confirmed"; event.status = "address=00:11:22:33:44:55"; event.origin = "Simulation";
        diagnostics->record(event);
        diagnostic_test_path = root / L"fixture.scdiag";
        more_action(sc::ui::MoreAction::diagnostic_export, 0, 0, true);
        const auto readback = d::DiagnosticLog::read_replay(*diagnostic_test_path);
        check("export_uses_existing_redacted_archive", !diagnostic_error && std::any_of(readback.begin(), readback.end(), [](const auto &entry) {
            return entry.entry.event == "fixture-confirmed" && entry.entry.status.find("[redacted]") != std::string::npos && entry.entry.status.find("00:11") == std::string::npos;
        }));
        more_action(sc::ui::MoreAction::diagnostic_replay, 0, 0, true);
        check("replay_opens_formal_read_only_timeline", replay_view && replay_entries && !replay_entries->empty() && !IsWindowVisible(hwnd));
        const auto retained = replay_entries;
        const auto retained_name = replay_name;
        diagnostic_test_path.reset();
        more_action(sc::ui::MoreAction::diagnostic_replay, 0, 0, true);
        check("file_dialog_cancel_preserves_timeline", replay_entries == retained && replay_name == retained_name);
        diagnostic_test_path = root / L"invalid.scdiag";
        { std::ofstream bad(*diagnostic_test_path); bad << "invalid archive"; }
        more_action(sc::ui::MoreAction::diagnostic_replay, 0, 0, true);
        check("corrupt_archive_preserves_previous_timeline_and_reports_error", diagnostic_error && replay_entries == retained && replay_name == retained_name);
        check("file_errors_do_not_become_headphone_readback_errors", !feedback_error);
        more_action(sc::ui::MoreAction::update_channel, 1, 0, true);
        check("update_channel_is_local_and_works_without_a_headphone", selected_update_channel == d::UpdateChannel::gitee &&
              GetPrivateProfileIntW(L"UI", L"UpdateChannel", 0, (root / L"desktop.ini").c_str()) == 1 && !state.ready);
        check("update_uses_existing_independent_service", start_update(d::UpdateChannel::gitee,
              [](const auto &, const auto &, auto *) { return d::UpdateHttpResponse{200, "[]"}; }));
        const auto until = GetTickCount64() + 2000;
        while (update_checking && GetTickCount64() < until) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) continue;
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            if (update_checking) MsgWaitForMultipleObjectsEx(0, nullptr, 8, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        check("update_result_projects_into_formal_page", !update_checking && last_update_result && last_update_result->status == d::UpdateStatus::no_release);
        check("no_hardware_hotkeys_network_or_legacy_view", !controller && !playback && queue.empty() && ui_dispatches == 0 &&
              !IsWindowVisible(hwnd) && !IsWindowVisible(diagnostics_view->handle()) && !IsWindowVisible(replay_view->handle()));
        std::ofstream output(report);
        output << "{\"Passed\":" << (passed ? "true" : "false") << ",\"HardwareVerified\":false,\"NetworkCalled\":false,\"LegacyWindowShown\":false,\"Checks\":[";
        for (std::size_t i = 0; i < checks.size(); ++i)
            output << (i ? "," : "") << "{\"Name\":\"" << checks[i].first << "\",\"Passed\":" << (checks[i].second ? "true" : "false") << "}";
        output << "]}";
        return passed;
    }
    bool update_self_test(const std::filesystem::path &report) {
        std::vector<std::pair<std::string, bool>> checks;
        const auto check = [&](const char *name, bool ok) { checks.emplace_back(name, ok); };
        const auto instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
        main_view = std::make_unique<sc::ui::MainWindow>([](auto, int, bool, std::uint64_t) {});
        main_view->create(instance, 96);
        more_view = std::make_unique<sc::ui::MoreWindow>([](auto, int, int, bool) {});
        more_view->create(instance, main_view->handle(), 96);
        state = sc::ui::more_preview().device;
        check("hidden_host_is_not_the_main_dialog_owner", settings_owner() == main_view->handle());
        check("background_host_has_no_retired_controls", GetWindow(hwnd, GW_CHILD) == nullptr);
        main_view->show(false);
        more_view->show();
        check("explicit_visible_owner_wins_over_other_settings", settings_owner(main_view->handle()) == main_view->handle());
        struct ModalProbe {
            HWND expected{};
            bool seen{}, owner_matches{}, owner_disabled{};
            static void CALLBACK tick(HWND host, UINT, UINT_PTR, DWORD) {
                auto *probe = static_cast<ModalProbe *>(GetPropW(host, L"SonyConnect.UpdateOwnerProbe"));
                if (!probe) return;
                EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM data) -> BOOL {
                    auto &probe = *reinterpret_cast<ModalProbe *>(data);
                    wchar_t title[128]{}, cls[32]{};
                    GetWindowTextW(window, title, 128); GetClassNameW(window, cls, 32);
                    if (wcscmp(cls, L"#32770") || wcscmp(title, L"SonyConnect — 软件更新")) return TRUE;
                    probe.seen = true;
                    probe.owner_matches = GetWindow(window, GW_OWNER) == probe.expected;
                    probe.owner_disabled = !IsWindowEnabled(probe.expected);
                    PostMessageW(window, TDM_CLICK_BUTTON, IDCANCEL, 0);
                    return FALSE;
                }, reinterpret_cast<LPARAM>(probe));
            }
        } modal{main_view->handle()};
        if (!SetPropW(hwnd, L"SonyConnect.UpdateOwnerProbe", &modal) || !SetTimer(hwnd, 99, 20, ModalProbe::tick))
            throw std::runtime_error("Cannot observe update dialog owner");
        open_update_dialog(main_view->handle());
        KillTimer(hwnd, 99);
        RemovePropW(hwnd, L"SonyConnect.UpdateOwnerProbe");
        check("real_update_dialog_disables_the_invoking_formal_window", modal.seen && modal.owner_matches && modal.owner_disabled);
        more_view->hide();
        main_view->hide();
        const auto wait = [&] {
            const auto deadline = GetTickCount64() + 2000;
            while (update_checking && GetTickCount64() < deadline) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                Sleep(1);
            }
            return !update_checking;
        };
        std::atomic_bool entered{false};
        const bool started = start_update(d::UpdateChannel::gitee, [&](const auto &, const auto &, const auto *canceled) {
            entered = true;
            while (!canceled->load()) Sleep(1);
            return d::UpdateHttpResponse{200, "[]"};
        });
        check("update_uses_independent_worker", started && update_checking && !busy && queue.empty());
        check("update_has_immediate_cancel_label", window_text(GetDlgItem(more_view->handle(), 1054)) == L"取消更新检查");
        check("duplicate_update_does_not_start", !start_update(d::UpdateChannel::github, [](const auto &, const auto &, auto *) { return d::UpdateHttpResponse{}; }));
        const auto enter_deadline = GetTickCount64() + 500;
        while (!entered.load() && GetTickCount64() < enter_deadline) Sleep(1);
        open_update_dialog();
        check("cancel_has_immediate_feedback", window_text(GetDlgItem(more_view->handle(), 1054)) == L"取消中…");
        check("cancel_completes_without_device_operations", wait() && !busy && queue.empty() && entered &&
              last_update_result && last_update_result->status == d::UpdateStatus::canceled && !update_worker.joinable());
        const auto current_version = d::ReleaseVersion::parse(SONYCONNECT_VERSION);
        if (!current_version) throw std::runtime_error("Invalid product version in update self-test");
        const std::string newer_version = "v" + std::to_string(static_cast<long long>(current_version->major) + 1) + ".0.0";
        start_update(d::UpdateChannel::github, [newer_version](const auto &, const auto &, auto *) {
            return d::UpdateHttpResponse{200, "{\"tag_name\":\"" + newer_version + "\",\"draft\":false,\"prerelease\":false}"};
        });
        check("new_version_result_reaches_host", wait() && last_update_result &&
              last_update_result->status == d::UpdateStatus::available && last_update_result->latest == wide(newer_version));
        start_update(d::UpdateChannel::gitee, [](const auto &, const auto &, auto *) { return d::UpdateHttpResponse{503, "{}"}; });
        check("network_error_is_not_current", wait() && last_update_result && last_update_result->status == d::UpdateStatus::failed);
        check("version_label_restores_after_completion", window_text(GetDlgItem(more_view->handle(), 1054)).starts_with(L"软件版本"));
        check("no_hardware_or_legacy_ui_used", !controller && !playback &&
              !IsWindowVisible(hwnd) && !IsWindowVisible(main_view->handle()) && !IsWindowVisible(more_view->handle()));
        const bool passed = std::all_of(checks.begin(), checks.end(), [](auto &c) { return c.second; });
        std::ofstream out(report);
        out << "{\"Passed\":" << (passed ? "true" : "false") << ",\"HardwareVerified\":false,\"NetworkCalled\":false,\"LegacyWindowShown\":false,\"Checks\":[";
        for (std::size_t i = 0; i < checks.size(); ++i)
            out << (i ? "," : "") << "{\"Name\":\"" << checks[i].first << "\",\"Passed\":" << (checks[i].second ? "true" : "false") << '}';
        out << "]}";
        return passed && out.good();
    }
    bool slider_self_test(const std::filesystem::path &report) {
        // Exercise the production adapters in hidden hosts, without opening
        // legacy pages or constructing a Bluetooth/controller/backend instance.
        std::vector<std::pair<std::string, bool>> checks;
        const auto check = [&](std::string name, bool ok) { checks.emplace_back(std::move(name), ok); };
        const auto known = [](auto value) {
            sc::Value<std::decay_t<decltype(value)>> result;
            result.status = sc::ValueStatus::Known;
            result.value = std::move(value);
            return result;
        };
        auto instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
        main_view = std::make_unique<sc::ui::MainWindow>([](auto, int, bool, std::uint64_t) {});
        main_view->create(instance, 96);
        more_view = std::make_unique<sc::ui::MoreWindow>([](auto, int, int, bool) {});
        more_view->create(instance, main_view->handle(), 96);
        check("hidden_hosts_without_hardware_or_hotkeys", !controller && !playback &&
              !IsWindowVisible(hwnd) && !IsWindowVisible(main_view->handle()) && !IsWindowVisible(more_view->handle()));
        bool shell_available = true;
        unsigned tray_adds = 0;
        tray = std::make_unique<d::TrayIcon>(hwnd, tray_message, nullptr,
            [&](DWORD command, NOTIFYICONDATAW *) -> BOOL {
                if (command == NIM_ADD) ++tray_adds;
                return shell_available;
            });
        tray->restore();
        const auto taskbar_rebuilt = RegisterWindowMessageW(L"TaskbarCreated");
        message(taskbar_rebuilt, 0, 0);
        check("taskbar_restart_readds_existing_icon", tray_adds == 2 && tray->added());
        shell_available = false;
        message(taskbar_rebuilt, 0, 0);
        check("failed_tray_restore_keeps_main_window_accessible", !tray->added() && IsWindowVisible(main_view->handle()));
        tray.reset();
        main_view->hide();
        const std::array<int, 9> sliders{eq_band, eq_band + 1, eq_band + 2, eq_band + 3, eq_band + 4,
                                        eq_band + 5, volume_prompt, ambient_level, local_volume};
        for (int id : sliders) {
            state = sc::ui::more_preview().device;
            state.link = 77;
            state.device = sc::DeviceDescriptor{};
            state.device->aep_id = L"fixture-device";
            state.sound.mode = known(p::SoundMode::Ambient);
            state.sound.ambient_level = known(8);
            state.equalizer.bands = known(std::vector<int>(5, 0));
            state.equalizer.clear_bass = known(0);
            local.volume.endpoint_id = L"fixture-endpoint";
            local.volume.can_write = true;
            local.volume.percent = 40;
            offline = false;
            busy = false;
            active_slider.reset();
            pending_slider.reset();
            deferred_sliders.clear();
            editing_slider = more_edit_control = -1;
            feedback_error = false;
            render();
            const int target = id == local_volume ? 41 : id == ambient_level ? 9 : 1;
            const auto displayed = [&] {
                const auto view = id == ambient_level || id == local_volume ? main_view->handle() : more_view->handle();
                const int control = id == ambient_level ? 29 : id == local_volume ? 30
                                  : id == volume_prompt ? 1041 : 1010 + id - eq_band;
                return static_cast<int>(SendDlgItemMessageW(view, control, TBM_GETPOS, 0, 0));
            };
            const auto name = "slider_" + std::to_string(id);
            const auto before = ui_dispatches;
            apply_slider(id, target);
            update_main();
            check(name + "_target_before_busy", active_slider && active_slider->id == id &&
                  ui_dispatches == before + 1 && displayed() == target);
            busy = true;
            message(changed_message, 0, 0);
            check(name + "_background_refresh_keeps_target", active_slider && busy && displayed() == target);
            if (id == local_volume) local.volume.percent = target;
            else if (id == ambient_level) state.sound.ambient_level = known(target);
            else if (id == volume_prompt) state.touch.prompt_volume = known(target);
            else if (id == eq_band + 5) state.equalizer.clear_bass = known(target);
            else (*state.equalizer.bands.value)[id - eq_band] = target;
            ++state.revision;
            message(changed_message, 1, 0);
            check(name + "_confirmed_handoff", !active_slider && displayed() == target);
            apply_slider(id, target + 1);
            busy = true;
            update_main();
            check(name + "_second_target", active_slider && displayed() == target + 1);
            feedback_error = true;
            message(changed_message, 1, 0);
            check(name + "_error_restores_confirmed_value", !active_slider && displayed() == target);
        }
        offline = true;
        personalization_view = std::make_unique<sc::ui::MoreWindow>([](auto, int, int, bool) {}, sc::ui::SettingsPage::personalization);
        personalization_view->create(instance, more_view->handle(), 96);
        advanced_view = std::make_unique<sc::ui::MoreWindow>([](auto, int, int, bool) {}, sc::ui::SettingsPage::advanced);
        advanced_view->create(instance, more_view->handle(), 96);
        state = sc::ui::more_preview().device;
        for (int slot = 0; slot < 4; ++slot) {
            slider_styles.fill(sc::ui::MainSliderStyle::wake);
            main_slider_style = sc::ui::MainSliderStyle::wake;
            more_action(sc::ui::MoreAction::slider_appearance, 1, slot, true);
            check("independent_appearance_slot_" + std::to_string(slot),
                  std::count(slider_styles.begin(), slider_styles.end(), sc::ui::MainSliderStyle::elastic) == 1 &&
                  slider_styles[slot] == sc::ui::MainSliderStyle::elastic &&
                  window_text(GetDlgItem(personalization_view->handle(), 1060 + slot)).find(L"Elastic") != std::wstring::npos);
        }
        const auto appearance_file = std::filesystem::absolute(report.parent_path() / (L"appearance-" + std::to_wstring(GetCurrentProcessId()) + L".ini"));
        WritePrivateProfileStringW(L"UI", L"SliderStyle", L"1", appearance_file.c_str());
        WritePrivateProfileStringW(L"Other", L"Keep", L"73", appearance_file.c_str());
        const auto legacy_styles = read_appearances(appearance_file);
        check("appearance_preserves_legacy_values", std::all_of(legacy_styles.begin(), legacy_styles.end(), [](auto v) { return v == sc::ui::MainSliderStyle::elastic; }));
        save_appearance(appearance_file, 2, 0);
        const auto restored = read_appearances(appearance_file);
        auto appearance_backup = appearance_file;
        appearance_backup += L".bak";
        check("appearance_round_trip_and_unrelated_config", restored[2] == sc::ui::MainSliderStyle::wake && restored[0] == sc::ui::MainSliderStyle::elastic &&
              GetPrivateProfileIntW(L"Other", L"Keep", 0, appearance_file.c_str()) == 73 && std::filesystem::exists(appearance_backup));
        for (int slot = 0; slot < 4; ++slot) save_appearance(appearance_file, slot, slot % 2);
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, appearance_file.c_str());
        const auto reloaded = read_appearances(appearance_file);
        check("all_appearance_slots_reload_from_disk", reloaded == SliderAppearances{
            sc::ui::MainSliderStyle::wake, sc::ui::MainSliderStyle::elastic,
            sc::ui::MainSliderStyle::wake, sc::ui::MainSliderStyle::elastic});
        WritePrivateProfileStringW(L"UI", appearance_keys[0], L"-1", appearance_file.c_str());
        WritePrivateProfileStringW(L"UI", appearance_keys[1], L"unknown", appearance_file.c_str());
        WritePrivateProfileStringW(L"UI", appearance_keys[2], L"2", appearance_file.c_str());
        const auto invalid = read_appearances(appearance_file);
        check("invalid_appearances_fall_back_without_changing_valid_slot", invalid[0] == sc::ui::MainSliderStyle::wake &&
              invalid[1] == sc::ui::MainSliderStyle::wake && invalid[2] == sc::ui::MainSliderStyle::wake &&
              invalid[3] == sc::ui::MainSliderStyle::elastic);
        offline = false;
        busy = true; // Hold staged edits in the adapter; never construct a transport.
        pending_slider = SliderIntent{local_volume, 41, state.link};
        editing_slider = local_volume;
        const auto canceled_dispatches = ui_dispatches;
        main_action(sc::ui::MainAction::cancel_edit, 0, true, 0);
        check("main_cancel_removes_unsent_slider_intent", !pending_slider && editing_slider < 0 && ui_dispatches == canceled_dispatches);
        pending_slider = SliderIntent{eq_band, 3, state.link};
        deferred_sliders.push_back(SliderIntent{eq_band + 1, 4, state.link});
        active_slider = SliderIntent{eq_band + 2, 5, state.link};
        editing_slider = more_edit_control = eq_band;
        more_action(sc::ui::MoreAction::cancel_edit, static_cast<int>(sc::ui::MoreAction::equalizer_band), 0, true);
        check("more_cancel_discards_only_unsent_edit_and_preserves_other_work",
              pending_slider && pending_slider->id == eq_band + 1 && deferred_sliders.empty() &&
              active_slider && active_slider->id == eq_band + 2 && editing_slider < 0 && more_edit_control < 0 &&
              ui_dispatches == canceled_dispatches);
        pending_slider.reset(); active_slider.reset(); deferred_sliders.clear();
        busy = false;
        check("new_settings_hosts_remain_hidden", !IsWindowVisible(personalization_view->handle()) && !IsWindowVisible(advanced_view->handle()));
        state = sc::ui::more_preview().device;
        state.model = "WF-1000XM5";
        state.touch.assignments = known(p::Bytes{0, 0});
        state.touch.assistant_writable = true;
        state.touch.voice_assistant = known(0xff);
        state.touch.voice_assistant_candidates = {0x30, 0xff};
        busy = false;
        offline = false;
        state.device = sc::DeviceDescriptor{};
        const auto prerequisite = sc::ui::plan_mobile_assistant(state, 0x30);
        check("assistant_without_playback_offers_advertised_ears", !prerequisite.plan && prerequisite.playback_keys == p::Bytes({0, 1}));
        const auto enable = sc::ui::plan_mobile_assistant(state, 0x30, p::Byte{1});
        check("assistant_enable_orders_touch_before_assistant", enable.plan && enable.plan->steps.size() == 2 &&
              enable.plan->steps[0].kind == sc::ui::SettingsStepKind::touch);
        state.touch.assignments = known(p::Bytes{0, 0x20});
        state.touch.voice_assistant = known(0x30);
        const auto remove = sc::ui::plan_touch_assignment(state, 1, 0);
        check("last_playback_removal_orders_assistant_disable_first", remove.plan && remove.plan->steps.size() == 2 &&
              remove.plan->steps[0].kind == sc::ui::SettingsStepKind::assistant && remove.plan->steps[0].value == 0xff);
        offline = true;
        const bool passed = std::all_of(checks.begin(), checks.end(), [](auto &c) { return c.second; });
        std::ofstream out(report);
        out << "{\"Passed\":" << (passed ? "true" : "false")
            << ",\"HardwareVerified\":false,\"BluetoothApisCalled\":false,\"LegacyWindowShown\":false,\"Scenarios\":[";
        for (std::size_t i = 0; i < checks.size(); ++i)
            out << (i ? "," : "") << "{\"Name\":\"" << checks[i].first << "\",\"Passed\":"
                << (checks[i].second ? "true" : "false") << '}';
        out << "]}";
        offline = true;
        return passed && out.good();
    }
    bool pre_translate(const MSG &message) {
        for (auto *view : {personalization_view.get(), advanced_view.get(), diagnostics_view.get(), replay_view.get()})
            if (view && (message.hwnd == view->handle() || IsChild(view->handle(), message.hwnd))) {
                auto copy = message;
                return view->pre_translate(copy);
            }
        if (more_view && (message.hwnd == more_view->handle() || IsChild(more_view->handle(), message.hwnd))) {
            auto copy = message;
            return more_view->pre_translate(copy);
        }
        if (main_view && (message.hwnd == main_view->handle() ||
                         IsChild(main_view->handle(), message.hwnd))) {
            auto copy = message;
            if (main_view->pre_translate(copy)) return true;
        }
        return false;
    }

  private:
    std::unique_ptr<sc::ui::MainWindow> main_view;
    std::unique_ptr<sc::ui::MoreWindow> more_view, personalization_view, advanced_view, diagnostics_view, replay_view;
    d::DiagnosticOptions diagnostic_options;
    std::wstring diagnostic_feedback, replay_name;
    bool diagnostic_error{};
    std::shared_ptr<const std::vector<d::DiagnosticReplayEntry>> replay_entries;
    std::optional<std::filesystem::path> diagnostic_test_path;
    HWND replay_request_owner{};
    std::array<sc::ui::MainSliderStyle, 4> slider_styles{};
    int more_edit_control{-1};
    std::uint64_t more_edit_link{};
    bool feedback_error{};
    sc::ui::MainSliderStyle main_slider_style{sc::ui::MainSliderStyle::wake};
    void update_main() {
        if (!main_view) return;
        if (!preview_main.empty()) {
            main_view->update(sc::ui::main_preview(preview_main));
            if (more_view) more_view->update(sc::ui::more_preview(main_slider_style));
            return;
        }
        sc::ui::MainModel model;
        model.device = state;
        model.slider_style = main_slider_style;
        model.ambient_slider_style = slider_styles[1];
        model.busy = busy;
        model.discovering = busy && !state.ready;
        model.pressure = sound_pressure_text(state);
        {
            std::lock_guard lock(data_mutex);
            model.local = local;
            model.discovered = found;
            model.feedback = feedback;
            model.feedback_error = feedback_error;
            model.operation_status = operation_status;
            model.operation_elapsed_ms = busy && foreground_enqueued_at
                                             ? GetTickCount64() - foreground_enqueued_at : 0;
            model.last_operation_elapsed_ms = last_operation_elapsed_ms;
            const auto target = [&](int id) -> std::optional<int> {
                const auto valid = [&](const SliderIntent &intent) {
                    return intent.id == id && intent.link == state.link &&
                           (id != local_volume ||
                            (state.device && state.device->aep_id == intent.device &&
                             !intent.endpoint.empty() && intent.endpoint == local.volume.endpoint_id));
                };
                if (pending_slider && valid(*pending_slider))
                    return pending_slider->value;
                for (const auto &intent : deferred_sliders)
                    if (valid(intent))
                        return intent.value;
                if (active_slider && valid(*active_slider))
                    return active_slider->value;
                return {};
            };
            model.volume_target = target(local_volume);
            model.ambient_target = target(ambient_level);
            const bool endpoint_current = state.device && state.device->address == local_device_address &&
                                          state.device->aep_id == local_device_aep &&
                                          state.link == local_device_link && local_read_at &&
                                          GetTickCount64() - local_read_at <= 3500;
            sc::ui::project_main_sources(model, endpoint_current);
        }
        main_view->update(std::move(model));
        update_more();
    }
    std::array<int, 2> more_touch_indices() const {
        std::array<int, 2> result{-1, -1};
        for (std::size_t i = 0; i < state.touch.keys.size(); ++i)
            if (state.touch.keys[i].id < 2)
                result[state.touch.keys[i].id] = static_cast<int>(i);
        for (std::size_t i = 0; i < state.touch.keys.size(); ++i) {
            if (result[0] == static_cast<int>(i) || result[1] == static_cast<int>(i))
                continue;
            if (result[0] < 0)
                result[0] = static_cast<int>(i);
            else if (result[1] < 0)
                result[1] = static_cast<int>(i);
        }
        return result;
    }
    void update_more() {
        if (!more_view && !personalization_view && !advanced_view && !diagnostics_view && !replay_view)
            return;
        if (more_edit_control >= 0 && (more_edit_link != state.link || !state.ready)) {
            if (editing_slider == more_edit_control)
                editing_slider = -1;
            more_edit_control = -1;
        }
        sc::ui::MoreModel m;
        m.device = state;
        m.busy = busy;
        m.offline = offline;
        m.slider_style = main_slider_style;
        m.update_checking = update_checking;
        m.update_canceling = update_checking && update_cancel.load();
        m.update_channel = selected_update_channel == d::UpdateChannel::gitee ? 1 : 0;
        m.log_mib = diagnostic_options.segment_limit_mib;
        m.log_runs = diagnostic_options.retained_runs;
        m.diagnostics_available = diagnostics != nullptr;
        m.diagnostic_busy = busy;
        m.diagnostic_status = diagnostics ? diagnostics->status() : L"诊断服务未启用";
        m.slider_styles = slider_styles;
        m.eq_slider_style = slider_styles[2];
        m.bass_slider_style = slider_styles[3];
        for (int value : state.touch.voice_assistant_candidates)
            m.assistant_choices.push_back({value, assistant_name(value)});
        m.assistant_name = state.touch.voice_assistant.known() ? assistant_name(*state.touch.voice_assistant.value) : L"尚未读取";
        m.linked_restore_available = state.touch.linked_backup_assignments.has_value() && state.touch.linked_backup_assistant.has_value();
        if (state.model != "WF-1000XM5") {
            for (std::size_t i = 0; i < state.touch.keys.size(); ++i)
                m.custom_choices[0].push_back({state.touch.keys[i].id, touch_key_name(state.touch.keys[i].id)});
            const int key = custom_key_index;
            if (key >= 0 && static_cast<std::size_t>(key) < state.touch.keys.size())
                m.custom_values[0] = state.touch.keys[key].id;
            if (auto preset = selected_touch_preset(); preset && key >= 0 &&
                static_cast<std::size_t>(key) < state.touch.keys.size()) {
                const int action = custom_action_value;
                m.custom_values[1] = action;
                for (const auto &a : preset->customizable) {
                    if (a.action == 0 && !a.functions.empty() &&
                        std::all_of(a.functions.begin(), a.functions.end(), [](auto f) { return f >= 1 && f <= 4; })) continue;
                    m.custom_choices[1].push_back({a.action, action_name(a.action)});
                    if (a.action == action)
                        for (int function : a.functions) m.custom_choices[2].push_back({function, function_name(function)});
                }
                if (state.touch.details.known())
                    for (const auto &actual : *state.touch.details.value)
                        if (actual.preset == preset->id)
                            for (const auto &a : actual.actions)
                                if (a.action == action) m.custom_values[2] = a.function;
                if (action >= 0)
                    m.custom_context = (state.touch.keys[key].id << 16) | (preset->id << 8) | action;
            }
        }
        m.startup = startup_preference;
        m.close_to_tray = close_to_tray;
        m.touch_indices = more_touch_indices();
        for (int i = 0; i < 2; ++i)
            if (m.touch_indices[i] >= 0) {
                const auto &key = state.touch.keys[m.touch_indices[i]];
                m.touch_labels[i] = touch_key_name(key.id);
                for (const auto &pr : key.presets)
                    m.touch_choices[i].push_back({pr.id, preset_name(pr.id)});
                if (state.touch.assignments.known() &&
                    static_cast<std::size_t>(m.touch_indices[i]) < state.touch.assignments.value->size())
                    m.touch_names[i] = preset_name((*state.touch.assignments.value)[m.touch_indices[i]]);
            }
        if (auto it = state.features.find(p::Feature::AutoPowerOff); it != state.features.end())
            for (int value : it->second.choices)
                m.auto_power_choices.push_back({value, choice_name(it->first, value)});
        {
            std::lock_guard lock(data_mutex);
            m.feedback = feedback;
            m.feedback_error = feedback_error;
            m.diagnostic_feedback = diagnostic_feedback;
            m.diagnostic_error = diagnostic_error;
            m.replay_entries = replay_entries;
            m.replay_name = replay_name;
            if (update_checking) m.update_message = m.update_canceling ? L"正在取消更新检查…" : L"正在检查 " + std::wstring(m.update_channel ? L"Gitee" : L"GitHub") + L"…";
            else if (last_update_result && checked_update_channel == selected_update_channel) {
                m.update_message = last_update_result->message;
                m.update_error = last_update_result->status == d::UpdateStatus::failed;
            }
            const auto context = sc::ui::settings_context(state);
            if (linked_ui_backup && (!state.ready || linked_ui_backup->after.link != state.link ||
                                     (!busy && (!context || *context != linked_ui_backup->after))))
                linked_ui_backup.reset();
            m.linked_restore_available |= linked_ui_backup && context && *context == linked_ui_backup->after;
            auto target = [&](int id) -> std::optional<int> {
                if (more_edit_control == id && more_edit_link == state.link)
                    return more_edit_value;
                if (pending_slider && pending_slider->id == id && pending_slider->link == state.link)
                    return pending_slider->value;
                for (const auto &intent : deferred_sliders)
                    if (intent.id == id && intent.link == state.link)
                        return intent.value;
                if (active_slider && active_slider->id == id && active_slider->link == state.link)
                    return active_slider->value;
                return {};
            };
            for (int i = 0; i < 6; ++i)
                m.equalizer_targets[i] = target(eq_band + i);
            m.prompt_target = target(volume_prompt);
        }
        if (more_view) more_view->update(m);
        if (personalization_view) personalization_view->update(m);
        if (advanced_view) advanced_view->update(m);
        if (diagnostics_view) diagnostics_view->update(m);
        if (replay_view) replay_view->update(std::move(m));
    }
    void diagnostic_result(std::wstring text, bool error = false) {
        std::lock_guard lock(data_mutex);
        diagnostic_feedback = std::move(text);
        diagnostic_error = error;
    }
    bool apply_diagnostic_limits(unsigned mib, unsigned runs) {
        try {
            d::DiagnosticOptions options{mib, runs};
            options.validate();
            if (!diagnostics) throw std::runtime_error("诊断服务未启用。");
            const auto path = root / L"desktop.ini";
            if (std::filesystem::exists(path))
                std::filesystem::copy_file(path, root / L"desktop.ini.bak", std::filesystem::copy_options::overwrite_existing);
            if (!WritePrivateProfileStringW(L"Diagnostics", L"SegmentMiB", std::to_wstring(mib).c_str(), path.c_str()) ||
                !WritePrivateProfileStringW(L"Diagnostics", L"RetainedRuns", std::to_wstring(runs).c_str(), path.c_str()))
                throw std::runtime_error("无法保存诊断限制。");
            diagnostics->apply_options(options);
            diagnostic_options = options;
            diagnostic_result(L"诊断限制已应用。");
            update_more();
            return true;
        } catch (const std::exception &e) {
            diagnostic_result(L"诊断限制未应用：" + wide(e.what()), true);
            update_more();
            return false;
        }
    }
    bool perform_diagnostic_file(const std::filesystem::path &path, bool save) {
        try {
            if (save) {
                if (!diagnostics) throw std::runtime_error("诊断服务未启用。");
                diagnostics->export_archive(path);
                diagnostic_result(L"脱敏诊断包已导出：" + path.wstring());
            } else {
                auto entries = std::make_shared<const std::vector<d::DiagnosticReplayEntry>>(d::DiagnosticLog::read_replay(path));
                { std::lock_guard lock(data_mutex); replay_entries = std::move(entries); replay_name = path.filename().wstring(); }
                diagnostic_result(L"诊断包已读取。离线只读，不发送设备指令。");
            }
            return true;
        } catch (const winrt::hresult_error &e) {
            diagnostic_result((save ? L"导出失败：" : L"诊断包读取失败，保留当前时间线：") + std::wstring(e.message()), true);
        } catch (const std::exception &e) {
            diagnostic_result((save ? L"导出失败：" : L"诊断包读取失败，保留当前时间线：") + wide(e.what()), true);
        }
        return false;
    }
    void diagnostic_file_action(bool save) {
        if (offline || busy || !preview_main.empty()) return;
        const auto path = ui_test ? diagnostic_test_path.value_or(std::filesystem::path{})
                                 : dialog_path(save, L"SonyConnect 诊断包\0*.scdiag\0\0", L"scdiag");
        if (path.empty()) return;
        if (!save) replay_request_owner = settings_owner();
        if (ui_test) {
            if (perform_diagnostic_file(path, save) && !save) message(diagnostic_loaded_message, 0, 0);
            update_more();
            return;
        }
        diagnostic_result(save ? L"正在导出脱敏诊断包…" : L"正在读取诊断包…");
        enqueue([this, path, save] {
            if (perform_diagnostic_file(path, save) && !save) PostMessageW(hwnd, diagnostic_loaded_message, 0, 0);
        }, true, save ? L"正在导出脱敏诊断包…" : L"正在读取离线时间线…");
    }
    void save_update_channel(d::UpdateChannel channel) {
        if (update_checking) return;
        if (channel != selected_update_channel) {
            const auto path = root / L"desktop.ini";
            if (std::filesystem::exists(path)) {
                auto backup = path; backup += L".bak";
                std::filesystem::copy_file(path, backup, std::filesystem::copy_options::overwrite_existing);
            }
            if (!WritePrivateProfileStringW(L"UI", L"UpdateChannel", channel == d::UpdateChannel::gitee ? L"1" : L"0", path.c_str()))
                throw std::runtime_error("无法保存更新渠道。");
        }
        selected_update_channel = channel;
        update_more();
    }
    void open_release_page() {
        const auto source = d::UpdateSource::load(executable_path().parent_path() / L"config" / L"update-source.json");
        const auto url = source.release_page(selected_update_channel);
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(settings_owner(), L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            throw std::runtime_error("无法打开系统浏览器中的发行版页面。");
    }
    void open_more() {
        if (!more_view) {
            more_view =
                std::make_unique<sc::ui::MoreWindow>([this](auto action, int value, int detail, bool final) {
                    more_action(action, value, detail, final);
                });
            more_view->create(reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)),
                              main_view->handle(), preview_dpi);
        }
        if (!preview_main.empty())
            more_view->update(sc::ui::more_preview(main_slider_style));
        else
            update_more();
        more_view->show();
    }
    void open_settings(sc::ui::SettingsPage kind, bool show = true) {
        auto &view = kind == sc::ui::SettingsPage::personalization ? personalization_view
                     : kind == sc::ui::SettingsPage::diagnostics ? diagnostics_view
                     : kind == sc::ui::SettingsPage::replay ? replay_view : advanced_view;
        if (!view) {
            view = std::make_unique<sc::ui::MoreWindow>([this](auto a, int v, int d, bool f) {
                more_action(a, v, d, f);
            }, kind);
            const HWND owner = kind == sc::ui::SettingsPage::replay && diagnostics_view ? diagnostics_view->handle()
                               : more_view ? more_view->handle() : main_view->handle();
            view->create(reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)), owner, preview_dpi);
        }
        if (!preview_main.empty()) view->update(sc::ui::more_preview(main_slider_style));
        else update_more();
        if (show) view->show();
    }
    HWND settings_owner(HWND preferred = nullptr) const {
        const auto visible = [](HWND window) {
            return window && IsWindow(window) && IsWindowVisible(window) && !IsIconic(window);
        };
        if (visible(preferred)) return preferred;
        const std::array windows{replay_view ? replay_view->handle() : nullptr,
                                 diagnostics_view ? diagnostics_view->handle() : nullptr,
                                 advanced_view ? advanced_view->handle() : nullptr,
                                 personalization_view ? personalization_view->handle() : nullptr,
                                 more_view ? more_view->handle() : nullptr,
                                 main_view ? main_view->handle() : nullptr, hwnd};
        const HWND active = GetActiveWindow();
        for (const HWND window : windows)
            if (window == active && visible(window)) return window;
        for (const HWND window : windows)
            if (visible(window)) return window;
        return main_view ? main_view->handle() : hwnd;
    }
    void settings_error(std::wstring text) {
        set_feedback(std::move(text), true);
        update_more();
    }
    void submit_settings_plan(sc::ui::LinkedSettingsPlan plan, bool restoring = false) {
        enqueue([this, plan = std::move(plan), restoring] {
            const auto flow = sc::ui::run_settings_flow(plan,
                [this] { return controller->snapshot(); },
                [this](const sc::ui::SettingsStep &step) {
                    return step.kind == sc::ui::SettingsStepKind::assistant
                        ? controller->set_voice_assistant(step.value)
                        : controller->set_touch(step.key, step.value, true);
                });
            {
                std::lock_guard lock(data_mutex);
                linked_ui_backup = sc::ui::merge_settings_backup(linked_ui_backup, plan, flow, restoring);
            }
            result(flow.result);
        });
    }
    std::wstring settings_plan_text(const sc::ui::LinkedSettingsPlan &plan) const {
        auto assignments = plan.before.assignments;
        int current_assistant = plan.before.assistant;
        std::wstring text;
        for (const auto &step : plan.steps) {
            if (step.kind == sc::ui::SettingsStepKind::assistant) {
                if (current_assistant != step.value)
                    text += L"语音助手：" + assistant_name(current_assistant) + L" → " + assistant_name(step.value) + L"\n";
                current_assistant = step.value;
            } else {
                const auto key = std::find(plan.before.keys.begin(), plan.before.keys.end(), step.key);
                if (key != plan.before.keys.end()) {
                    const auto i = static_cast<std::size_t>(key - plan.before.keys.begin());
                    text += touch_key_name(step.key) + L"：" + preset_name(assignments[i]) + L" → " + preset_name(step.value) + L"\n";
                    assignments[i] = step.value;
                }
            }
        }
        return text + L"\n按上述顺序提交，每一步均等待耳机确认。\n中途失败会停止，已经生效的设置会保留并显示。";
    }
    bool confirm_settings_plan(const sc::ui::LinkedSettingsPlan &plan, bool restoring = false) {
        return MessageBoxW(settings_owner(), settings_plan_text(plan).c_str(),
                           restoring ? L"恢复关联备份" : L"确认关联修改",
                           MB_OKCANCEL | MB_ICONQUESTION) == IDOK;
    }
    void request_mobile_assistant(p::Byte value) {
        const auto original = sc::ui::settings_context(state);
        auto proposed = sc::ui::plan_mobile_assistant(state, value);
        if (!proposed.reason.empty()) { settings_error(wide(proposed.reason)); return; }
        if (!proposed.playback_keys.empty()) {
            std::vector<std::wstring> labels;
            std::vector<TASKDIALOG_BUTTON> radios;
            labels.reserve(proposed.playback_keys.size());
            for (auto key : proposed.playback_keys) {
                const auto index = std::find(original->keys.begin(), original->keys.end(), key) - original->keys.begin();
                labels.push_back(touch_key_name(key) + L"：" + preset_name(original->assignments[index]) + L" → 播放控制");
            }
            for (std::size_t i = 0; i < proposed.playback_keys.size(); ++i)
                radios.push_back({100 + proposed.playback_keys[i], labels[i].c_str()});
            TASKDIALOGCONFIG dialog{sizeof(dialog)};
            dialog.hwndParent = settings_owner();
            dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
            dialog.dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
            dialog.pszWindowTitle = L"更改语音助手";
            dialog.pszMainInstruction = L"选择一侧耳机分配播放控制";
            dialog.pszContent = L"移动设备语音助手需要播放控制。确认后先修改所选耳侧，\n再启用移动设备语音助手，每一步均等待耳机回读。\n中途失败会停止，已经生效的设置会保留。";
            dialog.cRadioButtons = static_cast<UINT>(radios.size());
            dialog.pRadioButtons = radios.data();
            dialog.nDefaultRadioButton = radios.back().nButtonID;
            int button{}, selected{};
            winrt::check_hresult(TaskDialogIndirect(&dialog, &button, &selected, nullptr));
            if (button != IDOK) return;
            const auto current = controller->snapshot();
            if (sc::ui::settings_context(current) != original) {
                settings_error(L"设备连接或配置已变化，请重新选择。");
                return;
            }
            proposed = sc::ui::plan_mobile_assistant(current, value, static_cast<p::Byte>(selected - 100));
        }
        if (!proposed.plan) { settings_error(wide(proposed.reason)); return; }
        if (proposed.plan->steps.empty()) return;
        submit_settings_plan(std::move(*proposed.plan));
    }
    bool start_update(d::UpdateChannel channel, d::SoftwareUpdateService::Fetch fetch = {}, HWND owner = nullptr) {
        if (update_checking || (offline && !ui_test) || (ui_test && !fetch)) return false;
        if (update_worker.joinable()) update_worker.join();
        const auto source = d::UpdateSource::load(executable_path().parent_path() / L"config" / L"update-source.json");
        selected_update_channel = channel;
        checked_update_channel = channel;
        update_request_owner = settings_owner(owner);
        update_cancel = false;
        update_checking = true;
        update_more();
        try {
            update_worker = std::thread([this, source, channel, fetch = std::move(fetch)] {
                d::UpdateResult checked;
                try {
                    winrt::init_apartment(winrt::apartment_type::multi_threaded);
                    checked = d::SoftwareUpdateService(source, SONYCONNECT_VERSION, fetch).check(channel, L"", &update_cancel);
                    winrt::uninit_apartment();
                } catch (...) {
                    checked = {d::UpdateStatus::failed, L"更新检测未完成。", {}};
                }
                { std::lock_guard lock(data_mutex); last_update_result = std::move(checked); }
                if (!update_closing.load()) PostMessageW(hwnd, update_done_message, 0, 0);
            });
        } catch (...) {
            update_checking = false;
            update_more();
            throw;
        }
        return true;
    }
    void finish_update() {
        if (!update_checking) return;
        if (update_worker.joinable()) update_worker.join();
        update_checking = false;
        std::optional<d::UpdateResult> checked;
        { std::lock_guard lock(data_mutex); checked = last_update_result; }
        update_more();
        const HWND owner = settings_owner(update_request_owner);
        if (!checked || ui_test || !IsWindowVisible(owner) || IsIconic(owner) || checked->status == d::UpdateStatus::canceled ||
            (diagnostics_view && IsWindowVisible(diagnostics_view->handle()))) return;
        const auto source = d::UpdateSource::load(executable_path().parent_path() / L"config" / L"update-source.json");
        const auto channel = selected_update_channel;
        const std::wstring details = L"软件版本：v" SONYCONNECT_VERSION L"\n更新渠道：" +
            std::wstring(channel == d::UpdateChannel::github ? L"GitHub" : L"Gitee") + L"\n\n" + checked->message;
        TASKDIALOG_BUTTON open{100, L"查看对应发行版页面"};
        TASKDIALOGCONFIG dialog{sizeof(dialog)};
        dialog.hwndParent = owner;
        dialog.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
        dialog.dwCommonButtons = TDCBF_CLOSE_BUTTON;
        dialog.pszWindowTitle = L"SonyConnect — 软件更新";
        dialog.pszMainInstruction = checked->status == d::UpdateStatus::available ? L"发现新版本" : L"更新检查结果";
        dialog.pszContent = details.c_str();
        dialog.pszMainIcon = checked->status == d::UpdateStatus::failed ? TD_WARNING_ICON : TD_INFORMATION_ICON;
        dialog.cButtons = 1;
        dialog.pButtons = &open;
        dialog.nDefaultButton = IDCLOSE;
        int button{};
        winrt::check_hresult(TaskDialogIndirect(&dialog, &button, nullptr, nullptr));
        if (button == 100) {
            const auto url = source.release_page(channel);
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(owner, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                throw std::runtime_error("无法打开系统浏览器中的发行版页面。");
        }
    }
    void open_update_dialog(HWND owner = nullptr) {
        if (update_checking) {
            update_cancel = true;
            update_more();
            return;
        }
        owner = settings_owner(owner);
        TASKDIALOG_BUTTON buttons[] = {{100, L"检查更新"}, {101, L"查看发行版页面"}};
        TASKDIALOG_BUTTON radios[] = {{1, L"GitHub"}, {2, L"Gitee"}};
        std::wstring content = L"当前软件版本：v" SONYCONNECT_VERSION L"\n选择公开更新渠道。新版通过浏览器下载。";
        { std::lock_guard lock(data_mutex); if (last_update_result) content += L"\n\n上次检查：" + last_update_result->message; }
        TASKDIALOGCONFIG dialog{sizeof(dialog)};
        dialog.hwndParent = owner;
        dialog.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
        dialog.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        dialog.pszWindowTitle = L"SonyConnect — 软件更新";
        dialog.pszMainInstruction = L"选择更新渠道";
        dialog.pszContent = content.c_str();
        dialog.cButtons = 2;
        dialog.pButtons = buttons;
        dialog.cRadioButtons = 2;
        dialog.pRadioButtons = radios;
        dialog.nDefaultRadioButton = selected_update_channel == d::UpdateChannel::gitee ? 2 : 1;
        dialog.nDefaultButton = 100;
        int button{}, radio{};
        winrt::check_hresult(TaskDialogIndirect(&dialog, &button, &radio, nullptr));
        if (button != 100 && button != 101) return;
        const auto channel = radio == 2 ? d::UpdateChannel::gitee : d::UpdateChannel::github;
        save_update_channel(channel);
        if (button == 100) start_update(channel, {}, owner);
        else open_release_page();
    }
    void set_slider_appearance(int slot, int value) {
        if (slot < 0 || slot > 3 || value < 0 || value > 1 || !preview_main.empty()) return;
        if (static_cast<int>(slider_styles[slot]) == value) return;
        if (!offline) {
            save_appearance(root / L"desktop.ini", slot, value);
        }
        slider_styles[slot] = value ? sc::ui::MainSliderStyle::elastic : sc::ui::MainSliderStyle::wake;
        main_slider_style = slider_styles[0];
        update_main();
    }
    void more_action(sc::ui::MoreAction action, int value, int detail, bool final) {
        using A = sc::ui::MoreAction;
        if (action == A::personalization || action == A::advanced) {
            open_settings(action == A::personalization ? sc::ui::SettingsPage::personalization : sc::ui::SettingsPage::advanced);
            return;
        }
        if (action == A::slider_appearance) {
            set_slider_appearance(detail, value);
            return;
        }
        if (action == A::diagnostics) {
            open_settings(sc::ui::SettingsPage::diagnostics, !ui_test);
            return;
        }
        if (action == A::diagnostic_limits || action == A::diagnostic_export || action == A::diagnostic_replay ||
            action == A::update_channel || action == A::check_update || action == A::release_page) {
            if (offline || !preview_main.empty()) return;
            if (action == A::diagnostic_limits) {
                if (!busy) apply_diagnostic_limits(static_cast<unsigned>(value), static_cast<unsigned>(detail));
            } else if (action == A::diagnostic_export || action == A::diagnostic_replay) {
                try { diagnostic_file_action(action == A::diagnostic_export); }
                catch (const std::exception &e) { diagnostic_result(wide(e.what()), true); update_more(); }
            } else if (action == A::check_update) {
                if (update_checking) { update_cancel = true; update_more(); }
                else start_update(selected_update_channel, {}, diagnostics_view ? diagnostics_view->handle() : nullptr);
            } else {
                try {
                    if (action == A::update_channel && (value == 0 || value == 1))
                        save_update_channel(value ? d::UpdateChannel::gitee : d::UpdateChannel::github);
                    else if (action == A::release_page) { if (ui_test) ++ui_dispatches; else open_release_page(); }
                } catch (const std::exception &e) { diagnostic_result(wide(e.what()), true); update_more(); }
            }
            return;
        }
        if (action == A::releases) {
            if (!offline) open_update_dialog(more_view ? more_view->handle() : nullptr);
            return;
        }
        if (action == A::cancel_edit) {
            const auto original = static_cast<A>(value);
            const int id = original == A::prompt_volume ? volume_prompt
                           : original == A::clear_bass  ? eq_band + 5
                                                        : eq_band + detail;
            discard_unsent_slider(id);
            if (more_edit_control == id) {
                more_edit_control = -1;
                if (editing_slider == id)
                    editing_slider = -1;
                render();
            }
            return;
        }
        if (offline)
            return;
        if (action == A::startup || action == A::close_to_tray) {
            const bool old_tray = close_to_tray;
            try {
                if (action == A::startup) {
                    set_startup(value != 0);
                    startup_preference = startup_enabled();
                    set_feedback(L"原生版开机启动设置已保存。");
                } else {
                    const auto file = root / L"desktop.ini";
                    if (std::filesystem::exists(file))
                        std::filesystem::copy_file(file, root / L"desktop.ini.bak", std::filesystem::copy_options::overwrite_existing);
                    if (!WritePrivateProfileStringW(L"Desktop", L"CloseToTray", value ? L"1" : L"0", file.c_str()))
                        throw std::runtime_error("Cannot save desktop settings");
                    close_to_tray = value != 0;
                    set_feedback(L"关闭窗口行为已保存。");
                }
            } catch (const std::exception &e) {
                close_to_tray = old_tray;
                set_feedback(wide(e.what()), true);
            }
            render();
            return;
        }
        if (action == A::equalizer_band || action == A::clear_bass || action == A::prompt_volume) {
            const int id = action == A::prompt_volume ? volume_prompt
                           : action == A::clear_bass  ? eq_band + 5
                                                      : eq_band + detail;
            if (detail < 0 || detail > 4 || !state.ready || busy || !slider_writable(id))
                return;
            more_edit_value = value;
            if (!final) {
                more_edit_control = id;
                more_edit_link = state.link;
                editing_slider = id;
                update_more();
            } else {
                more_edit_control = -1;
                editing_slider = -1;
                apply_slider(id, value);
                update_more();
            }
            return;
        }
        if (busy || !state.ready || state.busy)
            return;
        if (action == A::custom_key || action == A::custom_action || action == A::custom_function) {
            if (state.model == "WF-1000XM5") return;
            if (action == A::custom_key) {
                const auto selected = std::find_if(state.touch.keys.begin(), state.touch.keys.end(),
                                                    [&](const auto &key) { return key.id == value; });
                if (selected == state.touch.keys.end()) return;
                custom_key_index = static_cast<int>(selected - state.touch.keys.begin());
                custom_action_value = -1;
                sync_touch_selection();
            } else {
                const int key = custom_key_index;
                const auto preset = selected_touch_preset();
                const int selected_action = custom_action_value;
                if (!preset || key < 0 || static_cast<std::size_t>(key) >= state.touch.keys.size() ||
                    selected_action < 0 || detail != ((state.touch.keys[key].id << 16) | (preset->id << 8) | selected_action)) return;
                if (action == A::custom_action) {
                    if (std::none_of(preset->customizable.begin(), preset->customizable.end(),
                                     [&](const auto &a) { return a.action == value; })) return;
                    custom_action_value = value;
                } else {
                    if (!state.touch.writable || !state.touch.details.known()) return;
                    bool allowed = false;
                    for (const auto &a : preset->customizable)
                        if (a.action == selected_action && std::find(a.functions.begin(), a.functions.end(), value) != a.functions.end()) allowed = true;
                    if (!allowed) return;
                    const auto link = state.link;
                    const auto assignments = state.touch.assignments.value;
                    const auto before = *state.touch.details.value;
                    auto after = before;
                    bool changed = false;
                    for (auto &p : after) if (p.preset == preset->id)
                        for (auto &a : p.actions) if (a.action == selected_action && a.function != value) {
                            a.function = static_cast<p::Byte>(value);
                            changed = true;
                        }
                    if (!changed) return;
                    enqueue([this, link, assignments, before, after] {
                        const auto current = controller->snapshot();
                        bool same_details = current.touch.details.known() && current.touch.details.value->size() == before.size();
                        if (same_details) for (std::size_t i = 0; i < before.size(); ++i)
                            if ((*current.touch.details.value)[i].preset != before[i].preset || (*current.touch.details.value)[i].actions != before[i].actions) same_details = false;
                        if (current.link != link || current.touch.assignments.value != assignments || !same_details) {
                            result({sc::ResultCode::Canceled, "触摸配置已变化，请重新选择手势。"});
                            return;
                        }
                        result(controller->set_touch_details(after));
                    });
                    return;
                }
            }
            update_more();
            return;
        }
        if (action == A::safe_listening) {
            request_safe_listening(value != 0);
            return;
        }
        if (action == A::voice_assistant) {
            if (!state.touch.assistant_writable ||
                std::find(state.touch.voice_assistant_candidates.begin(), state.touch.voice_assistant_candidates.end(), value) == state.touch.voice_assistant_candidates.end()) return;
            if (state.touch.voice_assistant.value == value) return;
            if (state.model == "WF-1000XM5" && value == 0x30) {
                request_mobile_assistant(static_cast<p::Byte>(value));
                return;
            }
            const auto link = state.link;
            enqueue([this, value, link] {
                if (controller->snapshot().link != link) {
                    result({sc::ResultCode::Canceled, "设备会话已变化，未修改语音助手。"});
                    return;
                }
                result(controller->set_voice_assistant(static_cast<p::Byte>(value)));
            });
            return;
        }
        if (action == A::restore_linked) {
            std::optional<sc::ui::LinkedSettingsBackup> backup;
            { std::lock_guard lock(data_mutex); backup = linked_ui_backup; }
            if (backup) {
                auto proposed = sc::ui::plan_linked_restore(state, *backup);
                if (!proposed.plan) { settings_error(wide(proposed.reason)); return; }
                if (confirm_settings_plan(*proposed.plan, true)) submit_settings_plan(std::move(*proposed.plan), true);
                return;
            }
            if (!state.touch.linked_backup_assignments || !state.touch.linked_backup_assistant) return;
            const auto link = state.link;
            if (MessageBoxW(settings_owner(), L"恢复本次连接中保存的触摸分配与语音助手配置？", L"恢复关联备份", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
            enqueue([this, link] {
                if (controller->snapshot().link != link) {
                    result({sc::ResultCode::Canceled, "设备会话已变化，未恢复关联备份。"});
                    return;
                }
                result(controller->restore_touch_linked_backup());
            });
            return;
        }
        if (action == A::equalizer_preset) {
            if (!state.equalizer.writable ||
                std::none_of(state.equalizer.presets.begin(), state.equalizer.presets.end(),
                             [&](auto &pr) { return pr.id == value; }))
                return;
            const auto link = state.link;
            enqueue([this, value, link] {
                if (controller->snapshot().link == link)
                    result(controller->set_equalizer(static_cast<p::Byte>(value)));
            });
        } else if (action == A::touch_preset) {
            const auto indices = more_touch_indices();
            if (detail < 0 || detail > 1 || indices[detail] < 0 || !state.touch.writable)
                return;
            const auto &key = state.touch.keys[indices[detail]];
            if (std::none_of(key.presets.begin(), key.presets.end(),
                             [&](auto &pr) { return pr.id == value; }))
                return;
            if (state.model == "WF-1000XM5") {
                auto proposed = sc::ui::plan_touch_assignment(state, key.id, static_cast<p::Byte>(value));
                if (!proposed.plan) { settings_error(wide(proposed.reason)); return; }
                if (proposed.plan->steps.size() > 1) {
                    if (confirm_settings_plan(*proposed.plan)) submit_settings_plan(std::move(*proposed.plan));
                    return;
                }
            }
            submit_touch(indices[detail], value);
        } else if (action == A::cycle) {
            if (!state.touch.cycle_writable || value < 1 || value > 4)
                return;
            const auto link = state.link;
            enqueue([this, value, link] {
                if (controller->snapshot().link == link)
                    result(controller->set(p::Feature::AmbientCycle, value));
            });
        } else {
            p::Feature feature;
            int raw = value;
            if (action == A::guidance) {
                feature = p::Feature::Notifications;
                raw = value ? 0 : 1;
                if (!state.touch.guidance_writable)
                    return;
            } else {
                feature = action == A::dsee            ? p::Feature::Dsee
                          : action == A::speak_to_chat ? p::Feature::SpeakToChat
                          : action == A::wear_pause    ? p::Feature::WearDetectionPause
                                                       : p::Feature::AutoPowerOff;
                if (action == A::speak_to_chat || action == A::wear_pause)
                    raw = value ? 0 : 1;
                const auto it = state.features.find(feature);
                if (it == state.features.end() || !it->second.writable ||
                    std::find(it->second.choices.begin(), it->second.choices.end(), raw) ==
                        it->second.choices.end())
                    return;
            }
            const auto link = state.link;
            enqueue([this, feature, raw, link] {
                if (controller->snapshot().link == link)
                    result(controller->set(feature, raw));
            });
        }
        update_more();
    }
    void connect_main(sc::DeviceDescriptor device) {
        try { device.aep_id = d::bluetooth_aep_id(device.address, &desktop_closing); }
        catch (const std::exception &) { device.aep_id.clear(); }
        if (desktop_closing.load()) return;
        result(controller->connect(device));
        refresh_local();
    }
    void discover_main() {
        if (offline || busy) return;
        enqueue([this] {
            auto r = controller->discover();
            {
                std::lock_guard lock(data_mutex);
                found = r.devices;
            }
            if (r.result.ok() && r.devices.size() == 1) connect_main(r.devices.front());
            else if (r.result.ok() && r.devices.size() > 1) {
                std::lock_guard lock(data_mutex);
                feedback = L"发现多副耳机，请从标题栏选择。";
            } else result(r.result);
        });
    }
    void main_action(sc::ui::MainAction action, int value, bool final, std::uint64_t address) {
        using A = sc::ui::MainAction;
        if (action == A::close) { PostMessageW(hwnd, main_close_message, 0, 0); return; }
        if (action == A::more) { open_more(); return; }
        if (action == A::cancel_edit) {
            if (value < 0 || value > 1) return;
            const int id = value == 0 ? local_volume : ambient_level;
            discard_unsent_slider(id);
            if (editing_slider == id) restore_slider(id);
            return;
        }
        if (action == A::slider_style && (value == 0 || value == 1)) {
            set_slider_appearance(0, value);
            return;
        }
        if (action == A::bluetooth_settings) {
            if (!offline) ShellExecuteW(main_view->handle(), L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        if (action == A::discover) { discover_main(); return; }
        if (action == A::select_device) {
            sc::DeviceDescriptor device;
            { std::lock_guard lock(data_mutex);
              auto selected = std::find_if(found.begin(), found.end(), [address](const auto &d) {
                  return d.address == address;
              });
              if (selected == found.end()) return;
              device = *selected; }
            enqueue([this, device] { connect_main(device); });
            return;
        }
        if (action == A::releases) { if (!offline) open_update_dialog(main_view ? main_view->handle() : nullptr); return; }
        if (offline || busy) {
            const bool own_ambient = action == A::ambient_level &&
                ((pending_slider && pending_slider->id == ambient_level) ||
                 (active_slider && active_slider->id == ambient_level));
            if ((action != A::volume && !own_ambient) || offline) return;
        }
        if (action == A::sound_mode) {
            if (!state.sound.writable || value < 0 || value > 2) return;
            const auto mode = value == 0 ? p::SoundMode::NoiseCanceling : value == 1 ? p::SoundMode::Ambient : p::SoundMode::Off;
            enqueue([this, mode] { result(controller->set_sound(mode)); }, true, L"正在切换声音模式，等待耳机回读…");
        }
        else if (action == A::voice_focus || action == A::multipoint) {
            if (action == A::voice_focus) {
                if (!state.sound.writable || state.sound.mode.value != p::SoundMode::Ambient) return;
                enqueue([this, value] { result(controller->set_sound({}, {}, value != 0)); },
                        true, L"正在设置关注语音，等待耳机回读…");
            } else {
                if (!state.sources.multipoint_writable) return;
                if (!ui_test) refresh_local_requested = true;
                enqueue([this, value] { result(controller->set(p::Feature::Multipoint, value ? 1 : 0)); },
                        true, L"正在切换双设备连接，等待耳机回读…");
            }
        } else if (action == A::ambient_level || action == A::volume) {
            auto id = action == A::ambient_level ? ambient_level : local_volume;
            editing_slider = final ? -1 : id;
            apply_slider(id, value, final);
        } else if (action == A::connection_preference) {
            auto f = state.features.find(p::Feature::ConnectionPreference);
            if (f == state.features.end() || !f->second.writable ||
                std::find(f->second.choices.begin(), f->second.choices.end(), value) == f->second.choices.end()) return;
            enqueue([this, value] { result(controller->set(p::Feature::ConnectionPreference, value)); },
                    true, L"正在切换连接质量，等待耳机回读…");
        } else if (action == A::source || action == A::fix_source) {
            const p::SourceEntry *entry = nullptr; int index = 0;
            for (const auto &candidate : state.sources.entries)
                if (candidate.connection_slot && index++ == value) { entry = &candidate; break; }
            if (!entry) return;
            auto source = entry->id; auto link = state.link;
            if (action == A::source) {
                if (!state.sources.switch_writable ||
                    (state.sources.active_source.known() && *state.sources.active_source.value == source)) return;
                enqueue([this, source, link] {
                    if (controller->snapshot().link == link) result(controller->switch_source(source));
                });
            } else {
                if (!state.touch.fixation_writable) return;
                enqueue([this, source, link] {
                    const auto current = controller->snapshot();
                    if (current.link == link && current.sources.active_source.known() &&
                        *current.sources.active_source.value == source)
                        result(controller->set(p::Feature::FixedPlayback, 1));
                });
            }
        } else if (action == A::release_source) {
            if (!state.touch.fixation_writable || state.touch.playback_fixed.value != true) return;
            enqueue([this] { result(controller->set(p::Feature::FixedPlayback, 0)); });
        } else if (action == A::previous || action == A::next || action == A::play_pause) {
            d::MediaAction requested;
            std::wstring expected;
            {
                std::lock_guard lock(data_mutex);
                requested = action == A::previous ? d::MediaAction::previous : action == A::next ? d::MediaAction::next
                            : local.media.playing ? d::MediaAction::pause : d::MediaAction::play;
                if (!local.media.allows(requested)) return;
                expected = local.media.session_id;
            }
            enqueue([this, expected, requested] {
                desktop_result(playback->execute_media(expected, requested));
                refresh_local();
            });
        } else if (action == A::power_off) {
            if (MessageBoxW(main_view->handle(), L"关闭耳机后将断开连接，是否继续？", L"关闭耳机",
                            MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
                enqueue([this] { result(controller->set(p::Feature::PowerOff, 1)); });
        }
        update_main();
    }
    struct TouchConfirmation {
        std::uint64_t link{};
        p::Byte key{}, preset{};
    };
    std::optional<TouchConfirmation> touch_confirmation;
    std::optional<sc::ui::LinkedSettingsBackup> linked_ui_backup;
    std::thread update_worker;
    std::atomic_bool update_cancel{false}, update_closing{false};
    bool update_checking{};
    HWND update_request_owner{};
    d::UpdateChannel selected_update_channel{d::UpdateChannel::github};
    std::optional<d::UpdateResult> last_update_result;
    std::optional<d::UpdateChannel> checked_update_channel;
    struct SliderIntent {
        int id{}, value{};
        std::uint64_t link{};
        ULONGLONG due{};
        std::wstring device, endpoint;
    };
    std::optional<SliderIntent> pending_slider;
    std::deque<SliderIntent> deferred_sliders;
    std::optional<SliderIntent> active_slider;
    int editing_slider{-1};
    bool background_work{};
    sc::ui::MainFixationReadback fixation_readback;
    bool refresh_local_requested{};
    ULONGLONG foreground_enqueued_at{}, last_operation_elapsed_ms{};
    std::wstring operation_status;
    std::uint64_t local_device_address{}, local_device_link{};
    std::wstring local_device_aep;
    ULONGLONG local_read_at{};
    bool close_to_tray{}, startup_preference{};
    std::atomic_bool desktop_closing{false};
    int more_edit_value{}, custom_key_index{-1}, custom_action_value{-1};
    std::uint64_t custom_selection_link{};
    std::unique_ptr<d::TrayIcon> tray;
    std::filesystem::path root;
    std::unique_ptr<sc::DeviceController> controller;
    std::unique_ptr<d::DiagnosticLog> diagnostics;
    std::unique_ptr<d::LocalPlaybackBackend> playback;
    std::thread worker;
    std::mutex queue_mutex, data_mutex;
    std::condition_variable queue_cv;
    std::deque<std::function<void()>> queue;
    bool stop{};
    std::vector<sc::DeviceDescriptor> found;
    sc::DeviceSnapshot state;
    d::LocalPlaybackSnapshot local;
    std::wstring feedback = L"就绪";
    unsigned tick{};
    std::uint64_t safe_preference_link{};
    unsigned ui_dispatches{};
    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto self = reinterpret_cast<ApplicationHost *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            self = static_cast<ApplicationHost *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            self->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(h, m, w, l);
        try {
            return self->message(m, w, l);
        } catch (const winrt::hresult_error &e) {
            self->set_feedback(L"Windows 操作失败：" + std::wstring(e.message()), true);
            return 0;
        } catch (const std::exception &e) {
            self->set_feedback(L"操作失败：" + wide(e.what()), true);
            return 0;
        } catch (...) {
            self->set_feedback(L"操作失败：未知异常。", true);
            return 0;
        }
    }
    const p::TouchPreset *selected_touch_preset() const {
        if (custom_key_index < 0 || static_cast<std::size_t>(custom_key_index) >= state.touch.keys.size() ||
            !state.touch.assignments.known() || static_cast<std::size_t>(custom_key_index) >= state.touch.assignments.value->size()) return nullptr;
        const int assigned = (*state.touch.assignments.value)[custom_key_index];
        for (const auto &preset : state.touch.keys[custom_key_index].presets)
            if (preset.id == assigned) return &preset;
        return nullptr;
    }
    void sync_touch_selection() {
        if (custom_selection_link != state.link) {
            custom_selection_link = state.link; custom_key_index = custom_action_value = -1;
        }
        if (custom_key_index < 0 || static_cast<std::size_t>(custom_key_index) >= state.touch.keys.size())
            custom_key_index = state.touch.keys.empty() ? -1 : 0;
        const auto preset = selected_touch_preset();
        const auto allowed = [](const auto &action) {
            return !(action.action == 0 && !action.functions.empty() &&
                     std::all_of(action.functions.begin(), action.functions.end(), [](auto f) { return f >= 1 && f <= 4; }));
        };
        if (!preset) { custom_action_value = -1; return; }
        if (std::none_of(preset->customizable.begin(), preset->customizable.end(),
                         [&](const auto &action) { return allowed(action) && action.action == custom_action_value; })) {
            custom_action_value = -1;
            for (const auto &action : preset->customizable) if (allowed(action)) { custom_action_value = action.action; break; }
        }
    }
    void submit_touch(int index, int value) {
        if (index < 0 || static_cast<std::size_t>(index) >= state.touch.keys.size() || value < 0) return;
        if (state.touch.assignments.known() && static_cast<std::size_t>(index) < state.touch.assignments.value->size() &&
            (*state.touch.assignments.value)[index] == value) return;
        const TouchConfirmation intent{state.link, state.touch.keys[index].id, static_cast<p::Byte>(value)};
        enqueue([this, intent] {
            auto r = controller->snapshot().link != intent.link
                         ? sc::Result{sc::ResultCode::Canceled, "设备会话已变化，请重新选择。"}
                         : controller->set_touch(intent.key, intent.preset);
            if (r.code == sc::ResultCode::ConfirmationRequired) {
                std::lock_guard lock(data_mutex); touch_confirmation = intent;
            }
            result(r);
        });
    }
    bool slider_writable(int id) {
        if (offline) return false;
        if (id == local_volume) {
            std::lock_guard lock(data_mutex);
            return local.volume.can_write && !local.volume.endpoint_id.empty();
        }
        if (!state.ready || state.busy) return false;
        if (id == ambient_level) return state.sound.writable && state.sound.mode.value == p::SoundMode::Ambient;
        if (id == volume_prompt) return state.touch.volume_writable;
        if (id >= eq_band && id < eq_band + 6) {
            const auto &eq = state.equalizer;
            int target = eq.preset.value.value_or(0);
            if (target != 0xa1 && target != 0xa2) target = 0xa0;
            return eq.writable && eq.five_band_layout && eq.preset.known() && *eq.preset.value != 0 &&
                std::any_of(eq.presets.begin(), eq.presets.end(), [target](const auto &preset) { return preset.id == target; });
        }
        return false;
    }
    void apply_slider(int id, int value, bool final = true) {
        bool local_writable = false;
        if (id == local_volume) {
            std::lock_guard lock(data_mutex);
            local_writable = !offline && local.volume.can_write && !local.volume.endpoint_id.empty();
        }
        const bool own_ambient = id == ambient_level && state.ready && state.sound.writable &&
            state.sound.mode.known() && *state.sound.mode.value == p::SoundMode::Ambient &&
            ((active_slider && active_slider->id == id) || (pending_slider && pending_slider->id == id));
        if (!slider_writable(id) && !local_writable && !own_ambient)
            return;
        SliderIntent intent{id, value, state.link,
                            final ? 0 : GetTickCount64() + (id == local_volume ? 80 : 120)};
        if (id == local_volume) {
            std::lock_guard lock(data_mutex);
            intent.device = state.device ? state.device->aep_id : L"";
            intent.endpoint = local.volume.endpoint_id;
        }
        if (pending_slider && pending_slider->id != intent.id) {
            std::erase_if(deferred_sliders, [this](const auto &old) { return old.id == pending_slider->id; });
            deferred_sliders.push_back(*pending_slider);
        }
        std::erase_if(deferred_sliders, [&intent](const auto &old) { return old.id == intent.id; });
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
        if (!deferred_sliders.empty()) {
            pending_slider = deferred_sliders.front();
            deferred_sliders.pop_front();
            SetTimer(hwnd, 3, 80, nullptr);
        }
        if (offline || intent.link != state.link || (intent.id != local_volume && !state.ready)) {
            restore_slider(intent.id);
            return;
        }
        auto id = intent.id, value = intent.value;
        // Every slider uses the same handoff: publish its intent before the
        // first busy render, and release it only on foreground completion.
        const auto submit = [&](std::function<void()> work,
                                std::wstring status = L"正在执行，等待实际结果…") {
            active_slider = intent;
            enqueue(std::move(work), true, std::move(status));
        };
        if (id == ambient_level) {
            if (!state.sound.writable || !state.sound.mode.known() ||
                *state.sound.mode.value != p::SoundMode::Ambient)
                return restore_slider(id);
            if (!state.sound.ambient_level.known() || *state.sound.ambient_level.value != value) {
                submit([this, intent] {
                    if (controller->snapshot().link != intent.link) {
                        result({sc::ResultCode::Canceled, "设备会话已变化，未修改环境声音。"});
                        return;
                    }
                    result(controller->set_sound({}, intent.value));
                },
                        L"正在调节环境声音，等待耳机回读…");
            }
        } else if (id >= eq_band && id < eq_band + 6) {
            int index = id - eq_band;
            bool changed =
                index == 5 ? !state.equalizer.clear_bass.known() || *state.equalizer.clear_bass.value != value
                           : !state.equalizer.bands.known() ||
                                 static_cast<std::size_t>(index) >= state.equalizer.bands.value->size() ||
                                 (*state.equalizer.bands.value)[index] != value;
            if (changed) {
                submit([this, index, intent] {
                    if (controller->snapshot().link != intent.link) {
                        result({sc::ResultCode::Canceled, "设备会话已变化，未修改均衡器。"});
                        return;
                    }
                    result(index == 5 ? controller->set_equalizer({}, {}, {}, intent.value)
                                      : controller->set_equalizer({}, index, intent.value));
                });
            }
        } else if (id == volume_prompt) {
            if (!state.touch.volume_writable) {
                restore_slider(id);
                return;
            }
            if (!state.touch.prompt_volume.known() || *state.touch.prompt_volume.value != value) {
                submit([this, intent] {
                    if (controller->snapshot().link != intent.link) {
                        result({sc::ResultCode::Canceled, "设备会话已变化，未修改提示音量。"});
                        return;
                    }
                    result(controller->set(p::Feature::PromptVolume, intent.value));
                });
            }
        } else if (id == local_volume) {
            {
                std::lock_guard lock(data_mutex);
                if (local.volume.endpoint_id == intent.endpoint &&
                    std::lround(local.volume.percent) == intent.value)
                    return;
            }
            submit([this, intent] {
                const auto current = controller->snapshot();
                if (current.link != intent.link || !current.device || current.device->aep_id != intent.device) {
                    result({sc::ResultCode::Canceled, "设备会话已变化，未修改本机音量。"});
                    return;
                }
                desktop_result(playback->set_volume(intent.device, intent.endpoint, intent.value));
                refresh_local();
            }, L"正在同步本机音量…");
        }
    }
    void discard_unsent_slider(int id) {
        if (pending_slider && pending_slider->id == id) pending_slider.reset();
        std::erase_if(deferred_sliders, [id](const auto &intent) { return intent.id == id; });
        if (!pending_slider && !deferred_sliders.empty()) {
            pending_slider = deferred_sliders.front();
            deferred_sliders.pop_front();
        }
        if (!pending_slider) KillTimer(hwnd, 3);
    }
    void restore_slider(int id) {
        if (editing_slider == id)
            editing_slider = -1;
        render();
    }
    void set_feedback(std::wstring text, bool error = false) {
        std::lock_guard lock(data_mutex);
        feedback = std::move(text);
        feedback_error = error;
    }
    void enqueue(std::function<void()> work, bool foreground = true,
                 std::wstring wait_status = L"正在执行，等待实际结果…") {
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
        if (foreground) {
            busy = true;
            {
                std::lock_guard lock(data_mutex);
                foreground_enqueued_at = GetTickCount64();
                operation_status = wait_status;
            }
        } else
            background_work = true;
        if (foreground) {
            set_feedback(std::move(wait_status));
            render();
        }
        {
            std::lock_guard lock(queue_mutex);
            queue.push_back([this, work = std::move(work), foreground] {
                try {
                    work();
                } catch (const winrt::hresult_error &e) {
                    std::lock_guard lock(data_mutex);
                    feedback = L"Windows 操作失败：" + std::wstring(e.message());
                    feedback_error = true;
                } catch (const std::exception &e) {
                    std::lock_guard lock(data_mutex);
                    feedback = L"操作失败：" + wide(e.what());
                    feedback_error = true;
                } catch (...) {
                    std::lock_guard lock(data_mutex);
                    feedback = L"操作失败：未知异常。";
                    feedback_error = true;
                }
                if (foreground) {
                    std::lock_guard lock(data_mutex);
                    last_operation_elapsed_ms = foreground_enqueued_at
                                                    ? GetTickCount64() - foreground_enqueued_at : 0;
                    foreground_enqueued_at = 0;
                    operation_status.clear();
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
        feedback_error = !r.ok();
    }
    void desktop_result(const d::DesktopResult &r) {
        std::lock_guard lock(data_mutex);
        feedback = r.message;
        feedback_error = !r.success;
    }
    void refresh_local() {
        if (desktop_closing.load()) return;
        if (!playback)
            playback = std::make_unique<d::LocalPlaybackBackend>(&desktop_closing);
        auto current = controller->snapshot();
        auto snap = playback->read(current.device ? current.device->aep_id : L"");
        const auto after = controller->snapshot();
        const bool same_device = current.device && after.device && current.link == after.link &&
                                 current.device->address == after.device->address &&
                                 current.device->aep_id == after.device->aep_id;
        std::lock_guard lock(data_mutex);
        local = same_device ? std::move(snap) : d::LocalPlaybackSnapshot{};
        local_device_address = same_device ? current.device->address : 0;
        local_device_link = same_device ? current.link : 0;
        local_device_aep = same_device ? current.device->aep_id : L"";
        local_read_at = same_device ? GetTickCount64() : 0;
    }
    void save_safe_listening(std::uint64_t address, bool value) {
        const auto path = root / L"safe-listening.ini";
        if (std::filesystem::exists(path)) {
            auto backup = path;
            backup += L".bak";
            if (!CopyFileW(path.c_str(), backup.c_str(), FALSE))
                throw std::runtime_error("无法备份安全聆听设置；耳机状态已改变，但偏好未保存。");
        }
        if (!WritePrivateProfileStringW(L"Devices", std::to_wstring(address).c_str(), value ? L"1" : L"0",
                                        path.c_str()))
            throw std::runtime_error("无法保存安全聆听设置；耳机状态已改变。");
    }
    void request_safe_listening(bool value) {
        if (!state.device || !state.safe_listening.writable)
            return;
        const auto link = state.link;
        const auto address = state.device->address;
        enqueue([this, value, link, address] {
            if (controller->snapshot().link != link) {
                result({sc::ResultCode::Canceled, "设备已切换，未修改安全聆听。"});
                return;
            }
            const auto r = controller->set_safe_listening(value);
            result(r);
            if (r.confirmed() && controller->snapshot().link == link)
                save_safe_listening(address, value);
        });
    }
    void render() {
        if (controller)
            state = controller->snapshot();
        sync_touch_selection();
        update_main();
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
        ofn.hwndOwner = settings_owner();
        ofn.lpstrFile = file.data();
        ofn.nMaxFile = static_cast<DWORD>(file.size());
        ofn.lpstrFilter = filter;
        ofn.lpstrDefExt = extension;
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
        return (save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn)) ? std::filesystem::path(file.data())
                                                                        : std::filesystem::path{};
    }
    LRESULT message(UINT m, WPARAM w, LPARAM l) {
        if (tray && tray->taskbar_created(m)) {
            if (!tray->restore() && main_view) main_view->show(false);
            return 0;
        }
        switch (m) {
        case diagnostic_loaded_message:
            open_settings(sc::ui::SettingsPage::replay, !ui_test && IsWindowVisible(replay_request_owner));
            return 0;
        case update_done_message:
            finish_update();
            return 0;
        case WM_TIMER:
            if (w == 3) {
                flush_slider();
            } else if (w == 1) {
                render();
                if (!offline && !busy && !background_work && state.ready && state.device &&
                    state.safe_listening.writable && state.link != safe_preference_link) {
                    safe_preference_link = state.link;
                    const bool remembered = GetPrivateProfileIntW(
                        L"Devices", std::to_wstring(state.device->address).c_str(), 0,
                        (root / L"safe-listening.ini").c_str()) != 0;
                    if (remembered)
                        request_safe_listening(true);
                }
                ++tick;
                const bool pressure_visible = (main_view && IsWindowVisible(main_view->handle()) && !IsIconic(main_view->handle()));
                const bool pressure_due = pressure_visible && state.ready &&
                    state.safe_listening.enabled.known() && *state.safe_listening.enabled.value;
                const auto fixation_due = !offline && !busy && !background_work
                    ? fixation_readback.next(state, GetTickCount64())
                    : std::optional<sc::ui::MainFixationReadback::Context>{};
                if (fixation_due) {
                    enqueue([this, expected = *fixation_due] {
                        const auto current = controller->snapshot();
                        if (current.link == expected.link && current.sources.active_source.known() &&
                            *current.sources.active_source.value == expected.source)
                            controller->poll_playback_fixed();
                    }, false);
                } else if (!offline && !busy && !background_work &&
                           (pressure_due || tick % 6 == 0)) {
                    const bool local_due = tick % 6 == 0;
                    enqueue(
                        [this, pressure_due, local_due] {
                            if (pressure_due)
                                controller->poll_sound_pressure();
                            if (local_due) {
                                controller->poll_codec();
                                if (auto available = d::bluetooth_available(&desktop_closing))
                                    controller->notify_bluetooth_available(*available);
                                refresh_local();
                            }
                        },
                        false);
                }
            }
            return 0;
        case WM_POWERBROADCAST:
            if (controller) {
                if (w == PBT_APMSUSPEND) {
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
                        if (auto available = d::bluetooth_available(&desktop_closing))
                            controller->notify_bluetooth_available(*available);
                    },
                    false);
            return 0;
        case changed_message: {
            if (w) {
                busy = false;
                active_slider.reset();
            } else
                background_work = false;
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
                               settings_owner(),
                               L"关闭右耳触摸分配会关闭关联语音助手。确认修改并保留本次连接内的原值备份？",
                               L"右耳触摸分配", MB_YESNO | MB_ICONQUESTION) == IDYES) {
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
            // Publish the earphone result first. Reuse the serial worker for a
            // fresh Windows endpoint without extending foreground confirmation.
            if (refresh_local_requested && !busy && !background_work) {
                refresh_local_requested = false;
                enqueue([this] { refresh_local(); }, false);
            }
            return 0;
        }
        case tray_message:
            if (l == WM_LBUTTONUP || l == NIN_SELECT || l == NIN_KEYSELECT) {
                if (main_view) main_view->show();
            } else if ((l == WM_RBUTTONUP || l == WM_CONTEXTMENU) && main_view) {
                using T = sc::ui::TrayAction;
                update_main();
                const auto action = main_view->tray_menu(hwnd);
                if (action == T::quit) {
                    quitting = true;
                    DestroyWindow(hwnd);
                } else if (action == T::open) main_view->show();
                else if (action == T::more) open_more();
                else if (action == T::personalization) open_settings(sc::ui::SettingsPage::personalization);
            }
            return 0;
        case WM_COPYDATA:
            if (main_view) main_view->show();
            return TRUE;
        case main_close_message:
            update_cancel = true;
            if (replay_view) replay_view->hide();
            if (diagnostics_view) diagnostics_view->hide();
            if (personalization_view) personalization_view->hide();
            if (advanced_view) advanced_view->hide();
            if (more_view) more_view->hide();
            SendMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        case WM_CLOSE:
            if (close_to_tray && tray && tray->added() && !quitting) {
                ShowWindow(hwnd, SW_HIDE);
                if (main_view) main_view->hide();
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            desktop_closing = true;
            tray.reset();
            update_closing = true;
            update_cancel = true;
            if (update_worker.joinable()) update_worker.join();
            replay_view.reset();
            diagnostics_view.reset();
            personalization_view.reset();
            advanced_view.reset();
            more_view.reset();
            main_view.reset();
            KillTimer(hwnd, 1);
            KillTimer(hwnd, 3);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, m, w, l);
        }
    }
};
} // namespace
namespace sonyconnect::ui {
int run_application(HINSTANCE instance, const AppOptions &options) {
    const bool checking = options.check != AppCheck::none;
    ApplicationHost app(options.offline, checking, options.start_hidden);
    app.preview_main = options.preview;
    app.preview_dpi = options.preview_dpi;
    app.create(instance);
    if (checking) {
        if (!options.report.parent_path().empty()) std::filesystem::create_directories(options.report.parent_path());
        const bool passed = options.check == AppCheck::diagnostics ? app.diagnostic_self_test(options.report)
                          : options.check == AppCheck::updates ? app.update_self_test(options.report)
                          : app.slider_self_test(options.report);
        DestroyWindow(app.hwnd);
        return passed ? 0 : 1;
    }
    if (!options.capture.empty()) {
        if (!options.capture.parent_path().empty()) std::filesystem::create_directories(options.capture.parent_path());
        app.capture_main(options.capture);
        DestroyWindow(app.hwnd);
        return 0;
    }
    MSG message{};
    BOOL result{};
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (!app.pre_translate(message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    return result == -1 ? 2 : static_cast<int>(message.wParam);
}
} // namespace sonyconnect::ui
