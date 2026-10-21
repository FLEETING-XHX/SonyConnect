#include "sonyconnect/app_host.hpp"
#include <commctrl.h>
#include <shellapi.h>
#include <winrt/base.h>
#include <fstream>
#include <set>
#include <utility>

namespace sc = sonyconnect;

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int count{};
    LPWSTR *args = CommandLineToArgvW(GetCommandLineW(), &count);
    bool offline = false, test = false, slider_test = false, update_test = false, diagnostic_test = false, distribution_test = false, hidden = false;
    std::wstring preview;
    UINT preview_dpi{};
    std::filesystem::path main_capture;
    std::filesystem::path report;
    for (int i = 1; i < count; ++i) {
        std::wstring arg = args[i];
        if (arg == L"--offline")
            offline = true;
        else if (arg == L"--ui-slider-self-test") {
            test = slider_test = offline = true;
        } else if (arg == L"--ui-update-self-test") {
            test = update_test = offline = true;
        } else if (arg == L"--ui-diagnostics-self-test") {
            test = diagnostic_test = offline = true;
        } else if (arg == L"--distribution-self-test") {
            test = distribution_test = offline = true;
        } else if (arg == L"--start-hidden")
            hidden = true;
        else if (arg == L"--preview-main" && i + 1 < count) {
            preview = args[++i]; offline = true;
            const std::set<std::wstring> states{L"disconnected",L"connecting",L"failed",L"connected",L"ambient",L"single",L"fixed"};
            if (!states.contains(preview)) { LocalFree(args); return 2; }
        } else if (arg == L"--preview-dpi" && i + 1 < count) {
            wchar_t *end{};
            auto parsed = wcstoul(args[++i], &end, 10);
            if (!end || *end || parsed < 96 || parsed > 384) { LocalFree(args); return 2; }
            preview_dpi = static_cast<UINT>(parsed);
        } else if (arg == L"--capture-main" && i + 1 < count) {
            main_capture = args[++i];
        } else if (arg == L"--report" && i + 1 < count)
            report = args[++i];
        else {
            LocalFree(args);
            MessageBoxW(nullptr,
                        L"未知参数。支持 --offline、--ui-slider-self-test --report <path>、--start-hidden、"
                        L"--preview-main <state> [--preview-dpi <96..384>] [--capture-main <png>]。",
                        L"SonyConnect", MB_ICONERROR);
            return 2;
        }
    }
    LocalFree(args);
    if (test && report.empty())
        return 2;
    if ((preview_dpi || !main_capture.empty()) && preview.empty()) return 2;
    if (test && !preview.empty()) return 2;
    HANDLE mutex{};
    if (!offline) {
        mutex = CreateMutexW(nullptr, FALSE, L"Local\\SonyConnect.Native.SingleInstance");
        if (!mutex)
            return 2;
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            auto existing = FindWindowW(sc::ui::app_host_class, nullptr);
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
        sc::ui::AppOptions options;
        options.offline = offline;
        options.start_hidden = hidden;
        options.check = distribution_test ? sc::ui::AppCheck::distribution : diagnostic_test ? sc::ui::AppCheck::diagnostics : update_test ? sc::ui::AppCheck::updates
                      : slider_test ? sc::ui::AppCheck::sliders : sc::ui::AppCheck::none;
        options.preview = std::move(preview);
        options.preview_dpi = preview_dpi;
        options.capture = std::move(main_capture);
        options.report = report;
        code = sc::ui::run_application(instance, options);
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
            MessageBoxW(nullptr, winrt::to_hstring(e.what()).c_str(), L"SonyConnect 初始化失败", MB_ICONERROR);
        code = 2;
    }
    winrt::uninit_apartment();
    if (mutex)
        CloseHandle(mutex);
    return code;
}
