#include "sonyconnect/more_window.hpp"
#include <commctrl.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <processsnapshot.h>
#include <winrt/base.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

// UI-only measurement. No DeviceController, transport, media or update service
// is created. Real tray/connection/reconnection costs belong to SC-27.
namespace ui = sonyconnect::ui;
namespace sonyconnect::ui { void resource_components(const std::filesystem::path &); }
namespace {
void pump(unsigned ms) {
    const auto until = GetTickCount64() + ms;
    do {
        MSG m{};
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) continue;
            TranslateMessage(&m); DispatchMessageW(&m);
        }
        const auto now = GetTickCount64();
        if (now < until) MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(until - now), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < until);
}
struct Sample {
    ULONGLONG wall{}, cpu{};
    SIZE_T private_bytes{}, working_set{};
    DWORD handles{}, threads{}, gdi{}, user{};
};
Sample sample(bool enumerate_threads = true) {
    Sample s;
    s.wall = GetTickCount64();
    FILETIME created{}, exited{}, kernel{}, user{};
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    const auto process = GetCurrentProcess();
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user) ||
        !GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory)) ||
        !GetProcessHandleCount(process, &s.handles)) throw std::runtime_error("Cannot sample process resources");
    const auto ticks = [](FILETIME t) { return (static_cast<ULONGLONG>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    s.cpu = ticks(kernel) + ticks(user);
    s.private_bytes = memory.PrivateUsage; s.working_set = memory.WorkingSetSize;
    s.gdi = GetGuiResources(process, GR_GDIOBJECTS); s.user = GetGuiResources(process, GR_USEROBJECTS);
    if (!enumerate_threads) return s;
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot sample thread count");
    THREADENTRY32 thread{sizeof(thread)};
    if (Thread32First(snapshot, &thread)) do {
        if (thread.th32OwnerProcessID == GetCurrentProcessId()) ++s.threads;
    } while (Thread32Next(snapshot, &thread));
    CloseHandle(snapshot);
    return s;
}
struct Phase { std::string name; Sample before, after, peak; double cpu{}; };
void handle_types(const std::filesystem::path &path) {
    HPSS snapshot{};
    const auto flags = static_cast<PSS_CAPTURE_FLAGS>(PSS_CAPTURE_HANDLES | PSS_CAPTURE_HANDLE_NAME_INFORMATION);
    if (PssCaptureSnapshot(GetCurrentProcess(), flags, 0, &snapshot) != ERROR_SUCCESS)
        throw std::runtime_error("Cannot capture own handle type snapshot");
    HPSSWALK marker{};
    if (PssWalkMarkerCreate(nullptr, &marker) != ERROR_SUCCESS) {
        PssFreeSnapshot(GetCurrentProcess(), snapshot);
        throw std::runtime_error("Cannot walk own handle types");
    }
    std::map<std::wstring, unsigned> counts;
    PSS_HANDLE_ENTRY entry{};
    DWORD status{};
    while ((status = PssWalkSnapshot(snapshot, PSS_WALK_HANDLES, marker, &entry, sizeof(entry))) == ERROR_SUCCESS)
        ++counts[entry.TypeName ? std::wstring(entry.TypeName, entry.TypeNameLength / sizeof(wchar_t)) : L"Unknown"];
    PssWalkMarkerFree(marker); PssFreeSnapshot(GetCurrentProcess(), snapshot);
    if (status != ERROR_NO_MORE_ITEMS) throw std::runtime_error("Handle type snapshot was incomplete");
    std::ofstream out(path);
    for (const auto &[type, count] : counts) {
        std::string ascii;
        for (const auto ch : type) if (ch) ascii.push_back(ch < 128 ? static_cast<char>(ch) : '?');
        out << ascii << "," << count << "\n";
    }
}
void peak(Sample &p, const Sample &s) {
    p.private_bytes = std::max(p.private_bytes, s.private_bytes); p.working_set = std::max(p.working_set, s.working_set);
    p.handles = std::max(p.handles, s.handles); p.threads = std::max(p.threads, s.threads);
    p.gdi = std::max(p.gdi, s.gdi); p.user = std::max(p.user, s.user);
}
template<class Step> Phase measure(const char *name, unsigned duration, Step step, unsigned minimum_steps = 0) {
    Phase p; p.name = name; p.before = p.peak = sample();
    const auto until = GetTickCount64() + duration;
    unsigned n{};
    do { step(n++); pump(250); peak(p.peak, sample(false)); } while (GetTickCount64() < until || n < minimum_steps);
    p.after = sample();
    peak(p.peak, p.after);
    p.cpu = static_cast<double>(p.after.cpu - p.before.cpu) / 100.0 / (p.after.wall - p.before.wall);
    return p;
}
}
int wmain(int argc, wchar_t **argv) {
    if (argc != 2 && argc != 3) return 2;
    const std::filesystem::path output = argv[1];
    std::filesystem::create_directories(output.parent_path());
    std::vector<Phase> phases;
    bool passed{};
    std::string failure;
    ULONGLONG startup_ms{};
    unsigned dispatches{}, window_cycles{};
    // Thresholds are declared before measurement, in core-equivalent percent.
    constexpr double hidden_cpu_limit = 2, visible_cpu_limit = 5;
    constexpr SIZE_T private_limit = 256ull * 1024 * 1024;
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_WIN95_CLASSES};
        if (!InitCommonControlsEx(&common)) throw std::runtime_error("Cannot initialize probe controls");
        if (argc == 3 && std::wstring(argv[2]) == L"--components") {
            ui::resource_components(output);
            return 0;
        }
        phases.push_back(measure("empty_probe_overhead", 3000, [](unsigned) {}));
        const auto started = GetTickCount64();
        ui::MainWindow main([&](auto, int, bool, std::uint64_t) { ++dispatches; });
        main.create(GetModuleHandleW(nullptr));
        auto model = ui::main_preview(L"connected");
        model.preview = false;
        main.update(model); main.show(false); pump(700);
        startup_ms = GetTickCount64() - started; // Warm-process UI creation, not OS cold boot.
        phases.push_back(measure("visible_main_idle", 5000, [](unsigned) {}));
        ui::MoreWindow more([&](auto, int, int, bool) { ++dispatches; });
        more.create(GetModuleHandleW(nullptr), main.handle());
        auto settings = ui::more_preview();
        more.update(settings); more.show(); pump(700);
        phases.push_back(measure("main_and_more_idle", 5000, [](unsigned) {}));
        more.hide();
        phases.push_back(measure("simulated_mode_and_volume_animation", 8000, [&](unsigned n) {
            if (n % 3) return;
            model.device.sound.mode.status = sonyconnect::ValueStatus::Known;
            model.device.sound.mode.value = (n / 3) % 2 ? sonyconnect::protocol::SoundMode::NoiseCanceling : sonyconnect::protocol::SoundMode::Ambient;
            model.local.volume.percent = static_cast<int>((n * 7) % 101);
            main.update(model);
        }));
        main.hide(); more.hide(); pump(700);
        phases.push_back(measure("hidden_ui_idle_no_tray_or_device", 5000, [](unsigned) {}));
        {
            ui::MoreWindow warm([](auto, int, int, bool) {}, ui::SettingsPage::personalization);
            warm.create(GetModuleHandleW(nullptr), main.handle()); warm.update(settings); warm.show(); pump(100); warm.hide();
        }
        pump(700);
        handle_types(output.parent_path() / "handles-before.csv");
        const auto create_page = [&](unsigned n) {
            if (n >= 20) return;
            ui::MoreWindow page([](auto, int, int, bool) {}, ui::SettingsPage::personalization);
            page.create(GetModuleHandleW(nullptr), main.handle()); page.update(settings); page.show(); pump(20); page.hide();
            ++window_cycles;
        };
        phases.push_back(measure("settings_create_destroy_first_20_cycles", 6500, create_page, 20));
        handle_types(output.parent_path() / "handles-first.csv");
        pump(1500);
        phases.push_back(measure("settings_create_destroy_second_20_cycles", 6500, create_page, 20));
        handle_types(output.parent_path() / "handles-second.csv");
        passed = dispatches == 0 && window_cycles == 40;
        for (const auto &p : phases) {
            if (p.peak.private_bytes > private_limit) passed = false;
            if (p.name == "hidden_ui_idle_no_tray_or_device" && p.cpu > hidden_cpu_limit) passed = false;
            if ((p.name == "visible_main_idle" || p.name == "main_and_more_idle") && p.cpu > visible_cpu_limit) passed = false;
            // The second identical batch distinguishes bounded system/font
            // cache initialization from resource growth on each batch.
            if (p.name == "settings_create_destroy_second_20_cycles" &&
                (p.after.gdi > p.before.gdi + 4 || p.after.user > p.before.user + 4 || p.after.handles > p.before.handles + 16)) passed = false;
        }
        if (!passed) failure = "Resource budget exceeded; inspect phase measurements";
    } catch (const std::exception &e) { failure = e.what(); }
      catch (const winrt::hresult_error &) { failure = "Windows UI initialization or rendering failed"; }
    std::ofstream out(output);
    out << "{\"Passed\":" << (passed ? "true" : "false")
        << ",\"HardwareVerified\":false,\"NetworkCalled\":false,\"LegacyWindowShown\":false,\"DeviceControllerCreated\":false,"
           "\"RealTrayAndReconnectMeasured\":false,\"UiStartupMs\":" << startup_ms << ",\"Dispatches\":" << dispatches
        << ",\"WindowCreateDestroyCycles\":" << window_cycles << ",\"LogicalProcessors\":" << GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)
        << ",\"Thresholds\":{\"HiddenCpuCorePercent\":2,\"VisibleCpuCorePercent\":5,\"PrivateMiB\":256},\"Phases\":[";
    for (std::size_t i = 0; i < phases.size(); ++i) {
        const auto &p = phases[i];
        out << (i ? "," : "") << "{\"Name\":\"" << p.name << "\",\"DurationMs\":" << p.after.wall - p.before.wall
            << ",\"CpuCorePercent\":" << p.cpu << ",\"PeakPrivateBytes\":" << p.peak.private_bytes
            << ",\"PeakWorkingSetBytes\":" << p.peak.working_set << ",\"PeakHandles\":" << p.peak.handles
            << ",\"PeakThreads\":" << p.peak.threads << ",\"PeakGdi\":" << p.peak.gdi << ",\"PeakUser\":" << p.peak.user
            << ",\"FinalHandleDelta\":" << static_cast<long>(p.after.handles) - static_cast<long>(p.before.handles)
            << ",\"FinalGdiDelta\":" << static_cast<long>(p.after.gdi) - static_cast<long>(p.before.gdi)
            << ",\"FinalUserDelta\":" << static_cast<long>(p.after.user) - static_cast<long>(p.before.user) << "}";
    }
    out << "],\"Failure\":\"" << failure << "\"}";
    std::cout << "UI resource baseline: " << (passed ? "passed" : "failed") << "\n";
    return passed ? 0 : 1;
}
