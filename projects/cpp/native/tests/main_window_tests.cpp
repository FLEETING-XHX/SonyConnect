#include "ui_test_geometry.hpp"
#include "sonyconnect/main_window.hpp"
#include <commctrl.h>
#include <windowsx.h>
#include <wincodec.h>
#include <winrt/base.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// All actions are recorded in memory. No controller, desktop service, Bluetooth
// transport, hooks, or network client is constructed by this executable.
namespace sc = sonyconnect;
namespace ui = sonyconnect::ui;
namespace {
constexpr int noise_id = 14, ambient_id = 15, off_id = 16, source_id = 21, volume_id = 30;
struct Action {
    ui::MainAction kind;
    int value;
    bool final;
    std::uint64_t address;
};
struct Diagnostic {
    std::string label;
    ULONGLONG at_ms{};
    double elapsed_ms{};
    RECT bounds{};
    bool visible{};
    unsigned messages{};
};
struct RasterObservation {
    std::string label;
    int slider_value{}, print_blue{}, print_right_blue{}, screen_blue{}, screen_right_blue{};
    bool screen_visible{}, accessible_matches{}, capture_matches_print{};
};
struct Report {
    std::vector<std::string> passed;
    std::filesystem::path directory;
    bool reduced_motion{};
    ULONGLONG started{GetTickCount64()};
    DWORD startup_flags{};
    WORD startup_show{};
    std::vector<Diagnostic> diagnostics;
    std::vector<RasterObservation> rasters;
    std::string failure;
    unsigned paint_messages{}, owner_draw_notifications{};
    void write() const {
        std::ofstream out(directory / "main-window-test-report.json", std::ios::binary);
        out << "{\n  \"HardwareVerified\": false,\n  \"fixture_dispatch_only\": true,\n"
            << "  \"reduced_motion\": " << (reduced_motion ? "true" : "false") << ",\n"
            << "  \"passed\": " << passed.size() << ",\n  \"scenarios\": [";
        for (std::size_t i = 0; i < passed.size(); ++i)
            out << (i ? "," : "") << "\n    \"" << passed[i] << "\"";
        out << "\n  ],\n  \"startup_flags\": " << startup_flags << ",\n  \"startup_show\": " << startup_show
            << ",\n  \"diagnostics\": [";
        for (std::size_t i = 0; i < diagnostics.size(); ++i) {
            const auto &d = diagnostics[i];
            out << (i ? "," : "") << "\n    {\"label\":\"" << d.label << "\",\"at_ms\":" << d.at_ms
                << ",\"elapsed_ms\":" << d.elapsed_ms << ",\"width\":" << d.bounds.right - d.bounds.left
                << ",\"height\":" << d.bounds.bottom - d.bounds.top << ",\"right\":" << d.bounds.right
                << ",\"bottom\":" << d.bounds.bottom << ",\"visible\":" << (d.visible ? "true" : "false")
                << ",\"messages\":" << d.messages << "}";
        }
        out << "\n  ],\n  \"label_rasters\": [";
        for (std::size_t i = 0; i < rasters.size(); ++i) {
            const auto &r = rasters[i];
            out << (i ? "," : "") << "\n    {\"label\":\"" << r.label
                << "\",\"slider_value\":" << r.slider_value
                << ",\"accessible_matches\":" << (r.accessible_matches ? "true" : "false")
                << ",\"print_blue\":" << r.print_blue << ",\"print_right_blue\":" << r.print_right_blue
                << ",\"screen_blue\":" << r.screen_blue << ",\"screen_right_blue\":" << r.screen_right_blue
                << ",\"screen_visible\":" << (r.screen_visible ? "true" : "false")
                << ",\"capture_matches_print\":" << (r.capture_matches_print ? "true" : "false") << "}";
        }
        out << "\n  ],\n  \"paint_messages\": " << paint_messages
            << ",\n  \"owner_draw_notifications\": " << owner_draw_notifications
            << ",\n  \"failure\": \"" << failure << "\"\n}\n";
    }
};
Report *active_report{};
void diagnostic(HWND hwnd, std::string label, double elapsed = 0, unsigned messages = 0) {
    if (!active_report)
        return;
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    active_report->diagnostics.push_back({std::move(label), GetTickCount64() - active_report->started,
                                          elapsed, rect, IsWindowVisible(hwnd) != FALSE, messages});
}
LRESULT CALLBACK observe(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR) {
    if (active_report) {
        if (message == WM_PAINT) ++active_report->paint_messages;
        if (message == WM_DRAWITEM) ++active_report->owner_draw_notifications;
    }
    const auto started = std::chrono::steady_clock::now();
    auto result = DefSubclassProc(hwnd, message, wparam, lparam);
    if (message == WM_TIMER && (wparam == 5 || wparam == 4))
        diagnostic(hwnd, wparam == 5 ? "animation_timer" : "source_hold_timer", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count());
    return result;
}
LRESULT CALLBACK observe_hold(HWND hwnd, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (message == WM_KILLFOCUS || message == WM_CAPTURECHANGED || message == WM_MOUSELEAVE || message == WM_CANCELMODE)
        diagnostic(GetParent(hwnd), "source_input_" + std::to_string(message), static_cast<double>(reinterpret_cast<UINT_PTR>(GetCapture())));
    return DefSubclassProc(hwnd, message, w, l);
}
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
RECT bounds(HWND hwnd) {
    RECT result{};
    require(GetWindowRect(hwnd, &result) != FALSE, "Cannot read fixture window bounds");
    return result;
}
RECT work_area(HWND hwnd) {
    MONITORINFO info{sizeof(info)};
    require(GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info) != FALSE,
            "Cannot read fixture monitor work area");
    return info.rcWork;
}
int height(RECT rect) { return rect.bottom - rect.top; }
int width(RECT rect) { return rect.right - rect.left; }
void same_anchor(RECT actual, RECT expected, const char *message) {
    require(actual.right == expected.right && actual.bottom == expected.bottom, message);
}
void pump(ui::MainWindow &window, unsigned milliseconds) {
    const auto started = GetTickCount64();
    const auto deadline = started + milliseconds;
    unsigned messages = 0;
    do {
        MSG msg{};
        // Animation invalidation continuously replenishes WM_PAINT. Draining the
        // queue to empty would overrun a short observation interval and sample
        // only the final frame, even when production rendered intermediate ones.
        while (GetTickCount64() < deadline && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ++messages;
            if (msg.message != WM_QUIT && !window.pre_translate(msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        const auto now = GetTickCount64();
        if (now >= deadline)
            break;
        MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(std::min<ULONGLONG>(8, deadline - now)),
                                    QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (true);
    diagnostic(window.handle(), "pump_" + std::to_string(milliseconds), static_cast<double>(GetTickCount64() - started), messages);
}
HWND control(ui::MainWindow &window, int id) {
    auto result = GetDlgItem(window.handle(), id);
    require(result != nullptr, "Fixture control is missing");
    return result;
}
std::wstring caption(HWND hwnd) {
    std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(hwnd)) + 1, L'\0');
    text.resize(static_cast<std::size_t>(GetWindowTextW(hwnd, text.data(), static_cast<int>(text.size()))));
    return text;
}
void click(ui::MainWindow &window, int id) {
    HWND child = control(window, id);
    require(IsWindowVisible(child) && IsWindowEnabled(child),
            "The fixture action must be visible and enabled");
    SendMessageW(child, BM_CLICK, 0, 0);
}
void wait_mode_ready(ui::MainWindow &window) {
    const auto until = GetTickCount64() + 700;
    while (!IsWindowEnabled(control(window, noise_id)) && GetTickCount64() < until)
        pump(window, 10);
    require(IsWindowEnabled(control(window, noise_id)),
            "Completed sound operation must unlock after its bounded cooldown");
}
std::vector<DWORD> pixels(HWND hwnd, int strip_height, bool keep_alpha = false);
int heading_dark_pixels(ui::MainWindow &window) {
    const auto raster = pixels(window.handle(), 122);
    const int stride = width(bounds(window.handle()));
    int dark = 0;
    for (int y = ui_test::pixel(99); y < ui_test::pixel(121); ++y)
        for (int x = ui_test::pixel(50); x < ui_test::pixel(170); ++x) {
            const auto p = raster[y * stride + x];
            if (((p >> 16) & 255) < 90 && ((p >> 8) & 255) < 95 && (p & 255) < 110)
                ++dark;
        }
    return dark;
}
void project(ui::MainModel &model) {
    model.preview = false;
    ui::project_main_sources(model);
}
template <class T> sc::Value<T> known(T value) {
    sc::Value<T> result;
    result.status = sc::ValueStatus::Known;
    result.value = std::move(value);
    return result;
}
std::vector<DWORD> pixels(HWND hwnd, int strip_height, bool keep_alpha) {
    RECT rect{};
    GetClientRect(hwnd, &rect);
    HDC dc = GetDC(hwnd), memory = CreateCompatibleDC(dc);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = rect.right;
    info.bmiHeader.biHeight = -rect.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *data{};
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &data, nullptr, 0);
    require(bitmap && data && memory, "Cannot create fixture capture bitmap");
    auto previous = SelectObject(memory, bitmap);
    SendMessageW(hwnd, WM_PRINT, reinterpret_cast<WPARAM>(memory),
                 PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    GdiFlush();
    const auto count = static_cast<std::size_t>(rect.right) * std::min<LONG>(strip_height, rect.bottom);
    std::vector<DWORD> result(static_cast<DWORD *>(data), static_cast<DWORD *>(data) + count);
    if (!keep_alpha)
        for (auto &pixel : result)
            pixel &= 0x00ffffff;
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(hwnd, dc);
    return result;
}
std::vector<DWORD> label_pixels(HWND hwnd, bool print, RECT region = {367, 541, 436, 561}) {
    RECT rect{};
    GetClientRect(hwnd, &rect);
    HDC dc = GetDC(print ? hwnd : nullptr), memory = CreateCompatibleDC(dc);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = rect.right;
    info.bmiHeader.biHeight = -rect.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *data{};
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &data, nullptr, 0);
    require(bitmap && data && memory, "Cannot create label probe bitmap");
    auto previous = SelectObject(memory, bitmap);
    if (print)
        SendMessageW(hwnd, WM_PRINT, reinterpret_cast<WPARAM>(memory),
                     PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    else {
        POINT origin{};
        ClientToScreen(hwnd, &origin);
        if (!BitBlt(memory, 0, 0, rect.right, rect.bottom, dc, origin.x, origin.y, SRCCOPY | CAPTUREBLT))
            throw std::runtime_error("Cannot sample on-screen fixture pixels: " +
                std::to_string(GetLastError()) + "; client=" + std::to_string(rect.right) + "x" +
                std::to_string(rect.bottom) + "; origin=" + std::to_string(origin.x) + "," + std::to_string(origin.y));
    }
    // Direct access to DIB memory must wait for GDI's command batch to finish.
    GdiFlush();
    auto result = ui_test::logical_sample(static_cast<DWORD *>(data), rect.right, rect.bottom, region);
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(print ? hwnd : nullptr, dc);
    return result;
}
void save_label(const std::filesystem::path &path, const std::vector<DWORD> &pixels, UINT width = 69, UINT height = 20) {
    winrt::com_ptr<IWICImagingFactory> factory;
    winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(factory.put())));
    winrt::com_ptr<IWICBitmap> bitmap;
    winrt::check_hresult(factory->CreateBitmapFromMemory(
        width, height, GUID_WICPixelFormat32bppBGR, width * 4, static_cast<UINT>(pixels.size() * sizeof(DWORD)),
        reinterpret_cast<BYTE *>(const_cast<DWORD *>(pixels.data())), bitmap.put()));
    winrt::com_ptr<IWICStream> stream;
    winrt::check_hresult(factory->CreateStream(stream.put()));
    winrt::check_hresult(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    winrt::com_ptr<IWICBitmapEncoder> encoder;
    winrt::check_hresult(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
    winrt::check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
    winrt::com_ptr<IWICBitmapFrameEncode> frame;
    winrt::check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
    winrt::check_hresult(frame->Initialize(nullptr));
    winrt::check_hresult(frame->WriteSource(bitmap.get(), nullptr));
    winrt::check_hresult(frame->Commit());
    winrt::check_hresult(encoder->Commit());
}
std::vector<DWORD> decode_label(const std::filesystem::path &path) {
    winrt::com_ptr<IWICImagingFactory> factory;
    winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(factory.put())));
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::check_hresult(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                            WICDecodeMetadataCacheOnLoad, decoder.put()));
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    winrt::check_hresult(decoder->GetFrame(0, frame.put()));
    winrt::com_ptr<IWICFormatConverter> converter;
    winrt::check_hresult(factory->CreateFormatConverter(converter.put()));
    winrt::check_hresult(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGR,
                                               WICBitmapDitherTypeNone, nullptr, 0.0,
                                               WICBitmapPaletteTypeCustom));
    UINT width{}, height{};
    winrt::check_hresult(converter->GetSize(&width, &height));
    std::vector<DWORD> full(width * height);
    winrt::check_hresult(converter->CopyPixels(nullptr, width * sizeof(DWORD),
        static_cast<UINT>(full.size() * sizeof(DWORD)), reinterpret_cast<BYTE *>(full.data())));
    const auto pixels = ui_test::logical_sample(full.data(), static_cast<int>(width), static_cast<int>(height), {367, 541, 436, 561});
    return pixels;
}
void probe_label(ui::MainWindow &window, Report &report, const std::string &prefix,
                 const wchar_t *capture_name) {
    require(SetWindowPos(window.handle(), HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) != FALSE,
            "Cannot expose the temporary fixture for real screen pixel checks");
    constexpr int label_width = 69, right_start = 49; // x = 416, after the first '8'.
    HWND slider = control(window, volume_id);
    const int value = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
    const bool accessible = caption(slider).find(std::to_wstring(value)) != std::wstring::npos;
    POINT point{ui_test::pixel(430), ui_test::pixel(551)};
    ClientToScreen(window.handle(), &point);
    const HWND covering = WindowFromPoint(point);
    const bool visible = covering == window.handle() || IsChild(window.handle(), covering);
    if (!visible) {
        wchar_t cls[256]{};
        GetClassNameW(covering, cls, 256);
        std::wcerr << L"Screen probe covered by " << cls << L" ownEnabled=" << IsWindowEnabled(window.handle())
                   << L" visible=" << IsWindowVisible(window.handle()) << L" iconic=" << IsIconic(window.handle())
                   << L" exStyle=" << std::hex << GetWindowLongPtrW(window.handle(), GWL_EXSTYLE) << std::dec
                   << L" point=" << point.x << L"," << point.y << L'\n';
    }
    require(visible, "Screen pixel checks require the fixture to be exposed, not covered by another app");
    const auto captured = decode_label(report.directory / capture_name);
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto label = prefix + "-" + std::to_string(attempt);
        const auto screen = label_pixels(window.handle(), false);
        const auto printed = label_pixels(window.handle(), true);
        RasterObservation observation{label, value, 0, 0, 0, 0, visible, accessible};
        const auto count_blue = [](const std::vector<DWORD> &pixels, bool right_only) {
            int count = 0;
            for (std::size_t i = 0; i < pixels.size(); ++i) {
                const DWORD pixel = pixels[i];
                const int red = (pixel >> 16) & 255, green = (pixel >> 8) & 255, blue = pixel & 255;
                if ((!right_only || i % label_width >= right_start) && blue > red + 35 && blue > green + 20)
                    ++count;
            }
            return count;
        };
        observation.print_blue = count_blue(printed, false);
        observation.print_right_blue = count_blue(printed, true);
        observation.screen_blue = count_blue(screen, false);
        observation.screen_right_blue = count_blue(screen, true);
        observation.capture_matches_print = captured == printed;
        report.rasters.push_back(observation);
        save_label(report.directory / (label + "-print.png"), printed);
        save_label(report.directory / (label + "-screen.png"), screen);
        require(observation.capture_matches_print,
                "The captured PNG volume label must exactly match completed GDI print pixels");
        require(observation.screen_blue >= observation.print_blue / 2 &&
                    observation.screen_right_blue >= observation.print_right_blue / 2,
                "The real screen must display both blue volume digits, including the trailing one");
    }
}
void capture(ui::MainWindow &window, const Report &report, const wchar_t *name) {
    window.capture(report.directory / name);
    require(std::filesystem::file_size(report.directory / name) > 1024,
            "Fixture screenshot must contain a rendered PNG");
}
std::size_t count(const std::vector<Action> &actions, ui::MainAction kind) {
    return static_cast<std::size_t>(std::count_if(
        actions.begin(), actions.end(), [kind](const Action &action) { return action.kind == kind; }));
}
bool blue_pixel(DWORD pixel) {
    const int r = (pixel >> 16) & 255, g = (pixel >> 8) & 255, b = pixel & 255;
    return b > r + 35 && b > g + 20;
}
RECT local_bounds(ui::MainWindow &window, HWND child) {
    auto box = bounds(child), parent = bounds(window.handle());
    OffsetRect(&box, -parent.left, -parent.top);
    return box;
}
std::vector<DWORD> control_pixels(ui::MainWindow &window, HWND child) {
    const auto r = local_bounds(window, child);
    const int w = width(bounds(window.handle()));
    const auto full = pixels(window.handle(), height(bounds(window.handle())));
    std::vector<DWORD> output;
    for (int y = r.top; y < r.bottom; ++y)
        for (int x = r.left; x < r.right; ++x)
            output.push_back(full[static_cast<std::size_t>(y) * w + x]);
    return output;
}
LPARAM wheel_position(HWND child) {
    auto r = bounds(child);
    return MAKELPARAM((r.left + r.right) / 2, (r.top + r.bottom) / 2);
}
struct TrayProbe {
    HMENU menu{};
    HWND popup{};
    bool enabled{}, selected{}, inspected{}, timed_out{};
    int choose{-1};
    unsigned steps{};
    std::filesystem::path screenshot;
    std::string failure;
    static LRESULT CALLBACK observe_menu(HWND hwnd, UINT msg, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
        auto &self = *reinterpret_cast<TrayProbe *>(data);
        if (msg == WM_INITMENUPOPUP) self.menu = reinterpret_cast<HMENU>(w);
        if (msg == WM_ENTERIDLE && w == MSGF_MENU) self.popup = reinterpret_cast<HWND>(l);
        return DefSubclassProc(hwnd, msg, w, l);
    }
    static void CALLBACK timer(HWND owner, UINT, UINT_PTR id, DWORD) {
        DWORD_PTR data{};
        if (!GetWindowSubclass(owner, observe_menu, 38, &data)) { KillTimer(owner, id); EndMenu(); return; }
        auto &self = *reinterpret_cast<TrayProbe *>(data);
        if (self.inspected) {
            if (++self.steps > 12) { self.timed_out = true; KillTimer(owner, id); EndMenu(); return; }
            const bool highlighted = (GetMenuState(self.menu, self.choose, MF_BYCOMMAND) & MF_HILITE) != 0;
            const auto key = highlighted ? VK_RETURN : VK_DOWN;
            PostThreadMessageW(GetCurrentThreadId(), WM_KEYDOWN, key, 1);
            PostThreadMessageW(GetCurrentThreadId(), WM_KEYUP, key, 1);
            return;
        }
        try {
            require(self.menu && GetMenuItemCount(self.menu) == 9, "Tray must retain Pen37 status, settings, modes and version");
            for (int i = 0; i < 9; ++i) {
                MENUITEMINFOW item{sizeof(item)};
                item.fMask = MIIM_FTYPE;
                require(GetMenuItemInfoW(self.menu, i, TRUE, &item) && (item.fType & MFT_OWNERDRAW),
                        "All tray rows must use the formal renderer");
                wchar_t text[160]{};
                GetMenuStringW(self.menu, i, text, 160, MF_BYPOSITION);
                require(std::wstring(text).find(L"查看蓝牙") == std::wstring::npos, "Tray must omit Bluetooth settings");
            }
            for (auto action : {ui::TrayAction::noise, ui::TrayAction::ambient, ui::TrayAction::off}) {
                const auto state = GetMenuState(self.menu, static_cast<UINT>(action), MF_BYCOMMAND);
                require(((state & (MF_DISABLED | MF_GRAYED)) == 0) == self.enabled, "Tray sound mode capability/busy guard mismatch");
            }
            require(((GetMenuState(self.menu, static_cast<UINT>(ui::TrayAction::noise), MF_BYCOMMAND) & MF_CHECKED) != 0) == self.selected,
                    "Tray must check the current sound mode");
            if (!self.screenshot.empty()) {
                require(self.popup && IsWindowVisible(self.popup), "Native menu popup must be visible");
                RECT r{}; GetClientRect(self.popup, &r);
                save_label(self.screenshot, pixels(self.popup, r.bottom, false), r.right, r.bottom);
            }
            self.inspected = true;
            if (self.choose < 0) { KillTimer(owner, id); EndMenu(); }
            else {
                PostThreadMessageW(GetCurrentThreadId(), WM_KEYDOWN, VK_DOWN, 1);
                PostThreadMessageW(GetCurrentThreadId(), WM_KEYUP, VK_DOWN, 1);
            }
        } catch (const std::exception &e) { self.failure = e.what(); KillTimer(owner, id); EndMenu(); }
    }
    ui::TrayAction run(ui::MainWindow &window) {
        const auto owner = window.handle();
        require(SetWindowSubclass(owner, observe_menu, 38, reinterpret_cast<DWORD_PTR>(this)) != FALSE,
                "Cannot observe tray fixture");
        require(SetTimer(owner, 700, 250, timer) != 0, "Cannot start bounded tray fixture");
        const auto action = window.tray_menu(owner);
        KillTimer(owner, 700);
        RemoveWindowSubclass(owner, observe_menu, 38);
        require(failure.empty(), failure.c_str());
        require(inspected && !timed_out, "Tray menu navigation must finish without timeout");
        return action;
    }
};
} // namespace

