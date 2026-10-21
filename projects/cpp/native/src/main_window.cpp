#include "sonyconnect/main_window.hpp"
#include "sonyconnect/more_window.hpp"
#include "sonyconnect/ui_motion.hpp"
#include "sonyconnect/ui_layout.hpp"
#include "sonyconnect/pen_icons.hpp"
#include "sonyconnect/battery_icon.hpp"
#include <windowsx.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <winrt/base.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
#include <tuple>
#ifdef SONYCONNECT_RESOURCE_PROBE
#include <fstream>
#endif

namespace sonyconnect::ui {
namespace {
constexpr wchar_t main_class[] = L"SonyConnect.MainWindow";
constexpr UINT icon_resource = 101;
constexpr UINT repaint_message = WM_APP + 10;
constexpr UINT frame_message = WM_APP + 11;
constexpr UINT_PTR mode_cooldown_timer = 6;
constexpr ULONGLONG mode_cooldown_ms = 200;
constexpr unsigned bg = 0xf8f9fc, ink = 0x202534, muted = 0x697386, blue = 0x4c5ed6, border = 0xdce0e8,
                   pale = 0xeef0fd, green = 0x23876c;
double animation_seconds() {
    static const double frequency = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / frequency;
}
D2D1_COLOR_F color(unsigned c, float a = 1) {
    return D2D1::ColorF(static_cast<float>((c >> 16) & 255) / 255, static_cast<float>((c >> 8) & 255) / 255,
                        static_cast<float>(c & 255) / 255, a);
}
std::wstring wtext(const std::string &s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring r(n, L' ');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), r.data(), n);
    return r;
}
std::wstring percent(const Value<int> &v) { return v.known() ? std::to_wstring(*v.value) + L"%" : L"—"; }
template <class T> Value<T> known(T v) {
    Value<T> r;
    r.status = ValueStatus::Known;
    r.value = std::move(v);
    return r;
}
enum ControlId {
    title = 10,
    minimize,
    close_button,
    power,
    noise,
    ambient,
    off,
    focus,
    multi,
    quality,
    stable,
    source0,
    source1,
    prev,
    media,
    next,
    more,
    bluetooth,
    retry,
    ambient_slider,
    volume_slider,
    releases,
    feedback_status,
    media_details,
    refresh
};
enum class Icon {
    none,
    minus,
    close,
    down,
    power,
    headphones,
    ear,
    wave,
    computer,
    phone,
    bluetooth,
    music,
    play,
    pause,
    previous,
    next,
    settings,
    laptop,
    radio,
    volume_low,
    volume,
    lock,
    check,
    alert,
    refresh
};
struct Box {
    float x{}, y{}, w{}, h{};
};
struct Control {
    HWND hwnd{};
    int id{};
    Box box;
    Icon icon{};
    std::wstring label, accessible;
    bool enabled{true}, visible{true}, selected{}, hover{}, slider{};
    int minimum{}, maximum{100}, value{};
    bool dragging{};
    motion::Wake wake;
    motion::Elastic elastic;
    motion::WheelAccumulator wheel;
    ULONGLONG wheel_due{};
    std::uint64_t wheel_link{};
    bool wake_initialized{};
    RECT presented{}, presented_clip{};
    bool presented_once{}, presented_visible{}, presented_enabled{};
};
// Common-controls owns hover tracking; text is requested on demand, so unrelated
// state refreshes do not rebuild the tooltip or flash its native window.
struct FullTextTips {
    HWND hwnd{};
    std::wstring text;
    void create(HWND owner) {
        hwnd = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                 CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                 owner, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!hwnd) throw std::runtime_error("Cannot create full text tooltip");
        SendMessageW(hwnd, TTM_SETMAXTIPWIDTH, 0, 560);
        SendMessageW(hwnd, TTM_SETDELAYTIME, TTDT_INITIAL, 450);
        SendMessageW(hwnd, TTM_SETDELAYTIME, TTDT_AUTOPOP, 20000);
    }
    void add(HWND owner, HWND child) {
        TOOLINFOW tool{sizeof(tool)};
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.hwnd = owner;
        tool.uId = reinterpret_cast<UINT_PTR>(child);
        tool.lpszText = LPSTR_TEXTCALLBACKW;
        if (!SendMessageW(hwnd, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool)))
            throw std::runtime_error("Cannot register full text tooltip");
    }
    void remove(HWND owner, HWND child) {
        TOOLINFOW tool{sizeof(tool)};
        tool.hwnd = owner;
        tool.uId = reinterpret_cast<UINT_PTR>(child);
        SendMessageW(hwnd, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }
    void dismiss() { if (hwnd) SendMessageW(hwnd, TTM_POP, 0, 0); }
    bool notify(LPARAM l) {
        auto *info = reinterpret_cast<NMTTDISPINFOW *>(l);
        if (!info || info->hdr.hwndFrom != hwnd || info->hdr.code != TTN_GETDISPINFOW) return false;
        auto child = reinterpret_cast<HWND>(info->hdr.idFrom);
        text.resize(GetWindowTextLengthW(child) + 1);
        text.resize(GetWindowTextW(child, text.data(), static_cast<int>(text.size())));
        info->lpszText = text.data();
        return true;
    }
    void inspect(HWND owner, HWND child) {
        std::wstring value(GetWindowTextLengthW(child) + 1, L'\0');
        value.resize(GetWindowTextW(child, value.data(), static_cast<int>(value.size())));
        dismiss();
        if (!value.empty()) MessageBoxW(owner, value.c_str(), L"完整内容", MB_OK);
    }
};
struct PaintScope {
    HWND hwnd;
    PAINTSTRUCT ps{};
    HDC dc;
    explicit PaintScope(HWND h) : hwnd(h), dc(BeginPaint(h, &ps)) {}
    ~PaintScope() { EndPaint(hwnd, &ps); }
};
class Painter {
  public:
    struct FactoryLifetime {
        winrt::com_ptr<ID2D1Factory> value;
        FactoryLifetime() {
            winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, value.put()));
        }
    };
    // Factories stay on their creating UI thread. A weak cache shares them only
    // while a painter is alive; the final painter releases all COM resources
    // before the apartment exits. Repeated device-backed factory creation left
    // a mutex/section pair per instance in the measured Windows graphics stack.
    std::shared_ptr<FactoryLifetime> factory_lifetime;
    HWND owner{};
    winrt::com_ptr<ID2D1Factory> factory;
    winrt::com_ptr<IDWriteFactory> write;
    winrt::com_ptr<ID2D1DCRenderTarget> target;
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    winrt::com_ptr<ID2D1StrokeStyle> stroke;
    std::unique_ptr<PenIcons> pen_icons;
    const wchar_t *font_family{L"Microsoft YaHei UI"};
    std::map<std::pair<int, int>, winrt::com_ptr<IDWriteTextFormat>> fonts;
    using TextKey = std::tuple<std::wstring, int, int, int, float, float>;
    std::map<TextKey, winrt::com_ptr<IDWriteTextLayout>> text_layouts;
    std::map<std::wstring, float> title_widths;
    std::map<std::pair<std::wstring, int>, float> measured_text_widths;
    winrt::com_ptr<ID2D1Layer> content_layer;
    winrt::com_ptr<ID2D1Bitmap> headset_bitmap;
    winrt::com_ptr<IWICBitmapSource> headset_source;
    winrt::com_ptr<ID2D1PathGeometry> battery_outline;
    winrt::com_ptr<ID2D1Bitmap> dsee_bitmap;
    winrt::com_ptr<IWICBitmapSource> dsee_source;
    unsigned generation{};
    HDC canvas{};
    HBITMAP canvas_bitmap{}, previous_bitmap{};
    void *canvas_bits{};
    int canvas_width{}, canvas_height{};
    ~Painter() {
        if (canvas) {
            SelectObject(canvas, previous_bitmap);
            DeleteObject(canvas_bitmap);
            DeleteDC(canvas);
        }
    }
    void prepare_canvas(HDC compatible, int w, int h) {
        if (canvas && w <= canvas_width && h <= canvas_height)
            return;
        if (canvas) {
            SelectObject(canvas, previous_bitmap);
            DeleteObject(canvas_bitmap);
        } else
            canvas = CreateCompatibleDC(compatible);
        canvas_width = std::max(w, canvas_width);
        canvas_height = std::max(h, canvas_height);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = canvas_width;
        info.bmiHeader.biHeight = -canvas_height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        canvas_bitmap = CreateDIBSection(compatible, &info, DIB_RGB_COLORS, &canvas_bits, nullptr, 0);
        if (!canvas || !canvas_bitmap)
            throw std::runtime_error("Cannot allocate the main window drawing buffer");
        previous_bitmap = static_cast<HBITMAP>(SelectObject(canvas, canvas_bitmap));
    }
    Painter() {
        static thread_local std::weak_ptr<FactoryLifetime> current_factory;
        factory_lifetime = current_factory.lock();
        if (!factory_lifetime) {
            factory_lifetime = std::make_shared<FactoryLifetime>();
            current_factory = factory_lifetime;
        }
        factory.copy_from(factory_lifetime->value.get());
        pen_icons = std::make_unique<PenIcons>(factory.get());
        winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                 reinterpret_cast<IUnknown **>(write.put())));
        winrt::com_ptr<IDWriteFontCollection> collection;
        winrt::check_hresult(write->GetSystemFontCollection(collection.put()));
        UINT32 family{};
        BOOL exists{};
        collection->FindFamilyName(L"Noto Sans SC", &family, &exists);
        if (exists)
            font_family = L"Noto Sans SC";
        create_target();
        auto st = D2D1::StrokeStyleProperties();
        st.startCap = st.endCap = st.dashCap = D2D1_CAP_STYLE_ROUND;
        st.lineJoin = D2D1_LINE_JOIN_ROUND;
        winrt::check_hresult(factory->CreateStrokeStyle(st, nullptr, 0, stroke.put()));
    }
    void create_target() {
        ++generation;
        headset_bitmap = nullptr;
        dsee_bitmap = nullptr;
        content_layer = nullptr;
        brush = nullptr;
        target = nullptr;
        auto properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        winrt::check_hresult(factory->CreateDCRenderTarget(&properties, target.put()));
        winrt::check_hresult(target->CreateSolidColorBrush(color(ink), brush.put()));
        winrt::check_hresult(target->CreateLayer(content_layer.put()));
        if (headset_source)
            winrt::check_hresult(target->CreateBitmapFromWicBitmap(headset_source.get(), headset_bitmap.put()));
    }
    void dsee_wordmark(Box box, bool extreme = true) {
        if (!dsee_source) {
            auto module = GetModuleHandleW(nullptr);
            auto resource = FindResourceW(module, MAKEINTRESOURCEW(102), RT_RCDATA);
            auto loaded = resource ? LoadResource(module, resource) : nullptr;
            auto bytes = loaded ? static_cast<BYTE *>(LockResource(loaded)) : nullptr;
            if (!bytes) throw std::runtime_error("Cannot load embedded DSEE wordmark");
            winrt::com_ptr<IWICImagingFactory> wic;
            winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                                  IID_PPV_ARGS(wic.put())));
            winrt::com_ptr<IWICStream> stream;
            winrt::check_hresult(wic->CreateStream(stream.put()));
            winrt::check_hresult(stream->InitializeFromMemory(bytes, SizeofResource(module, resource)));
            winrt::com_ptr<IWICBitmapDecoder> decoder;
            winrt::check_hresult(wic->CreateDecoderFromStream(stream.get(), nullptr,
                                      WICDecodeMetadataCacheOnLoad, decoder.put()));
            winrt::com_ptr<IWICBitmapFrameDecode> frame;
            winrt::check_hresult(decoder->GetFrame(0, frame.put()));
            winrt::com_ptr<IWICFormatConverter> converted;
            winrt::check_hresult(wic->CreateFormatConverter(converted.put()));
            winrt::check_hresult(converted->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA,
                                          WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
            // Downsample once to four pixels per DIP. The original 2400x216
            // transparent Pen asset remains embedded; no external file is read.
            winrt::com_ptr<IWICBitmapScaler> scaled;
            winrt::check_hresult(wic->CreateBitmapScaler(scaled.put()));
            winrt::check_hresult(scaled->Initialize(converted.get(), 400, 36, WICBitmapInterpolationModeFant));
            dsee_source = scaled.as<IWICBitmapSource>();
        }
        if (!dsee_bitmap)
            winrt::check_hresult(target->CreateBitmapFromWicBitmap(dsee_source.get(), dsee_bitmap.put()));
        const auto pixels = dsee_bitmap->GetSize();
        // Original alpha ends at x=992 for DSEE; Extreme begins at x=1112.
        // Crop at draw time, preserving the original embedded resource unchanged.
        const auto source = D2D1::RectF(0, 0, extreme ? pixels.width : pixels.width * (992.f / 2400.f), pixels.height);
        target->DrawBitmap(dsee_bitmap.get(), D2D1::RectF(box.x, box.y, box.x + box.w, box.y + box.h),
                           1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &source);
    }
    void headphone_battery(Box box, const Value<int> &level, const Value<bool> &charging) {
        if (!battery_outline) {
            winrt::check_hresult(factory->CreatePathGeometry(battery_outline.put()));
            winrt::com_ptr<ID2D1GeometrySink> sink;
            winrt::check_hresult(battery_outline->Open(sink.put()));
            sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
            bool open = false;
            for (const auto &command : battery_path) {
                const auto &v = command.points;
                if (command.operation == 1) {
                    if (open) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sink->BeginFigure({v[4], v[5]}, D2D1_FIGURE_BEGIN_FILLED);
                    open = true;
                } else if (command.operation == 2) sink->AddLine({v[4], v[5]});
                else sink->AddBezier(D2D1::BezierSegment({v[0], v[1]}, {v[2], v[3]}, {v[4], v[5]}));
            }
            if (open) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            winrt::check_hresult(sink->Close());
        }
        D2D1_MATRIX_3X2_F previous;
        target->GetTransform(&previous);
        const auto previous_color = brush->GetColor();
        target->SetTransform(D2D1::Matrix3x2F::Scale(box.w / 20, box.h / 12) *
                             D2D1::Matrix3x2F::Translation(box.x, box.y) * previous);
        const bool is_charging = charging.known() && *charging.value;
        const auto tint = is_charging ? green : level.known() && *level.value <= 20 ? 0xb56c25 : 0x76767d;
        brush->SetColor(color(tint));
        target->FillGeometry(battery_outline.get(), brush.get());
        const int percent = level.known() ? std::clamp(*level.value, 0, 100) : 0;
        if (percent > 0) rect({3, 3, 13.f * percent / 100, 6}, tint, .6f);
        if (is_charging) {
            const auto bolt = percent >= 75 ? 0xffffff : green;
            line(10.5f, 3, 8, 6.5f, bolt, 1.2f);
            line(8, 6.5f, 11.5f, 5.5f, bolt, 1.2f);
            line(11.5f, 5.5f, 9, 9, bolt, 1.2f);
        }
        brush->SetColor(previous_color);
        target->SetTransform(previous);
    }
    void begin(HDC dc, RECT rect, float scale) {
        winrt::check_hresult(target->BindDC(dc, &rect));
        target->BeginDraw();
        target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
        target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        // ClearType on a transparent canvas/layer can give unpredictable results.
        // Grayscale keeps text coverage consistent with premultiplied alpha.
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    void end() {
        auto hr = target->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            create_target();
            if (owner)
                PostMessageW(owner, repaint_message, 0, 0);
        } else
            winrt::check_hresult(hr);
    }
    void line(float x1, float y1, float x2, float y2, unsigned c, float width = 1.3f) {
        brush->SetColor(color(c));
        target->DrawLine({x1, y1}, {x2, y2}, brush.get(), width, stroke.get());
    }
    void rect(Box r, unsigned c, float radius = 0, unsigned outline = 0) {
        auto shape = D2D1::RoundedRect(D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h), radius, radius);
        brush->SetColor(color(c));
        target->FillRoundedRectangle(shape, brush.get());
        if (outline) {
            brush->SetColor(color(outline));
            target->DrawRoundedRectangle(shape, brush.get(), 1);
        }
    }
    void wake_bar(Box r, unsigned c) {
        const auto corner = motion::wake_corners(r.w, r.h);
        brush->SetColor(color(c));
        target->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h),
                             static_cast<float>(corner.x), static_cast<float>(corner.y)),
            brush.get());
    }
    void circle(float x, float y, float radius, unsigned c, bool fill = true, float width = 1.3f) {
        brush->SetColor(color(c));
        auto e = D2D1::Ellipse({x, y}, radius, radius);
        if (fill)
            target->FillEllipse(e, brush.get());
        else
            target->DrawEllipse(e, brush.get(), width);
    }
    void text(const std::wstring &s, Box r, unsigned c = ink, int size = 13, bool bold = false,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, int weight = 0) {
        const auto font_weight = static_cast<DWRITE_FONT_WEIGHT>(weight ? weight : (bold ? 600 : 400));
        auto key = std::make_pair(size, static_cast<int>(font_weight));
        if (!fonts.contains(key)) {
            winrt::com_ptr<IDWriteTextFormat> f;
            winrt::check_hresult(write->CreateTextFormat(font_family, nullptr, font_weight,
                                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                         static_cast<float>(size), L"zh-CN", f.put()));
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            fonts.emplace(key, std::move(f));
        }
        auto f = fonts.at(key).get();
        f->SetTextAlignment(align);
        const TextKey layout_key{s, size, static_cast<int>(font_weight), static_cast<int>(align), r.w, r.h};
        auto cached = text_layouts.find(layout_key);
        if (cached == text_layouts.end()) {
            winrt::com_ptr<IDWriteTextLayout> layout;
            winrt::check_hresult(write->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f,
                                                         std::max(1.f, r.w), std::max(1.f, r.h), layout.put()));
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            winrt::com_ptr<IDWriteInlineObject> ellipsis;
            write->CreateEllipsisTrimmingSign(f, ellipsis.put());
            layout->SetTrimming(&trim, ellipsis.get());
            if (text_layouts.size() >= 512) text_layouts.erase(text_layouts.begin());
            cached = text_layouts.emplace(layout_key, std::move(layout)).first;
        }
        const auto &layout = cached->second;
        brush->SetColor(color(c));
        target->DrawTextLayout({r.x, r.y}, layout.get(), brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    float text_width(const std::wstring &s, int size) {
        const auto key = std::make_pair(s, size);
        if (auto cached = measured_text_widths.find(key); cached != measured_text_widths.end())
            return cached->second;
        winrt::com_ptr<IDWriteTextFormat> format;
        winrt::check_hresult(write->CreateTextFormat(font_family, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                  static_cast<float>(size), L"zh-CN", format.put()));
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        winrt::com_ptr<IDWriteTextLayout> layout;
        winrt::check_hresult(write->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()),
                                                      format.get(), 1000, 100, layout.put()));
        DWRITE_TEXT_METRICS metrics{};
        winrt::check_hresult(layout->GetMetrics(&metrics));
        return measured_text_widths.emplace(key, metrics.widthIncludingTrailingWhitespace).first->second;
    }
    float title_width(const std::wstring &s) {
        if (auto cached = title_widths.find(s); cached != title_widths.end()) return cached->second;
        winrt::com_ptr<IDWriteTextFormat> f;
        winrt::check_hresult(write->CreateTextFormat(font_family, nullptr, DWRITE_FONT_WEIGHT_MEDIUM,
                                                     DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13,
                                                     L"zh-CN", f.put()));
        winrt::com_ptr<IDWriteTextLayout> l;
        winrt::check_hresult(
            write->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f.get(), 2000, 32, l.put()));
        DWRITE_TEXT_METRICS metrics{};
        l->GetMetrics(&metrics);
        const float width = metrics.widthIncludingTrailingWhitespace + 28;
        if (title_widths.size() >= 32) title_widths.erase(title_widths.begin());
        title_widths.emplace(s, width);
        return width;
    }
    void icon(Icon i, float x, float y, float size, unsigned c) {
        PenIcon name = PenIcon::none;
        switch (i) {
        case Icon::minus:
            name = PenIcon::minus;
            break;
        case Icon::close:
            name = PenIcon::x;
            break;
        case Icon::down:
            name = PenIcon::chevron_down;
            break;
        case Icon::power:
            name = PenIcon::power;
            break;
        case Icon::alert:
            name = PenIcon::circle_alert;
            break;
        case Icon::headphones:
            name = PenIcon::headphones;
            break;
        case Icon::ear:
            name = PenIcon::ear;
            break;
        case Icon::wave:
            name = PenIcon::audio_lines;
            break;
        case Icon::computer:
            name = PenIcon::monitor;
            break;
        case Icon::phone:
            name = PenIcon::smartphone;
            break;
        case Icon::bluetooth:
            name = PenIcon::bluetooth;
            break;
        case Icon::refresh:
            name = PenIcon::rotate_cw;
            break;
        case Icon::music:
            name = PenIcon::music_2;
            break;
        case Icon::play:
            name = PenIcon::play;
            break;
        case Icon::pause:
            name = PenIcon::pause;
            break;
        case Icon::previous:
            name = PenIcon::skip_back;
            break;
        case Icon::next:
            name = PenIcon::skip_forward;
            break;
        case Icon::settings:
            name = PenIcon::settings_2;
            break;
        case Icon::laptop:
            name = PenIcon::laptop_minimal;
            break;
        case Icon::radio:
            name = PenIcon::radio;
            break;
        case Icon::volume_low:
            name = PenIcon::volume_1;
            break;
        case Icon::volume:
            name = PenIcon::volume_2;
            break;
        case Icon::lock:
            name = PenIcon::lock_keyhole;
            break;
        case Icon::check:
            name = PenIcon::check;
            break;
        default:
            break;
        }
        pen_icons->draw(target.get(), brush.get(), name, x, y, size, color(c));
    }
    void outline_progress(Box r, float radius, float progress, unsigned c) {
        auto points = motion::outline_ccw(r.w, r.h, radius, progress);
        if (points.size() < 2)
            return;
        winrt::com_ptr<ID2D1PathGeometry> path;
        winrt::check_hresult(factory->CreatePathGeometry(path.put()));
        winrt::com_ptr<ID2D1GeometrySink> sink;
        winrt::check_hresult(path->Open(sink.put()));
        sink->BeginFigure({r.x + static_cast<float>(points[0].x), r.y + static_cast<float>(points[0].y)},
                          D2D1_FIGURE_BEGIN_HOLLOW);
        for (std::size_t i = 1; i < points.size(); ++i)
            sink->AddLine({r.x + static_cast<float>(points[i].x), r.y + static_cast<float>(points[i].y)});
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        winrt::check_hresult(sink->Close());
        brush->SetColor(color(c));
        target->DrawGeometry(path.get(), brush.get(), 1.5f, stroke.get());
    }
};
} // namespace
#ifdef SONYCONNECT_RESOURCE_PROBE
void resource_components(const std::filesystem::path &path) {
    std::ofstream out(path);
    const auto record = [&](const char *stage) {
        DWORD count{}; GetProcessHandleCount(GetCurrentProcess(), &count);
        out << stage << "," << count << "\n";
    };
    record("before");
    for (int n = 0; n < 20; ++n) {
        winrt::com_ptr<ID2D1Factory> f;
        winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, f.put()));
    }
    record("d2d_factories_20");
    for (int n = 0; n < 20; ++n) {
        winrt::com_ptr<IDWriteFactory> f;
        winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown **>(f.put())));
        winrt::com_ptr<IDWriteFontCollection> c;
        winrt::check_hresult(f->GetSystemFontCollection(c.put()));
    }
    record("dwrite_font_collections_20");
    for (int n = 0; n < 20; ++n) { Painter p; }
    record("painters_20");
    for (int n = 0; n < 20; ++n) { Painter p; }
    record("painters_second_20");
    {
        winrt::com_ptr<ID2D1Factory> f;
        winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, f.put()));
        for (int n = 0; n < 20; ++n) {
            winrt::com_ptr<ID2D1DCRenderTarget> target;
            const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
            winrt::check_hresult(f->CreateDCRenderTarget(&props, target.put()));
        }
    }
    record("shared_factory_dc_targets_20");
    for (int n = 0; n < 20; ++n) {
        auto h = CreateWindowExW(0, L"STATIC", L"probe", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (h) DestroyWindow(h);
    }
    record("plain_windows_20");
}
#endif
struct MainWindow::Impl {
    HWND hwnd{};
    Dispatch dispatch;
    MainModel model;
    std::unique_ptr<Painter> painter;
    FullTextTips tips;
    std::map<int, Control> controls;
    UINT dpi{96}, forced_dpi{};
    int content_height{232}, height{232}, width{360}, scroll{};
    int drag_id{-1}, hold_source{-1};
    ULONGLONG hold_start{};
    std::uint64_t interaction_link{};
    bool layout_busy{};
    bool presenting{}, frame_dirty{true}, frame_posted{};
    ULONGLONG mode_cooldown_until{};
    struct BusyScope {
        bool &value;
        explicit BusyScope(bool &flag) : value(flag) { value = true; }
        ~BusyScope() { value = false; }
    };
    void post_frame() {
        if (frame_posted || !hwnd) return;
        frame_posted = true;
        if (!PostMessageW(hwnd, frame_message, 0, 0)) {
            frame_posted = false;
            throw std::runtime_error("Cannot schedule the window frame");
        }
    }
    void flush_frame() {
        if (!frame_dirty || presenting || layout_busy) return;
        RECT bounds{};
        GetWindowRect(hwnd, &bounds);
        present(bounds);
    }
    void request_frame(bool immediate = false) {
        frame_dirty = true;
        // A layered window owns its pixels. Explicitly schedule its submission;
        // native paint validation must not discard a pending model/animation frame.
        if (immediate && !presenting && !layout_busy) flush_frame();
        else post_frame();
    }
    bool mode_cooling() const { return GetTickCount64() < mode_cooldown_until; }
    void reset_mode_cooldown() {
        KillTimer(hwnd, mode_cooldown_timer);
        mode_cooldown_until = 0;
    }
    void resume_mode_cooldown() {
        const auto now = GetTickCount64();
        if (mode_cooldown_until > now) {
            if (!SetTimer(hwnd, mode_cooldown_timer, static_cast<UINT>(mode_cooldown_until - now), nullptr))
                throw std::runtime_error("Cannot resume mode cooldown");
        } else reset_mode_cooldown();
    }
    std::wstring signature;
    HICON big_icon{}, small_icon{};
    bool graphics_error_reported{};
    int pending_mode{-1};
    std::optional<bool> pending_multi;
    motion::Switch multipoint_motion;
    bool multipoint_initialized{}, switch_dragging{}, switch_moved{};
    float switch_origin{}, switch_position{};
    int confirming_source{-1};
    bool keyboard_cues{};
    motion::WheelAccumulator scroll_wheel;
    int suppress_source_click{-1};
    motion::Segment segment;
    bool segment_initialized{}, positioned{}, resizing{}, animation_timer{}, reduce_motion{}, suspended{};
    ModuleFlow detail_flow;
    enum DetailKey { level_module = 1, focus_module = 2 };
    std::vector<LayoutModule> detail_modules() const {
        if (!ambient_mode()) return {};
        std::vector<LayoutModule> rows;
        if (model.device.sound.ambient_level.status != ValueStatus::Unsupported)
            rows.push_back({level_module, 60});
        if (model.device.sound.voice_focus.status != ValueStatus::Unsupported)
            rows.push_back({focus_module, 42});
        return rows;
    }
    MainSections sections() const { return MainSections(detail_flow.height()); }
    double frame_clock{}, resize_clock{};
    bool timer_resolution{};
    winrt::com_ptr<ID2D1Bitmap> previous_scene;
    float previous_scene_width{}, previous_scene_height{}, scene_progress{1};
    int rendered_pixel_width{}, rendered_pixel_height{};
    unsigned scene_generation{};
    bool scene_input_locked() const { return previous_scene && scene_progress < 1; }
    void remember_scene() {
        if (!painter->canvas || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return;
        previous_scene = nullptr;
        const auto properties = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        winrt::check_hresult(painter->target->CreateBitmap(
            D2D1::SizeU(rendered_pixel_width, rendered_pixel_height), painter->canvas_bits,
            painter->canvas_width * 4, properties, previous_scene.put()));
        previous_scene_width = rendered_pixel_width / scale();
        previous_scene_height = rendered_pixel_height / scale();
        scene_generation = painter->generation;
        scene_progress = 0;
    }
    RECT resize_from{}, resize_to{};
    int content_from{}, content_to{};
    explicit Impl(Dispatch d) : dispatch(std::move(d)) {}
    ~Impl() {
        if (IsWindow(hwnd))
            DestroyWindow(hwnd);
        if (big_icon)
            DestroyIcon(big_icon);
        if (small_icon)
            DestroyIcon(small_icon);
        stop_animation();
    }
    int px(float v) const { return static_cast<int>(std::lround(v * scale())); }
    float scale() const { return dpi / 96.f * interface_scale; }
    bool connected() const { return model.device.ready; }
    bool control_failed() const {
        return model.device.connected || (model.device.device && model.device.device->connected_hint);
    }
    bool sound_unsupported() const {
        if (model.device.sound.mode.status == ValueStatus::Unsupported)
            return true;
        const auto capability = model.device.capabilities.features.find(protocol::Feature::SoundMode);
        return capability != model.device.capabilities.features.end() &&
               capability->second.access.read == protocol::Support::Unsupported;
    }
    bool ambient_mode() const { return connected() && !sound_unsupported() && visual_mode() == 1; }
    int visual_mode() const {
        if (pending_mode >= 0)
            return pending_mode;
        return model.device.sound.mode.known() ? static_cast<int>(*model.device.sound.mode.value) : -1;
    }
    void read_motion_preference() {
        BOOL enabled = TRUE;
        reduce_motion = SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) && !enabled;
    }
    void schedule_animation() {
        if (suspended || reduce_motion || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return;
        if (!animation_timer) {
            frame_clock = animation_seconds();
            timer_resolution = timeBeginPeriod(1) == TIMERR_NOERROR;
            animation_timer = SetTimer(hwnd, 5, 16, nullptr) != 0;
            if (!animation_timer) {
                stop_animation();
                throw std::runtime_error("Cannot start the UI animation timer");
            }
        }
    }
    void stop_animation() {
        if (hwnd)
            KillTimer(hwnd, 5);
        animation_timer = false;
        if (timer_resolution) {
            timeEndPeriod(1);
            timer_resolution = false;
        }
    }
    void set_slider_target(Control &c) {
        double value = 100.0 * (c.value - c.minimum) / std::max(1, c.maximum - c.minimum);
        if (!c.wake_initialized || !c.presented_visible || reduce_motion || !IsWindowVisible(hwnd)) {
            c.wake.reset(value);
            c.elastic.reset(value);
            c.wake_initialized = true;
        } else if (c.dragging) {
            c.wake.reset(value);
            c.elastic.jump(value);
        } else {
            c.wake.set_target(value, c.wheel_due ? .16 : .22);
            c.elastic.set_target(value, c.wheel_due ? .16 : .22);
            if (c.wake.active() || c.elastic.active())
                schedule_animation();
        }
        c.elastic.set_active(IsWindowVisible(hwnd) && !IsIconic(hwnd) && c.visible &&
                             (c.dragging || c.hover || (keyboard_cues && GetFocus() == c.hwnd)));
        if (reduce_motion)
            c.elastic.advance(1);
        else if (c.elastic.active())
            schedule_animation();
    }
    void select_multipoint(bool on) {
        pending_multi = on;
        multipoint_motion.set_target(on);
        layout();
        schedule_animation();
        send(MainAction::multipoint, on ? 1 : 0);
    }
    void select_mode(int value) {
        if (value < 0 || value > 2 || !writable() || sound_unsupported() || !model.device.sound.writable ||
            pending_mode >= 0 || mode_cooling() || value == visual_mode())
            return;
        // A one-shot UI timer unlocks input; the serial worker/readback remains
        // authoritative, even when it takes longer than the minimum interval.
        if (!SetTimer(hwnd, mode_cooldown_timer, static_cast<UINT>(mode_cooldown_ms), nullptr))
            throw std::runtime_error("Cannot start the sound-mode cooldown timer");
        mode_cooldown_until = GetTickCount64() + mode_cooldown_ms;
        pending_mode = value;
        segment.set_target(value);
        resize();
        // An animated resize does not lay out until its first timer tick. Lock
        // native hit targets now as well as guarding queued notifications.
        if (resizing) layout();
        schedule_animation();
        // The visual response never waits for the animation, ACK, or readback.
        send(MainAction::sound_mode, value);
    }
    bool writable() const { return connected() && !model.busy && !model.device.busy && !model.preview; }
    std::wstring status_label() const {
        if (model.feedback_error && !model.busy && !model.device.busy)
            return L"回读错误，请重试";
        return {};
    }
    std::wstring device_name() const {
        if (!model.device.model.empty())
            return wtext(model.device.model);
        return model.device.device ? wtext(model.device.device->name) : L"";
    }
    void send(MainAction a, int value = 0, bool final = true, std::uint64_t device_address = 0) {
        dispatch(a, value, final, device_address);
    }
    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto self = reinterpret_cast<Impl *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            self = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            self->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(h, m, w, l);
        try {
            return self->message(m, w, l);
        } catch (const winrt::hresult_error &e) {
            self->report_error(h, m, e.message().c_str());
            return 0;
        } catch (const std::exception &e) {
            self->report_error(h, m, wtext(e.what()));
            return 0;
        }
    }
    void report_error(HWND h, UINT m, const std::wstring &detail) {
        if (m == WM_PAINT) {
            ValidateRect(h, nullptr);
            if (graphics_error_reported)
                return;
            graphics_error_reported = true;
        }
        MessageBoxW(h, detail.c_str(), L"SonyConnect 界面操作失败", MB_OK | MB_ICONERROR);
    }
    void create(HINSTANCE instance, UINT forced) {
        forced_dpi = forced;
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = proc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = main_class;
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(icon_resource));
        wc.hIconSm = wc.hIcon;
        RegisterClassExW(&wc);
        painter = std::make_unique<Painter>();
        POINT launch{};
        GetCursorPos(&launch);
        MONITORINFO launch_monitor{sizeof(launch_monitor)};
        GetMonitorInfoW(MonitorFromPoint(launch, MONITOR_DEFAULTTONEAREST), &launch_monitor);
        hwnd = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_CONTROLPARENT | WS_EX_LAYERED, main_class, L"SonyConnect",
                               WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX, launch_monitor.rcWork.left + 8,
                               launch_monitor.rcWork.top + 8, 360, 232, nullptr, nullptr, instance, this);
        if (!hwnd)
            throw std::runtime_error("Cannot create SonyConnect main window");
        painter->owner = hwnd;
        read_motion_preference();
        dpi = forced_dpi ? forced_dpi : GetDpiForWindow(hwnd);
        tips.create(hwnd);
        update_icons();
        auto menu = GetSystemMenu(hwnd, FALSE);
        DeleteMenu(menu, SC_SIZE, MF_BYCOMMAND);
        DeleteMenu(menu, SC_MAXIMIZE, MF_BYCOMMAND);
        for (int id = title; id <= refresh; ++id) {
            Control c;
            c.id = id;
            c.slider = id == ambient_slider || id == volume_slider;
            const bool status = id == feedback_status;
            c.hwnd = CreateWindowExW(WS_EX_TRANSPARENT,
                                     status     ? L"STATIC"
                                     : c.slider ? TRACKBAR_CLASSW
                                                : L"BUTTON",
                                     L"",
                                     WS_CHILD | (status ? 0 : WS_TABSTOP) |
                                         (status     ? SS_LEFT
                                          : c.slider ? TBS_NOTICKS
                                                     : BS_OWNERDRAW),
                                     0, 0, 1, 1, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                     instance, nullptr);
            if (!c.hwnd)
                throw std::runtime_error("Cannot create main window control");
            SetWindowSubclass(c.hwnd, control_proc, id, reinterpret_cast<DWORD_PTR>(this));
            tips.add(hwnd, c.hwnd);
            controls.emplace(id, std::move(c));
        }
        resize();
    }
    void update_icons() {
        auto instance = GetModuleHandleW(nullptr);
        if (big_icon)
            DestroyIcon(big_icon);
        if (small_icon)
            DestroyIcon(small_icon);
        big_icon = static_cast<HICON>(
            LoadImageW(instance, MAKEINTRESOURCEW(icon_resource), IMAGE_ICON, px(32), px(32), 0));
        small_icon = static_cast<HICON>(
            LoadImageW(instance, MAKEINTRESOURCEW(icon_resource), IMAGE_ICON, px(16), px(16), 0));
        painter->headset_bitmap = nullptr;
        winrt::com_ptr<IWICImagingFactory> wic;
        winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                              IID_PPV_ARGS(wic.put())));
        winrt::com_ptr<IWICBitmap> bitmap;
        winrt::check_hresult(wic->CreateBitmapFromHICON(big_icon, bitmap.put()));
        winrt::com_ptr<IWICFormatConverter> converted;
        winrt::check_hresult(wic->CreateFormatConverter(converted.put()));
        winrt::check_hresult(converted->Initialize(bitmap.get(), GUID_WICPixelFormat32bppPBGRA,
                                                  WICBitmapDitherTypeNone, nullptr, 0,
                                                  WICBitmapPaletteTypeCustom));
        painter->headset_source = converted.as<IWICBitmapSource>();
        winrt::check_hresult(painter->target->CreateBitmapFromWicBitmap(converted.get(), painter->headset_bitmap.put()));
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
    }
    void apply_frame(const RECT &bounds, float progress, int content) {
        BusyScope busy_scope(layout_busy);
        detail_flow.sample(progress);
        width = static_cast<int>(std::lround((bounds.right - bounds.left) / scale()));
        height = static_cast<int>(std::lround((bounds.bottom - bounds.top) / scale()));
        content_height = std::max(height, content);
        scroll = std::clamp(scroll, 0, content_height - height);
        layout();
        // One OS submission changes size, position and premultiplied pixels.
        // There is no resized window containing the preceding frame's bitmap.
        present(bounds);
    }
    void resize(bool animate = true) {
        if (!hwnd || layout_busy)
            return;
        const int target_width = connected() ? 460 : 360;
        const bool modules_changed = detail_flow.retarget(detail_modules());
        content_to = connected() ? static_cast<int>(std::lround(MainSections(detail_flow.target_height()).content)) : 232;
        MONITORINFO monitor{sizeof(monitor)};
        POINT cursor{};
        GetCursorPos(&cursor);
        GetMonitorInfoW(positioned ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
                                   : MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST),
                        &monitor);
        const int available = static_cast<int>((monitor.rcWork.bottom - monitor.rcWork.top - px(16)) / scale());
        const int target_height = std::min(content_to, std::max(180, available));
        RECT old{};
        GetWindowRect(hwnd, &old);
        int right = positioned ? old.right : monitor.rcWork.right - px(8);
        int bottom = positioned ? old.bottom : monitor.rcWork.bottom - px(8);
        const int w = px(static_cast<float>(target_width)), h = px(static_cast<float>(target_height));
        const auto fit = fit_desktop({monitor.rcWork.left, monitor.rcWork.top, monitor.rcWork.right, monitor.rcWork.bottom},
                                       right - w, bottom - h, w, h);
        const RECT target{fit.left, fit.top, fit.right, fit.bottom};
        if (animate && !modules_changed && positioned && EqualRect(&target, &resize_to)) {
            layout();
            return;
        }
        resize_to = target;
        resize_from = old;
        if (!modules_changed) detail_flow.rebase();
        content_from = content_height;
        resize_clock = animation_seconds();
        resizing = animate && positioned && IsWindowVisible(hwnd) && !IsIconic(hwnd) && !reduce_motion && !suspended;
        positioned = true;
        if (resizing)
            schedule_animation();
        else {
            scene_progress = 1;
            previous_scene = nullptr;
            apply_frame(resize_to, 1, content_to);
        }
    }
    void animate_frame() {
        const auto now = GetTickCount64();
        const double clock = animation_seconds();
        const double dt = clock - frame_clock;
        frame_clock = clock;
        bool changed = false, layout_frame = resizing;
        RECT frame{};
        float layout_progress = 1;
        int frame_content = content_height;
        if (resizing) {
            float t = static_cast<float>(std::min(1.0, (clock - resize_clock) / .28));
            // Zero endpoint velocity/acceleration avoids a first-frame height jerk.
            float ease = layout_ease(t);
            scene_progress = t;
            auto mix = [ease](LONG a, LONG b) { return a + static_cast<LONG>(std::lround((b - a) * ease)); };
            frame = {mix(resize_from.left, resize_to.left), mix(resize_from.top, resize_to.top),
                     mix(resize_from.right, resize_to.right), mix(resize_from.bottom, resize_to.bottom)};
            layout_progress = ease;
            frame_content = static_cast<int>(std::lround(content_from + (content_to - content_from) * ease));
            resizing = t < 1;
        }
        changed |= segment.advance(dt);
        changed |= multipoint_motion.advance(dt);
        for (int id : {ambient_slider, volume_slider}) {
            auto &c = controls.at(id);
            changed |= c.wake.advance(dt);
            changed |= c.elastic.advance(dt);
            if (c.wheel_due && now >= c.wheel_due) {
                c.wheel_due = 0;
                if (model.device.link == c.wheel_link && connected())
                    send(c.id == volume_slider ? MainAction::volume : MainAction::ambient_level, c.value,
                         true);
            }
        }
        if (layout_frame) {
            apply_frame(frame, layout_progress, frame_content);
            if (!resizing) previous_scene = nullptr;
        } else if (changed)
            request_frame(true);
        // Layout, dispatch callbacks and synchronous native notifications can
        // retarget motion during presentation. Inspect the resulting state.
        bool active = resizing || segment.active() || multipoint_motion.active();
        for (int id : {ambient_slider, volume_slider}) {
            const auto &c = controls.at(id);
            active |= c.wake.active() || c.elastic.active() || c.wheel_due != 0;
        }
        if (!active) {
            stop_animation();
        }
    }
    void put(int id, Box b, std::wstring label, Icon icon, bool enable = true, bool selected = false) {
        auto &c = controls.at(id);
        c.box = b;
        c.icon = icon;
        c.selected = selected;
        c.visible = true;
        c.enabled = enable;
        if (c.label != label)
            c.label = std::move(label);
    }
    void layout() {
        if (controls.empty())
            return;
        for (auto &[id, c] : controls)
            c.visible = false;
        auto caption = L"SonyConnect" + (device_name().empty() ? L"" : L" — " + device_name());
        put(title, {12, 6, std::min(static_cast<float>(width - 94), painter->title_width(caption)), 32},
            caption, Icon::down, !model.preview);
        put(minimize, {static_cast<float>(width - 64), 6, 28, 32}, L"最小化", Icon::minus);
        put(close_button, {static_cast<float>(width - 38), 6, 28, 32}, L"关闭窗口", Icon::close);
        if (!connected()) {
            put(refresh, {304, 76, 32, 32}, L"刷新耳机连接", Icon::refresh,
                !model.discovering && !model.busy && !model.preview);
            put(bluetooth, {24, 136, 312, 40}, L"查看蓝牙", Icon::bluetooth, !model.preview);
            if (control_failed() || model.discovering || !model.discovered.empty())
                put(bluetooth, {24, 136, 312, 40}, model.discovering ? L"正在搜索耳机…" : L"重试连接",
                    Icon::bluetooth, !model.discovering && !model.preview);
            put(releases, {24, 200, 115, 22}, L"软件版本 v" SONYCONNECT_VERSION, Icon::none, !model.preview);
        } else {
            auto &d = model.device;
            put(power, {104, 60, 28, 28}, L"关闭耳机", Icon::power,
                writable() && d.features.contains(protocol::Feature::PowerOff) &&
                    d.features.at(protocol::Feature::PowerOff).writable);
            bool sound = writable() && !sound_unsupported() && d.sound.writable && pending_mode < 0 && !mode_cooling();
            const int mode = visual_mode();
            if (!segment_initialized || reduce_motion || !IsWindowVisible(hwnd)) {
                segment.reset(std::max(0, mode));
                segment_initialized = true;
            } else if (mode >= 0 && segment.target() != mode) {
                segment.set_target(mode);
                schedule_animation();
            }
            put(noise, {28, 136, 132, 44}, L"降噪", Icon::headphones, sound, mode == 0);
            put(ambient, {164, 136, 132, 44}, L"通透", Icon::ear, sound, ambient_mode());
            put(off, {300, 136, 132, 44}, L"关闭", Icon::power, sound, mode == 2);
            const auto level_slot = detail_flow.slot(level_module);
            const auto focus_slot = detail_flow.slot(focus_module);
            if (level_slot.height > .1f) {
                const bool own_level =
                    model.ambient_target.has_value() && connected() && !model.preview && d.sound.writable;
                put(ambient_slider, {24, level_slot.top + 30, 412, 28}, L"环境声音等级", Icon::none,
                    (sound || own_level) && ambient_mode() && d.sound.ambient_level.status != ValueStatus::Unsupported);
                auto &s = controls.at(ambient_slider);
                s.minimum = 1;
                s.maximum = 20;
                auto range =
                    d.sound.ranges.find(d.sound.voice_focus.known() && *d.sound.voice_focus.value ? 1 : 0);
                if (range != d.sound.ranges.end()) {
                    s.minimum = range->second.minimum;
                    s.maximum = range->second.maximum;
                }
                if (!s.dragging)
                    s.value = model.ambient_target.value_or(d.sound.ambient_level.value.value_or(s.minimum));
            }
            if (focus_slot.height > .1f)
                put(focus, {24, focus_slot.top + 3, 412, 25}, L"关注语音", Icon::check,
                    sound && ambient_mode() && d.sound.voice_focus.status != ValueStatus::Unsupported,
                    d.sound.voice_focus.known() && *d.sound.voice_focus.value);
            const auto blocks = sections();
            const bool mp_on = pending_multi.value_or(d.sources.multipoint.value.value_or(false));
            if (!multipoint_initialized || reduce_motion || !IsWindowVisible(hwnd)) {
                multipoint_motion.reset(mp_on);
                multipoint_initialized = true;
            } else if (!switch_dragging && multipoint_motion.target() != mp_on) {
                multipoint_motion.set_target(mp_on);
                schedule_animation();
            }
            put(multi, {382, blocks.sources + 10, 54, 30}, L"同时连接到两台设备", Icon::none,
                writable() && d.sources.multipoint_writable, mp_on);
            for (int i = 0; i < 2; ++i) {
                const auto *s = source_row(i);
                bool active = s && s->active;
                auto label = s ? s->label : L"音源列表待回读";
                auto category = s ? s->category : protocol::SourceCategory::Unknown;
                if (i == 1 && d.sources.multipoint.known() && !*d.sources.multipoint.value && !s)
                    continue;
                put(source0 + i, {24, blocks.sources + 47 + i * 44, 412, 40}, label,
                    category == protocol::SourceCategory::Phone
                        ? Icon::phone
                        : (category == protocol::SourceCategory::Computer ? Icon::computer : Icon::bluetooth),
                    writable() && s && (s->can_switch || s->can_fix), active);
            }
            auto it = d.features.find(protocol::Feature::ConnectionPreference);
            auto value = it != d.features.end() && it->second.value.known() ? *it->second.value.value : -1;
            auto choices = it != d.features.end() ? it->second.choices : std::vector<int>{};
            auto available = [&](int v) {
                return writable() && it != d.features.end() && it->second.writable &&
                       std::find(choices.begin(), choices.end(), v) != choices.end();
            };
            put(quality, {24, blocks.bluetooth + 47, 200, 38}, L"声音质量优先", Icon::music, available(0), value == 0);
            put(stable, {232, blocks.bluetooth + 47, 204, 38}, L"稳定连接优先", Icon::radio, available(1), value == 1);
            bool external = external_source();
            put(media_details, {78, blocks.media + 44, 230, 44},
                (external ? L"移动设备音源" : model.local.media.title.empty() ? L"暂无媒体" : model.local.media.title) +
                    L"\n" + main_media_source(model), Icon::none);
            put(prev, {320, blocks.media + 47, 36, 36}, L"上一首", Icon::previous,
                !model.preview && !external && model.local.media.can_previous);
            put(media, {360, blocks.media + 47, 36, 36}, model.local.media.playing ? L"暂停" : L"播放",
                model.local.media.playing ? Icon::pause : Icon::play,
                !model.preview && !external &&
                    (model.local.media.playing ? model.local.media.can_pause : model.local.media.can_play));
            put(next, {400, blocks.media + 47, 36, 36}, L"下一首", Icon::next,
                !model.preview && !external && model.local.media.can_next);
            put(volume_slider, {54, blocks.media + 126, 352, 32}, L"系统音量", Icon::none,
                !model.preview && model.local.volume.can_write);
            if (!controls.at(volume_slider).dragging)
                controls.at(volume_slider).value = model.volume_target.value_or(
                    std::clamp(static_cast<int>(std::lround(model.local.volume.percent)), 0, 100));
            put(more, {335, blocks.footer + 4, 101, 36}, L"更多", Icon::settings);
            put(releases, {24, blocks.footer + 10, 115, 24}, L"软件版本 v" SONYCONNECT_VERSION, Icon::none,
                !model.preview);
            const auto feedback = status_label();
            if (!feedback.empty())
                put(feedback_status, {217, 97, 219, 26}, feedback, Icon::alert, false);
        }
        auto moves = BeginDeferWindowPos(static_cast<int>(controls.size()));
        if (!moves)
            throw std::runtime_error("Cannot start main window control layout");
        for (auto &[id, c] : controls) {
            float y = c.box.y -
                      (id == title || id == minimize || id == close_button ? 0 : static_cast<float>(scroll));
            bool show = c.visible;
            bool header = id == title || id == minimize || id == close_button;
            if (c.slider) {
                set_slider_target(c);
                if (SendMessageW(c.hwnd, TBM_GETRANGEMIN, 0, 0) != c.minimum ||
                    SendMessageW(c.hwnd, TBM_GETRANGEMAX, 0, 0) != c.maximum)
                    SendMessageW(c.hwnd, TBM_SETRANGE, FALSE, MAKELPARAM(c.minimum, c.maximum));
                if (!c.dragging && SendMessageW(c.hwnd, TBM_GETPOS, 0, 0) != c.value)
                    SendMessageW(c.hwnd, TBM_SETPOS, FALSE, c.value);
            }
            const bool header_control = id == title || id == minimize || id == close_button;
            const bool native_enabled = c.visible && c.enabled && (header_control || !scene_input_locked());
            if (!c.presented_once || c.presented_enabled != native_enabled)
                EnableWindow(c.hwnd, native_enabled);
            const RECT position{px(c.box.x), px(y), px(c.box.x) + px(c.box.w), px(y) + px(c.box.h)};
            if (!c.presented_once || !EqualRect(&position, &c.presented) || show != c.presented_visible) {
                UINT flags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW | SWP_NOCOPYBITS;
                if (!c.presented_once || show != c.presented_visible)
                    flags |= show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
                moves = DeferWindowPos(moves, c.hwnd, nullptr, position.left, position.top,
                                       position.right - position.left, position.bottom - position.top, flags);
                if (!moves)
                    throw std::runtime_error("Cannot position main window controls");
            }
            int clip_bottom = px(c.box.h);
            if (id == ambient_slider || id == focus)
            {
                const auto slot = detail_flow.slot(id == ambient_slider ? level_module : focus_module);
                clip_bottom = std::clamp(px(slot.top + slot.height - c.box.y), 0, clip_bottom);
            }
            const int clip_top = header ? 0 : std::clamp(px(44 - y), 0, clip_bottom);
            const RECT clipping{0, clip_top, px(c.box.w), clip_bottom};
            if (!c.presented_once || !EqualRect(&clipping, &c.presented_clip)) {
                auto clip = CreateRectRgn(0, clip_top, px(c.box.w), clip_bottom);
                if (!SetWindowRgn(c.hwnd, clip, FALSE))
                    DeleteObject(clip);
            }
            c.presented = position;
            c.presented_clip = clipping;
            c.presented_enabled = native_enabled;
            c.presented_visible = show;
            c.presented_once = true;
            auto accessible = c.label + (c.slider ? L"，" + std::to_wstring(c.value) : L"") +
                              (c.selected ? L"，已选中" : L"");
            if (id == source0 || id == source1) {
                const auto *row = source_row(id - source0);
                accessible += L"，" + (row ? row->status : model.source_summary);
            }
            if (accessible != c.accessible) {
                c.accessible = std::move(accessible);
                SetWindowTextW(c.hwnd, c.accessible.c_str());
            }
        }
        if (!EndDeferWindowPos(moves))
            throw std::runtime_error("Cannot finish main window control layout");
        request_frame();
    }
    const MainSourceRow *source_row(int i) const {
        return i >= 0 && static_cast<std::size_t>(i) < model.source_rows.size() ? &model.source_rows[i]
                                                                                : nullptr;
    }
    const protocol::SourceEntry *source_entry(int i) const {
        const auto *row = source_row(i);
        if (!row || row->protocol_index < 0)
            return nullptr;
        int n = 0;
        for (const auto &s : model.device.sources.entries)
            if (s.connection_slot && n++ == row->protocol_index)
                return &s;
        return nullptr;
    }
    bool external_source() const {
        if (!model.device.sources.active_source.known())
            return false;
        for (const auto &s : model.device.sources.entries)
            if (s.id == *model.device.sources.active_source.value)
                return protocol::AudioSourceParameters::category(s.class_of_device) ==
                       protocol::SourceCategory::Phone;
        return false;
    }
    void invalidate_slider_label(Control &c) {
        request_frame();
        c.accessible = c.label + L"，" + std::to_wstring(c.value);
        SetWindowTextW(c.hwnd, c.accessible.c_str());
        NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, c.hwnd, OBJID_CLIENT, CHILDID_SELF);
    }
    void draw_slider(Control &c) {
        auto &p = *painter;
        p.rect({0, 0, c.box.w, c.box.h}, bg);
        if ((c.id == ambient_slider ? model.ambient_slider_style.value_or(model.slider_style)
                                  : model.slider_style) == MainSliderStyle::elastic) {
            const float thickness = static_cast<float>(c.elastic.thickness());
            const float y = (c.box.h - thickness) / 2;
            p.rect({0, y, c.box.w, thickness}, 0xd9deea, thickness / 2);
            const float fill = c.box.w * static_cast<float>(c.elastic.head()) / 100;
            if (fill > 0)
                p.rect({0, y, fill, thickness}, blue, thickness / 2);
            if (keyboard_cues && GetFocus() == c.hwnd) {
                p.brush->SetColor(color(blue));
                p.target->DrawRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(.5f, .5f, c.box.w - .5f, c.box.h - .5f), 5, 5),
                    p.brush.get());
            }
            return;
        }
        const int bars = c.id == ambient_slider ? 20 : 32;
        // Supplied Wake: fixed-height bars; only the fill value is tweened.
        const float gap = 4;
        const float bar_width = (c.box.w - gap * (bars - 1)) / bars;
        const float step = bar_width + gap;
        const float head = static_cast<float>(c.wake.head()) / 100 * (bars - 1);
        const int filled = static_cast<int>(std::round(head));
        if (keyboard_cues && GetFocus() == c.hwnd) {
            p.brush->SetColor(color(blue, .65f));
            p.target->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(.5f, .5f, c.box.w - .5f, c.box.h - .5f), 4, 4), p.brush.get(),
                1);
        }
        for (int k = 0; k < bars; ++k) {
            const float resting_height = c.id == ambient_slider ? 12.f : 14.f;
            float h = static_cast<float>(c.wake.bar_height(k, bars, resting_height * 2, resting_height));
            const float center = c.box.h / 2;
            p.wake_bar({k * step, center - h / 2, bar_width, h},
                       k <= filled && (c.id == ambient_slider || c.value > c.minimum) ? blue : 0xd9deea);
        }
    }
    void draw_segment(float dx, float dy) {
        if (sound_unsupported()) return;
        painter->rect({dx + static_cast<float>(segment.left()), dy,
                       static_cast<float>(segment.right() - segment.left()), 44},
                      0xffffff, 8, 0xd8def7);
    }
    void draw_control(Control &c, bool focused) {
        if (c.id == media_details) {
            if (focused) {
                painter->brush->SetColor(color(blue));
                painter->target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, c.box.w, c.box.h), 3, 3),
                                                       painter->brush.get(), 1);
            }
            return;
        }
        bool mode = c.id == noise || c.id == ambient || c.id == off;
        auto &p = *painter;
        p.rect({0, 0, c.box.w, c.box.h}, mode ? 0xedeff5 : bg);
        unsigned fg = c.enabled || model.preview ? muted : 0xb6bece;
        bool connection = c.id == quality || c.id == stable;
        bool source = c.id == source0 || c.id == source1;
        if (mode && model.device.sound.writable)
            fg = muted;
        // Readback temporarily locks input. It must not fade otherwise available
        // controls or change the appearance of a connected, inactive source.
        if (c.id == power) {
            const auto capability = model.device.features.find(protocol::Feature::PowerOff);
            if (capability != model.device.features.end() && capability->second.writable)
                fg = muted;
        }
        if (source) {
            const auto *row = source_row(c.id - source0);
            if (row && row->connected)
                fg = muted;
        }
        if (connection && (model.busy || model.device.busy)) {
            const auto preference = model.device.features.find(protocol::Feature::ConnectionPreference);
            if (preference != model.device.features.end() && preference->second.writable)
                fg = muted;
        }
        if (mode) {
            draw_segment(28 - c.box.x, 0);
            if (sound_unsupported())
                fg = 0xadb5cb;
            else if (c.selected)
                fg = blue;
        } else if (connection) {
            p.rect({.5f, .5f, c.box.w - 1, c.box.h - 1}, c.selected ? pale : 0xffffff, 7,
                   c.selected ? 0x8d9af0 : border);
            if (c.selected)
                fg = blue;
        } else if (source) {
            if (c.selected) {
                p.rect({0, 0, c.box.w, c.box.h}, pale, 6);
                fg = blue;
            }
        } else if (c.id == bluetooth) {
            p.rect({0, 0, c.box.w, c.box.h}, blue, 8);
            fg = 0xffffff;
        } else if (c.id == more) {
            p.rect({.5f, .5f, c.box.w - 1, c.box.h - 1}, c.hover ? pale : 0xffffff, 7, border);
        } else if (c.id == media) {
            p.circle(c.box.w / 2, c.box.h / 2, 18, c.enabled || model.preview ? blue : 0xc7cde0);
            fg = 0xffffff;
        } else if (c.hover && c.id != feedback_status)
            p.rect({0, 0, c.box.w, c.box.h}, c.id == close_button ? 0xffe8ec : pale, 5);
        if (c.id == title) {
            p.text(c.label, {4, 0, c.box.w - 24, c.box.h}, ink, 13, false, DWRITE_TEXT_ALIGNMENT_LEADING,
                   500);
            p.icon(Icon::down, c.box.w - 18, 10, 12, muted);
        } else if (c.id == minimize || c.id == close_button || c.id == power || c.id == prev ||
                   c.id == media || c.id == next || c.id == refresh)
            p.icon(c.icon, (c.box.w - 18) / 2, (c.box.h - 18) / 2, 18, fg);
        else if (c.id == multi) {
            auto blend = [](unsigned a, unsigned b, double t) {
                unsigned out = 0;
                for (int shift : {0, 8, 16})
                    out |= static_cast<unsigned>(
                               std::lround(((a >> shift) & 255) * (1 - t) + ((b >> shift) & 255) * t))
                           << shift;
                return out;
            };
            p.rect({18, 5, 36, 20}, blend(0xd9deea, blue, multipoint_motion.tint()), 10);
            p.circle(28.f + static_cast<float>(multipoint_motion.position()) * 16, 15, 7, 0xffffff);
        } else if (c.id == focus) {
            p.rect({0, 5, 14, 14}, c.selected ? blue : 0xffffff, 2, c.selected ? blue : border);
            if (c.selected)
                p.icon(Icon::check, 1, 6, 12, 0xffffff);
            p.text(c.label, {23, 0, 100, c.box.h}, ink, 12);
            p.text(L"突出人声，减少背景噪音", {165, 0, 247, c.box.h}, muted, 11, false,
                   DWRITE_TEXT_ALIGNMENT_TRAILING);
        } else if (source) {
            bool fixed =
                model.device.touch.playback_fixed.known() && *model.device.touch.playback_fixed.value;
            if (c.selected && fixed) {
                p.icon(Icon::volume, 12, 12, 12, fg);
                p.rect({20, 20, 8, 8}, pale, 1);
                p.icon(Icon::lock, 20, 20, 8, fg);
            } else {
                p.circle(14, c.box.h / 2, 7, fg, false);
                if (c.selected)
                    p.circle(14, c.box.h / 2, 3, fg);
            }
            p.icon(c.icon, 32, 11, 17, fg);
            p.text(c.label, {57, 0, 215, c.box.h}, fg, 12);
            const auto *row = source_row(c.id - source0);
            p.text(confirming_source == c.id ? L"正在确认…"
                   : hold_source == c.id     ? L"长按固定播放"
                   : row                     ? row->status
                                             : L"待回读",
                   {277, 0, 123, c.box.h}, fg, 11, false, DWRITE_TEXT_ALIGNMENT_TRAILING);
            if (hold_source == c.id || confirming_source == c.id) {
                float progress =
                    confirming_source == c.id ? 1.f : std::min(1.f, (GetTickCount64() - hold_start) / 1000.f);
                p.outline_progress({1.5f, 1.5f, c.box.w - 3, c.box.h - 3}, 5, progress, blue);
            }
        } else if (c.id == releases)
            p.text(c.label, {0, 0, c.box.w, c.box.h}, muted, 11);
        else if (c.id == feedback_status) {
            p.rect({0, 0, c.box.w, c.box.h}, 0xfff2e4, 6);
            p.icon(Icon::alert, 10, 6, 14, 0xb56c25);
            p.text(c.label, {30, 0, c.box.w - 40, c.box.h}, 0xb56c25, 11);
        }
        else if (c.id == more) {
            p.icon(Icon::settings, 14, 10, 16, fg);
            p.text(c.label, {38, 8.5f, 27, 19}, 0x414d63, 13, false, DWRITE_TEXT_ALIGNMENT_LEADING, 500);
            p.icon(Icon::down, 73, 11, 14, fg);
        } else {
            int label_size = mode ? 14 : 12;
            float label_width = static_cast<float>(c.label.size()) * label_size;
            float start = std::max(8.f, (c.box.w - label_width - (c.icon == Icon::none ? 0 : 26)) / 2);
            if (c.icon != Icon::none)
                p.icon(c.icon, start, (c.box.h - 18) / 2, 18, fg);
            p.text(c.label, {start + (c.icon == Icon::none ? 0 : 26), 0, c.box.w - start - 8, c.box.h}, fg,
                   label_size, mode && c.selected, DWRITE_TEXT_ALIGNMENT_LEADING,
                   mode ? (c.selected ? 600 : 500) : 0);
        }
        if (focused && c.id != title && c.id != feedback_status &&
            !(source && (hold_source == c.id || confirming_source == c.id))) {
            p.brush->SetColor(color(blue));
            p.target->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(2, 2, c.box.w - 2, c.box.h - 2), 5, 5), p.brush.get(), 1);
        }
    }
    void render_canvas(int pixel_width, int pixel_height) {
        auto &p = *painter;
        HDC compatible = GetDC(hwnd);
        p.prepare_canvas(compatible, std::max(pixel_width, px(460)),
                         std::max(pixel_height, px(822)));
        ReleaseDC(hwnd, compatible);
        const RECT buffer{0, 0, p.canvas_width, p.canvas_height};
        const auto dc = p.canvas;
        p.begin(dc, buffer, scale());
        // This bitmap is reused. Fully overwrite antialiased corner pixels too,
        // otherwise translucent border coverage accumulates across frames.
        p.target->Clear(D2D1::ColorF(0, 0, 0, 0));
        if (previous_scene && scene_generation != p.generation) {
            previous_scene = nullptr;
            scene_progress = 1;
        }
        p.rect({0, 0, pixel_width / scale(), pixel_height / scale()}, bg, 14, border);
        const float incoming = previous_scene ? layout_ease((scene_progress - .45f) / .55f) : 1;
        const float outgoing = previous_scene ? 1 - layout_ease(scene_progress / .35f) : 0;
        if (incoming < 1)
            p.target->PushLayer(D2D1::LayerParameters(D2D1::RectF(1, 44, width - 1.f, height - 1.f),
                                                     nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                     D2D1::IdentityMatrix(), incoming), p.content_layer.get());
        p.target->PushAxisAlignedClip(D2D1::RectF(1, 44.f, width - 1.f, height - 1.f),
                                      D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        p.target->SetTransform(D2D1::Matrix3x2F::Translation(0, -static_cast<float>(scroll)) *
                               D2D1::Matrix3x2F::Scale(scale(), scale()));
        if (!connected()) {
            p.circle(48, 92, 24, 0xeef0ff);
            if (p.headset_bitmap)
                p.target->DrawBitmap(p.headset_bitmap.get(), D2D1::RectF(29, 73, 67, 111));
            auto heading = model.discovering  ? L"正在连接 Sony 耳机"
                           : control_failed() ? L"耳机控制连接失败"
                                              : L"未连接 Sony 耳机";
            p.text(heading, {86, 67, 212, 26}, ink, 16, true);
            std::wstring desc = model.discovering  ? L"正在建立控制连接…"
                                : control_failed() ? L"音频仍可播放，请重试控制连接"
                                                   : L"请连接耳机后，再返回此处";
            if (!model.feedback.empty() && model.feedback != L"就绪")
                desc = model.feedback;
            p.text(desc, {86, 97, 212, 22}, muted, 12);
            p.text(L"耳机固件 " + (model.device.firmware.empty() ? L"未连接" : wtext(model.device.firmware)),
                   {148, 200, 188, 22}, muted, 11);
        } else {
            auto &d = model.device;
            const auto blocks = sections();
            const auto dsee = d.features.find(protocol::Feature::Dsee);
            const bool dsee_on = dsee != d.features.end() && dsee->second.value.known() &&
                                 *dsee->second.value.value != 0;
            p.circle(28, 74, 3.5f, green);
            p.text(L"已连接", {38, 62, 72, 24}, green, 13, true);
            float battery_text_left{};
            const bool single_battery = d.battery.single.known() ||
                d.capabilities.readable(protocol::Feature::SingleBattery) || d.model.starts_with("WH-");
            if (single_battery) {
                const auto label = percent(d.battery.single);
                const float text_left = 436 - p.text_width(label, 12);
                battery_text_left = text_left - 6 - 24;
                p.headphone_battery({battery_text_left, 66.8f, 24, 14.4f},
                                   d.battery.single, d.battery.single_charging);
                p.text(label, {text_left, 65.5f, 436 - text_left, 17}, 0x667084, 12, false,
                       DWRITE_TEXT_ALIGNMENT_TRAILING);
            } else {
                const auto label = L"L " + percent(d.battery.left) + L"  R " + percent(d.battery.right);
                battery_text_left = 387 - p.text_width(label, 12);
                p.text(label,
                       {277, 65.5f, 110, 17}, 0x667084, 12, false, DWRITE_TEXT_ALIGNMENT_TRAILING);
                // Exact Pen charging-case mark: square lid and lower body with
                // only the two bottom corners rounded; no substitute text glyph.
                p.rect({393, 69.75f, 12, 2.25f}, 0x111111);
                p.rect({393, 73.25f, 12, 6}, 0x111111, 2);
                p.rect({393, 73.25f, 12, 3}, 0x111111);
                p.text(percent(d.battery.case_level), {411, 65.5f, 25, 17}, 0x667084, 12);
            }
            if (dsee_on) {
                const bool extreme = more_control_presentation(d).dsee_label == L"DSEE Extreme";
                const float mark_width = extreme ? 96.f : 96.f * (992.f / 2400.f);
                p.dsee_wordmark({battery_text_left - 8 - mark_width, 69.68f, mark_width, 8.64f}, extreme);
            }
            p.icon(Icon::wave, 24, 100, 18, sound_unsupported() ? 0xadb5cb : muted);
            p.text(L"环境声音控制", {50, 99, 170, 22}, sound_unsupported() ? 0xadb5cb : ink, 14, true);
            if (sound_unsupported())
                p.text(L"设备不支持", {325, 99, 111, 22}, 0xadb5cb, 11, false,
                       DWRITE_TEXT_ALIGNMENT_TRAILING);
            p.rect({24, 132, 412, 52}, 0xedeff5, 10);
            draw_segment(28, 136);
            const auto level_slot = detail_flow.slot(level_module);
            if (level_slot.height > .1f) {
                p.target->PushAxisAlignedClip(D2D1::RectF(24, level_slot.top, 436, level_slot.top + level_slot.height),
                                              D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                p.text(L"环境声音", {24, level_slot.top + 3, 200, 20}, muted, 12);
                p.text(d.sound.ambient_level.known() ? std::to_wstring(controls.at(ambient_slider).value)
                                                     : L"—",
                       {372, level_slot.top + 3, 64, 20}, blue, 13, true, DWRITE_TEXT_ALIGNMENT_TRAILING);
                p.target->PopAxisAlignedClip();
            }
            p.line(24, blocks.sources, 436, blocks.sources, 0xe1e5ed, 1);
            p.icon(Icon::laptop, 24, blocks.sources + 15, 18, muted);
            p.text(L"同时连接到 2 台设备", {49, blocks.sources + 13, 280, 24}, ink, 14, true);
            p.text(model.source_count.empty() ? L"— / 2" : model.source_count, {329, blocks.sources + 13, 48, 24}, muted,
                   11, false, DWRITE_TEXT_ALIGNMENT_TRAILING);
            p.line(24, blocks.bluetooth, 436, blocks.bluetooth, 0xe1e5ed, 1);
            p.icon(Icon::bluetooth, 24, blocks.bluetooth + 16, 16, muted);
            p.text(L"Bluetooth 连接质量", {48, blocks.bluetooth + 13, 250, 24}, ink, 14, true);
            p.text(d.codec.known() ? wtext(*d.codec.value) : L"—", {350, blocks.bluetooth + 13, 86, 24}, muted, 12, false,
                   DWRITE_TEXT_ALIGNMENT_TRAILING);
            p.line(24, blocks.media, 436, blocks.media, 0xe1e5ed, 1);
            p.icon(Icon::music, 24, blocks.media + 16, 18, muted);
            p.text(L"播放控制器", {49, blocks.media + 14, 180, 22}, ink, 14, true);
            p.rect({24, blocks.media + 44, 42, 42}, 0xe9ecfc, 8);
            p.icon(Icon::music, 35, blocks.media + 55, 20, blue);
            bool ext = external_source();
            p.text(ext                               ? L"移动设备音源"
                   : model.local.media.title.empty() ? L"暂无媒体"
                                                     : model.local.media.title,
                   {78, blocks.media + 45, 230, 19}, 0x30384b, 13, false, DWRITE_TEXT_ALIGNMENT_LEADING, 500);
            p.text(main_media_source(model), {78, blocks.media + 68, 230, 17}, muted, 12);
            p.text(L"音量", {24, blocks.media + 97, 30, 20}, muted, 13);
            p.text(L"同步系统音量", {59, blocks.media + 98, 240, 18}, muted, 12);
            p.text(model.local.volume.endpoint_id.empty()
                       ? L"—"
                       : std::to_wstring(controls.at(volume_slider).value) + L"%",
                   {367, blocks.media + 97, 69, 20}, blue, 13, true, DWRITE_TEXT_ALIGNMENT_TRAILING);
            p.icon(Icon::volume_low, 24, blocks.media + 133, 18, 0x606a7d);
            p.icon(Icon::volume, 418, blocks.media + 133, 18, 0x606a7d);
            p.rect({24, blocks.media + 168, 412, 40}, 0xeef1f7, 7);
            p.icon(Icon::wave, 35, blocks.media + 181, 14, muted);
            p.text(model.pressure.empty() ? L"当前音压：—" : model.pressure, {57, blocks.media + 173, 367, 28}, muted,
                   12);
            p.text(L"耳机固件 " + (d.firmware.empty() ? L"—" : wtext(d.firmware)), {148, blocks.footer + 10, 179, 24},
                   muted, 11);
        }
        if (model.preview)
            p.text(L"离线设计预览 · 示例数据", {24, static_cast<float>(content_height - 17), 300, 14},
                   0x929cb0, 9);
        p.target->PopAxisAlignedClip();
        p.target->SetTransform(D2D1::Matrix3x2F::Scale(scale(), scale()));
        if (content_height > height && !resizing) {
            float track = height - 54.f;
            float thumb = track * (height - 44.f) / (content_height - 44.f);
            float y = 48 + scroll * track / (content_height - 44.f);
            p.rect({width - 5.f, y, 2, thumb}, 0xc1c8d8, 1);
        }
        // Native children retain keyboard/UIA/input semantics. All visual content
        // belongs to this one canvas, so moving a hit target cannot copy old pixels.
        for (auto &[id, c] : controls) {
            if (!c.visible)
                continue;
            const bool header = id == title || id == minimize || id == close_button;
            if (header) continue;
            const float y = c.box.y - (header ? 0.f : static_cast<float>(scroll));
            float top = std::max(header ? 0.f : 44.f, y);
            float bottom = std::min(static_cast<float>(height), y + c.box.h);
            if (id == ambient_slider || id == focus)
            {
                const auto slot = detail_flow.slot(id == ambient_slider ? level_module : focus_module);
                bottom = std::min(bottom, slot.top + slot.height - scroll);
            }
            if (top >= bottom)
                continue;
            p.target->PushAxisAlignedClip(D2D1::RectF(c.box.x, top, c.box.x + c.box.w, bottom),
                                          D2D1_ANTIALIAS_MODE_ALIASED);
            p.target->SetTransform(D2D1::Matrix3x2F::Translation(c.box.x, y) *
                                   D2D1::Matrix3x2F::Scale(scale(), scale()));
            if (c.slider)
                draw_slider(c);
            else
                draw_control(c, keyboard_cues && GetFocus() == c.hwnd);
            p.target->SetTransform(D2D1::Matrix3x2F::Scale(scale(), scale()));
            p.target->PopAxisAlignedClip();
        }
        if (incoming < 1) p.target->PopLayer();
        if (previous_scene && outgoing > 0) {
            const float right = std::min(width - 1.f, previous_scene_width - 1);
            const float bottom = std::min(height - 14.f, previous_scene_height - 1);
            if (right > 1 && bottom > 44) {
                const auto area = D2D1::RectF(1, 44, right, bottom);
                const auto source = D2D1::RectF(scale(), 44 * scale(), right * scale(), bottom * scale());
                p.target->DrawBitmap(previous_scene.get(), area, outgoing,
                                      D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, source);
            }
        }
        for (int id : {title, minimize, close_button}) {
            auto &c = controls.at(id);
            p.target->SetTransform(D2D1::Matrix3x2F::Translation(c.box.x, c.box.y) *
                                   D2D1::Matrix3x2F::Scale(scale(), scale()));
            draw_control(c, keyboard_cues && GetFocus() == c.hwnd);
        }
        p.target->SetTransform(D2D1::Matrix3x2F::Scale(scale(), scale()));
        p.end();
        GdiFlush();
        rendered_pixel_width = pixel_width;
        rendered_pixel_height = pixel_height;
    }
    void present(const RECT &bounds) {
        if (presenting) {
            frame_dirty = true;
            return;
        }
        BusyScope presenting_scope(presenting);
        frame_dirty = false;
        SIZE size{bounds.right - bounds.left, bounds.bottom - bounds.top};
        render_canvas(size.cx, size.cy);
        POINT destination{bounds.left, bounds.top}, source{};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        // Submit complete geometry even for same-size frames. The optional-null
        // geometry path retained old screen pixels in the real-screen regression.
        if (!UpdateLayeredWindow(hwnd, nullptr, &destination, &size,
                                 painter->canvas, &source,
                                 0, &blend, ULW_ALPHA))
            throw std::runtime_error("Cannot submit the complete window frame");
        ValidateRect(hwnd, nullptr);
        if (frame_dirty) post_frame();
    }
    void paint(HDC output) {
        RECT r{};
        GetClientRect(hwnd, &r);
        if (!presenting) {
            BusyScope presenting_scope(presenting);
            render_canvas(r.right, r.bottom);
        }
        const auto dc = painter->canvas;
        BitBlt(output, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
    }
    void cancel_interaction(HWND owner = nullptr) {
        // Focus/capture notifications for the previous control may arrive after
        // its successor has already acquired capture. Local cancellation must
        // never release or reset that successor's interaction.
        const bool cancel_hold = hold_source >= 0 &&
                                 (!owner || controls.at(hold_source).hwnd == owner);
        if (cancel_hold)
            request_frame();
        if (cancel_hold || !owner) {
            hold_source = -1;
            KillTimer(hwnd, 4);
        }
        if (switch_dragging && (!owner || controls.at(multi).hwnd == owner)) {
            switch_dragging = false;
            multipoint_motion.set_target(
                pending_multi.value_or(model.device.sources.multipoint.value.value_or(false)));
            schedule_animation();
        }
        std::vector<int> canceled;
        for (auto &[id, c] : controls)
            if ((c.dragging || c.wheel_due) && (!owner || c.hwnd == owner)) {
                c.dragging = false;
                c.wheel_due = 0;
                c.wheel.reset();
                canceled.push_back(id);
                request_frame();
            }
        if (!owner || (drag_id >= 0 && controls.at(drag_id).hwnd == owner))
            drag_id = -1;
        const auto capture = GetCapture();
        if (capture && IsChild(hwnd, capture) && (!owner || capture == owner))
            ReleaseCapture();
        for (const int id : canceled) send(MainAction::cancel_edit, id == volume_slider ? 0 : 1);
    }
    void pause_visuals() {
        tips.dismiss();
        cancel_interaction();
        KillTimer(hwnd, mode_cooldown_timer);
        resizing = false;
        scene_progress = 1;
        previous_scene = nullptr;
        suppress_source_click = -1;
        stop_animation();
    }
    void scroll_to(int value) {
        scroll = std::clamp(value, 0, content_height - height);
        layout();
    }
    bool wheel_slider(WPARAM w, LPARAM l) {
        if (scene_input_locked()) return false;
        if (GET_KEYSTATE_WPARAM(w) & MK_CONTROL)
            return false;
        POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        const HWND hit = WindowFromPoint(point);
        ScreenToClient(hwnd, &point);
        if (hit != hwnd && !IsChild(hwnd, hit))
            return false;
        for (int id : {ambient_slider, volume_slider}) {
            auto &c = controls.at(id);
            if (!c.visible || !c.enabled || c.dragging)
                continue;
            const auto slot = detail_flow.slot(level_module);
            const float top = (id == volume_slider ? sections().media + 96 : slot.top + 3) - scroll;
            RECT region{px(24), std::max(px(44), px(top)), px(436), px(c.box.y + c.box.h - scroll)};
            if (id == ambient_slider)
                region.bottom = std::min<LONG>(region.bottom, px(slot.top + slot.height - scroll));
            if (!PtInRect(&region, point))
                continue;
            const int ticks = c.wheel.consume(GET_WHEEL_DELTA_WPARAM(w));
            const int next_value = std::clamp(c.value + ticks, c.minimum, c.maximum);
            if (next_value != c.value) {
                c.value = next_value;
                c.wheel_due = GetTickCount64() + 200;
                c.wheel_link = model.device.link;
                SendMessageW(c.hwnd, TBM_SETPOS, FALSE, c.value);
                set_slider_target(c);
                invalidate_slider_label(c);
                send(id == volume_slider ? MainAction::volume : MainAction::ambient_level, c.value, false);
                schedule_animation();
            }
            return true;
        }
        return false;
    }
    void click(int id) {
        if (scene_input_locked() && id != title && id != minimize && id != close_button) return;
        if (id == suppress_source_click) {
            suppress_source_click = -1;
            return;
        }
        if (!controls.at(id).enabled)
            return;
        switch (id) {
        case title:
            device_menu();
            break;
        case minimize:
            ShowWindow(hwnd, SW_MINIMIZE);
            break;
        case close_button:
            send(MainAction::close);
            break;
        case power:
            send(MainAction::power_off);
            break;
        case noise:
            select_mode(0);
            break;
        case ambient:
            select_mode(1);
            break;
        case off:
            select_mode(2);
            break;
        case focus:
            send(MainAction::voice_focus, controls.at(id).selected ? 0 : 1);
            break;
        case multi:
            select_multipoint(!controls.at(id).selected);
            break;
        case quality:
            send(MainAction::connection_preference, 0);
            break;
        case stable:
            send(MainAction::connection_preference, 1);
            break;
        case source0:
        case source1:
            if (auto row = source_row(id - source0); row && row->can_switch && row->protocol_index >= 0)
                send(MainAction::source, row->protocol_index);
            break;
        case prev:
            send(MainAction::previous);
            break;
        case media:
            send(MainAction::play_pause);
            break;
        case next:
            send(MainAction::next);
            break;
        case more:
            send(MainAction::more);
            break;
        case releases:
            send(MainAction::releases);
            break;
        case media_details:
            tips.inspect(hwnd, controls.at(media_details).hwnd);
            break;
        case refresh:
            send(MainAction::discover);
            break;
        case bluetooth:
            if (control_failed() || !model.discovered.empty())
                send(MainAction::discover);
            else {
                send(MainAction::bluetooth_settings);
                send(MainAction::discover);
            }
            break;
        default:
            break;
        }
    }
    void device_menu() {
        const auto discovered = model.discovered;
        auto menu = CreatePopupMenu();
        for (std::size_t i = 0; i < discovered.size(); ++i) {
            auto name = wtext(discovered[i].name);
            AppendMenuW(menu, MF_STRING, 100 + static_cast<UINT_PTR>(i), name.c_str());
        }
        if (model.discovered.empty())
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"暂无已连接的 Sony 耳机");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 1, L"重新搜索已连接耳机");
        AppendMenuW(menu, MF_STRING, 2, L"打开蓝牙设置");
        RECT r{};
        GetWindowRect(controls.at(title).hwnd, &r);
        int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN, r.left, r.bottom, 0, hwnd, nullptr);
        DestroyMenu(menu);
        if (choice >= 100 && static_cast<std::size_t>(choice - 100) < discovered.size())
            send(MainAction::select_device, 0, true, discovered[choice - 100].address);
        else if (choice == 1)
            send(MainAction::discover);
        else if (choice == 2)
            send(MainAction::bluetooth_settings);
    }
    void style_menu() {
        const auto menu = CreatePopupMenu();
        const auto item = [&](UINT id, const wchar_t *name, MainSliderStyle style) {
            AppendMenuW(menu,
                        MF_STRING | (model.slider_style == style ? MF_CHECKED : 0) |
                            (model.preview ? MF_GRAYED : 0),
                        id, name);
        };
        item(1, L"条形滑条（Wake）", MainSliderStyle::wake);
        item(2, L"连续滑条（Elastic）", MainSliderStyle::elastic);
        RECT bounds{};
        GetWindowRect(controls.at(more).hwnd, &bounds);
        const int choice =
            TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN, bounds.right, bounds.top, 0, hwnd, nullptr);
        DestroyMenu(menu);
        if (choice >= 1 && choice <= 2)
            send(MainAction::slider_style, choice - 1);
    }
    void source_context(int id) {
        const auto *entry = source_entry(id - source0);
        if (!entry || !model.device.sources.active_source.known() ||
            entry->id != *model.device.sources.active_source.value)
            return;
        auto menu = CreatePopupMenu();
        bool fixed = model.device.touch.playback_fixed.known() && *model.device.touch.playback_fixed.value;
        AppendMenuW(menu, MF_STRING | (!writable() || !model.device.touch.fixation_writable ? MF_GRAYED : 0),
                    1, fixed ? L"解除固定播放" : L"按住当前音源 1 秒可固定播放");
        POINT pt{};
        GetCursorPos(&pt);
        auto choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
        if (choice == 1 && fixed)
            send(MainAction::release_source);
    }
    static LRESULT CALLBACK control_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
        auto self = reinterpret_cast<Impl *>(data);
        try {
            return self->control_message(h, m, w, l, id);
        } catch (const winrt::hresult_error &e) {
            self->report_error(self->hwnd, m, e.message().c_str());
        } catch (const std::exception &e) {
            self->report_error(self->hwnd, m, wtext(e.what()));
        }
        return 0;
    }
    LRESULT control_message(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id) {
        auto self = this;
        auto &c = self->controls.at(static_cast<int>(id));
        if (m == WM_KEYDOWN && w == VK_F1) {
            tips.inspect(hwnd, h);
            return 0;
        }
        if (m == WM_ERASEBKGND)
            return 1;
        if (m == WM_PAINT) {
            ValidateRect(h, nullptr);
            return 0;
        }
        if (m == WM_PRINTCLIENT || m == WM_PRINT)
            return 0; // Parent's canvas already contains this control.
        if (c.id == feedback_status) {
            if (m == WM_NCHITTEST)
                return HTTRANSPARENT;
            if (m == WM_LBUTTONDOWN || m == WM_LBUTTONUP || m == WM_KEYDOWN || m == WM_SETFOCUS)
                return 0;
        }
        if (m == WM_LBUTTONDOWN)
            keyboard_cues = false;
        if (m == WM_KEYDOWN)
            keyboard_cues = true;
        if (m == WM_MOUSEMOVE && !c.hover) {
            c.hover = true;
            TRACKMOUSEEVENT tr{sizeof(tr), TME_LEAVE, h, 0};
            TrackMouseEvent(&tr);
            request_frame();
            if (c.slider) {
                c.elastic.set_active(true);
                if (reduce_motion)
                    c.elastic.advance(1);
                else
                    schedule_animation();
            }
        }
        if (m == WM_MOUSELEAVE) {
            c.hover = false;
            request_frame();
            if (c.slider) {
                c.elastic.set_active(c.dragging || (keyboard_cues && GetFocus() == h));
                if (reduce_motion)
                    c.elastic.advance(1);
                else
                    schedule_animation();
            }
            if (self->hold_source == static_cast<int>(id))
                self->cancel_interaction(h);
        }
        if (m == WM_MOUSEMOVE && hold_source == static_cast<int>(id)) {
            RECT bounds{};
            GetClientRect(h, &bounds);
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (!PtInRect(&bounds, pt))
                cancel_interaction(h);
        }
        if (m == WM_SETFOCUS) {
            int y = self->px(c.box.y - static_cast<float>(self->scroll)), bottom = y + self->px(c.box.h);
            if (id != title && id != minimize && id != close_button) {
                if (y < self->px(44))
                    self->scroll_to(static_cast<int>(c.box.y) - 48);
                else if (bottom > self->px(static_cast<float>(self->height)))
                    self->scroll_to(static_cast<int>(c.box.y + c.box.h) - self->height + 8);
            }
            request_frame();
        }
        if (m == WM_KILLFOCUS || m == WM_CANCELMODE) {
            self->cancel_interaction(h);
            request_frame();
        }
        if (m == WM_MOUSEWHEEL) {
            SendMessageW(self->hwnd, m, w, l);
            return 0;
        }
        if (id == more && m == WM_CONTEXTMENU) {
            style_menu();
            return 0;
        }
        if (m == WM_KEYDOWN && w == VK_ESCAPE) {
            self->cancel_interaction();
            if (GetCapture() == h)
                ReleaseCapture();
            return 0;
        }
        if (id == multi) {
            if (m == WM_LBUTTONDOWN && c.enabled) {
                SetFocus(h);
                switch_dragging = true;
                switch_moved = false;
                switch_origin = GET_X_LPARAM(l) / scale();
                switch_position = static_cast<float>(multipoint_motion.position());
                interaction_link = model.device.link;
                SetCapture(h);
                return 0;
            }
            if (m == WM_MOUSEMOVE && switch_dragging) {
                const float delta = GET_X_LPARAM(l) / scale() - switch_origin;
                if (std::abs(delta) > 4)
                    switch_moved = true;
                if (switch_moved) {
                    multipoint_motion.drag(switch_position + delta / 16.f);
                    request_frame();
                }
                return 0;
            }
            if (m == WM_LBUTTONUP && switch_dragging) {
                const bool on = switch_moved ? multipoint_motion.position() >= .5 : !c.selected;
                switch_dragging = false;
                ReleaseCapture();
                if (interaction_link == model.device.link && c.enabled && on != c.selected)
                    select_multipoint(on);
                else {
                    multipoint_motion.set_target(c.selected);
                    schedule_animation();
                }
                return 0;
            }
            if (m == WM_CAPTURECHANGED && switch_dragging) {
                cancel_interaction(h);
                return 0;
            }
        }
        if (c.slider) {
            if (m == WM_LBUTTONDOWN && c.enabled) {
                SetFocus(h);
                c.dragging = true;
                self->interaction_link = self->model.device.link;
                SetCapture(h);
            }
            if ((m == WM_LBUTTONDOWN || m == WM_MOUSEMOVE) && c.dragging) {
                c.value =
                    std::clamp(c.minimum + static_cast<int>(std::round(GET_X_LPARAM(l) / self->scale() /
                                                                       c.box.w * (c.maximum - c.minimum))),
                               c.minimum, c.maximum);
                SendMessageW(h, TBM_SETPOS, FALSE, c.value);
                self->set_slider_target(c);
                request_frame();
                self->invalidate_slider_label(c);
                if (c.id == volume_slider)
                    self->send(MainAction::volume, c.value, false);
                return 0;
            }
            if (m == WM_LBUTTONUP && c.dragging) {
                c.dragging = false;
                ReleaseCapture();
                if (self->interaction_link == self->model.device.link)
                    self->send(c.id == ambient_slider ? MainAction::ambient_level : MainAction::volume,
                               c.value, true);
                return 0;
            }
            if (m == WM_CAPTURECHANGED && c.dragging) {
                c.dragging = false;
                self->layout();
                return 0;
            }
            if (m == WM_KEYDOWN && c.enabled) {
                int v = c.value;
                if (w == VK_LEFT || w == VK_DOWN)
                    --v;
                else if (w == VK_RIGHT || w == VK_UP)
                    ++v;
                else if (w == VK_HOME)
                    v = c.minimum;
                else if (w == VK_END)
                    v = c.maximum;
                else if (w == VK_PRIOR)
                    v += 5;
                else if (w == VK_NEXT)
                    v -= 5;
                else
                    return DefSubclassProc(h, m, w, l);
                c.value = std::clamp(v, c.minimum, c.maximum);
                SendMessageW(h, TBM_SETPOS, FALSE, c.value);
                self->set_slider_target(c);
                self->invalidate_slider_label(c);
                self->send(c.id == ambient_slider ? MainAction::ambient_level : MainAction::volume, c.value,
                           true);
                request_frame();
                return 0;
            }
        }
        if ((id == source0 || id == source1) && c.enabled) {
            if (m == WM_CONTEXTMENU) {
                self->source_context(static_cast<int>(id));
                return 0;
            }
            if (m == WM_LBUTTONDOWN || (m == WM_KEYDOWN && w == VK_SPACE && !(l & (1ll << 30)))) {
                self->suppress_source_click = -1;
                // Focus loss on the previous control cancels its interaction before
                // this hold starts, rather than cancelling the newly started hold.
                SetFocus(h);
                const auto *s = self->source_entry(static_cast<int>(id) - source0);
                if (s && self->model.device.sources.active_source.known() &&
                    s->id == *self->model.device.sources.active_source.value &&
                    self->model.device.touch.fixation_writable &&
                    !(self->model.device.touch.playback_fixed.known() &&
                      *self->model.device.touch.playback_fixed.value)) {
                    self->hold_source = static_cast<int>(id);
                    self->hold_start = GetTickCount64();
                    self->interaction_link = self->model.device.link;
                    SetTimer(self->hwnd, 4, 30, nullptr);
                }
            }
            if (m == WM_LBUTTONUP || m == WM_KEYUP) {
                // BUTTON must finish its release and emit BN_CLICKED before
                // cleanup. Releasing capture here first silently eats source clicks.
                const auto result = DefSubclassProc(h, m, w, l);
                self->cancel_interaction(h);
                return result;
            }
            if (m == WM_CAPTURECHANGED)
                self->cancel_interaction(h);
        }
        if (m == WM_KEYDOWN && w == VK_RETURN && !c.slider) {
            self->click(static_cast<int>(id));
            return 0;
        }
        if (m == WM_KILLFOCUS)
            request_frame();
        return DefSubclassProc(h, m, w, l);
    }
    LRESULT message(UINT m, WPARAM w, LPARAM l) {
        switch (m) {
        case WM_NOTIFY:
            if (tips.notify(l)) return 0;
            break;
        case WM_SHOWWINDOW:
            if (!w) pause_visuals();
            break;
        case WM_SIZE:
            if (w == SIZE_MINIMIZED) pause_visuals();
            break;
        case WM_POWERBROADCAST:
            if (w == PBT_APMSUSPEND) { suspended = true; pause_visuals(); }
            else if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) {
                suspended = false;
                if (IsWindowVisible(hwnd)) resume_mode_cooldown();
                resize(false);
            }
            return TRUE;
        case WM_PAINT: {
            PaintScope painting(hwnd);
            request_frame();
            return 0;
        }
        case frame_message:
            frame_posted = false;
            flush_frame();
            return 0;
        case repaint_message:
            request_frame();
            return 0;
        case WM_PRINTCLIENT:
            paint(reinterpret_cast<HDC>(w));
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DRAWITEM: {
            // Input HWNDs never draw pixels. Requesting another parent paint
            // here feeds native redraw notifications back into layered updates.
            return TRUE;
        }
        case WM_COMMAND:
            if (HIWORD(w) == BN_CLICKED && controls.contains(LOWORD(w)))
                click(LOWORD(w));
            return 0;
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ScreenToClient(hwnd, &pt);
            return pt.y < px(44) ? HTCAPTION : HTCLIENT;
        }
        case WM_LBUTTONDOWN:
            if (GET_Y_LPARAM(l) < px(44)) {
                ReleaseCapture();
                SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            }
            return 0;
        case WM_NCLBUTTONDBLCLK:
        case WM_LBUTTONDBLCLK:
            return 0;
        case WM_SYSCOMMAND:
            if ((w & 0xfff0) == SC_MAXIMIZE || (w & 0xfff0) == SC_SIZE)
                return 0;
            break;
        case WM_GETMINMAXINFO: {
            auto mi = reinterpret_cast<MINMAXINFO *>(l);
            mi->ptMinTrackSize =
                mi->ptMaxTrackSize = {px(static_cast<float>(width)), px(static_cast<float>(height))};
            return 0;
        }
        case WM_DPICHANGED: {
            cancel_interaction();
            tips.dismiss();
            if (!forced_dpi)
                dpi = HIWORD(w);
            auto r = reinterpret_cast<RECT *>(l);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
            update_icons();
            resize(false);
            return 0;
        }
        case WM_DISPLAYCHANGE:
            cancel_interaction();
            tips.dismiss();
            resize(false);
            return 0;
        case WM_SETTINGCHANGE:
            read_motion_preference();
            if (reduce_motion) {
                segment.reset(std::max(0, visual_mode()));
                for (int id : {ambient_slider, volume_slider})
                    set_slider_target(controls.at(id));
            }
            resize(false);
            return 0;
        case WM_ENTERSIZEMOVE:
            if (resizing) {
                resizing = false;
                scene_progress = 1;
                previous_scene = nullptr;
                apply_frame(resize_to, 1, content_to);
            }
            return 0;
        case WM_MOUSEWHEEL:
            if (wheel_slider(w, l))
                return 0;
            if (GET_KEYSTATE_WPARAM(w) & MK_CONTROL)
                return 0;
            cancel_interaction();
            scroll_to(scroll - scroll_wheel.consume(GET_WHEEL_DELTA_WPARAM(w)) * 48);
            return 0;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE)
                cancel_interaction();
            return 0;
        case WM_CANCELMODE:
        case WM_ACTIVATEAPP:
            if (m == WM_CANCELMODE || !w)
                cancel_interaction();
            break;
        case WM_TIMER:
            if (w == mode_cooldown_timer) {
                if (mode_cooling()) return 0;
                reset_mode_cooldown();
                layout();
                return 0;
            }
            if (w == 5) {
                animate_frame();
                return 0;
            }
            if (w == 4 && hold_source >= 0) {
                const auto *s = source_entry(hold_source - source0);
                if (!s || model.device.link != interaction_link || !writable()) {
                    cancel_interaction();
                    return 0;
                }
                if (GetTickCount64() - hold_start >= 1000) {
                    const auto *row = source_row(hold_source - source0);
                    const int i = row ? row->protocol_index : -1;
                    suppress_source_click = hold_source;
                    confirming_source = hold_source;
                    cancel_interaction();
                    request_frame(true);
                    if (i >= 0)
                        send(MainAction::fix_source, i);
                } else
                    request_frame();
                return 0;
            }
            break;
        case WM_CLOSE:
            send(MainAction::close);
            return 0;
        case WM_DESTROY:
            reset_mode_cooldown();
            cancel_interaction();
            stop_animation();
            return 0;
        case WM_NCDESTROY: {
            auto result = DefWindowProcW(hwnd, m, w, l);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            hwnd = nullptr;
            return result;
        }
        default:
            break;
        }
        return DefWindowProcW(hwnd, m, w, l);
    }
};
MainWindow::MainWindow(Dispatch d) : impl_(std::make_unique<Impl>(std::move(d))) {}
MainWindow::~MainWindow() = default;
TrayAction MainWindow::tray_menu(HWND owner) {
    // Native menu tracking provides keyboard navigation, accessibility and
    // outside-click dismissal; only its rows use the existing Pen renderer.
    struct Menu {
        struct Row { TrayAction action; std::wstring text; PenIcon icon; bool enabled{true}, checked{}; };
        std::vector<Row> rows;
        Painter painter;
        HMENU handle{CreatePopupMenu()};
        HBRUSH background{CreateSolidBrush(RGB(248, 249, 252))};
        HWND owner{};
        float scale{1};
        bool failed{}, subclassed{};
        ~Menu() {
            if (subclassed) RemoveWindowSubclass(owner, procedure, 37);
            if (handle) DestroyMenu(handle);
            if (background) DeleteObject(background);
        }
        static LRESULT CALLBACK procedure(HWND hwnd, UINT msg, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
            auto &self = *reinterpret_cast<Menu *>(data);
            try {
                if (msg == WM_MEASUREITEM && !w) {
                    auto &item = *reinterpret_cast<MEASUREITEMSTRUCT *>(l);
                    if (item.CtlType == ODT_MENU && item.itemData && item.itemData <= self.rows.size()) {
                        const auto index = item.itemData - 1;
                        item.itemWidth = static_cast<UINT>(std::lround(272 * self.scale));
                        item.itemHeight = static_cast<UINT>(std::lround((index == 0 ? 46 : index == self.rows.size() - 1 ? 34 : 39) * self.scale));
                        return TRUE;
                    }
                }
                if (msg == WM_DRAWITEM && !w) {
                    auto &item = *reinterpret_cast<DRAWITEMSTRUCT *>(l);
                    if (item.CtlType == ODT_MENU && reinterpret_cast<HMENU>(item.hwndItem) == self.handle &&
                        item.itemData && item.itemData <= self.rows.size()) {
                        const auto index = item.itemData - 1;
                        const auto &row = self.rows[index];
                        auto &p = self.painter;
                        const float width = (item.rcItem.right - item.rcItem.left) / self.scale;
                        const float height = (item.rcItem.bottom - item.rcItem.top) / self.scale;
                        p.begin(item.hDC, item.rcItem, self.scale);
                        p.target->Clear(color(bg));
                        if (row.enabled && (item.itemState & ODS_SELECTED))
                            p.rect({0, 1, width, height - 3}, pale, 6);
                        const auto tint = row.action == TrayAction::none ? muted : row.enabled ? 0x414d63 : 0xadb5cb;
                        if (index == self.rows.size() - 1) {
                            p.line(0, 0, width, 0, 0xe1e5ed, 1);
                            p.text(row.text, {12, 4, width - 24, height - 4}, muted, 11);
                        } else {
                            p.pen_icons->draw(p.target.get(), p.brush.get(), row.icon, 12, (height - 16) / 2, 16,
                                             color(index == 0 || row.enabled ? blue : tint));
                            p.text(row.text, {38, 0, width - 66, height}, tint, 12);
                            if (row.checked) p.icon(Icon::check, width - 27, (height - 16) / 2, 16, blue);
                        }
                        p.end();
                        return TRUE;
                    }
                }
            } catch (...) { self.failed = true; EndMenu(); }
            return DefSubclassProc(hwnd, msg, w, l);
        }
    } menu;
    menu.owner = owner;
    menu.scale = GetDpiForWindow(owner) / 96.f * interface_scale;
    const auto &device = impl_->model.device;
    const bool sound = impl_->writable() && !impl_->sound_unsupported() && device.sound.writable &&
                       impl_->pending_mode < 0 && !impl_->mode_cooling();
    const int mode = impl_->visual_mode();
    menu.rows = {
        {TrayAction::none, device.ready ? wtext(device.model) + L" · 已连接" : L"未连接 Sony 耳机", PenIcon::headphones, false},
        {TrayAction::open, L"打开 SonyConnect", PenIcon::panel_top},
        {TrayAction::more, L"更多功能", PenIcon::settings_2},
        {TrayAction::personalization, L"个性化设置", PenIcon::sliders_horizontal},
        {TrayAction::noise, L"降噪", PenIcon::headphones, sound, device.ready && mode == 0},
        {TrayAction::ambient, L"通透", PenIcon::ear, sound, device.ready && mode == 1},
        {TrayAction::off, L"关闭", PenIcon::power, sound, device.ready && mode == 2},
        {TrayAction::quit, L"退出 SonyConnect", PenIcon::log_out},
        {TrayAction::none, L"SonyConnect v" SONYCONNECT_VERSION, PenIcon::none, false}
    };
    if (!menu.handle || !menu.background) throw std::runtime_error("Cannot create tray menu");
    MENUINFO style{sizeof(style)};
    style.fMask = MIM_BACKGROUND | MIM_STYLE;
    style.hbrBack = menu.background;
    style.dwStyle = MNS_NOCHECK;
    if (!SetMenuInfo(menu.handle, &style)) throw std::runtime_error("Cannot style tray menu");
    for (std::size_t i = 0; i < menu.rows.size(); ++i) {
        auto &row = menu.rows[i];
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_FTYPE | MIIM_STRING | MIIM_DATA | MIIM_ID | MIIM_STATE;
        item.fType = MFT_OWNERDRAW;
        item.wID = static_cast<UINT>(row.action);
        item.dwItemData = i + 1;
        item.dwTypeData = row.text.data();
        item.fState = row.enabled ? MFS_ENABLED : MFS_DISABLED;
        if (row.checked) item.fState |= MFS_CHECKED;
        if (!InsertMenuItemW(menu.handle, static_cast<UINT>(i), TRUE, &item))
            throw std::runtime_error("Cannot create tray menu row");
    }
    menu.subclassed = SetWindowSubclass(owner, Menu::procedure, 37, reinterpret_cast<DWORD_PTR>(&menu)) != FALSE;
    if (!menu.subclassed) throw std::runtime_error("Cannot attach tray menu renderer");
    POINT at{};
    GetCursorPos(&at);
    SetForegroundWindow(owner);
    const auto action = static_cast<TrayAction>(TrackPopupMenu(menu.handle,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_RIGHTALIGN | TPM_BOTTOMALIGN, at.x, at.y, 0, owner, nullptr));
    PostMessageW(owner, WM_NULL, 0, 0);
    if (menu.failed) throw std::runtime_error("Cannot render tray menu");
    if (action >= TrayAction::noise && action <= TrayAction::off)
        impl_->select_mode(static_cast<int>(action) - static_cast<int>(TrayAction::noise));
    return action;
}
void MainWindow::create(HINSTANCE instance, UINT dpi) { impl_->create(instance, dpi); }
HWND MainWindow::handle() const { return impl_->hwnd; }
void MainWindow::update(MainModel m) {
    auto &s = *impl_;
    std::wostringstream digest;
    digest << m.device.revision << L":" << m.device.link << L":" << m.device.ready << L":"
           << m.device.connected << L":" << m.busy << L":" << m.device.busy << L":" << m.discovering << L":"
           << m.preview << L":" << m.feedback_error << L":" << m.feedback << L":" << m.pressure << L":"
           << m.local.volume.percent << L":" << m.local.volume.can_write << L":" << m.local.volume.muted
           << L":" << m.local.volume.endpoint_id << L":" << m.local.media.session_id << L":"
           << m.local.media.title << L":" << m.local.media.application_id << L":" << m.local.media.playing
           << L":" << m.local.media.can_previous << m.local.media.can_next << m.local.media.can_play
           << m.local.media.can_pause;
    for (const auto &d : m.discovered)
        digest << L":" << d.address << L":" << wtext(d.name);
    digest << L":" << m.source_count << L":" << m.source_summary << L":" << m.operation_status << L":"
           << m.volume_target.value_or(-1) << L":" << m.ambient_target.value_or(-1) << L":"
           << static_cast<int>(m.slider_style) << static_cast<int>(m.ambient_slider_style.value_or(m.slider_style));
    for (const auto &row : m.source_rows)
        digest << L":" << row.label << L":" << row.status << L":" << row.protocol_index << L":" << row.active
               << row.connected << row.can_switch << row.can_fix << row.windows_endpoint;
    auto signature = digest.str();
    if (signature == s.signature)
        return;
    s.signature = std::move(signature);
    const bool old_ready = s.connected();
    if (old_ready != m.device.ready) {
        s.remember_scene();
        s.scroll = 0;
    }
    if (s.model.device.link != m.device.link || (!m.device.ready && s.model.device.ready)) {
        s.reset_mode_cooldown();
        s.cancel_interaction();
        s.confirming_source = -1;
        s.suppress_source_click = -1;
        s.pending_mode = -1;
        s.pending_multi.reset();
        for (auto &[id, c] : s.controls) {
            c.wheel.reset();
            c.wheel_due = 0;
        }
    }
    s.model = std::move(m);
    if (!s.model.busy && !s.model.device.busy) {
        s.pending_mode = -1;
        s.pending_multi.reset();
        s.confirming_source = -1;
    }
    s.resize();
}
void MainWindow::show(bool activate) {
    if (!IsWindowVisible(impl_->hwnd)) impl_->resize(false);
    impl_->resume_mode_cooldown();
    impl_->request_frame();
    ShowWindow(impl_->hwnd, activate ? SW_RESTORE : SW_SHOWNOACTIVATE);
    if (activate)
        SetForegroundWindow(impl_->hwnd);
}
void MainWindow::hide() {
    ShowWindow(impl_->hwnd, SW_HIDE);
    impl_->pause_visuals();
    impl_->resize(false);
}
bool MainWindow::pre_translate(MSG &m) {
    if (m.message == WM_KEYDOWN && m.wParam == VK_F1) {
        impl_->tips.inspect(impl_->hwnd, m.hwnd);
        return true;
    }
    if (m.message == WM_KEYDOWN && m.wParam == VK_TAB) {
        impl_->keyboard_cues = true;
        impl_->request_frame();
    }
    return IsDialogMessageW(impl_->hwnd, &m) != FALSE;
}
void MainWindow::capture(const std::filesystem::path &path) {
    auto &s = *impl_;
    RECT r{};
    GetClientRect(s.hwnd, &r);
    HDC dc = GetDC(s.hwnd), mem = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, r.right, r.bottom);
    auto old = SelectObject(mem, bitmap);
    SendMessageW(s.hwnd, WM_PRINT, reinterpret_cast<WPARAM>(mem), PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    // Direct2D's DC target copies through batched GDI. Complete those writes
    // and release the bitmap from its DC before WIC reads the native bitmap.
    GdiFlush();
    SelectObject(mem, old);
    winrt::com_ptr<IWICImagingFactory> factory;
    winrt::com_ptr<IWICBitmap> image;
    winrt::com_ptr<IWICStream> stream;
    winrt::com_ptr<IWICBitmapEncoder> encoder;
    winrt::com_ptr<IWICBitmapFrameEncode> frame;
    HRESULT hr =
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put()));
    if (SUCCEEDED(hr))
        hr = factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, image.put());
    if (SUCCEEDED(hr))
        hr = factory->CreateStream(stream.put());
    if (SUCCEEDED(hr))
        hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr))
        hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put());
    if (SUCCEEDED(hr))
        hr = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr))
        hr = encoder->CreateNewFrame(frame.put(), nullptr);
    if (SUCCEEDED(hr))
        hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr))
        hr = frame->WriteSource(image.get(), nullptr);
    if (SUCCEEDED(hr))
        hr = frame->Commit();
    if (SUCCEEDED(hr))
        hr = encoder->Commit();
    DeleteObject(bitmap);
    DeleteDC(mem);
    ReleaseDC(s.hwnd, dc);
    winrt::check_hresult(hr);
}
MainModel main_preview(const std::wstring &name) {
    MainModel m;
    m.preview = true;
    if (name == L"elastic")
        m.slider_style = MainSliderStyle::elastic;
    if (name == L"disconnected")
        return m;
    if (name == L"connecting") {
        m.discovering = true;
        return m;
    }
    if (name == L"failed") {
        m.device.connected = true;
        m.feedback = L"音频仍可播放，请重试控制连接";
        return m;
    }
    auto &d = m.device;
    d.connected = d.ready = true;
    d.link = 1;
    d.revision = 1;
    d.model = name == L"single" ? "WH-1000XM5" : "WF-1000XM5";
    d.firmware = "X.X.X";
    d.sound.mode =
        known(name == L"ambient" ? protocol::SoundMode::Ambient : protocol::SoundMode::NoiseCanceling);
    d.sound.ambient_level = known(12);
    d.sound.voice_focus = known(true);
    d.sound.writable = true;
    if (name == L"single")
        d.battery.single = known(80);
    else {
        d.battery.left = known(100);
        d.battery.right = known(100);
        d.battery.case_level = known(80);
    }
    d.sources.available = true;
    d.sources.multipoint = known(true);
    d.sources.multipoint_writable = d.sources.switch_writable = true;
    d.sources.entries = {{"preview-pc", "PC 设备", 1, 0x100}, {"preview-phone", "移动设备", 2, 0x200}};
    d.sources.active_source = known(std::string("preview-pc"));
    d.codec = known(std::string("AAC"));
    d.touch.fixation_writable = true;
    d.touch.playback_fixed = known(name == L"fixed");
    d.features[protocol::Feature::ConnectionPreference].value = known(1);
    d.features[protocol::Feature::ConnectionPreference].choices = {0, 1};
    d.features[protocol::Feature::ConnectionPreference].writable = true;
    d.features[protocol::Feature::PowerOff].writable = true;
    m.local.volume.endpoint_id = L"preview";
    m.local.volume.percent = 40;
    m.local.volume.can_write = true;
    m.local.media.title = L"音乐标题名称";
    m.local.media.application_id = L"媒体来源";
    m.local.media.playing = true;
    m.local.media.can_pause = m.local.media.can_previous = m.local.media.can_next = true;
    m.pressure = L"当前音压：41 dB";
    project_main_sources(m);
    return m;
}
ReplayText replay_text(const desktop::DiagnosticReplayEntry &source) {
    const auto &e = source.entry;
    ReplayText r;
    r.time = wtext(source.timestamp);
    if (r.time.size() >= 23 && r.time[10] == L'T') r.time = r.time.substr(11, 12);
    r.title = wtext(e.event.empty() ? "未命名事件" : e.event);
    for (const auto &[event, label] : std::initializer_list<std::pair<const char *, const wchar_t *>>{
             {"connected", L"控制连接建立"}, {"disconnected", L"控制连接断开"},
             {"confirmed", L"设置已确认"}, {"rejected", L"设置被拒绝"},
             {"command-failed", L"设置写入失败"}, {"diagnostic-options-updated", L"诊断限制已更新"}})
        if (e.event == event) r.title = label;
    const bool explicit_error = !e.error_code.empty() && e.error_code != "None" && e.error_code != "none" &&
                                e.error_code != "0" && e.error_code != "Ok" && e.error_code != "Success";
    r.error = explicit_error || e.event == "command-failed" || e.event == "rejected" || e.status == "Failed";
    std::wostringstream details;
    const auto add = [&](const wchar_t *label, const std::string &value) {
        if (!value.empty()) details << label << L"：" << wtext(value) << L"  ";
    };
    add(L"时间", source.timestamp); add(L"事件", e.event); add(L"来源", e.origin);
    add(L"状态", e.status); add(L"阶段", e.phase); add(L"错误", e.error_code);
    add(L"型号", e.model); add(L"固件", e.firmware); add(L"参数", e.parameter);
    add(L"请求", e.request_id); add(L"连接", e.link_id); add(L"选择", e.selection_id);
    const auto number = [&](const wchar_t *label, const auto &value) {
        if (value) details << label << L"：" << *value << L"  ";
    };
    number(L"请求值", e.requested_value); number(L"确认值", e.confirmed_value);
    number(L"代次", e.generation); number(L"接收顺序", e.receive_order);
    number(L"类型", e.type); number(L"序号", e.sequence); number(L"命令", e.command); number(L"字节数", e.payload_bytes);
    r.detail = details.str();
    return r;
}
// The second UI layer shares the established canvas, text cache and exact Pen
// geometry. It owns its HWND/input state; device work stays in Window's adapter.
struct MoreWindow::Impl {
    enum Id {
        close_id = 1000,
        preset_id,
        band_id = 1010,
        bass_id = 1015,
        dsee_id = 1020,
        left_id,
        right_id,
        cycle_id = 1030,
        guidance_id = 1040,
        prompt_id,
        speak_id,
        wear_id,
        auto_id,
        startup_id,
        tray_id,
        personalization_id = 1051,
        advanced_id,
        diagnostics_id,
        release_id,
        licenses_id,
        status_id = 1090,
        menu_id = 1100,
        menu_scroll_id = 1200,
        appearance_id = 1060,
        safe_id = 1064,
        assistant_id,
        restore_id,
        touch_text_id = 1070,
        custom_id = 1080,
        log_mib_id = 900, log_runs_id, apply_log_id, export_log_id, replay_log_id,
        channel_id, check_update_id, release_page_id, timeline_id, replay_select_id,
        log_mib_label_id, log_runs_label_id
    };
    enum Kind { button, slider, toggle, choice, cycle_button, menu_button, status_text, menu_scrollbar, numeric, timeline };
    struct Item : Control {
        Kind kind{button};
        MoreAction action{};
        int detail{};
        bool known{}, vertical{}, on{};
        std::optional<int> pending;
        std::vector<MoreChoice> choices;
        motion::Switch switch_motion;
        bool numeric_dirty{}, numeric_syncing{}, numeric_initialized{};
    };
    HWND hwnd{}, owner{};
    HINSTANCE instance{};
    UINT dpi{96}, forced_dpi{};
    float fitted_scale{1};
    int height{772}, scroll{}, active_menu{-1}, menu_offset{}, menu_cursor{}, menu_rows{6};
    bool menu_scrolling{};
    float menu_scroll_grab{};
    ULONGLONG prompt_cooldown_until{};
    std::optional<int> last_auto_power_on;
    static constexpr UINT prompt_cooldown_timer = 3;
    static constexpr UINT prompt_cooldown_ms = 150;
    std::uint64_t menu_link{};
    int menu_key{-1};
    int menu_detail{};
    std::vector<MoreChoice> menu_choices;
    int timeline_top{}, timeline_selected{};
    bool timeline_dragging{};
    float timeline_grab{};
    motion::WheelAccumulator timeline_wheel;
    std::wstring numeric_error;
    std::map<int, Item> items;
    MoreModel model;
    Dispatch dispatch;
    std::unique_ptr<Painter> painter;
    FullTextTips tips;
    std::wstring signature;
    bool posted{}, dirty{true}, presenting{}, positioned{}, keyboard{}, reduced{}, animating{}, suspended{},
        error_reported{};
    double frame_clock{};
    motion::WheelAccumulator scroll_wheel;
    static constexpr UINT frame_id = WM_APP + 30;
    SettingsPage page{SettingsPage::more};
    int width() const { return page == SettingsPage::personalization ? 416 : 680; }
    int content_height() const { return page == SettingsPage::personalization ? 410 : page == SettingsPage::advanced ? 690
        : page == SettingsPage::diagnostics ? 592 : page == SettingsPage::replay ? 527 : 772; }
    const wchar_t *page_title() const {
        return page == SettingsPage::personalization ? L"SonyConnect — 个性化设置"
             : page == SettingsPage::advanced ? L"SonyConnect — 高级设置"
             : page == SettingsPage::diagnostics ? L"SonyConnect — 诊断与维护"
             : page == SettingsPage::replay ? L"SonyConnect — 诊断回放" : L"SonyConnect — 更多功能";
    }
    explicit Impl(Dispatch callback, SettingsPage kind) : dispatch(std::move(callback)), page(kind) {}
    ~Impl() {
        if (hwnd && IsWindow(hwnd))
            DestroyWindow(hwnd);
        stop_animation();
    }
    float scale() const { return fitted_scale; }
    int px(float n) const { return static_cast<int>(std::lround(n * scale())); }
    int px(int n) const { return px(static_cast<float>(n)); }
    bool writable() const {
        return !model.offline && !model.preview && model.device.ready && !model.busy && !model.device.busy;
    }
    bool control_visual_available(int id) const {
        const auto &d = model.device;
        const bool ready = d.ready && !model.offline && !model.preview;
        if (id == left_id || id == right_id)
            return ready && d.touch.writable && model.touch_indices[id - left_id] >= 0;
        if (id >= cycle_id && id < cycle_id + 3) return ready && d.touch.cycle_writable;
        if (id == prompt_id) return ready && d.touch.volume_writable && d.touch.prompt_volume.known();
        if (id == guidance_id) return ready && d.touch.guidance_writable;
        if (id == dsee_id || id == speak_id || id == wear_id || id == auto_id) {
            const auto feature = id == dsee_id ? protocol::Feature::Dsee
                               : id == speak_id ? protocol::Feature::SpeakToChat
                               : id == wear_id ? protocol::Feature::WearDetectionPause
                                               : protocol::Feature::AutoPowerOff;
            const auto found = d.features.find(feature);
            return ready && found != d.features.end() && found->second.writable;
        }
        return items.at(id).enabled;
    }
    void request_frame() {
        dirty = true;
        if (!posted && hwnd) {
            posted = true;
            if (!PostMessageW(hwnd, frame_id, 0, 0))
                throw std::runtime_error("Cannot schedule more settings frame");
        }
    }
    void start_animation() {
        if (reduced || suspended || animating || !IsWindowVisible(hwnd))
            return;
        frame_clock = animation_seconds();
        if (!SetTimer(hwnd, 1, 16, nullptr))
            throw std::runtime_error("Cannot schedule settings animation");
        timeBeginPeriod(1);
        animating = true;
    }
    void stop_animation() {
        if (!animating)
            return;
        KillTimer(hwnd, 1);
        timeEndPeriod(1);
        animating = false;
    }
    void pen(PenIcon icon, float x, float y, float size, unsigned tint = muted) {
        painter->pen_icons->draw(painter->target.get(), painter->brush.get(), icon, x, y, size, color(tint));
    }
    void create_item(int id, Kind kind, MoreAction action, Box box, int detail = 0, bool vertical = false) {
        Item c;
        c.id = id;
        c.kind = kind;
        c.action = action;
        c.box = box;
        c.detail = detail;
        c.slider = kind == slider;
        c.vertical = vertical;
        c.hwnd = CreateWindowExW(WS_EX_TRANSPARENT,
                                 kind == numeric ? L"EDIT"
                                 : kind == status_text ? L"STATIC"
                                 : c.slider          ? TRACKBAR_CLASSW
                                                     : L"BUTTON",
                                 L"",
                                 WS_CHILD | (kind == status_text || kind == menu_scrollbar ? 0 : WS_TABSTOP) |
                                     (kind == numeric ? ES_NUMBER | ES_AUTOHSCROLL
                                      : kind == status_text ? SS_LEFT
                                      : c.slider          ? (TBS_NOTICKS | (vertical ? TBS_VERT : 0))
                                                          : BS_OWNERDRAW),
                                 0, 0, 1, 1, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 instance, nullptr);
        if (!c.hwnd)
            throw std::runtime_error("Cannot create settings input control");
        items.emplace(id, std::move(c));
        if (!SetWindowSubclass(items.at(id).hwnd, child_proc, id, reinterpret_cast<DWORD_PTR>(this)))
            throw std::runtime_error("Cannot install settings input handler");
        tips.add(hwnd, items.at(id).hwnd);
        if (kind == numeric) SendMessageW(items.at(id).hwnd, EM_SETLIMITTEXT, 3, 0);
    }
    void create(HINSTANCE inst, HWND parent, UINT forced) {
        instance = inst;
        owner = parent;
        forced_dpi = forced;
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"SonyConnect.MoreWindow";
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(icon_resource));
        wc.hIconSm = wc.hIcon;
        RegisterClassExW(&wc);
        painter = std::make_unique<Painter>();
        hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_CONTROLPARENT, L"SonyConnect.MoreWindow",
                               L"SonyConnect-更多功能", WS_POPUP | WS_CLIPCHILDREN, 0, 0, width(), content_height(), parent,
                               nullptr, inst, this);
        if (!hwnd)
            throw std::runtime_error("Cannot create more settings window");
        painter->owner = hwnd;
        dpi = forced ? forced : GetDpiForWindow(parent ? parent : hwnd);
        tips.create(hwnd);
        BOOL animate = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0);
        reduced = !animate;
        SetWindowTextW(hwnd, page_title());
        create_item(close_id, button, MoreAction::releases, {static_cast<float>(width() - 38), 8, 28, 28});
        if (page != SettingsPage::more) {
            create_settings_items();
            resize();
            return;
        }
        create_item(preset_id, choice, MoreAction::equalizer_preset, {224, 100, 102, 36});
        for (int i = 0; i < 5; ++i)
            create_item(band_id + i, slider, MoreAction::equalizer_band, {48.6f + 57.2f * i, 184, 24, 114}, i,
                        true);
        create_item(bass_id, slider, MoreAction::clear_bass, {36, 354, 278, 32});
        create_item(dsee_id, toggle, MoreAction::dsee, {290, 442, 36, 24});
        create_item(left_id, choice, MoreAction::touch_preset, {393, 100, 263, 36}, 0);
        create_item(right_id, choice, MoreAction::touch_preset, {393, 150, 263, 36}, 1);
        for (int i = 0; i < 3; ++i)
            create_item(cycle_id + i, cycle_button, MoreAction::cycle,
                        {354 + i * (310.f / 3), 228, 294.f / 3, 32}, i);
        create_item(guidance_id, toggle, MoreAction::guidance, {290, 555, 36, 24});
        create_item(prompt_id, slider, MoreAction::prompt_volume, {24, 643, 302, 32});
        create_item(speak_id, toggle, MoreAction::speak_to_chat, {620, 318, 36, 24});
        create_item(wear_id, toggle, MoreAction::wear_pause, {620, 385, 36, 24});
        create_item(auto_id, toggle, MoreAction::auto_power_off, {620, 451, 36, 24});
        create_item(startup_id, toggle, MoreAction::startup, {620, 555, 36, 24});
        create_item(tray_id, toggle, MoreAction::close_to_tray, {620, 585, 36, 24});
        create_item(personalization_id, button, MoreAction::personalization, {354, 615, 147, 36});
        create_item(advanced_id, button, MoreAction::advanced, {509, 615, 147, 36});
        create_item(diagnostics_id, button, MoreAction::diagnostics, {354, 661, 302, 36});
        create_item(release_id, button, MoreAction::releases, {24, 730, 120, 26});
        create_item(licenses_id, button, MoreAction::licenses, {540, 730, 116, 26});
        create_item(status_id, status_text, MoreAction::releases, {24, 722, 632, 36});
        for (auto &[id, c] : items)
            if (c.slider) {
                c.minimum = id == prompt_id ? -2 : -10;
                c.maximum = id == prompt_id ? 2 : 10;
                SendMessageW(c.hwnd, TBM_SETRANGE, FALSE, MAKELPARAM(c.minimum, c.maximum));
            }
        resize();
    }
    void create_settings_items() {
        if (page == SettingsPage::diagnostics) {
            create_item(log_mib_label_id, status_text, MoreAction::cancel_edit, {24, 96, 268, 20});
            create_item(log_mib_id, numeric, MoreAction::diagnostic_limits, {24, 123, 268, 36});
            create_item(log_runs_label_id, status_text, MoreAction::cancel_edit, {304, 96, 268, 20});
            create_item(log_runs_id, numeric, MoreAction::diagnostic_limits, {304, 123, 268, 36});
            create_item(apply_log_id, button, MoreAction::diagnostic_limits, {583, 123, 73, 36});
            create_item(export_log_id, button, MoreAction::diagnostic_export, {24, 229, 109, 34});
            create_item(replay_log_id, button, MoreAction::diagnostic_replay, {143, 229, 109, 34});
            create_item(channel_id, choice, MoreAction::update_channel, {24, 394, 456, 36});
            create_item(check_update_id, button, MoreAction::check_update, {494, 394, 73, 36});
            create_item(release_page_id, button, MoreAction::release_page, {583, 394, 73, 36});
        } else if (page == SettingsPage::replay) {
            create_item(replay_select_id, button, MoreAction::diagnostic_replay, {553, 68, 103, 36});
            create_item(timeline_id, timeline, MoreAction::cancel_edit, {24, 184, 632, 290});
        } else if (page == SettingsPage::personalization) {
            for (int i = 0; i < 4; ++i)
                create_item(appearance_id + i, choice, MoreAction::slider_appearance,
                            {252, 100.f + 68 * i, 140, 36}, i);
        } else {
            create_item(safe_id, toggle, MoreAction::safe_listening, {290, 117, 36, 24});
            create_item(assistant_id, choice, MoreAction::voice_assistant, {24, 296, 302, 36});
            create_item(restore_id, button, MoreAction::restore_linked, {514, 575, 142, 36});
            for (int i = 0; i < 2; ++i)
                create_item(touch_text_id + i, status_text, MoreAction::cancel_edit,
                            {366, 154.f + 172 * i, 278, 110}, i);
            const MoreAction actions[] = {MoreAction::custom_key, MoreAction::custom_action, MoreAction::custom_function};
            for (int i = 0; i < 3; ++i)
                create_item(custom_id + i, choice, actions[i], {354, 145.f + 84 * i, 302, 36});
        }
        create_item(status_id, status_text, MoreAction::releases,
                    {24, static_cast<float>(content_height() - 50), static_cast<float>(width() - 48), 36});
    }
    void resize() {
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(
            MonitorFromWindow(positioned ? hwnd : (owner ? owner : hwnd), MONITOR_DEFAULTTONEAREST),
            &monitor);
        fitted_scale = fit_window_scale(dpi, width(), monitor.rcWork.right - monitor.rcWork.left) * interface_scale;
        height =
            std::min(content_height(), std::max(96, static_cast<int>((monitor.rcWork.bottom - monitor.rcWork.top - 16) / scale())));
        RECT r{};
        GetWindowRect(positioned ? hwnd : owner, &r);
        const DesktopRect work{monitor.rcWork.left, monitor.rcWork.top, monitor.rcWork.right, monitor.rcWork.bottom};
        const DesktopRect anchor{r.left, r.top, r.right, r.bottom};
        const auto bounds = positioned ? fit_desktop(work, r.left, r.top, px(width()), px(height))
                                       : anchor_settings(work, anchor, px(width()), px(height), px(12));
        SetWindowPos(hwnd, nullptr, bounds.left, bounds.top, bounds.width(), bounds.height(),
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW);
        positioned = true;
        scroll = std::clamp(scroll, 0, content_height() - height);
        layout();
        request_frame();
    }
    void layout() {
        for (auto &[id, c] : items) {
            if (id == status_id)
                c.box.y = static_cast<float>(height + scroll - 50);
            float y = c.box.y - (id == close_id ? 0.f : static_cast<float>(scroll));
            RECT desired{px(c.box.x), px(y), px(c.box.x + c.box.w), px(y + c.box.h)};
            const bool visible = c.visible && (id == close_id || (y + c.box.h > 44 && y < height));
            if (!c.presented_once || !EqualRect(&c.presented, &desired) || c.presented_visible != visible) {
                SetWindowPos(c.hwnd, id >= menu_id ? HWND_TOP : nullptr, desired.left, desired.top,
                             desired.right - desired.left, desired.bottom - desired.top,
                             SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS |
                                 (id >= menu_id ? 0 : SWP_NOZORDER) |
                                 (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
                const int top = id == close_id ? 0 : std::clamp(px(44 - y), 0, px(c.box.h));
                const int bottom = std::clamp(px(height - y), top, px(c.box.h));
                auto region = CreateRectRgn(0, top, px(c.box.w), bottom);
                if (!SetWindowRgn(c.hwnd, region, FALSE))
                    DeleteObject(region);
                c.presented = desired;
                c.presented_visible = visible;
            }
            if (!c.presented_once || c.presented_enabled != c.enabled)
                EnableWindow(c.hwnd, c.enabled);
            const auto name = c.label +
                              (c.slider ? L"，" + (c.known ? std::to_wstring(c.value) : L"尚未读取") : L"") +
                              ((c.kind == toggle || c.kind == cycle_button) ? (!c.known ? L"，尚未读取"
                                                                               : c.on   ? L"，开启"
                                                                                        : L"，关闭")
                                                                            : L"");
            if (c.accessible != name) {
                if (c.kind != numeric) SetWindowTextW(c.hwnd, name.c_str());
                c.accessible = name;
            }
            if (c.slider && SendMessageW(c.hwnd, TBM_GETPOS, 0, 0) != c.value)
                SendMessageW(c.hwnd, TBM_SETPOS, FALSE, c.value);
            c.presented_enabled = c.enabled;
            c.presented_once = true;
        }
    }
    void sync_item(int id, std::wstring label, bool enabled, bool known_value, int value = 0,
                   bool on = false) {
        auto &c = items.at(id);
        c.label = std::move(label);
        c.visible = true;
        c.enabled = enabled;
        c.known = known_value;
        if (c.pending && !model.busy && !model.device.busy)
            c.pending.reset();
        if (!c.dragging && !c.wheel_due)
            c.value = c.pending.value_or(value);
        c.on = c.pending ? (id == auto_id ? *c.pending != 0x11 : *c.pending != 0) : on;
        if (c.kind == toggle) {
            if (!c.presented_once || reduced || !IsWindowVisible(hwnd))
                c.switch_motion.reset(c.on);
            else {
                c.switch_motion.set_target(c.on);
                if (c.switch_motion.active())
                    start_animation();
            }
        }
        if (c.slider) {
            double pct = 100.0 * (c.value - c.minimum) / std::max(1, c.maximum - c.minimum);
            if (!c.wake_initialized || reduced || !IsWindowVisible(hwnd) || c.dragging) {
                c.wake.reset(pct);
                c.elastic.reset(pct);
                c.wake_initialized = true;
            } else {
                c.wake.set_target(pct);
                c.elastic.set_target(pct);
                if (c.wake.active() || c.elastic.active())
                    start_animation();
            }
        }
    }
    void update(MoreModel value) {
        if (page != SettingsPage::more) {
            update_settings(std::move(value));
            return;
        }
        if (model.device.link != value.device.link || (!value.device.ready && model.device.ready)) {
            last_auto_power_on.reset();
            KillTimer(hwnd, prompt_cooldown_timer);
            prompt_cooldown_until = 0;
            cancel_gesture();
            close_menu();
            for (auto &[id, c] : items) {
                c.pending.reset();
                c.wheel_due = 0;
                c.wheel.reset();
            }
        }
        model = std::move(value);
        const auto presentation = more_control_presentation(model.device);
        if (presentation.physical_nc_button) {
            model.touch_labels = {L"[NC/AMB] 按钮", presentation.no_touch_sensor ? L"触摸控制" : L"右侧触摸"};
            model.touch_indices[0] = -1; // A fixed physical button is not a touch assignment slot.
            model.touch_names[0] = L"降噪 / 环境声音";
            model.touch_choices[0].clear();
            if (presentation.no_touch_sensor) {
                model.touch_indices[1] = -1;
                model.touch_names[1] = L"此型号不支持";
                model.touch_choices[1].clear();
            } else if (model.touch_indices[1] < 0) {
                model.touch_names[1] = L"此版本尚未适配";
            }
        }
        const bool write = writable();
        const auto &d = model.device;
        sync_item(close_id, L"关闭更多功能", true, true);
        std::wstring preset = L"尚未读取";
        std::vector<MoreChoice> presets;
        for (const auto &p : d.equalizer.presets) {
            presets.push_back({p.id, wtext(p.name)});
            if (d.equalizer.preset.value == p.id)
                preset = wtext(p.name);
        }
        sync_item(preset_id, preset, write && d.equalizer.writable, d.equalizer.preset.known(),
                  d.equalizer.preset.value.value_or(-1));
        items.at(preset_id).choices = std::move(presets);
        const int custom = d.equalizer.preset.value == 0xa1   ? 0xa1
                           : d.equalizer.preset.value == 0xa2 ? 0xa2
                                                              : 0xa0;
        const bool can_edit = std::any_of(d.equalizer.presets.begin(), d.equalizer.presets.end(),
                                          [&](auto &p) { return p.id == custom; });
        const bool eq = write && d.equalizer.writable && d.equalizer.preset.known() &&
                        *d.equalizer.preset.value != 0 && can_edit;
        const wchar_t *bands[] = {L"400", L"1k", L"2.5k", L"6.3k", L"16k"};
        for (int i = 0; i < 5; ++i) {
            const bool known_value = d.equalizer.five_band_layout && d.equalizer.bands.known() &&
                                     d.equalizer.bands.value->size() > static_cast<std::size_t>(i);
            sync_item(band_id + i, std::wstring(bands[i]) + L" Hz 均衡器", eq && d.equalizer.five_band_layout,
                      known_value,
                      model.equalizer_targets[i].value_or(known_value ? (*d.equalizer.bands.value)[i] : 0));
        }
        sync_item(bass_id, L"CLEAR BASS", eq, d.equalizer.clear_bass.known(),
                  model.equalizer_targets[5].value_or(d.equalizer.clear_bass.value.value_or(0)));
        const auto feature = [&](int id, protocol::Feature key, const wchar_t *label, bool zero_on) {
            const auto f = d.features.find(key);
            const bool exists = f != d.features.end();
            const bool known_value = exists && f->second.value.known();
            const int actual = known_value ? *f->second.value.value : 0;
            sync_item(id, label, write && exists && f->second.writable, known_value, actual,
                      known_value && (key == protocol::Feature::AutoPowerOff ? actual != 0x11
                                      : zero_on                              ? actual == 0
                                                                             : actual != 0));
        };
        feature(dsee_id, protocol::Feature::Dsee, presentation.dsee_label.c_str(), false);
        feature(speak_id, protocol::Feature::SpeakToChat, L"智能免摘", true);
        feature(wear_id, protocol::Feature::WearDetectionPause, L"摘下耳机时暂停", true);
        feature(auto_id, protocol::Feature::AutoPowerOff, L"自动断电", false);
        auto &power = items.at(auto_id);
        power.choices = model.auto_power_choices;
        if (power.known && power.value != 0x11)
            last_auto_power_on = power.value;
        power.enabled = power.enabled && auto_power_target(!power.on).has_value();
        power.label = L"自动断电，" + auto_power_hint();
        for (int i = 0; i < 2; ++i) {
            const int k = model.touch_indices[i];
            const bool known_value = k >= 0 && d.touch.assignments.known() &&
                                     static_cast<std::size_t>(k) < d.touch.assignments.value->size();
            sync_item(left_id + i, model.touch_names[i].empty() ? L"尚未读取" : model.touch_names[i],
                      write && d.touch.writable && k >= 0, known_value,
                      known_value ? (*d.touch.assignments.value)[k] : -1);
            items.at(left_id + i).choices = model.touch_choices[i];
            auto &box = items.at(left_id + i).box;
            box.x = model.touch_labels[i].size() > 2 ? 472.f : 393.f;
            box.w = 656 - box.x;
        }
        const int cycle = d.touch.cycle.value.value_or(0);
        const bool selected[] = {cycle == 1 || cycle == 2 || cycle == 3,
                                 cycle == 1 || cycle == 2 || cycle == 4,
                                 cycle == 1 || cycle == 3 || cycle == 4};
        const wchar_t *names[] = {L"降噪", L"通透", L"关闭"};
        for (int i = 0; i < 3; ++i)
            sync_item(cycle_id + i, names[i], write && d.touch.cycle_writable, d.touch.cycle.known(), 0,
                      selected[i]);
        sync_item(guidance_id, L"通知和语音向导", write && d.touch.guidance_writable,
                  d.touch.guidance.known(), d.touch.guidance.value.value_or(1), d.touch.guidance.value == 0);
        sync_item(prompt_id, L"提示音量调整",
                  write && d.touch.volume_writable && GetTickCount64() >= prompt_cooldown_until,
                  d.touch.prompt_volume.known(),
                  model.prompt_target.value_or(d.touch.prompt_volume.value.value_or(0)));
        sync_item(startup_id, L"开机启动", !model.offline && !model.preview, true, model.startup,
                  model.startup);
        sync_item(tray_id, L"关闭窗口时最小化到托盘", !model.offline && !model.preview, true,
                  model.close_to_tray, model.close_to_tray);
        const wchar_t *links[] = {L"个性化设置", L"高级设置", L"诊断与维护"};
        for (int i = 0; i < 3; ++i)
            sync_item(personalization_id + i, links[i], true, true);
        sync_item(licenses_id, L"第三方许可", true, true);
        sync_item(release_id, model.update_canceling ? L"取消中…" : model.update_checking ? L"取消更新检查" : L"软件版本 v" SONYCONNECT_VERSION,
                  !model.offline && !model.preview && !model.update_canceling, true);
        sync_item(status_id, model.feedback_error ? model.feedback : L"", false, true);
        items.at(status_id).visible = model.feedback_error && !model.feedback.empty();
        items.at(status_id).box.y = static_cast<float>(height + scroll - 50);
        refresh_open_menu();
        layout();
        std::wostringstream digest;
        digest << static_cast<int>(model.slider_style) << wtext(model.device.firmware) << model.preview
               << static_cast<int>(model.eq_slider_style.value_or(model.slider_style))
               << static_cast<int>(model.bass_slider_style.value_or(model.slider_style))
               << model.touch_labels[0] << model.touch_labels[1] << model.feedback_error
               << (model.feedback_error ? model.feedback : L"");
        for (auto &[id, c] : items)
            digest << id << c.label << c.enabled << c.known << c.value << c.on;
        const auto next = digest.str();
        if (signature != next) {
            signature = next;
            request_frame();
        }
    }
    std::wstring gesture_description(int slot) const {
        const auto &d = model.device;
        const int index = model.touch_indices[slot];
        if (!d.ready || index < 0 || !d.touch.assignments.known() ||
            static_cast<std::size_t>(index) >= d.touch.assignments.value->size())
            return L"等待耳机回读触摸分配";
        const int preset = (*d.touch.assignments.value)[index];
        if (d.model != "WF-1000XM5")
            return L"当前分配：" + model.touch_names[slot] + L"\n具体手势请参阅此型号的使用说明。";
        if (preset == 0xff) return L"未分配触摸功能\n四次或更多次轻按：音量操作禁用";
        const auto volume = slot == 0 ? L"降低音量" : L"提高音量";
        std::wstring text;
        if (preset == 0 || preset == 0x35)
            text = L"单击：切换环境声音模式\n按住期间：快速注意（不可单独关闭）";
        else if (preset == 0x20) {
            text = L"单击：播放 / 暂停\n双击：下一曲 · 三击：上一曲\n长按：";
            text += !d.touch.voice_assistant.known() ? L"等待助手状态回读"
                    : d.touch.voice_assistant.value == 0x30 ? L"移动设备语音助手"
                    : d.touch.voice_assistant.value == 0xff ? L"语音助手已关闭" : L"当前设备语音助手";
        } else text = L"当前分配：" + model.touch_names[slot];
        return text + L"\n四次或更多次轻按：" + volume;
    }
    static std::wstring input_text(HWND input) {
        std::wstring text(GetWindowTextLengthW(input) + 1, L'\0');
        text.resize(GetWindowTextW(input, text.data(), static_cast<int>(text.size())));
        return text;
    }
    void sync_numeric(int id, unsigned value, bool enabled) {
        auto &c = items.at(id);
        c.visible = c.known = true;
        c.enabled = enabled;
        c.label = id == log_mib_id ? L"单段日志（MiB）" : L"保留运行数";
        const auto confirmed = std::to_wstring(value);
        const auto text = input_text(c.hwnd);
        if (!c.numeric_initialized || !c.numeric_dirty || text == confirmed) {
            c.numeric_dirty = false;
            if (text != confirmed) {
                c.numeric_syncing = true;
                SetWindowTextW(c.hwnd, confirmed.c_str());
                c.numeric_syncing = false;
            }
            c.numeric_initialized = true;
        }
    }
    int timeline_count() const { return model.replay_entries ? static_cast<int>(model.replay_entries->size()) : 0; }
    int timeline_rows() const { return 4; }
    int timeline_max_top() const { return std::max(0, timeline_count() - timeline_rows()); }
    void timeline_position(int selected, int top) {
        const auto old_selected = timeline_selected, old_top = timeline_top;
        timeline_selected = std::clamp(selected, 0, std::max(0, timeline_count() - 1));
        timeline_top = std::clamp(top, 0, timeline_max_top());
        auto &item = items.at(timeline_id);
        auto label = L"文件：" + model.replay_name + L"\n离线只读时间线，" + std::to_wstring(timeline_count()) + L" 条事件";
        if (timeline_count()) {
            const auto row = replay_text((*model.replay_entries)[timeline_selected]);
            label += L"\n" + std::to_wstring(timeline_selected + 1) + L"：" + row.title + L"\n" + row.detail;
        }
        if (item.label != label || old_top != timeline_top || old_selected != timeline_selected) {
            item.label = std::move(label); layout(); request_frame();
        }
    }
    void update_diagnostics(MoreModel value) {
        const bool changed_file = model.replay_entries != value.replay_entries;
        model = std::move(value);
        model.feedback = numeric_error.empty() ? model.diagnostic_feedback : numeric_error;
        model.feedback_error = !numeric_error.empty() || model.diagnostic_error;
        sync_item(close_id, L"关闭" + std::wstring(page_title()), true, true);
        if (page == SettingsPage::diagnostics) {
            const bool local = model.diagnostics_available && !model.offline && !model.preview && !model.diagnostic_busy && !model.busy;
            sync_item(log_mib_label_id, L"单段日志（MiB）", false, true);
            sync_item(log_runs_label_id, L"保留运行数", false, true);
            sync_numeric(log_mib_id, model.log_mib, local);
            sync_numeric(log_runs_id, model.log_runs, local);
            sync_item(apply_log_id, L"应用限制", local, true);
            sync_item(export_log_id, L"导出脱敏诊断包", local, true);
            sync_item(replay_log_id, L"打开诊断包回放", !model.offline && !model.preview && !model.busy, true);
            sync_item(channel_id, model.update_channel ? L"Gitee" : L"GitHub",
                      !model.offline && !model.preview && !model.update_checking, true, model.update_channel);
            items.at(channel_id).choices = {{0, L"GitHub"}, {1, L"Gitee"}};
            sync_item(check_update_id, model.update_canceling ? L"取消中…" : model.update_checking ? L"取消检查" : L"检查更新",
                      !model.offline && !model.preview && !model.update_canceling, true);
            sync_item(release_page_id, L"发布页面", !model.offline && !model.preview, true);
        } else {
            sync_item(replay_select_id, model.diagnostic_busy ? L"正在读取…" : L"选择诊断包",
                      !model.offline && !model.preview && !model.busy, true);
            items.at(timeline_id).visible = items.at(timeline_id).enabled = items.at(timeline_id).known = true;
            if (changed_file) timeline_top = timeline_selected = 0;
            timeline_position(timeline_selected, timeline_top);
        }
        sync_item(status_id, model.feedback_error ? model.feedback : L"", false, true);
        items.at(status_id).visible = model.feedback_error && !model.feedback.empty();
        refresh_open_menu(); layout();
        std::wostringstream digest;
        digest << model.log_mib << model.log_runs << model.diagnostic_status << model.diagnostic_feedback
               << model.diagnostic_error << model.update_message << model.update_error << model.replay_name
               << model.replay_entries.get() << numeric_error;
        for (auto &[id, c] : items) digest << id << c.label << c.enabled << c.value;
        const auto next = digest.str();
        if (next != signature) { signature = next; request_frame(); }
    }
    void update_settings(MoreModel value) {
        if (page == SettingsPage::diagnostics || page == SettingsPage::replay) {
            update_diagnostics(std::move(value));
            return;
        }
        if (model.device.link != value.device.link || (!value.device.ready && model.device.ready)) {
            close_menu();
            for (auto &[id, c] : items) c.pending.reset();
        }
        model = std::move(value);
        sync_item(close_id, L"关闭" + std::wstring(page_title()), true, true);
        if (page == SettingsPage::personalization) {
            const wchar_t *labels[] = {L"音量条", L"环境声音控制条", L"EQ 调节条", L"BASS 条"};
            for (int i = 0; i < 4; ++i) {
                const int selected_style = static_cast<int>(model.slider_styles[i]);
                sync_item(appearance_id + i, std::wstring(labels[i]) + (selected_style ? L"，Elastic 样式" : L"，Wake 样式"),
                          !model.preview, true, selected_style);
                items.at(appearance_id + i).choices = {{0, L"Wake 样式"}, {1, L"Elastic 样式"}};
            }
        } else {
            const auto &d = model.device;
            sync_item(safe_id, L"安全聆听", writable() && d.safe_listening.writable && d.safe_listening.enabled.known(),
                      d.safe_listening.enabled.known(), d.safe_listening.enabled.value.value_or(false),
                      d.safe_listening.enabled.value.value_or(false));
            sync_item(assistant_id, model.assistant_name.empty() ? L"尚未读取" : model.assistant_name,
                      writable() && d.touch.assistant_writable && d.touch.voice_assistant.known(), d.touch.voice_assistant.known(),
                      d.touch.voice_assistant.value.value_or(-1));
            items.at(assistant_id).choices = model.assistant_choices;
            items.at(assistant_id).enabled &= !model.assistant_choices.empty();
            sync_item(restore_id, L"恢复关联备份", writable() && model.linked_restore_available, true);
            for (int i = 0; i < 2; ++i) {
                sync_item(touch_text_id + i, gesture_description(i), false, true);
                items.at(touch_text_id + i).visible = d.model == "WF-1000XM5";
            }
            for (int i = 0; i < 3; ++i) {
                std::wstring label = L"未上报可编辑选项";
                for (const auto &c : model.custom_choices[i])
                    if (c.value == model.custom_values[i]) label = c.label;
                const bool can_write_function = d.touch.writable && model.custom_context >= 0 &&
                                                model.custom_values[2] >= 0 && d.touch.assignments.known() &&
                                                d.touch.details.known();
                sync_item(custom_id + i, label, writable() && (i < 2 || can_write_function) && !model.custom_choices[i].empty(),
                          model.custom_values[i] >= 0, model.custom_values[i]);
                auto &c = items.at(custom_id + i);
                c.visible = d.model != "WF-1000XM5";
                c.choices = model.custom_choices[i];
                c.detail = model.custom_context;
            }
        }
        sync_item(status_id, model.feedback_error ? model.feedback : L"", false, true);
        items.at(status_id).visible = model.feedback_error && !model.feedback.empty();
        refresh_open_menu();
        layout();
        std::wostringstream digest;
        digest << model.device.ready << model.preview << wtext(model.device.model) << model.feedback_error;
        for (int i = 0; i < 2; ++i) digest << model.touch_names[i] << model.touch_labels[i];
        for (auto &[id, c] : items) {
            digest << id << c.label << c.enabled << c.known << c.value << c.on;
            for (auto &option : c.choices) digest << option.value << option.label;
        }
        const auto next = digest.str();
        if (signature != next) { signature = next; request_frame(); }
    }
    void style_sample(Box b, MainSliderStyle style, bool vertical, int bars = 12) {
        auto &p = *painter;
        if (vertical) {
            for (int i = 0; i < 5; ++i) {
                const float x = b.x + i * b.w / 5 + 4, h = b.h * (.4f + .12f * (i % 3));
                if (style == MainSliderStyle::elastic) {
                    p.rect({x, b.y, 4, b.h}, 0xd9deea, 2);
                    p.rect({x, b.y + b.h - h, 4, h}, blue, 2);
                } else for (int j = 0; j < 8; ++j)
                    p.wake_bar({x, b.y + b.h - (j + 1) * b.h / 8, 6, b.h / 8 - 1},
                               j < 3 + i % 3 ? blue : 0xd9deea);
            }
        } else if (style == MainSliderStyle::elastic) {
            p.rect({b.x, b.y + b.h / 2 - 2, b.w, 4}, 0xd9deea, 2);
            p.rect({b.x, b.y + b.h / 2 - 2, b.w * .64f, 4}, blue, 2);
        } else for (int i = 0; i < bars; ++i) {
            const float pitch = b.w / bars;
            p.wake_bar({b.x + i * pitch, b.y + b.h / 2 - 4, pitch - 2, 8}, i < bars * .64 ? blue : 0xd9deea);
        }
    }
    void draw_settings() {
        auto &p = *painter;
        if (page == SettingsPage::diagnostics) {
            pen(PenIcon::file_text, 24, 65, 18, blue);
            p.text(L"诊断日志", {50, 61, 300, 24}, ink, 14, true);
            p.text(L"单段日志（MiB）", {24, 96, 268, 20}, muted, 12);
            p.text(L"保留运行数", {304, 96, 268, 20}, muted, 12);
            p.text(L"单段日志 1–8 MiB；保留 1–100 次运行。", {24, 169, 632, 22}, muted, 12);
            p.text(model.diagnostic_status, {24, 200, 632, 20},
                   model.diagnostic_status == L"自动日志已开启。" || model.diagnostic_status == L"诊断记录已启用" ? green : muted, 12);
            p.text(L"回放仅展示脱敏时间线，不连接耳机，也不发送控制指令。", {24, 275, 632, 22}, muted, 12);
            p.line(24, 313, 656, 313, border, 1);
            pen(PenIcon::download, 24, 336, 18, blue);
            p.text(L"软件更新", {50, 332, 300, 24}, ink, 14, true);
            p.text(L"更新来源", {24, 367, 632, 22}, muted, 12);
            p.text(L"仅手动检测，不自动下载或安装。检测失败时保留当前版本，可切换来源后重试。", {24, 440, 632, 22}, muted, 12);
            p.rect({24, 471, 632, 43}, model.update_error ? 0xfff2e4 : pale, 8);
            p.text(model.update_message, {36, 480, 608, 26}, model.update_error ? 0xb56c25 : 0x59647a, 12);
            if (!model.diagnostic_error && !model.diagnostic_feedback.empty())
                p.text(model.diagnostic_feedback, {24, 521, 632, 22}, muted, 11);
            p.text(L"软件版本 v" SONYCONNECT_VERSION L"  ·  耳机固件 " +
                       (model.device.firmware.empty() ? L"—" : wtext(model.device.firmware)),
                   {24, 550, 632, 26}, muted, 12);
        } else if (page == SettingsPage::replay) {
            p.text(model.replay_name.empty() ? L"尚未选择诊断包" : model.replay_name, {24, 61, 510, 24}, ink, 14, true);
            p.text(L"离线只读 · " + std::to_wstring(timeline_count()) + L" 条事件", {24, 90, 510, 22}, blue, 12);
            p.rect({24, 125, 632, 43}, pale, 8);
            p.text(L"仅查看脱敏事件时间线。此页面不会连接耳机或重发控制指令。", {36, 133, 608, 26}, 0x59647a, 12);
            p.text(L"滚轮或方向键浏览 · F1 查看完整事件。文件读取失败时保留当前时间线。", {24, 485, 632, 26}, muted, 12);
        } else if (page == SettingsPage::personalization) {
            p.text(L"选择轨道外观，直接预览样式", {16, 60, 384, 22}, muted, 12);
            const wchar_t *labels[] = {L"音量条", L"环境声音控制条", L"EQ 调节条", L"BASS 条"};
            const wchar_t *hints[] = {L"同步系统音量", L"通透模式 · 整数档位", L"五段均衡器 · −10～+10", L"CLEAR BASS · −10～+10"};
            for (int i = 0; i < 4; ++i) {
                const float y = 90.f + 68 * i;
                p.rect({16, y, 384, 60}, 0xffffff, 8, border);
                p.text(labels[i], {28, y + 8, 210, 22}, 0x30384b, 13, true);
                p.text(hints[i], {28, y + 31, 210, 20}, muted, 11);
            }
            p.text(L"选择后即时生效，四项独立设置。", {16, 370, 384, 22}, muted, 12);
        } else {
            const auto title = model.device.ready ? wtext(model.device.model) + L" · 已连接" : L"未连接 Sony 耳机";
            p.text(title, {24, 60, 632, 22}, model.device.ready ? 0x23876c : muted, 12);
            pen(PenIcon::ear, 24, 91, 18, blue);
            p.text(L"聆听保护", {50, 89, 270, 24}, ink, 14, true);
            p.text(L"安全聆听", {24, 118, 250, 22}, 0x30384b);
            p.text(L"控制耳机的安全聆听设置。\n声压读数仍在主界面显示。\n关注语音位于主界面的通透模式中。",
                   {24, 154, 302, 72}, muted, 12);
            p.line(24, 239, 326, 239, border, 1);
            pen(PenIcon::volume_2, 24, 257, 18, blue);
            p.text(L"语音助手", {50, 255, 270, 24}, ink, 14, true);
            p.text(L"仅显示当前耳机支持的助手选项。", {24, 342, 302, 22}, muted, 12);
            p.rect({24, 378, 302, 88}, pale, 8);
            p.text(L"移动设备语音助手的长按操作\n依赖播放控制分配。\n涉及关联修改时会先确认。", {36, 387, 278, 70}, muted, 12);
            pen(PenIcon::hand, 354, 91, 18, blue);
            p.text(model.device.model == "WF-1000XM5" ? L"触摸操作说明" : L"高级触摸", {380, 89, 276, 24}, ink, 14, true);
            if (model.device.model == "WF-1000XM5") for (int i = 0; i < 2; ++i) {
                const float y = 122.f + 172 * i;
                p.rect({354, y, 302, 158}, 0xffffff, 8, border);
                p.text(model.touch_labels[i] + L" · " + (model.device.ready && !model.touch_names[i].empty() ? model.touch_names[i] : L"尚未读取"),
                       {366, y + 8, 278, 24}, ink, 12, true);
                p.text(items.at(touch_text_id + i).label, {366, y + 34, 278, 110}, muted, 12);
            } else {
                const wchar_t *labels[] = {L"耳机按键", L"触摸手势", L"动作功能"};
                for (int i = 0; i < 3; ++i)
                    p.text(labels[i], {354, 118.f + 84 * i, 302, 22}, muted, 12);
                p.text(L"仅显示耳机实际上报的可编辑手势。\n修改后等待耳机回读确认。", {354, 377, 302, 58}, muted, 12);
            }
            p.text(model.device.model == "WF-1000XM5"
                       ? L"音乐播放时，轻按四次或更多次可调音量。\nQuick Access 服务请在手机 App 中配置。"
                       : L"操作说明以当前型号的使用指南为准。\nQuick Access 服务请在手机 App 中配置。",
                   {354, 472, 302, 48}, muted, 12);
            p.line(24, 539, 656, 539, border, 1);
            p.text(L"关联配置备份", {24, 553, 300, 24}, ink, 14, true);
            p.text(L"恢复本次连接内保存的触摸与助手配置。\n可用时启用；恢复前会再次确认。", {24, 585, 472, 44}, muted, 12);
        }
    }
    void render() {
        auto &p = *painter;
        HDC compatible = GetDC(hwnd);
        p.prepare_canvas(compatible, px(width()), px(content_height()));
        ReleaseDC(hwnd, compatible);
        const RECT canvas{0, 0, p.canvas_width, p.canvas_height};
        p.begin(p.canvas, canvas, scale());
        p.target->Clear(D2D1::ColorF(0, 0, 0, 0));
        p.rect({0, 0, static_cast<float>(width()), static_cast<float>(height)}, bg, 14, border);
        p.target->PushAxisAlignedClip(D2D1::RectF(1, 44, static_cast<float>(width() - 1), static_cast<float>(height - 1)),
                                      D2D1_ANTIALIAS_MODE_ALIASED);
        p.target->SetTransform(D2D1::Matrix3x2F::Translation(0, -static_cast<float>(scroll)) *
                               D2D1::Matrix3x2F::Scale(scale(), scale()));
        auto section = [&](float x, float y, PenIcon icon, const wchar_t *label) {
            pen(icon, x, y + 1, 18, blue);
            p.text(label, {x + 26, y, 250, 20}, ink, 14, true);
        };
        if (page == SettingsPage::more) {
        section(24, 64, PenIcon::sliders_horizontal, L"音质");
        section(354, 64, PenIcon::hand, L"耳机控制");
        section(24, 513, PenIcon::volume_2, L"通知与提示");
        section(354, 279, PenIcon::ear, L"佩戴与电源");
        p.text(L"均衡器", {24, 106, 150, 20}, 0x30384b);
        p.rect({24, 150, 302, 265}, 0xffffff, 8, border);
        const wchar_t *bands[] = {L"400", L"1k", L"2.5k", L"6.3k", L"16k"};
        auto signed_value = [](int value) { return (value > 0 ? L"+" : L"") + std::to_wstring(value); };
        for (int i = 0; i < 5; ++i) {
            auto &c = items.at(band_id + i);
            p.text(c.known ? signed_value(c.value) : L"—", {c.box.x - 8, 166, 40, 17}, 0x4051c1, 12, false,
                   DWRITE_TEXT_ALIGNMENT_CENTER);
            p.text(bands[i], {c.box.x - 8, 299, 40, 17}, muted, 12, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        p.text(L"CLEAR BASS", {36, 341, 150, 17}, 0x414d63, 12);
        const auto &bass = items.at(bass_id);
        p.text(bass.known ? signed_value(bass.value) : L"—", {264, 341, 50, 17}, 0x4051c1, 12, false,
               DWRITE_TEXT_ALIGNMENT_TRAILING);
        for (int i = 0; i < 3; ++i)
            p.text(i == 0 ? L"−10" : i == 1 ? L"0" : L"+10", {36 + 129.f * i, 386, 20, 17}, muted, 12);
        auto setting = [&](float x, float y, const wchar_t *label, const wchar_t *hint, int id) {
            const bool enabled = control_visual_available(id);
            p.text(label, {x, y, 254, 19}, enabled ? 0x30384b : 0xa2a8b5);
            p.text(hint, {x, y + 25, 254, 18}, enabled ? muted : 0xb5bac5, 12);
        };
        const auto presentation = more_control_presentation(model.device);
        setting(24, 442, presentation.dsee_label.c_str(), L"补偿压缩音源的高频细节", dsee_id);
        setting(24, 555, L"通知和语音向导", L"播放耳机提示音与语音通知", guidance_id);
        p.text(L"提示音量调整", {24, 619, 240, 19}, control_visual_available(prompt_id) ? 0x30384b : 0xa2a8b5);
        auto &prompt = items.at(prompt_id);
        p.text(prompt.known ? signed_value(prompt.value) : L"—", {296, 619, 30, 19}, 0x4051c1, 13, false,
               DWRITE_TEXT_ALIGNMENT_TRAILING);
        for (int i = 0; i < 5; ++i)
            p.text(signed_value(i - 2), {24 + 71.5f * i, 680, 20, 16}, muted, 11);
        for (int i = 0; i < 2; ++i)
            p.text(model.touch_labels[i], {354, 106.f + 50.f * i, 115, 19},
                   control_visual_available(left_id + i) || (i == 0 && presentation.physical_nc_button) ? 0x59647a : 0xa2a8b5, 12);
        p.text(presentation.cycle_label, {354, 198, 302, 17},
               control_visual_available(cycle_id) ? muted : 0xb5bac5, 12);
        setting(354, 318, L"智能免摘", presentation.no_touch_sensor ? L"此型号不支持" : L"说话时启用通透模式", speak_id);
        setting(354, 385, L"摘下耳机时暂停", presentation.no_touch_sensor ? L"此型号不支持" : L"重新戴上后恢复播放", wear_id);
        const auto power_hint = auto_power_hint();
        setting(354, 451, L"自动断电", power_hint.c_str(), auto_id);
        p.text(L"软件设置", {354, 517, 302, 20}, ink, 14, true);
        p.text(L"开机启动", {354, 555, 254, 19}, 0x30384b);
        p.text(L"关闭窗口时最小化到托盘", {354, 585, 254, 19}, 0x30384b);
        p.line(24, 717, 656, 717, 0xe1e5ed, 1);
        p.text(L"耳机固件 " + (model.device.firmware.empty() ? L"未连接" : wtext(model.device.firmware)),
               {135, 735, 280, 17}, muted, 12);
        if (model.preview)
            p.text(L"离线设计预览 · 示例数据", {290, 735, 230, 17}, muted, 11, false,
                   DWRITE_TEXT_ALIGNMENT_TRAILING);
        } else draw_settings();
        for (auto &[id, c] : items)
            if (id != close_id && id < menu_id && c.visible)
                draw_item(c);
        if (active_menu >= 0) {
            auto &anchor = items.at(active_menu);
            const auto first = items.find(menu_id);
            if (first != items.end()) {
                const auto &box = first->second.box;
                p.rect({box.x - 4, box.y - 4, box.w + 8 + (menu_max_offset() > 0 ? 14.f : 0.f),
                        static_cast<float>(menu_rows * 32 + 8)},
                       0xffffff, 8, border);
                for (auto &[id, c] : items)
                    if (id >= menu_id && c.visible)
                        draw_item(c);
            }
            (void)anchor;
        }
        if (model.feedback_error && !model.feedback.empty()) {
            const float y = static_cast<float>(height + scroll - 50);
            p.rect({24, y, static_cast<float>(width() - 48), 36}, 0xfff2e4, 8);
            pen(PenIcon::circle_alert, 36, y + 10, 16, 0xb56c25);
            p.text(model.feedback, {60, y + 5, static_cast<float>(width() - 84), 26}, 0xb56c25, 12);
        }
        p.target->PopAxisAlignedClip();
        p.target->SetTransform(D2D1::Matrix3x2F::Scale(scale(), scale()));
        pen(page == SettingsPage::replay ? PenIcon::history : page == SettingsPage::diagnostics ? PenIcon::activity : PenIcon::settings_2,
            16, 13.5f, 17, blue);
        p.text(page_title(), {43, 6, static_cast<float>(width() - 88), 32}, ink, 13, false, DWRITE_TEXT_ALIGNMENT_LEADING, 500);
        pen(PenIcon::x, static_cast<float>(width() - 32), 14, 16, 0x606a7d);
        if (keyboard && GetFocus() == items.at(close_id).hwnd)
            p.rect({static_cast<float>(width() - 37), 9, 26, 26}, pale, 5, border);
        p.end();
        GdiFlush();
    }
    void draw_item(Item &c) {
        if (c.kind == status_text)
            return;
        auto &p = *painter;
        const auto b = c.box;
        const bool visual = page == SettingsPage::more ? control_visual_available(c.id)
                             : c.enabled || ((model.busy || model.device.busy) && c.known);
        const bool faded = !visual && c.kind != status_text;
        if (faded)
            p.target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), .45f), nullptr);
        if (c.kind == slider) {
            const float pct = static_cast<float>(c.wake.head() / 100.0);
            const bool available = c.id == prompt_id ? model.device.touch.volume_writable
                                                     : model.device.equalizer.writable &&
                                                           model.device.equalizer.preset.known() &&
                                                           *model.device.equalizer.preset.value != 0;
            const unsigned fill = available ? blue : 0xadb5cb;
            if (((c.id == bass_id ? model.bass_slider_style : model.eq_slider_style).value_or(model.slider_style) == MainSliderStyle::elastic) || c.id == prompt_id) {
                const float v = c.known ? static_cast<float>(c.elastic.head() / 100.0) : 0.f;
                const float thickness = static_cast<float>(c.elastic.thickness());
                if (c.vertical) {
                    p.rect({b.x + (b.w - thickness) / 2, b.y + 7, thickness, b.h - 14}, 0xd9deea,
                           thickness / 2);
                    p.rect({b.x + (b.w - thickness) / 2, b.y + 7 + (b.h - 14) * (1 - v), thickness,
                            (b.h - 14) * v},
                           fill, thickness / 2);
                } else {
                    p.rect({b.x, b.y + 16 - thickness / 2, b.w, thickness}, 0xd9deea, thickness / 2);
                    p.rect({b.x, b.y + 16 - thickness / 2, b.w * v, thickness}, fill, thickness / 2);
                }
            } else
                for (int i = 0; i < 20; ++i) {
                    const bool filled = c.known && (i + .5f) / 20 <= pct;
                    if (c.vertical) {
                        float pitch = (b.h - 14) / 20;
                        p.wake_bar({b.x + 2, b.y + b.h - 7 - (i + 1) * pitch + 1, b.w - 4, pitch - 2},
                                   filled ? fill : 0xd9deea);
                    } else {
                        float pitch = (b.w + 4) / 20;
                        p.wake_bar({b.x + i * pitch, b.y + 12, pitch - 4, 8}, filled ? fill : 0xd9deea);
                    }
                }
        } else if (c.kind == numeric) {
            p.rect(b, 0xffffff, 6, border);
            const auto text = input_text(c.hwnd);
            DWORD start{}, end{};
            SendMessageW(c.hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
            const float before = p.text_width(text.substr(0, start), 13);
            const float after = p.text_width(text.substr(0, end), 13);
            if (GetFocus() == c.hwnd && start != end)
                p.rect({b.x + 12 + before, b.y + 7, std::max(1.f, after - before), b.h - 14}, pale, 2);
            p.text(text, {b.x + 12, b.y + 4, b.w - 24, b.h - 8}, visual ? ink : muted, 13);
            if (GetFocus() == c.hwnd && start == end)
                p.line(b.x + 12 + before, b.y + 9, b.x + 12 + before, b.y + b.h - 9, blue, 1);
        } else if (c.kind == timeline) {
            p.rect(b, 0xffffff, 8, border);
            if (!timeline_count()) p.text(L"选择 .scdiag 诊断包后查看只读时间线", {b.x + 20, b.y + 100, b.w - 40, 50}, muted, 13,
                                          false, DWRITE_TEXT_ALIGNMENT_CENTER);
            for (int row = 0; row < timeline_rows() && timeline_top + row < timeline_count(); ++row) {
                const int index = timeline_top + row;
                const auto entry = replay_text((*model.replay_entries)[index]);
                const float y = b.y + 6 + row * 71.f;
                if (index == timeline_selected && GetFocus() == c.hwnd)
                    p.rect({b.x + 6, y, b.w - 22, 69}, pale, 5);
                p.text(entry.time, {b.x + 16, y + 9, 108, 22}, muted, 10);
                p.text(entry.title, {b.x + 140, y + 8, b.w - 168, 24}, entry.error ? 0xb56c25 : ink, 13, true);
                p.text(entry.detail, {b.x + 140, y + 34, b.w - 168, 22}, muted, 11);
            }
            if (timeline_max_top()) {
                const float track = b.h - 12, thumb = std::max(24.f, track * timeline_rows() / timeline_count());
                p.rect({b.x + b.w - 10, b.y + 6, 4, track}, 0xedeff5, 2);
                p.rect({b.x + b.w - 11, b.y + 6 + (track - thumb) * timeline_top / timeline_max_top(), 6, thumb}, 0xadb5cb, 3);
            }
        } else if (c.kind == toggle) {
            const float position = static_cast<float>(c.switch_motion.position());
            auto mix = [&](int shift) {
                int a = (0xcbd2df >> shift) & 255, z = (blue >> shift) & 255;
                return static_cast<unsigned>(std::lround(a + (z - a) * position)) << shift;
            };
            p.rect({b.x, b.y + 3, 36, 18}, mix(16) | mix(8) | mix(0), 9);
            p.circle(b.x + 9 + 18 * position, b.y + 12, 6, 0xffffff);
            if (!c.known)
                p.line(b.x + 14, b.y + 12, b.x + 22, b.y + 12, muted, 1);
        } else if (c.kind == choice) {
            p.rect(b, 0xffffff, 6, border);
            if (c.action == MoreAction::slider_appearance)
                style_sample({b.x + 12, b.y + 9, b.w - 46, b.h - 18}, static_cast<MainSliderStyle>(c.value), c.detail == 2);
            else p.text(c.label, {b.x + 12, b.y + 4, b.w - 34, b.h - 8}, visual ? 0x414d63 : muted, 12);
            if (!c.choices.empty()) pen(PenIcon::chevron_down, b.x + b.w - 26, b.y + 11, 14);
        } else if (c.kind == cycle_button) {
            p.rect(b, c.on && visual ? pale : 0xffffff, 6, c.on && visual ? 0xd8def7 : border);
            // Pen07 centers the icon and label together, with a 4 DIP gap.
            constexpr float icon_size = 13, gap = 4;
            constexpr int font_size = 11;
            const float label_width = p.text_width(c.label, font_size);
            const float left = b.x + (b.w - icon_size - gap - label_width) / 2;
            pen(c.on ? PenIcon::check : PenIcon::x, left, b.y + (b.h - icon_size) / 2, icon_size,
                c.on && visual ? blue : 0x8a93a6);
            p.text(c.label, {left + icon_size + gap, b.y, label_width, b.h},
                   c.on && visual ? 0x4051c1 : muted, font_size);
        } else if (c.kind == menu_button) {
            // Selection is device state. Keyboard focus and scroll position
            // must never turn another candidate into the selected preset.
            const bool selected = c.value == items.at(active_menu).value;
            p.rect(b, selected ? pale : 0xffffff, 4);
            if (c.action == MoreAction::slider_appearance) {
                p.text(c.value ? L"Elastic" : L"Wake", {b.x + 10, b.y + 4, 46, 24}, selected ? 0x4051c1 : ink, 12);
                style_sample({b.x + 60, b.y + 8, b.w - 90, 16}, static_cast<MainSliderStyle>(c.value), c.detail == 2, 8);
            } else p.text(c.label, {b.x + 10, b.y + 4, b.w - 35, 24}, selected ? 0x4051c1 : ink, 12);
            if (selected)
                pen(PenIcon::check, b.x + b.w - 24, b.y + 9, 14, blue);
        } else if (c.kind == menu_scrollbar) {
            const float thumb = menu_thumb_height();
            const float y = b.y + (b.h - thumb) * menu_offset / menu_max_offset();
            p.rect({b.x + 4, b.y, 4, b.h}, 0xedeff5, 2);
            p.rect({b.x + 3, y, 6, thumb}, 0xadb5cb, 3);
        } else if (c.id >= log_mib_id && c.id <= replay_select_id) {
            const bool primary = c.id == export_log_id || c.id == check_update_id;
            p.rect(b, primary && visual ? blue : 0xffffff, 6, primary && visual ? blue : border);
            p.text(c.label, b, !visual ? muted : primary ? 0xffffff : ink, 11, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        } else if (c.id == release_id || c.id == licenses_id)
            p.text(c.label, b, muted, 12);
        else if (c.id == restore_id) {
            p.rect(b, 0xffffff, 6, border);
            p.text(c.label, b, visual ? ink : muted, 12, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            p.rect(b, 0xffffff, 6, border);
            const PenIcon icons[] = {PenIcon::sliders_horizontal, PenIcon::sliders_horizontal, PenIcon::activity};
            pen(icons[c.id - personalization_id], b.x + 10, b.y + 10, 16, visual ? 0x59647a : 0xadb5cb);
            p.text(c.label, {b.x + 34, b.y + 6, b.w - 54, 24}, visual ? 0x30384b : 0xadb5cb);
            pen(PenIcon::chevron_right, b.x + b.w - 24, b.y + 11, 14, visual ? muted : 0xadb5cb);
        }
        if (c.enabled && keyboard && GetFocus() == c.hwnd) {
            p.brush->SetColor(color(blue));
            p.target->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(b.x + 1, b.y + 1, b.x + b.w - 1, b.y + b.h - 1), 5, 5),
                p.brush.get(), 1.5f);
        }
        if (faded) p.target->PopLayer();
    }
    void present() {
        if (presenting || !dirty)
            return;
        presenting = true;
        dirty = false;
        try {
            render();
            RECT r{};
            GetWindowRect(hwnd, &r);
            POINT destination{r.left, r.top}, source{};
            SIZE size{px(width()), px(static_cast<float>(height))};
            BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            if (!UpdateLayeredWindow(hwnd, nullptr, &destination, &size, painter->canvas, &source, 0, &blend,
                                     ULW_ALPHA))
                throw std::runtime_error("Cannot submit settings frame");
            ValidateRect(hwnd, nullptr);
            presenting = false;
            if (dirty)
                request_frame();
        } catch (...) {
            presenting = false;
            throw;
        }
    }
    void cancel_gesture(HWND only = nullptr) {
        if (timeline_dragging && items.contains(timeline_id) && (!only || items.at(timeline_id).hwnd == only)) {
            timeline_dragging = false;
            if (GetCapture() == items.at(timeline_id).hwnd) ReleaseCapture();
        }
        for (auto &[id, c] : items)
            if ((c.dragging || c.wheel_due) && (!only || c.hwnd == only)) {
                c.dragging = false;
                c.wheel_due = 0;
                if (GetCapture() == c.hwnd)
                    ReleaseCapture();
                dispatch(MoreAction::cancel_edit, static_cast<int>(c.action), c.detail, true);
            }
    }
    void close_menu(bool restore_focus = true) {
        if (active_menu < 0)
            return;
        menu_scrolling = false;
        if (auto bar = items.find(menu_scroll_id); bar != items.end() && GetCapture() == bar->second.hwnd)
            ReleaseCapture();
        const auto anchor = items.at(active_menu).hwnd;
        const bool focus_in_menu = GetDlgCtrlID(GetFocus()) >= menu_id && IsChild(hwnd, GetFocus());
        active_menu = -1;
        for (auto it = items.lower_bound(menu_id); it != items.end();) {
            tips.remove(hwnd, it->second.hwnd);
            DestroyWindow(it->second.hwnd);
            it = items.erase(it);
        }
        menu_choices.clear();
        if (focus_in_menu && restore_focus && GetActiveWindow() == hwnd)
            SetFocus(anchor);
        request_frame();
    }
    void refresh_open_menu() {
        if (active_menu < 0) return;
        const auto &anchor = items.at(active_menu);
        if (!anchor.enabled || !anchor.visible ||
            (anchor.action == MoreAction::touch_preset && menu_key != touch_context(anchor.detail)) ||
            ((anchor.action == MoreAction::custom_function || anchor.action == MoreAction::custom_action) && menu_detail != anchor.detail) ||
            anchor.choices.size() != menu_choices.size() ||
            !std::equal(anchor.choices.begin(), anchor.choices.end(), menu_choices.begin(),
                        [](const auto &a, const auto &b) { return a.value == b.value && a.label == b.label; })) {
            close_menu();
            return;
        }
        for (int i = 0; i < menu_rows; ++i) {
            auto &option = items.at(menu_id + i);
            const bool selected = option.value == anchor.value;
            if (option.selected != selected) { option.selected = selected; request_frame(); }
        }
    }
    void hide_window() {
        ShowWindow(hwnd, SW_HIDE);
        pause_visuals();
    }
    void pause_visuals() {
        tips.dismiss();
        close_menu(false);
        cancel_gesture();
        stop_animation();
        KillTimer(hwnd, 2);
        KillTimer(hwnd, prompt_cooldown_timer);
        for (auto &[id, c] : items) { c.wheel.reset(); c.hover = false; }
    }
    void resume_prompt_cooldown() {
        const auto now = GetTickCount64();
        if (prompt_cooldown_until > now) {
            if (!SetTimer(hwnd, prompt_cooldown_timer, static_cast<UINT>(prompt_cooldown_until - now), nullptr))
                throw std::runtime_error("Cannot resume prompt cooldown");
        } else prompt_cooldown_until = 0;
    }
    void escape() {
        if (active_menu >= 0) {
            close_menu();
            return;
        }
        const bool editing = std::any_of(items.begin(), items.end(), [](const auto &entry) {
            return entry.second.dragging || entry.second.wheel_due;
        });
        if (editing) {
            cancel_gesture();
            request_frame();
        } else
            hide_window();
    }
    int touch_context(int slot) const {
        const int index = model.touch_indices[slot];
        return index >= 0 && static_cast<std::size_t>(index) < model.device.touch.keys.size()
                   ? model.device.touch.keys[index].id
                   : -1;
    }
    int menu_max_offset() const { return std::max(0, static_cast<int>(menu_choices.size()) - menu_rows); }
    float menu_thumb_height() const {
        return std::max(24.f, items.at(menu_scroll_id).box.h * menu_rows / menu_choices.size());
    }
    void scroll_menu(int offset, bool focus = true) {
        menu_offset = std::clamp(offset, 0, menu_max_offset());
        menu_cursor = std::clamp(menu_cursor, menu_offset,
                                 std::min(menu_offset + menu_rows - 1, static_cast<int>(menu_choices.size()) - 1));
        layout_menu();
        if (focus)
            SetFocus(items.at(menu_id + menu_cursor - menu_offset).hwnd);
    }
    void drag_menu_scrollbar(float y) {
        const float travel = items.at(menu_scroll_id).box.h - menu_thumb_height();
        scroll_menu(static_cast<int>(std::lround((y - menu_scroll_grab) * menu_max_offset() / travel)), false);
    }
    void layout_menu() {
        const auto &anchor = items.at(active_menu);
        const int count = menu_rows;
        const auto placement = place_popup(anchor.box.x, anchor.box.y - scroll, anchor.box.w, anchor.box.h,
                                           static_cast<float>(this->width()), static_cast<float>(height),
                                           static_cast<int>(menu_choices.size()));
        const float width = placement.width, top = placement.top + scroll;
        for (int i = 0; i < count; ++i) {
            auto &c = items.at(menu_id + i);
            auto &option = menu_choices[menu_offset + i];
            c.box = {placement.left, top + 32.f * i, width - (menu_max_offset() > 0 ? 14.f : 0.f), 32};
            c.label = option.label;
            c.value = option.value;
            c.selected = option.value == anchor.value;
            c.visible = c.enabled = c.known = true;
        }
        if (auto bar = items.find(menu_scroll_id); bar != items.end()) {
            bar->second.box = {placement.left + width - 12, top, 12, count * 32.f};
            bar->second.label = L"滚动选项";
            bar->second.visible = bar->second.enabled = true;
        }
        layout();
        request_frame();
    }
    void open_menu(int id) {
        if (active_menu == id) {
            close_menu();
            return;
        }
        close_menu();
        auto &anchor = items.at(id);
        if (anchor.choices.empty())
            return;
        active_menu = id;
        menu_link = model.device.link;
        menu_choices = anchor.choices;
        menu_rows = place_popup(anchor.box.x, anchor.box.y - scroll, anchor.box.w, anchor.box.h,
                                  static_cast<float>(width()), static_cast<float>(height),
                                  static_cast<int>(menu_choices.size())).rows;
        menu_offset = menu_cursor = 0;
        menu_key = anchor.action == MoreAction::touch_preset ? touch_context(anchor.detail) : -1;
        menu_detail = anchor.detail;
        for (std::size_t i = 0; i < menu_choices.size(); ++i)
            if (menu_choices[i].value == anchor.value)
                menu_cursor = static_cast<int>(i);
        menu_offset = std::clamp(menu_cursor - menu_rows / 2, 0, menu_max_offset());
        for (int i = 0; i < menu_rows; ++i)
            create_item(menu_id + i, menu_button, anchor.action, {}, anchor.detail);
        if (menu_max_offset() > 0)
            create_item(menu_scroll_id, menu_scrollbar, anchor.action, {});
        layout_menu();
        SetFocus(items.at(menu_id + menu_cursor - menu_offset).hwnd);
    }
    void choose_menu(int id) {
        const auto anchor_id = active_menu;
        auto &anchor = items.at(anchor_id);
        const auto option = items.at(id).value;
        const bool current =
            (anchor.action == MoreAction::update_channel || anchor.action == MoreAction::slider_appearance || menu_link == model.device.link) && anchor.enabled &&
            (anchor.action != MoreAction::touch_preset || menu_key == touch_context(anchor.detail)) &&
            ((anchor.action != MoreAction::custom_function && anchor.action != MoreAction::custom_action) || menu_detail == anchor.detail) &&
            std::any_of(anchor.choices.begin(), anchor.choices.end(),
                        [&](auto &c) { return c.value == option; });
        const auto action = anchor.action;
        const int detail = anchor.detail;
        close_menu();
        SetFocus(anchor.hwnd);
        if (current && option != anchor.value) {
            dispatch(action, option, detail, true);
        }
    }
    std::wstring auto_power_hint() const {
        const auto &c = items.at(auto_id);
        if (!c.known) return L"尚未读取";
        if (c.value == 0x11) return L"摘下耳机后不自动关机";
        for (const auto &option : c.choices)
            if (option.value == c.value) return option.label;
        return L"尚未读取";
    }
    std::optional<int> auto_power_target(bool on) const {
        const auto &choices = items.at(auto_id).choices;
        const auto has = [&](int value) {
            return std::any_of(choices.begin(), choices.end(),
                               [&](const auto &c) { return c.value == value; });
        };
        if (!on) return has(0x11) ? std::optional<int>(0x11) : std::nullopt;
        if (last_auto_power_on && has(*last_auto_power_on)) return last_auto_power_on;
        if (has(0x10)) return 0x10;
        for (const auto &choice : choices)
            if (choice.value != 0x11) return choice.value;
        return {};
    }
    void click(int id) {
        auto &c = items.at(id);
        if (!c.enabled || c.kind == menu_scrollbar)
            return;
        if (id >= menu_id) {
            choose_menu(id);
            return;
        }
        if (id == close_id) {
            hide_window();
            return;
        }
        if (c.kind == choice) {
            open_menu(id);
            present();
            return;
        }
        close_menu();
        if (id == apply_log_id) {
            const auto parse = [&](int input, unsigned maximum) -> unsigned {
                const auto text = input_text(items.at(input).hwnd);
                if (text.empty() || text.size() > 3 || std::any_of(text.begin(), text.end(), [](wchar_t c) { return c < L'0' || c > L'9'; })) return 0;
                const auto value = static_cast<unsigned>(std::stoul(text));
                return value >= 1 && value <= maximum ? value : 0;
            };
            const auto mib = parse(log_mib_id, 8), runs = parse(log_runs_id, 100);
            if (!mib || !runs) {
                numeric_error = L"单段日志请输入 1–8 MiB，保留运行数请输入 1–100。";
                update_diagnostics(model);
                return;
            }
            numeric_error.clear();
            dispatch(MoreAction::diagnostic_limits, static_cast<int>(mib), static_cast<int>(runs), true);
            return;
        }
        if (c.kind == toggle) {
            const bool on = !c.on;
            const auto target = id == auto_id ? auto_power_target(on) : std::optional<int>(on ? 1 : 0);
            if (!target) return;
            c.pending = *target;
            if (id == auto_id) {
                c.value = *target;
                c.label = L"自动断电，" + auto_power_hint();
            }
            c.on = on;
            if (reduced)
                c.switch_motion.reset(on);
            else
                c.switch_motion.set_target(on);
            start_animation();
            layout();
            request_frame();
            dispatch(c.action, *target, c.detail, true);
        } else if (c.kind == cycle_button) {
            bool n = items.at(cycle_id).on, a = items.at(cycle_id + 1).on, o = items.at(cycle_id + 2).on;
            if (c.detail == 0)
                n = !n;
            else if (c.detail == 1)
                a = !a;
            else
                o = !o;
            const int value = n && a && o ? 1 : n && a ? 2 : n && o ? 3 : a && o ? 4 : 0;
            if (value)
                dispatch(MoreAction::cycle, value, 0, true);
        } else
            dispatch(c.action, 0, c.detail, true);
    }
    void commit_slider(Item &c) {
        if (c.id == prompt_id) {
            if (GetTickCount64() < prompt_cooldown_until)
                return;
            prompt_cooldown_until = GetTickCount64() + prompt_cooldown_ms;
            if (!SetTimer(hwnd, prompt_cooldown_timer, prompt_cooldown_ms, nullptr))
                throw std::runtime_error("Cannot schedule prompt-volume cooldown");
            c.enabled = false;
            layout();
        }
        dispatch(c.action, c.value, c.detail, true);
    }
    void drag(Item &c, LPARAM point, bool final) {
        const double position = c.vertical ? 1.0 - (GET_Y_LPARAM(point) / scale() - 7) / (c.box.h - 14)
                                           : (GET_X_LPARAM(point) / scale()) / c.box.w;
        c.value = std::clamp(static_cast<int>(std::lround(c.minimum + (c.maximum - c.minimum) * position)),
                             c.minimum, c.maximum);
        c.wake.reset(100.0 * (c.value - c.minimum) / (c.maximum - c.minimum));
        c.elastic.reset(c.wake.head());
        SendMessageW(c.hwnd, TBM_SETPOS, FALSE, c.value);
        layout();
        request_frame();
        if (final)
            commit_slider(c);
        else
            dispatch(c.action, c.value, c.detail, false);
    }
    void wheel(Item &c, int delta) {
        if (c.id == prompt_id) return;
        const int steps = c.wheel.consume(delta);
        if (!steps)
            return;
        c.value = std::clamp(c.value + steps, c.minimum, c.maximum);
        c.wheel_link = model.device.link;
        c.wheel_due = GetTickCount64() + 120;
        const double pct = 100.0 * (c.value - c.minimum) / (c.maximum - c.minimum);
        if (reduced) {
            c.wake.reset(pct);
            c.elastic.reset(pct);
        } else {
            c.wake.set_target(pct, .16);
            c.elastic.set_target(pct, .16);
        }
        layout();
        request_frame();
        start_animation();
        if (!SetTimer(hwnd, 2, 16, nullptr))
            throw std::runtime_error("Cannot schedule settings wheel commit");
        dispatch(c.action, c.value, c.detail, false);
    }
    void flush_wheels() {
        bool pending = false;
        for (auto &[id, c] : items)
            if (c.wheel_due) {
                if (GetTickCount64() < c.wheel_due) {
                    pending = true;
                    continue;
                }
                c.wheel_due = 0;
                if (c.wheel_link == model.device.link && writable())
                    dispatch(c.action, c.value, c.detail, true);
                else
                    dispatch(MoreAction::cancel_edit, static_cast<int>(c.action), c.detail, true);
            }
        if (!pending)
            KillTimer(hwnd, 2);
    }
    void tick() {
        const double now = animation_seconds(), dt = now - frame_clock;
        frame_clock = now;
        bool changed = false;
        for (auto &[id, c] : items) {
            changed |= c.wake.advance(dt);
            changed |= c.elastic.advance(dt);
            changed |= c.switch_motion.advance(dt);
        }
        if (changed)
            request_frame();
        bool active = false;
        for (auto &[id, c] : items)
            active |= c.wake.active() || c.elastic.active() || c.switch_motion.active();
        if (!active)
            stop_animation();
    }
    static LRESULT CALLBACK child_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
        auto *self = reinterpret_cast<Impl *>(data);
        try {
            return self->child_message(h, m, w, l, static_cast<int>(id));
        } catch (const winrt::hresult_error &e) {
            self->report(winrt::to_string(e.message()).c_str());
            return 0;
        } catch (const std::exception &e) {
            self->report(e.what());
            return 0;
        }
    }
    void report(const char *message) {
        if (!error_reported) {
            error_reported = true;
            MessageBoxW(hwnd, wtext(message).c_str(), L"SonyConnect 设置界面错误", MB_OK | MB_ICONERROR);
        }
    }
    LRESULT child_message(HWND h, UINT m, WPARAM w, LPARAM l, int id) {
        auto found = items.find(id);
        if (found == items.end())
            return DefSubclassProc(h, m, w, l);
        auto &c = found->second;
        if (m == WM_KEYDOWN && w == VK_F1) {
            tips.inspect(hwnd, h);
            return 0;
        }
        if (c.kind == numeric && m == WM_SETFOCUS) {
            const auto result = DefSubclassProc(h, m, w, l);
            HideCaret(h);
            SendMessageW(h, EM_SETSEL, 0, -1);
            const int top = static_cast<int>(c.box.y) - scroll;
            if (top < 44) scroll = std::max(0, static_cast<int>(c.box.y) - 48);
            else if (top + c.box.h > height) scroll = std::min(content_height() - height, static_cast<int>(c.box.y + c.box.h) - height + 6);
            layout(); request_frame();
            return result;
        }
        if (c.kind == numeric && m == WM_KEYDOWN && w == VK_RETURN) { click(apply_log_id); return 0; }
        if (c.kind == numeric && m == WM_KEYDOWN && w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(h, EM_SETSEL, 0, -1); request_frame(); return 0;
        }
        if (c.kind == timeline) {
            if (m == BM_CLICK) return 0;
            if (m == WM_MOUSEWHEEL) {
                const int top = std::clamp(timeline_top - timeline_wheel.consume(GET_WHEEL_DELTA_WPARAM(w)), 0, timeline_max_top());
                timeline_position(std::clamp(timeline_selected, top, top + timeline_rows() - 1), top);
                return 0;
            }
            if (m == WM_LBUTTONDOWN) {
                keyboard = false; SetFocus(h);
                const float x = GET_X_LPARAM(l) / scale(), y = GET_Y_LPARAM(l) / scale();
                if (x > c.box.w - 18 && timeline_max_top()) {
                    const float thumb = std::max(24.f, (c.box.h - 12) * timeline_rows() / timeline_count());
                    const float current = 6 + (c.box.h - 12 - thumb) * timeline_top / timeline_max_top();
                    timeline_grab = y >= current && y <= current + thumb ? y - current : thumb / 2;
                    timeline_dragging = true; SetCapture(h);
                    const int top = static_cast<int>(std::lround((y - 6 - timeline_grab) * timeline_max_top() / (c.box.h - 12 - thumb)));
                    timeline_position(timeline_selected, top);
                } else timeline_position(timeline_top + std::clamp(static_cast<int>((y - 6) / 71), 0, timeline_rows() - 1), timeline_top);
                return 0;
            }
            if (m == WM_MOUSEMOVE && timeline_dragging) {
                const float thumb = std::max(24.f, (c.box.h - 12) * timeline_rows() / timeline_count());
                const int top = static_cast<int>(std::lround((GET_Y_LPARAM(l) / scale() - 6 - timeline_grab) * timeline_max_top() / (c.box.h - 12 - thumb)));
                timeline_position(timeline_selected, top); return 0;
            }
            if (m == WM_LBUTTONUP || m == WM_CAPTURECHANGED || m == WM_CANCELMODE) { cancel_gesture(h); return 0; }
            if (m == WM_LBUTTONDBLCLK || (m == WM_KEYDOWN && (w == VK_RETURN || w == VK_SPACE))) {
                tips.inspect(hwnd, h); return 0;
            }
            if (m == WM_KEYDOWN && (w == VK_UP || w == VK_DOWN || w == VK_HOME || w == VK_END || w == VK_PRIOR || w == VK_NEXT)) {
                const int selected = w == VK_HOME ? 0 : w == VK_END ? timeline_count() - 1
                    : std::clamp(timeline_selected + (w == VK_UP ? -1 : w == VK_DOWN ? 1 : w == VK_PRIOR ? -timeline_rows() : timeline_rows()), 0, std::max(0, timeline_count() - 1));
                const int top = selected < timeline_top ? selected : selected >= timeline_top + timeline_rows() ? selected - timeline_rows() + 1 : timeline_top;
                timeline_position(selected, top); return 0;
            }
        }
        if (c.kind == status_text && m == WM_NCHITTEST)
            return HTTRANSPARENT;
        if (m == WM_PAINT) {
            ValidateRect(h, nullptr);
            return 0;
        }
        if (m == WM_ERASEBKGND)
            return 1;
        if (m == WM_PRINT || m == WM_PRINTCLIENT)
            return 0;
        if (c.kind == menu_scrollbar) {
            if (m == WM_LBUTTONDOWN) {
                const float y = GET_Y_LPARAM(l) / scale();
                const float thumb_top = (c.box.h - menu_thumb_height()) * menu_offset / menu_max_offset();
                menu_scroll_grab = y >= thumb_top && y <= thumb_top + menu_thumb_height()
                                       ? y - thumb_top : menu_thumb_height() / 2;
                menu_scrolling = true;
                SetCapture(h);
                drag_menu_scrollbar(y);
                return 0;
            }
            if (m == WM_MOUSEMOVE && menu_scrolling) {
                drag_menu_scrollbar(GET_Y_LPARAM(l) / scale());
                return 0;
            }
            if (m == WM_LBUTTONUP && menu_scrolling) {
                menu_scrolling = false;
                if (GetCapture() == h) ReleaseCapture();
                return 0;
            }
            if (m == WM_CAPTURECHANGED || m == WM_CANCELMODE) menu_scrolling = false;
            if (m == WM_MOUSEWHEEL) SendMessageW(hwnd, m, w, l);
            return 0;
        }
        if (c.slider && m == WM_MOUSEMOVE && !c.hover) {
            c.hover = true;
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0};
            TrackMouseEvent(&track);
            c.elastic.set_active(true);
            if (reduced)
                c.elastic.advance(1);
            else
                start_animation();
            request_frame();
        }
        if (c.slider && m == WM_MOUSELEAVE) {
            c.hover = false;
            c.elastic.set_active(c.dragging || (keyboard && GetFocus() == h));
            if (reduced)
                c.elastic.advance(1);
            else
                start_animation();
            request_frame();
        }
        if (m == WM_SETFOCUS) {
            if (c.slider) {
                c.elastic.set_active(true);
                if (reduced)
                    c.elastic.advance(1);
                else
                    start_animation();
            }
            if (id < menu_id && id != close_id) {
                const int top = static_cast<int>(c.box.y) - scroll,
                          bottom = static_cast<int>(c.box.y + c.box.h) - scroll;
                if (top < 44)
                    scroll = std::max(0, static_cast<int>(c.box.y) - 48);
                else if (bottom > height - 6)
                    scroll = std::min(content_height() - height, static_cast<int>(c.box.y + c.box.h) - height + 6);
                layout();
            }
            request_frame();
        }
        if (m == WM_KILLFOCUS || m == WM_CANCELMODE || m == WM_CAPTURECHANGED) {
            if (c.slider) {
                c.elastic.set_active(c.hover);
                if (reduced)
                    c.elastic.advance(1);
                else
                    start_animation();
            }
            if (c.dragging && (m != WM_CAPTURECHANGED || reinterpret_cast<HWND>(l) != h))
                cancel_gesture(h);
            request_frame();
        }
        if (m == WM_LBUTTONDOWN) {
            keyboard = false;
            if (id < menu_id && id != active_menu)
                close_menu();
        }
        // The popup anchors respond on press. They never enter the native
        // owner's pressed/capture state or wait for a later BN_CLICKED/frame.
        if (c.kind == choice) {
            if (m == BM_CLICK) {
                click(id);
                return 0;
            }
            if (m == WM_LBUTTONDOWN || m == WM_LBUTTONDBLCLK) {
                if (c.enabled) {
                    SetFocus(h);
                    click(id);
                }
                return 0;
            }
            if (m == WM_LBUTTONUP || (m == WM_KEYUP && w == VK_SPACE)) return 0;
        }
        if (m == WM_KEYDOWN) {
            keyboard = true;
            if (w == VK_ESCAPE) {
                escape();
                return 0;
            }
            if (id >= menu_id && (w == VK_UP || w == VK_DOWN || w == VK_HOME || w == VK_END ||
                                  w == VK_PRIOR || w == VK_NEXT)) {
                menu_cursor = w == VK_HOME ? 0 : w == VK_END ? static_cast<int>(menu_choices.size()) - 1
                    : std::clamp(menu_cursor + (w == VK_UP ? -1 : w == VK_DOWN ? 1
                                              : w == VK_PRIOR ? -menu_rows : menu_rows),
                                 0, static_cast<int>(menu_choices.size()) - 1);
                menu_offset =
                    std::clamp(menu_cursor - menu_rows / 2, 0, menu_max_offset());
                layout_menu();
                SetFocus(items.at(menu_id + menu_cursor - menu_offset).hwnd);
                return 0;
            }
            if ((w == VK_RETURN || w == VK_SPACE) && !c.slider && c.kind != numeric) {
                click(id);
                return 0;
            }
        }
        if (m == WM_MOUSEWHEEL) {
            if (GetKeyState(VK_CONTROL) & 0x8000)
                return 0;
            if (c.id == prompt_id && active_menu < 0)
                return 0;
            if (c.slider && c.enabled) {
                wheel(c, GET_WHEEL_DELTA_WPARAM(w));
                return 0;
            }
            SendMessageW(hwnd, m, w, l);
            return 0;
        }
        if (c.slider) {
            if (m == WM_LBUTTONDOWN && c.enabled) {
                SetFocus(h);
                c.dragging = true;
                SetCapture(h);
                drag(c, l, false);
                return 0;
            }
            if (m == WM_MOUSEMOVE && c.dragging) {
                drag(c, l, false);
                return 0;
            }
            if (m == WM_LBUTTONUP && c.dragging) {
                c.dragging = false;
                drag(c, l, true);
                if (GetCapture() == h)
                    ReleaseCapture();
                return 0;
            }
            if (m == WM_KEYDOWN && c.enabled &&
                (w == VK_LEFT || w == VK_DOWN || w == VK_RIGHT || w == VK_UP || w == VK_HOME ||
                 w == VK_END)) {
                c.value = w == VK_HOME  ? c.minimum
                          : w == VK_END ? c.maximum
                                        : std::clamp(c.value + ((w == VK_LEFT || w == VK_DOWN) ? -1 : 1),
                                                     c.minimum, c.maximum);
                const double pct = 100.0 * (c.value - c.minimum) / (c.maximum - c.minimum);
                if (reduced) {
                    c.wake.reset(pct);
                    c.elastic.reset(pct);
                } else {
                    c.wake.set_target(pct);
                    c.elastic.set_target(pct);
                }
                layout();
                request_frame();
                start_animation();
                commit_slider(c);
                return 0;
            }
        }
        // Native button notifications can close the menu and erase c before
        // DefSubclassProc returns. Do not retain an Item reference across it.
        const bool refresh_numeric = c.kind == numeric &&
            (m == WM_KEYDOWN || m == WM_CHAR || m == WM_LBUTTONDOWN || m == WM_LBUTTONUP || m == WM_PASTE || m == EM_SETSEL);
        const auto result = DefSubclassProc(h, m, w, l);
        if (refresh_numeric)
            request_frame();
        return result;
    }
    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto *self = reinterpret_cast<Impl *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            self = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            self->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(h, m, w, l);
        try {
            return self->message(m, w, l);
        } catch (const winrt::hresult_error &e) {
            self->report(winrt::to_string(e.message()).c_str());
            return 0;
        } catch (const std::exception &e) {
            self->report(e.what());
            return 0;
        }
    }
    LRESULT message(UINT m, WPARAM w, LPARAM l) {
        if (m == frame_id) {
            posted = false;
            present();
            return 0;
        }
        switch (m) {
        case WM_NOTIFY:
            if (tips.notify(l)) return 0;
            break;
        case WM_SHOWWINDOW:
            if (!w) pause_visuals();
            break;
        case WM_POWERBROADCAST:
            if (w == PBT_APMSUSPEND) { suspended = true; pause_visuals(); }
            else if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) {
                suspended = false;
                if (IsWindowVisible(hwnd)) resume_prompt_cooldown();
                update(model);
                resize();
            }
            return TRUE;
        case WM_PAINT: {
            PaintScope paint(hwnd);
            request_frame();
            return 0;
        }
        case repaint_message:
            request_frame();
            return 0;
        case WM_PRINT:
        case WM_PRINTCLIENT:
            render();
            BitBlt(reinterpret_cast<HDC>(w), 0, 0, px(width()), px(static_cast<float>(height)), painter->canvas,
                   0, 0, SRCCOPY);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DRAWITEM:
            return TRUE;
        case WM_MOUSEACTIVATE:
            return MA_ACTIVATE;
        case WM_COMMAND:
            if (HIWORD(w) == EN_CHANGE && items.contains(LOWORD(w)) && items.at(LOWORD(w)).kind == numeric) {
                auto &input = items.at(LOWORD(w));
                if (!input.numeric_syncing) { input.numeric_dirty = true; numeric_error.clear(); update_diagnostics(model); }
                return 0;
            }
            if (HIWORD(w) == BN_CLICKED && items.contains(LOWORD(w)))
                click(LOWORD(w));
            return 0;
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ScreenToClient(hwnd, &pt);
            return pt.y < px(44) ? HTCAPTION : HTCLIENT;
        }
        case WM_SYSCOMMAND:
            if ((w & 0xfff0) == SC_SIZE || (w & 0xfff0) == SC_MAXIMIZE)
                return 0;
            break;
        case WM_NCLBUTTONDBLCLK:
            return 0;
        case WM_GETMINMAXINFO: {
            auto *v = reinterpret_cast<MINMAXINFO *>(l);
            v->ptMinTrackSize = v->ptMaxTrackSize = {px(width()), px(static_cast<float>(height))};
            return 0;
        }
        case WM_DPICHANGED: {
            cancel_gesture();
            tips.dismiss();
            if (!forced_dpi)
                dpi = HIWORD(w);
            if (const auto *r = reinterpret_cast<const RECT *>(l))
                SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW);
            close_menu();
            resize();
            return 0;
        }
        case WM_DISPLAYCHANGE:
            cancel_gesture();
            tips.dismiss();
            close_menu();
            resize();
            return 0;
        case WM_SETTINGCHANGE: {
            BOOL animate = TRUE;
            SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0);
            reduced = !animate;
            if (reduced) {
                stop_animation();
                for (auto &[id, c] : items) {
                    c.switch_motion.reset(c.on);
                    const double pct = 100.0 * (c.value - c.minimum) / std::max(1, c.maximum - c.minimum);
                    c.wake.reset(pct);
                    c.elastic.reset(pct);
                }
            }
            resize();
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (GetKeyState(VK_CONTROL) & 0x8000)
                return 0;
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ScreenToClient(hwnd, &pt);
            if (active_menu >= 0) {
                scroll_menu(menu_offset - (GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1 : -1));
                return 0;
            }
            if (page == SettingsPage::replay && PtInRect(&items.at(timeline_id).presented, pt)) {
                SendMessageW(items.at(timeline_id).hwnd, WM_MOUSEWHEEL, w, l);
                return 0;
            }
            for (auto &[id, c] : items)
                if (c.slider && c.enabled && PtInRect(&c.presented, pt)) {
                    if (c.id == prompt_id) return 0;
                    wheel(c, GET_WHEEL_DELTA_WPARAM(w));
                    return 0;
                }
            scroll =
                std::clamp(scroll - scroll_wheel.consume(GET_WHEEL_DELTA_WPARAM(w)) * 48, 0, content_height() - height);
            layout();
            request_frame();
            return 0;
        }
        case WM_LBUTTONDOWN:
            close_menu();
            return 0;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) {
                escape();
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(w) == WA_INACTIVE) {
                close_menu(false);
                cancel_gesture();
            }
            break;
        case WM_ACTIVATEAPP:
            if (!w) {
                close_menu(false);
                cancel_gesture();
            }
            break;
        case WM_CANCELMODE:
            close_menu();
            cancel_gesture();
            return 0;
        case WM_TIMER:
            if (w == 1)
                tick();
            else if (w == 2)
                flush_wheels();
            else if (w == prompt_cooldown_timer) {
                if (GetTickCount64() >= prompt_cooldown_until) {
                    KillTimer(hwnd, prompt_cooldown_timer);
                    prompt_cooldown_until = 0;
                    update(model);
                }
            }
            return 0;
        case WM_CLOSE:
            hide_window();
            return 0;
        case WM_DESTROY:
            stop_animation();
            KillTimer(hwnd, 2);
            KillTimer(hwnd, prompt_cooldown_timer);
            return 0;
        }
        return DefWindowProcW(hwnd, m, w, l);
    }
};
MoreWindow::MoreWindow(Dispatch d, SettingsPage page) : impl_(std::make_unique<Impl>(std::move(d), page)) {}
MoreWindow::~MoreWindow() = default;
void MoreWindow::create(HINSTANCE i, HWND owner, UINT dpi) { impl_->create(i, owner, dpi); }
HWND MoreWindow::handle() const { return impl_->hwnd; }
void MoreWindow::update(MoreModel m) { impl_->update(std::move(m)); }
void MoreWindow::show() {
    impl_->resume_prompt_cooldown();
    impl_->update(impl_->model);
    impl_->resize();
    ShowWindow(impl_->hwnd, SW_SHOW);
    SetForegroundWindow(impl_->hwnd);
    impl_->request_frame();
}
void MoreWindow::hide() { impl_->hide_window(); }
bool MoreWindow::pre_translate(MSG &m) {
    if (m.message == WM_KEYDOWN) {
        impl_->keyboard = true;
        if (m.wParam == VK_F1) {
            impl_->tips.inspect(impl_->hwnd, m.hwnd);
            return true;
        }
        const auto focused = impl_->items.find(GetDlgCtrlID(m.hwnd));
        if (focused != impl_->items.end() &&
            ((focused->second.kind == Impl::numeric && m.wParam == VK_RETURN) ||
             (focused->second.kind == Impl::timeline && (m.wParam == VK_UP || m.wParam == VK_DOWN ||
              m.wParam == VK_HOME || m.wParam == VK_END || m.wParam == VK_PRIOR || m.wParam == VK_NEXT ||
              m.wParam == VK_RETURN || m.wParam == VK_SPACE)))) {
            SendMessageW(m.hwnd, m.message, m.wParam, m.lParam);
            return true;
        }
        if (m.wParam == VK_ESCAPE) {
            impl_->escape();
            return true;
        }
        if (impl_->active_menu >= 0 &&
            (m.wParam == VK_UP || m.wParam == VK_DOWN || m.wParam == VK_HOME || m.wParam == VK_END ||
             m.wParam == VK_PRIOR || m.wParam == VK_NEXT || m.wParam == VK_RETURN || m.wParam == VK_SPACE)) {
            SendMessageW(m.hwnd, m.message, m.wParam, m.lParam);
            return true;
        }
        if (m.wParam == VK_TAB && impl_->active_menu >= 0)
            impl_->close_menu();
        impl_->request_frame();
    }
    return IsDialogMessageW(impl_->hwnd, &m) != FALSE;
}
void MoreWindow::capture(const std::filesystem::path &path) {
    auto &s = *impl_;
    s.render();
    winrt::com_ptr<IWICImagingFactory> f;
    winrt::check_hresult(
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(f.put())));
    winrt::com_ptr<IWICBitmap> bitmap;
    winrt::check_hresult(
        f->CreateBitmapFromHBITMAP(s.painter->canvas_bitmap, nullptr, WICBitmapIgnoreAlpha, bitmap.put()));
    winrt::com_ptr<IWICBitmapClipper> clip;
    winrt::check_hresult(f->CreateBitmapClipper(clip.put()));
    WICRect rect{0, 0, s.px(s.width()), s.px(static_cast<float>(s.height))};
    winrt::check_hresult(clip->Initialize(bitmap.get(), &rect));
    winrt::com_ptr<IWICStream> stream;
    winrt::check_hresult(f->CreateStream(stream.put()));
    winrt::check_hresult(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    winrt::com_ptr<IWICBitmapEncoder> encoder;
    winrt::check_hresult(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
    winrt::check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
    winrt::com_ptr<IWICBitmapFrameEncode> frame;
    winrt::check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
    winrt::check_hresult(frame->Initialize(nullptr));
    winrt::check_hresult(frame->WriteSource(clip.get(), nullptr));
    winrt::check_hresult(frame->Commit());
    winrt::check_hresult(encoder->Commit());
}
MoreControlPresentation more_control_presentation(const DeviceSnapshot &device) {
    MoreControlPresentation result;
    result.no_touch_sensor = device.model == "WH-CH720N";
    result.physical_nc_button = result.no_touch_sensor || device.model == "WH-1000XM5";
    if (result.physical_nc_button)
        result.cycle_label = L"[NC/AMB] 按钮切换的声音模式";
    if (device.model == "WF-1000XM5" || device.model == "WH-1000XM5")
        result.dsee_label = L"DSEE Extreme";
    return result;
}
MoreModel more_preview(MainSliderStyle style) {
    MoreModel m;
    m.device = main_preview(L"connected").device;
    m.slider_style = style;
    m.preview = true;
    m.device.equalizer.preset = known(0xa2);
    m.device.equalizer.bands = known(std::vector<int>{2, 1, 0, 2, 3});
    m.device.equalizer.clear_bass = known(4);
    m.device.equalizer.presets = {
        {0, "关闭"}, {0x10, "明亮"}, {0xa0, "手动"}, {0xa1, "自定义 1"}, {0xa2, "自定义 2"}};
    m.device.equalizer.writable = m.device.equalizer.available = m.device.equalizer.five_band_layout = true;
    for (auto feature : {protocol::Feature::Dsee, protocol::Feature::SpeakToChat,
                         protocol::Feature::WearDetectionPause, protocol::Feature::AutoPowerOff}) {
        auto &f = m.device.features[feature];
        f.advertised = f.available = f.writable = true;
        f.value = known(feature == protocol::Feature::AutoPowerOff  ? 0x10
                        : feature == protocol::Feature::SpeakToChat ? 1
                                                                    : 0);
        f.choices = feature == protocol::Feature::AutoPowerOff ? std::vector<int>{0x10, 0x11}
                                                               : std::vector<int>{0, 1};
    }
    m.auto_power_choices = {{0x10, L"摘下耳机后自动关机"}, {0x11, L"不自动关机"}};
    m.device.touch.guidance = known(0);
    m.device.touch.prompt_volume = known(0);
    m.device.touch.cycle = known(1);
    m.device.touch.assignments = known(protocol::Bytes{0, 0x20});
    m.device.touch.keys = {{0, 0, 0, {{0}, {0x20}, {0x10}, {0xff}}},
                           {1, 0, 0x20, {{0}, {0x20}, {0x10}, {0xff}}}};
    m.device.touch.guidance_writable = m.device.touch.volume_writable = m.device.touch.cycle_writable =
        m.device.touch.writable = true;
    m.touch_names = {L"环境声音控制", L"播放控制"};
    m.touch_indices = {0, 1};
    for (auto &c : m.touch_choices)
        c = {{0, L"环境声音控制"}, {0x20, L"播放控制"}, {0x10, L"音量调节"}, {0xff, L"无功能"}};
    m.close_to_tray = true;
    return m;
}
} // namespace sonyconnect::ui
