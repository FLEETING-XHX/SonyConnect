#pragma once
#include <d2d1.h>
#include <memory>

namespace sonyconnect::ui {
// Canonical icon names from the current Pen document. Geometry is the exported
// Lucide font outline, so its shape matches Pen rather than a hand-drawn proxy.
enum class PenIcon {
    none,
    activity,
    arrow_up_right,
    audio_lines,
    battery_medium,
    bluetooth,
    check,
    chevron_down,
    chevron_right,
    circle_alert,
    download,
    ear,
    file_text,
    hand,
    headphones,
    history,
    keyboard,
    laptop_minimal,
    loader_circle,
    lock_keyhole,
    log_out,
    mic,
    minus,
    monitor,
    music_2,
    panel_top,
    pause,
    pencil,
    play,
    plus,
    power,
    radio,
    rotate_ccw,
    rotate_cw,
    settings_2,
    shield_check,
    skip_back,
    skip_forward,
    sliders_horizontal,
    smartphone,
    volume_1,
    volume_2,
    x,
    count
};

inline constexpr wchar_t pen_font_family[] = L"Noto Sans SC";

// Own alongside the D2D factory. Geometry is created lazily once per icon and
// remains valid when a device-dependent render target is recreated.
class PenIcons {
  public:
    explicit PenIcons(ID2D1Factory *factory);
    ~PenIcons();
    PenIcons(const PenIcons &) = delete;
    PenIcons &operator=(const PenIcons &) = delete;
    PenIcons(PenIcons &&) noexcept;
    PenIcons &operator=(PenIcons &&) noexcept;

    // x/y/size are layout DIPs. The existing target transform (including DPI),
    // brush color and opacity are preserved. No external SVG/font file is read.
    void draw(ID2D1RenderTarget *target, ID2D1SolidColorBrush *brush, PenIcon icon,
              float x, float y, float size, const D2D1_COLOR_F &color);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

const char *pen_icon_name(PenIcon icon) noexcept;
} // namespace sonyconnect::ui