int wmain(int argc, wchar_t **argv) {
    Report report;
    active_report = &report;
    try {
        STARTUPINFOW startup{sizeof(startup)};
        GetStartupInfoW(&startup);
        report.startup_flags = startup.dwFlags;
        report.startup_show = startup.wShowWindow;
        report.directory = argc > 1 ? std::filesystem::path(argv[1])
                                    : std::filesystem::absolute(argv[0]).parent_path() / "main-window-tests";
        std::filesystem::create_directories(report.directory);
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        // Match production's control initialization and retain its v6 manifest.
        // Initialization failure is a test failure, never a reason to skip HWND checks.
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_WIN95_CLASSES};
        if (!InitCommonControlsEx(&common))
            throw std::runtime_error("Cannot initialize fixture controls; Win32 error " +
                                     std::to_string(GetLastError()));
        BOOL animation = TRUE;
        report.reduced_motion =
            SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animation, 0) && !animation;
        std::vector<Action> actions;
        std::function<void()> on_mode_dispatch;
        ui::MainWindow window([&](ui::MainAction action, int value, bool final, std::uint64_t address) {
            actions.push_back({action, value, final, address});
            if (action == ui::MainAction::sound_mode && on_mode_dispatch) on_mode_dispatch();
        });
        window.create(GetModuleHandleW(nullptr), 96);
        require(SetWindowSubclass(window.handle(), observe, 100, 0) != FALSE,
                "Cannot install fixture timing observer");
        auto model = ui::main_preview(L"connected");
        project(model);
        {
            ui::MainFixationReadback readback;
            auto state = model.device;
            state.ready = true;
            state.busy = false;
            state.sources.available = true;
            state.sources.multipoint = known(true);
            state.sources.active_source = known(std::string("source-a"));
            state.features[sc::protocol::Feature::FixedPlayback].advertised = true;
            state.touch.playback_fixed = {};
            const auto first = readback.next(state, 100);
            require(first && first->link == state.link && first->source == "source-a",
                    "Unknown fixation after an external source change needs a narrow follow-up");
            require(!readback.next(state, 1599), "Fixation follow-up must wait before retrying");
            require(readback.next(state, 1600).has_value(), "Fixation follow-up needs one bounded retry");
            require(!readback.next(state, 10000), "Fixation follow-up must stop after two attempts");
            state.sources.active_source = known(std::string("source-b"));
            require(readback.next(state, 10000).has_value(), "A changed source needs a fresh readback budget");
            ++state.link;
            require(readback.next(state, 10001).has_value(), "A new link needs a fresh readback budget");
            state.touch.playback_fixed = known(false);
            require(!readback.next(state, 10002), "Known fixation must not trigger background I/O");
            state.touch.playback_fixed = {};
            require(readback.next(state, 10003).has_value(), "A newly invalidated value needs a fresh readback");
            ui::MainFixationReadback gated;
            for (int condition = 0; condition < 6; ++condition) {
                auto unavailable = state;
                if (condition == 0) unavailable.ready = false;
                if (condition == 1) unavailable.busy = true;
                if (condition == 2) unavailable.sources.available = false;
                if (condition == 3) unavailable.sources.multipoint = known(false);
                if (condition == 4) unavailable.sources.active_source = {};
                if (condition == 5) unavailable.features[sc::protocol::Feature::FixedPlayback].advertised = false;
                require(!gated.next(unavailable, 0), "Unavailable fixation must not schedule a read");
            }
            require(gated.next(state, 0).has_value(), "Gated updates must not consume the retry budget");
        }
        report.passed.push_back("fixation_readback_scheduler_guards_context_retry_delay_and_budget");
        std::uint64_t revision = model.device.revision;
        const auto update = [&] {
            const auto started = GetTickCount64();
            model.device.revision = ++revision;
            project(model);
            window.update(model);
            diagnostic(window.handle(), "update", static_cast<double>(GetTickCount64() - started));
        };
        update();
        window.show(true);
        SetWindowPos(window.handle(), HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        diagnostic(window.handle(), "first_show");
        pump(window, 40);
        auto initial = bounds(window.handle());
        auto work = work_area(window.handle());
        require(initial.right == work.right - ui_test::pixel(8) && initial.bottom == work.bottom - ui_test::pixel(8),
                "The main window must initially open 8 DIP from the work-area bottom right");
        require(width(initial) == ui_test::pixel(460), "The connected fixture must keep the Pen 460 DIP width");
        const auto style = GetWindowLongPtrW(window.handle(), GWL_STYLE);
        require(!(style & WS_THICKFRAME) && !(style & WS_MAXIMIZEBOX),
                "The main window must not expose resizing or maximizing");
        require(GetWindowLongPtrW(window.handle(), GWL_EXSTYLE) & WS_EX_LAYERED,
                "The window must submit its complete premultiplied frame through the layered presenter");
        SendMessageW(window.handle(), WM_SYSCOMMAND, SC_MAXIMIZE, 0);
        same_anchor(bounds(window.handle()), initial, "A maximize command must not move the fixture");
        require(!IsZoomed(window.handle()), "A maximize command must remain blocked");
        {
            const auto before_wordmark = label_pixels(window.handle(), true);
            // Probe the battery text, excluding the adjacent DSEE decoration.
            const auto battery_before_wordmark = label_pixels(window.handle(), true, {297, 60, 436, 89});
            auto &dsee = model.device.features[sc::protocol::Feature::Dsee];
            dsee.value = known(1);
            update();
            pump(window, 30);
            capture(window, report, L"00-dsee-wordmark.png");
            require(label_pixels(window.handle(), true, {297, 60, 436, 89}) == battery_before_wordmark,
                    "DSEE must preserve exact battery text size, position and pixels");
            dsee.value = known(0);
            update();
            pump(window, 30);
            require(label_pixels(window.handle(), true) == before_wordmark,
                    "DSEE indicator must not change volume label rendering");
            report.passed.push_back("embedded_dsee_wordmark_and_disabled_state_keep_other_ui_stable");
        }
        const auto supported_sound = model.device.sound;
        const auto supported_capabilities = model.device.capabilities;
        const auto before_unsupported = actions.size();
        for (int unavailable = 0; unavailable < 2; ++unavailable) {
            model.device.sound = supported_sound;
            model.device.capabilities = supported_capabilities;
            if (unavailable == 0)
                model.device.capabilities.features[sc::protocol::Feature::SoundMode].access.read =
                    sc::protocol::Support::Unsupported;
            else
                model.device.sound.mode.status = sc::ValueStatus::Unsupported;
            update();
            pump(window, 350);
            for (int id : {noise_id, ambient_id, off_id}) {
                require(IsWindowVisible(control(window, id)) && !IsWindowEnabled(control(window, id)),
                        "Unsupported sound module must preserve all three visible, disabled buttons");
                SendMessageW(control(window, id), BM_CLICK, 0, 0);
                SendMessageW(window.handle(), WM_COMMAND, MAKEWPARAM(id, BN_CLICKED),
                             reinterpret_cast<LPARAM>(control(window, id)));
            }
            require(actions.size() == before_unsupported, "Unsupported sound controls must never dispatch");
        }
        capture(window, report, L"00-unsupported-sound.png");
        model.device.sound = supported_sound;
        model.device.capabilities = supported_capabilities;
        update();
        pump(window, 350);
        require(IsWindowEnabled(control(window, noise_id)),
                "Returning to a supported headphone must restore sound controls");
        report.passed.push_back("unsupported_sound_module_stays_visible_disabled_and_restores");
        require(ui::main_media_source(model) == L"来自PC 设备",
                "Media subtitle must identify the actual active audio source");
        auto alternate = model;
        alternate.local.media.application_id = L"cloudmusic.exe";
        alternate.device.sources.active_source = known(std::string("preview-phone"));
        ui::project_main_sources(alternate);
        require(ui::main_media_source(alternate) == L"来自移动设备",
                "Media source must follow earphone source changes rather than the player executable");
        alternate.source_rows.clear();
        require(ui::main_media_source(alternate) == L"音源待回读",
                "Unknown audio sources must not be invented from a local application name");
        report.passed.push_back("media_subtitle_uses_confirmed_active_device");
        const auto tile_pixels = pixels(window.handle(), height(initial));
        const auto alpha_pixels = pixels(window.handle(), 125, true);
        std::cout << "Canvas interior alpha=" << (alpha_pixels[ui_test::pixel(110) * width(initial) + ui_test::pixel(100)] >> 24)
                  << " background=" << (alpha_pixels[ui_test::pixel(95) * width(initial) + ui_test::pixel(100)] >> 24) << '\n';
        require((alpha_pixels[ui_test::pixel(95) * width(initial) + ui_test::pixel(100)] >> 24) == 255,
                "The layered canvas interior must remain opaque for rendering and pointer hit testing");
        require((tile_pixels[ui_test::pixel(500) * width(initial) + ui_test::pixel(30)] & 0xffffff) == 0xe9ecfc &&
                    width(bounds(control(window, 24))) == ui_test::pixel(36) &&
                    local_bounds(window, control(window, 23)).left == ui_test::pixel(320),
                "Player must use the Pen 42-DIP artwork tile and 36-DIP transport buttons");
        report.passed.push_back("media_tile_and_transport_geometry_match_pen");
        HWND wake_slider = control(window, volume_id);
        const auto wake_box = bounds(wake_slider);
        const auto wake_raster = control_pixels(window, wake_slider);
        int segments = 0;
        bool was_bar = false;
        for (int x = 0; x < width(wake_box); ++x) {
            const DWORD pixel = wake_raster[(height(wake_box) / 2) * width(wake_box) + x] & 0xffffff;
            const bool bar = pixel == 0x4c5ed6 || pixel == 0xd9deea;
            segments += bar && !was_bar;
            was_bar = bar;
        }
        require(segments == 32, "Volume Wake must render exactly thirty-two separated bars");
        report.passed.push_back("wake_volume_keeps_thirty_two_separate_segments");
        capture(window, report, L"01-connected.png");
        report.passed.push_back("initial_bottom_right_and_fixed_window");

        const auto prior_label = label_pixels(window.handle(), false);
        const auto prior_print = label_pixels(window.handle(), true);
        model.local.volume.percent = 87;
        update();
        // A native paint validation must not consume the model's pending
        // layered-window submission (same-size updates have no resize fallback).
        ValidateRect(window.handle(), nullptr);
        pump(window, 300);
        const auto next_label = label_pixels(window.handle(), false);
        const auto next_print = label_pixels(window.handle(), true);
        save_label(report.directory / "same-size-before-screen.png", prior_label);
        save_label(report.directory / "same-size-after-screen.png", next_label);
        save_label(report.directory / "same-size-after-print.png", next_print);
        require(caption(control(window, volume_id)).find(L"87") != std::wstring::npos,
                "Same-size update must refresh native and accessible volume values");
        require(prior_print != next_print, "Same-size update must render the new value into the canvas");
        require(prior_label != next_label,
                "Same-size model changes must reach the real screen despite native paint validation");
        model.local.volume.percent = 40;
        update();
        pump(window, 300);
        report.passed.push_back("same_size_submission_survives_native_paint_validation");

        SetFocus(control(window, noise_id));
        SetCapture(control(window, off_id));
        SetFocus(control(window, off_id));
        require(GetCapture() == control(window, off_id),
                "Losing focus must not release mouse capture already owned by the next button");
        ReleaseCapture();
        SetCapture(control(window, source_id));
        SetCapture(control(window, noise_id));
        require(GetCapture() == control(window, noise_id),
                "An old source's capture-changed notification must not cancel the next button's capture");
        ReleaseCapture();
        report.passed.push_back("focus_and_source_capture_handoff_preserve_next_control_input");

        const auto source_before = count(actions, ui::MainAction::source);
        HWND other_source = control(window, source_id + 1);
        require(IsWindowEnabled(other_source), "The second fixture device must support source switching");
        SetFocus(control(window, noise_id));
        SendMessageW(other_source, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(ui_test::pixel(100), ui_test::pixel(20)));
        SendMessageW(other_source, WM_LBUTTONUP, 0, MAKELPARAM(ui_test::pixel(100), ui_test::pixel(20)));
        require(count(actions, ui::MainAction::source) == source_before + 1 && actions.back().value == 1,
                "A device source must switch once after focus transfers from another control");
        const auto hold_before = count(actions, ui::MainAction::fix_source);
        HWND active_source = control(window, source_id);
        SendMessageW(active_source, WM_KEYDOWN, VK_SPACE, 0);
        SendMessageW(other_source, WM_CAPTURECHANGED, 0, reinterpret_cast<LPARAM>(active_source));
        pump(window, 1100);
        SendMessageW(active_source, WM_KEYUP, VK_SPACE, 0);
        require(count(actions, ui::MainAction::fix_source) == hold_before + 1,
                "An unrelated old source capture notification must not cancel the active source hold");
        report.passed.push_back("source_click_and_hold_survive_unrelated_focus_capture_notifications");

        on_mode_dispatch = [&] { model.busy = true; update(); };
        for (bool queued : {false, true}) {
        for (const auto [from, to] : {std::pair{2, 0}, {0, 2}, {2, 1}, {1, 0}, {0, 1}, {1, 2}}) {
            model.busy = false;
            model.device.sound.mode = known(static_cast<sc::protocol::SoundMode>(from));
            update();
            pump(window, 350);
            wait_mode_ready(window);
            HWND button = control(window, noise_id + to);
            RECT client{};
            GetClientRect(button, &client);
            const auto point = MAKELPARAM(client.right / 2, client.bottom / 2);
            const auto before = count(actions, ui::MainAction::sound_mode);
            SetFocus(control(window, noise_id + from));
            // Cover both the BUTTON procedure's capture/focus ordering and the
            // queued production pre_translate path, without replacing it by BM_CLICK.
            if (queued) {
                require(PostMessageW(button, WM_LBUTTONDOWN, MK_LBUTTON, point) &&
                            PostMessageW(button, WM_LBUTTONUP, 0, point), "Cannot queue fixture mouse input");
            } else {
                SendMessageW(button, WM_LBUTTONDOWN, MK_LBUTTON, point);
                SendMessageW(button, WM_LBUTTONUP, 0, point);
            }
            pump(window, 20);
            if (count(actions, ui::MainAction::sound_mode) != before + 1)
                std::cerr << "Mode mouse routing failed from=" << from << " to=" << to
                          << " enabled=" << IsWindowEnabled(button) << '\n';
            require(count(actions, ui::MainAction::sound_mode) == before + 1 && actions.back().value == to,
                    "Every directed mode transition must dispatch from a single queued mouse click");
            model.busy = false;
            model.device.sound.mode = known(static_cast<sc::protocol::SoundMode>(to));
            update();
            pump(window, 350);
            require(caption(button).find(L"已选中") != std::wstring::npos,
                    "Synchronous busy publication and confirmed result must retain the requested mode");
        }
        }
        on_mode_dispatch = {};
        model.device.sound.mode = known(sc::protocol::SoundMode::NoiseCanceling);
        update();
        pump(window, 350);
        report.passed.push_back("all_six_mode_transitions_route_one_mouse_click_with_synchronous_busy");

        model.device.sound.mode = known(sc::protocol::SoundMode::Ambient);
        update();
        pump(window, 64);
        const auto ambient_middle = bounds(window.handle());
        same_anchor(ambient_middle, initial, "Opening ambient controls must preserve bottom and right edges");
        const int ambient_target = ui_test::main_height(822, height(work));
        if (!report.reduced_motion && ambient_target > height(initial) + 2)
            require(height(ambient_middle) > height(initial) && height(ambient_middle) < ambient_target,
                    "The ambient transition must include intermediate heights");
        capture(window, report, L"02-ambient-expanding.png");
        pump(window, 350);
        auto ambient = bounds(window.handle());
        require(height(ambient) == ambient_target, "Ambient controls must settle at their target height");
        same_anchor(ambient, initial, "The expanded main window must retain its bottom-right anchor");
        report.passed.push_back("ambient_height_animates_with_bottom_anchor");

        model.device.sound.voice_focus = {};
        model.device.sound.voice_focus.status = sc::ValueStatus::Unsupported;
        update();
        pump(window, 64);
        require(!IsWindowEnabled(control(window, 17)),
                "A departing unsupported control must immediately stop accepting input");
        for (int i = 0; i < 8; ++i) {
            update(); // Unrelated revisions must not restart the same transition.
            pump(window, 12);
        }
        pump(window, 160);
        const int without_focus = ui_test::main_height(780, height(work));
        require(height(bounds(window.handle())) == without_focus && !IsWindowVisible(control(window, 17)),
                "A removed row must release its measured height despite repeated state updates");
        model.device.sound.voice_focus = known(true);
        update();
        pump(window, 350);
        require(height(bounds(window.handle())) == ambient_target && IsWindowVisible(control(window, 17)),
                "Restoring a supported row must restore layout without model-specific window heights");
        capture(window, report, L"11-module-restored.png");
        report.passed.push_back("measured_optional_rows_and_repeated_state_updates_preserve_animation");

        // A click dispatches immediately, while the fixture still exposes the
        // previously confirmed mode. Only memory is changed by this callback.
        const auto old_actions = actions.size();
        click(window, noise_id);
        require(actions.size() == old_actions + 1 && actions.back().kind == ui::MainAction::sound_mode &&
                    actions.back().value == 0,
                "ANC input must dispatch immediately without waiting for animation completion");
        model.busy = true;
        update();
        pump(window, 64);
        auto shrinking = bounds(window.handle());
        same_anchor(shrinking, ambient, "ANC contraction must preserve the bottom-right anchor");
        const int connected_target = ui_test::main_height(720, height(work));
        if (!report.reduced_motion && height(ambient) > connected_target + 2)
            require(height(shrinking) < height(ambient) && height(shrinking) > connected_target,
                    "ANC contraction must animate through intermediate heights");
        model.busy = false;
        model.feedback_error = true;
        model.feedback = L"Fixture: readback rejected; confirmed ambient mode retained";
        update();
        pump(window, 350);
        same_anchor(bounds(window.handle()), ambient, "A rejected ANC change must keep the window anchor");
        require(height(bounds(window.handle())) == height(ambient),
                "A rejected ANC change must animate back to confirmed ambient height");
        require(caption(control(window, ambient_id)).find(L"已选中") != std::wstring::npos &&
                    caption(control(window, noise_id)).find(L"已选中") == std::wstring::npos,
                "A failed command must restore the confirmed segment selection");
        capture(window, report, L"03-anc-failure-rollback.png");
        report.passed.push_back("anc_immediate_dispatch_interrupted_animation_and_failure_rollback");

        model.feedback.clear();
        model.feedback_error = false;
        wait_mode_ready(window);
        click(window, off_id);
        model.busy = true;
        update();
        pump(window, 350);
        model.device.sound.mode = known(sc::protocol::SoundMode::Off);
        model.busy = false;
        update();
        pump(window, 30);
        require(height(bounds(window.handle())) == connected_target,
                "Successful ANC readback must retain the selected target height");
        require(caption(control(window, off_id)).find(L"已选中") != std::wstring::npos,
                "Successful ANC readback must keep the target segment selected");
        report.passed.push_back("anc_confirmed_readback_retains_target_selection");

        // SetWindowPos models a completed title-bar drag. Subsequent state changes
        // must use the actual user-selected position rather than startup coordinates.
        auto dragged = bounds(window.handle());
        const int move_up = std::min<LONG>(20, std::max<LONG>(0, dragged.top - work.top));
        SetWindowPos(window.handle(), nullptr, dragged.left - 24, dragged.top - move_up, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        dragged = bounds(window.handle());
        model.device.sound.mode = known(sc::protocol::SoundMode::Ambient);
        update();
        pump(window, 350);
        // A taller window may need clamping to the top of a small monitor.
        auto expected_dragged = dragged;
        expected_dragged.bottom = std::max(dragged.bottom, work.top + ambient_target);
        same_anchor(bounds(window.handle()), expected_dragged,
                    "A completed title-bar drag must survive later content resizing");
        report.passed.push_back("dragged_position_survives_resize_with_monitor_clamping");

        auto pre_disconnect = bounds(window.handle());
        auto connected_model = model;
        model = ui::main_preview(L"disconnected");
        update();
        pump(window, 64);
        same_anchor(bounds(window.handle()), pre_disconnect, "Disconnecting must not jump the window upward");
        pump(window, 350);
        require(width(bounds(window.handle())) == ui_test::pixel(360) && height(bounds(window.handle())) == ui_test::pixel(232),
                "The disconnected fixture must settle to its small fixed window");
        same_anchor(bounds(window.handle()), pre_disconnect,
                    "The disconnected fixture must keep its bottom-right position");
        capture(window, report, L"04-disconnected.png");
        report.passed.push_back("disconnection_keeps_bottom_right_anchor");

        project(model);
        update();
        pump(window, 50);
        const auto refresh = GetDlgItem(window.handle(), 34);
        require(refresh && IsWindowVisible(refresh) && IsWindowEnabled(refresh), "Disconnected page needs an enabled refresh icon");
        const auto before_refresh = actions.size();
        SendMessageW(refresh, BM_CLICK, 0, 0);
        require(actions.size() == before_refresh + 1 && actions.back().kind == ui::MainAction::discover,
                "Refresh must only dispatch existing discovery");
        model.discovering = true;
        update();
        require(!IsWindowEnabled(refresh), "Refresh must reject duplicate discovery while busy");
        model.discovering = false;
        update();
        TrayProbe disconnected_tray;
        require(disconnected_tray.run(window) == ui::TrayAction::none, "Dismissing tray must not invoke an action");
        report.passed.push_back("disconnected_refresh_and_tray_modes_are_guarded");

        model = std::move(connected_model);
        model.device.sound.mode = known(sc::protocol::SoundMode::NoiseCanceling);
        update();
        pump(window, 64);
        require(!IsWindowEnabled(control(window, volume_id)) || report.reduced_motion,
                "Incoming page controls must remain unavailable during the scene transition");
        capture(window, report, L"12-connecting-scene-transition.png");
        pump(window, 350);
        {
            model.busy = true;
            update();
            TrayProbe busy_tray;
            busy_tray.selected = true;
            busy_tray.run(window);
            model.busy = false;
            model.device.sound.mode.status = sc::ValueStatus::Unsupported;
            update();
            TrayProbe unsupported_tray;
            unsupported_tray.run(window);
            model.device.sound.mode = known(sc::protocol::SoundMode::NoiseCanceling);
            update();
            TrayProbe tray;
            tray.enabled = tray.selected = true;
            tray.screenshot = report.directory / "13-tray-menu.png";
            tray.choose = static_cast<int>(ui::TrayAction::ambient);
            const auto before = actions.size();
            const auto tray_action = tray.run(window);
            std::cout << "Tray keyboard action=" << static_cast<int>(tray_action) << "\n";
            require(tray_action == ui::TrayAction::ambient, "Tray keyboard selection must choose Ambient");
            require(actions.size() == before + 1 && actions.back().kind == ui::MainAction::sound_mode && actions.back().value == 1,
                    "Tray must use the same mode dispatch as the main window");
            model.device.sound.mode = known(sc::protocol::SoundMode::Ambient);
            update();
            pump(window, 350);
            model.device.sound.mode = known(sc::protocol::SoundMode::NoiseCanceling);
            update();
            pump(window, 350);
            report.passed.push_back("tray_pen37_render_keyboard_and_shared_sound_dispatch");
        }
        report.passed.push_back("connection_scene_transition_preserves_native_input_and_final_geometry");
        HWND volume = control(window, volume_id);
        require(IsWindowEnabled(volume), "Fixture volume control must be available");
        RECT slider{};
        GetClientRect(volume, &slider);
        const auto volume_actions = count(actions, ui::MainAction::volume);
        SendMessageW(volume, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(slider.right / 5, slider.bottom / 2));
        SendMessageW(volume, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(slider.right * 4 / 5, slider.bottom / 2));
        const int live_value = static_cast<int>(SendMessageW(volume, TBM_GETPOS, 0, 0));
        require(live_value >= 79 && live_value <= 81,
                "The native slider position must follow the pointer before release");
        require(caption(volume).find(std::to_wstring(live_value)) != std::wstring::npos,
                "The accessible volume value must follow the pointer before release");
        require(count(actions, ui::MainAction::volume) >= volume_actions + 2 && !actions.back().final,
                "Volume changes must dispatch continuously during the drag");
        pump(window, 48);
        capture(window, report, L"05-volume-live-drag.png");
        probe_label(window, report, "05-volume-label", L"05-volume-live-drag.png");
        SendMessageW(volume, WM_LBUTTONUP, 0, MAKELPARAM(slider.right * 4 / 5, slider.bottom / 2));
        require(actions.back().kind == ui::MainAction::volume && actions.back().final &&
                    actions.back().value == live_value,
                "Pointer release must send the final volume intent");
        report.passed.push_back("wake_drag_updates_native_and_accessible_values_before_release");

        HWND source = control(window, source_id);
        SetWindowSubclass(source, observe_hold, 101, 0);
        require(IsWindowEnabled(source), "The active fixture source must support long-press fixation");
        const auto fixed_before = count(actions, ui::MainAction::fix_source);
        SetFocus(volume);
        SendMessageW(source, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(ui_test::pixel(100), ui_test::pixel(20)));
        pump(window, 480);
        require(count(actions, ui::MainAction::fix_source) == fixed_before,
                "Fixation must not dispatch before the one-second hold completes");
        capture(window, report, L"06-source-hold-progress.png");
        probe_label(window, report, "06-hold-label", L"06-source-hold-progress.png");
        report.passed.push_back("capture_png_labels_match_completed_gdi_printing");
        pump(window, 750);
        require(count(actions, ui::MainAction::fix_source) == fixed_before + 1,
                "A completed hold must dispatch fixation exactly once");
        const auto enclosure = control_pixels(window, source);
        const auto source_rect = bounds(source);
        const int sw = width(source_rect), sh = height(source_rect);
        bool top = false, bottom = false, left = false, right = false;
        for (int x = 8; x < sw - 8; ++x)
            for (int y = 1; y <= 3; ++y) {
                top |= blue_pixel(enclosure[y * sw + x]);
                bottom |= blue_pixel(enclosure[(sh - 1 - y) * sw + x]);
            }
        for (int y = 8; y < sh - 8; ++y)
            for (int x = 1; x <= 3; ++x) {
                left |= blue_pixel(enclosure[y * sw + x]);
                right |= blue_pixel(enclosure[y * sw + sw - 1 - x]);
            }
        require(top && bottom && left && right, "A completed hold must visibly close all four sides");
        capture(window, report, L"08-enclosure-complete.png");
        report.passed.push_back("completed_source_enclosure_has_visible_top_bottom_left_right");
        pump(window, 250);
        SendMessageW(source, WM_LBUTTONUP, 0, MAKELPARAM(ui_test::pixel(100), ui_test::pixel(20)));
        require(count(actions, ui::MainAction::fix_source) == fixed_before + 1,
                "A completed hold must not repeat while held or on release");
        report.passed.push_back("one_second_source_hold_dispatches_once_after_focus_transfer");

        const auto fixed_after = count(actions, ui::MainAction::fix_source);
        SendMessageW(source, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(ui_test::pixel(100), ui_test::pixel(20)));
        pump(window, 180);
        SendMessageW(source, WM_KEYDOWN, VK_ESCAPE, 0);
        pump(window, 1050);
        SendMessageW(source, WM_LBUTTONUP, 0, MAKELPARAM(ui_test::pixel(100), ui_test::pixel(20)));
        require(count(actions, ui::MainAction::fix_source) == fixed_after,
                "Escape must cancel the long-press without dispatching fixation");
        SendMessageW(source, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(ui_test::pixel(100), ui_test::pixel(20)));
        pump(window, 180);
        SendMessageW(source, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(ui_test::pixel(-10), ui_test::pixel(20)));
        pump(window, 1050);
        SendMessageW(source, WM_LBUTTONUP, 0, MAKELPARAM(ui_test::pixel(-10), ui_test::pixel(20)));
        require(count(actions, ui::MainAction::fix_source) == fixed_after,
                "Leaving the source row must cancel the long-press");
        SendMessageW(source, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(ui_test::pixel(100), ui_test::pixel(20)));
        pump(window, 180);
        ++model.device.link;
        update();
        pump(window, 1050);
        SendMessageW(source, WM_LBUTTONUP, 0, MAKELPARAM(ui_test::pixel(100), ui_test::pixel(20)));
        require(count(actions, ui::MainAction::fix_source) == fixed_after,
                "A changed connection generation must cancel stale long-press fixation");
        report.passed.push_back("source_hold_escape_leave_and_link_change_cancel_without_dispatch");

        // Ordinary pointer focus must not create a decorative underline.
        SendMessageW(source, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(ui_test::pixel(100), ui_test::pixel(20)));
        SendMessageW(source, WM_LBUTTONUP, 0, MAKELPARAM(ui_test::pixel(100), ui_test::pixel(20)));
        pump(window, 30);
        const auto focused_source = control_pixels(window, source);
        int underline = 0;
        for (int x = 4; x < sw - 4; ++x)
            for (int y = sh - 3; y < sh; ++y)
                underline += blue_pixel(focused_source[y * sw + x]);
        require(underline == 0, "Mouse focus on the source must not leave a bottom underline");
        report.passed.push_back("source_mouse_focus_has_no_underline");

        const auto quality_before = control_pixels(window, control(window, 19));
        const auto stable_before = control_pixels(window, control(window, 20));
        const auto power_before = control_pixels(window, control(window, 13));
        const auto secondary_before = control_pixels(window, control(window, 22));
        model.busy = true;
        model.operation_status = L"正在切换声音模式，等待耳机回读…";
        model.feedback = L"耳机回读确认 · 用时 125 ms";
        update();
        pump(window, 20);
        HWND status = control(window, 32);
        wchar_t status_class[32]{};
        GetClassNameW(status, status_class, 32);
        require(wcscmp(status_class, L"Static") == 0 || wcscmp(status_class, L"STATIC") == 0,
                "Readback status must be a static text control");
        require(!(GetWindowLongPtrW(status, GWL_STYLE) & WS_TABSTOP) && !IsWindowEnabled(status),
                "Readback status must not be focusable or clickable");
        require(!IsWindowVisible(status), "Successful and in-progress readback status must stay hidden");
        const auto status_actions = actions.size();
        SendMessageW(status, BM_CLICK, 0, 0);
        SendMessageW(status, WM_MOUSEMOVE, 0, 0);
        pump(window, 20);
        require(actions.size() == status_actions, "Clicking static status must not dispatch any action");
        require(control_pixels(window, control(window, 19)) == quality_before &&
                    control_pixels(window, control(window, 20)) == stable_before,
                "Bluetooth choices must not flash or recolor during unrelated mode confirmation");
        require(control_pixels(window, control(window, 13)) == power_before &&
                    control_pixels(window, control(window, 22)) == secondary_before,
                "Power and the second source must not fade during temporary input locks");
        require(!IsWindowEnabled(control(window, 13)) && !IsWindowEnabled(control(window, 22)),
                "Stable appearance must not bypass the existing operation input lock");
        model.device.busy = true;
        update();
        pump(window, 20);
        require(control_pixels(window, control(window, 13)) == power_before &&
                    control_pixels(window, control(window, 22)) == secondary_before &&
                    !IsWindowVisible(status),
                "Controller busy notifications must also preserve stable pixels and hide status");
        model.device.busy = false;
        model.busy = false;
        model.operation_status.clear();
        update();
        pump(window, 20);
        require(!IsWindowVisible(status), "Successful confirmation must not show an acknowledgement badge");
        model.feedback_error = true;
        update();
        pump(window, 20);
        require(IsWindowVisible(status) && caption(status) == L"回读错误，请重试",
                "Only readback failure displays the noninteractive warning");
        const auto warning = control_pixels(window, status);
        require(std::count_if(warning.begin(), warning.end(), [](DWORD pixel) {
                    return (pixel & 0xffffff) == 0xfff2e4;
                }) > 2000,
                "Warning must use the Pen FFF2E4 background");
        require(std::any_of(warning.begin(), warning.end(), [](DWORD pixel) {
                    return (pixel & 0xffffff) == 0xb56c25;
                }), "Warning icon/text must use Pen B56C25");
        capture(window, report, L"09-error-only-warning.png");
        model.feedback_error = false;
        update();
        report.passed.push_back("error_only_pen_warning_and_no_busy_power_source_quality_fading");

        // Wheel routing uses the pointer's slider, not the focused source row.
        SetWindowPos(window.handle(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetFocus(source);
        const int before_wheel = static_cast<int>(SendMessageW(volume, TBM_GETPOS, 0, 0));
        const auto wheel_actions = count(actions, ui::MainAction::volume);
        for (int i = 0; i < 3; ++i)
            SendMessageW(window.handle(), WM_MOUSEWHEEL, MAKEWPARAM(0, 30), wheel_position(volume));
        require(count(actions, ui::MainAction::volume) == wheel_actions,
                "Fractional notches must not dispatch writes");
        SendMessageW(window.handle(), WM_MOUSEWHEEL, MAKEWPARAM(0, 30), wheel_position(volume));
        if (SendMessageW(volume, TBM_GETPOS, 0, 0) != before_wheel + 1 || GetFocus() != source) {
            const auto at = wheel_position(volume);
            const HWND hit = WindowFromPoint({GET_X_LPARAM(at), GET_Y_LPARAM(at)});
            std::cerr << "Wheel diagnostic: before=" << before_wheel << " after="
                      << SendMessageW(volume, TBM_GETPOS, 0, 0) << " source_focus="
                      << (GetFocus() == source) << " own_hit="
                      << (hit == window.handle() || IsChild(window.handle(), hit)) << '\n';
            wchar_t hit_class[256]{};
            GetClassNameW(hit, hit_class, 256);
            std::wcerr << L"Hit class=" << hit_class << L" title=" << caption(hit)
                       << L" ownExStyle=" << std::hex << GetWindowLongPtrW(window.handle(), GWL_EXSTYLE)
                       << std::dec << L" point=" << GET_X_LPARAM(at) << L"," << GET_Y_LPARAM(at) << L'\n';
        }
        require(SendMessageW(volume, TBM_GETPOS, 0, 0) == before_wheel + 1 && GetFocus() == source,
                "One accumulated wheel notch changes the hovered slider without stealing focus");
        SendMessageW(window.handle(), WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, 120), wheel_position(volume));
        require(SendMessageW(volume, TBM_GETPOS, 0, 0) == before_wheel + 1,
                "Control-wheel must not change a numeric value");
        pump(window, 240);
        require(actions.back().kind == ui::MainAction::volume && actions.back().final,
                "Wheel bursts must eventually flush a final intent");
        model.local.volume.percent = before_wheel + 1;
        model.local.volume.can_write = false;
        update();
        const auto disabled_actions = actions.size();
        SendMessageW(window.handle(), WM_MOUSEWHEEL, MAKEWPARAM(0, 120), wheel_position(volume));
        require(actions.size() == disabled_actions, "Disabled sliders must not consume wheel writes");
        model.local.volume.can_write = true;
        model.slider_style = ui::MainSliderStyle::elastic;
        update();
        SendMessageW(volume, WM_MOUSEMOVE, 0, MAKELPARAM(ui_test::pixel(100), ui_test::pixel(15)));
        pump(window, 200);
        capture(window, report, L"10-elastic-hover.png");
        SendMessageW(volume, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(slider.right / 2, slider.bottom / 2));
        require(SendMessageW(volume, TBM_GETPOS, 0, 0) == 50,
                "Elastic drag must use the same direct numeric mapping");
        SendMessageW(volume, WM_LBUTTONUP, 0, MAKELPARAM(slider.right / 2, slider.bottom / 2));
        report.passed.push_back("wheel_precision_routing_disabled_ctrl_and_elastic_drag");

        HWND toggle = control(window, 18);
        const auto toggle_actions = count(actions, ui::MainAction::multipoint);
        SendMessageW(toggle, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(ui_test::pixel(44), ui_test::pixel(15)));
        SendMessageW(toggle, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(ui_test::pixel(24), ui_test::pixel(15)));
        SendMessageW(toggle, WM_LBUTTONUP, 0, MAKELPARAM(ui_test::pixel(24), ui_test::pixel(15)));
        require(count(actions, ui::MainAction::multipoint) == toggle_actions + 1 && actions.back().value == 0,
                "A switch drag must commit one off request without a synthetic second click");
        model.busy = true;
        update();
        pump(window, 250);
        model.busy = false;
        model.feedback_error = true;
        update();
        pump(window, 250);
        require(caption(toggle).find(L"已选中") != std::wstring::npos,
                "A failed switch request must restore the earphone's confirmed on state");
        report.passed.push_back("switch_drag_commits_once_and_failed_readback_restores_state");

        model = ui::main_preview(L"connected");
        update();
        pump(window, 350);
        wait_mode_ready(window);
        const auto text_baseline = heading_dark_pixels(window);
        require(text_baseline > 100, "The static heading must contain visible dark text");
        const auto gdi_before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        const auto user_before = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
        for (int cycle = 0; cycle < 12; ++cycle) {
            wait_mode_ready(window);
            const int target = (cycle + 1) % 3;
            const int target_id = noise_id + target;
            const auto before = count(actions, ui::MainAction::sound_mode);
            const auto started = GetTickCount64();
            // Exercise both real button input and already queued notifications.
            click(window, target_id);
            for (int n = 0; n < 100; ++n)
                SendMessageW(window.handle(), WM_COMMAND, MAKEWPARAM(noise_id + n % 3, BN_CLICKED),
                             reinterpret_cast<LPARAM>(control(window, noise_id + n % 3)));
            require(count(actions, ui::MainAction::sound_mode) == before + 1,
                    "A rapid sound-mode burst must dispatch exactly once, without building a queue");
            require(!IsWindowEnabled(control(window, target_id)),
                    "Sound-mode input must lock immediately, before the worker publishes busy");
            model.busy = true;
            update();
            pump(window, 80);
            model.busy = false;
            model.device.sound.mode = known(static_cast<sc::protocol::SoundMode>(target));
            update();
            require(GetTickCount64() - started < 200 && !IsWindowEnabled(control(window, noise_id)),
                    "Fast readback must not bypass the minimum 200ms switch interval");
            wait_mode_ready(window);
            require(GetTickCount64() - started >= 200,
                    "The next sound-mode command must wait for the minimum interval");
            require(heading_dark_pixels(window) == text_baseline,
                    "Repeated control updates must preserve the exact dark static heading pixels");
        }
        report.passed.push_back("sound_mode_rapid_bursts_cooldown_and_repeated_text_stability");
        // A slow worker must stay locked even after the timer expires.
        click(window, ambient_id);
        model.busy = true;
        update();
        pump(window, 680);
        require(!IsWindowEnabled(control(window, noise_id)),
                "Cooldown expiry must not override an unfinished operation");
        model.busy = false;
        model.device.sound.mode = known(sc::protocol::SoundMode::Ambient);
        update();
        pump(window, 350);
        wait_mode_ready(window);
        const auto duplicate_before = count(actions, ui::MainAction::sound_mode);
        click(window, ambient_id);
        require(count(actions, ui::MainAction::sound_mode) == duplicate_before,
                "Clicking the confirmed selected mode must not dispatch redundant hardware work");
        report.passed.push_back("sound_mode_slow_readback_and_duplicate_selection_guards");
        capture(window, report, L"13-small-rounded-wake-bars.png");
        model = ui::main_preview(L"ambient");
        model.preview = false;
        model.slider_style = ui::MainSliderStyle::wake;
        model.ambient_slider_style = ui::MainSliderStyle::wake;
        update();
        pump(window, 350);
        const auto volume_wake = control_pixels(window, control(window, volume_id));
        const auto ambient_wake = control_pixels(window, control(window, 29));
        model.ambient_slider_style = ui::MainSliderStyle::elastic;
        update();
        pump(window, 350);
        const auto ambient_elastic = control_pixels(window, control(window, 29));
        require(control_pixels(window, control(window, volume_id)) == volume_wake && ambient_elastic != ambient_wake,
                "Ambient appearance must change its real pixels without changing system volume appearance");
        model.slider_style = ui::MainSliderStyle::elastic;
        update();
        pump(window, 350);
        require(control_pixels(window, control(window, volume_id)) != volume_wake &&
                    control_pixels(window, control(window, 29)) == ambient_elastic,
                "System volume appearance must change independently of ambient appearance");
        report.passed.push_back("volume_and_ambient_appearances_are_independent_in_real_pixels");

        window.hide();
        pump(window, 25);
        window.show(false);
        pump(window, 50);
        require(heading_dark_pixels(window) == text_baseline,
                "Tray hide/restore must preserve text colors after repeated input");
        // Synthetic native owner-draw notifications must not schedule ULW paints.
        pump(window, 350);
        const auto paints_before = report.paint_messages;
        DRAWITEMSTRUCT notification{};
        notification.CtlType = ODT_BUTTON;
        notification.CtlID = noise_id;
        notification.hwndItem = control(window, noise_id);
        for (int n = 0; n < 500; ++n)
            SendMessageW(window.handle(), WM_DRAWITEM, noise_id, reinterpret_cast<LPARAM>(&notification));
        pump(window, 200);
        require(report.paint_messages - paints_before <= 1,
                "Owner-draw acknowledgements and idle time must not feed a repaint storm");
        require(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= gdi_before + 2 &&
                    GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) <= user_before + 2,
                "Repeated interactions must not accumulate window/GDI resources");
        report.passed.push_back("idle_owner_draw_does_not_loop_tray_text_and_resources_stay_stable");

        window.hide();
        ui::MainWindow high_dpi([&](ui::MainAction action, int value, bool final, std::uint64_t address) {
            actions.push_back({action, value, final, address});
        });
        high_dpi.create(GetModuleHandleW(nullptr), 192);
        auto tall_model = ui::main_preview(L"ambient");
        project(tall_model);
        high_dpi.update(tall_model);
        high_dpi.show(false);
        pump(high_dpi, 30);
        const auto high_work = work_area(high_dpi.handle());
        const auto high_bounds = bounds(high_dpi.handle());
        require(high_bounds.right == high_work.right - ui_test::pixel(8, 192) && high_bounds.bottom == high_work.bottom - ui_test::pixel(8, 192),
                "The high-DPI fixture must preserve the same 8 DIP work-area margin");
        // The fractional boundary pixel also contains antialiased content below
        // the header. Compare only rows wholly inside the fixed title bar.
        const auto header_rows = static_cast<int>(std::floor(44.f * 192 / 96.f * ui_test::expected_zoom));
        const auto header_before = pixels(high_dpi.handle(), header_rows);
        const auto title_before = bounds(control(high_dpi, 10));
        SendMessageW(high_dpi.handle(), WM_MOUSEWHEEL, MAKEWPARAM(0, -12 * WHEEL_DELTA), 0);
        pump(high_dpi, 30);
        const auto header_after = pixels(high_dpi.handle(), header_rows);
        const auto title_after = bounds(control(high_dpi, 10));
        require(title_before.left == title_after.left && title_before.top == title_after.top,
                "The title bar must stay fixed while the main content scrolls");
        if (header_before != header_after) {
            int printed = 0;
            for (std::size_t i = 0; i < header_before.size() && printed < 20; ++i)
                if (header_before[i] != header_after[i]) {
                    std::cout << "Header delta x=" << i % width(high_bounds) << " y=" << i / width(high_bounds) << " before=" << std::hex
                              << header_before[i] << " after=" << header_after[i] << std::dec << '\n';
                    ++printed;
                }
            capture(high_dpi, report, L"header-failure.png");
        }
        require(header_before == header_after,
                "Scrolled child controls must not overwrite the title-bar pixels");
        bool needs_scroll = height(bounds(high_dpi.handle())) < ui_test::pixel(822, 192);
        bool clipped = false;
        if (needs_scroll) {
            auto parent = bounds(high_dpi.handle());
            for (int id = 13; id <= 32; ++id) {
                HWND child = control(high_dpi, id);
                if (!IsWindowVisible(child))
                    continue;
                auto rect = bounds(child);
                if (rect.top >= parent.top + ui_test::pixel(44, 192))
                    continue;
                HRGN region = CreateRectRgn(0, 0, 0, 0);
                int kind = GetWindowRgn(child, region);
                RECT clip{};
                GetRgnBox(region, &clip);
                DeleteObject(region);
                require(kind == NULLREGION || (kind != ERROR && rect.top + clip.top >= parent.top + ui_test::pixel(44, 192)),
                        "A scrolled child must have no hit-test area inside the sticky title bar");
                clipped = true;
            }
            require(clipped, "The high-DPI fixture must exercise a clipped child control");
        }
        capture(high_dpi, report, L"07-high-dpi-scrolled.png");
        report.passed.push_back(needs_scroll ? "high_dpi_scroll_preserves_header_pixels_and_hit_testing"
                                             : "high_dpi_header_preserved_monitor_did_not_require_scroll");
        high_dpi.hide();
        report.write();
        std::cout << "Native main-window fixture scenarios passed: " << report.passed.size()
                  << "; HardwareVerified=false; screenshots=" << report.directory.string() << '\n';
        return 0;
    } catch (const winrt::hresult_error &error) {
        report.failure = "WinRT or rendering failure";
        report.write();
        std::wcerr << error.message().c_str() << L'\n';
        return 1;
    } catch (const std::exception &error) {
        report.failure = error.what();
        report.write();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
