#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace sonyconnect::ui {
// Product zoom is independent of Windows DPI; layout stays in logical units.
inline constexpr float interface_scale = 0.85f;
// Physical desktop coordinates may be negative. Keep placement independent of
// monitor enumeration so taskbar/workarea arrangements can be replayed offline.
struct DesktopRect {
    int left{}, top{}, right{}, bottom{};
    int width() const { return right - left; }
    int height() const { return bottom - top; }
};
inline DesktopRect fit_desktop(DesktopRect work, int x, int y, int width, int height) {
    x = std::clamp(x, work.left, std::max(work.left, work.right - width));
    y = std::clamp(y, work.top, std::max(work.top, work.bottom - height));
    return {x, y, x + width, y + height};
}
inline DesktopRect anchor_settings(DesktopRect work, DesktopRect owner, int width, int height, int gap) {
    int x = owner.left - gap - width;
    if (x < work.left) x = owner.right + gap;
    return fit_desktop(work, x, owner.bottom - height, width, height);
}
inline float fit_window_scale(unsigned dpi, int logical_width, int physical_work_width) {
    return std::min(dpi / 96.f, std::max(1, physical_work_width) / static_cast<float>(std::max(1, logical_width)));
}
struct PopupPlacement {
    float left{}, top{}, width{};
    int rows{};
    bool above{};
};
inline PopupPlacement place_popup(float x, float y, float width, float height,
                                   float viewport_width, float viewport_height, int options) {
    constexpr float row = 32, margin = 8, header = 48, gap = 4;
    const float below = std::max(0.f, viewport_height - margin - y - height - gap);
    const float above = std::max(0.f, y - gap - header);
    const int desired = std::min(6, std::max(1, options));
    const bool up = below < desired * row && above > below;
    const int rows = std::clamp(static_cast<int>((up ? above : below) / row), 1, desired);
    width = std::min(std::max(152.f, width), viewport_width - 2 * margin);
    const float top = std::clamp(up ? y - gap - rows * row : y + height + gap,
                                 header, std::max(header, viewport_height - margin - rows * row));
    return {std::clamp(x, margin, std::max(margin, viewport_width - margin - width)), top, width, rows, up};
}
// Presentation-only keys and measured rows. Future controls register their own
// measured row here; neither the window nor the animator knows device models.
struct LayoutModule {
    int key{};
    float height{};
    bool operator==(const LayoutModule &) const = default;
};
struct LayoutSlot {
    int key{};
    float top{}, height{}, full_height{};
};
class ModuleFlow {
    struct Entry { int key{}; float from{}, current{}, target{}, full{}; };
    std::vector<Entry> entries_;
    std::vector<LayoutModule> target_;
  public:
    bool retarget(const std::vector<LayoutModule> &modules) {
        std::vector<LayoutModule> measured;
        for (const auto &m : modules)
            if (m.height > 0 && std::isfinite(m.height) &&
                std::none_of(measured.begin(), measured.end(), [&](auto &e) { return e.key == m.key; }))
                measured.push_back(m);
        if (measured == target_)
            return false;
        std::vector<Entry> next;
        for (const auto &module : measured) {
            if (!(module.height > 0) || !std::isfinite(module.height))
                continue;
            const auto old = std::find_if(entries_.begin(), entries_.end(), [&](auto &e) { return e.key == module.key; });
            const float current = old == entries_.end() ? 0 : old->current;
            next.push_back({module.key, current, current, module.height, module.height});
        }
        // Departing rows retain their former order while collapsing, so later
        // controls move through the released space rather than jumping into it.
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            const auto &old = entries_[i];
            if (std::none_of(next.begin(), next.end(), [&](auto &e) { return e.key == old.key; }))
                next.insert(next.begin() + std::min(i, next.size()),
                            {old.key, old.current, old.current, 0, old.full});
        }
        entries_ = std::move(next);
        target_ = std::move(measured);
        return true;
    }
    void sample(float progress) {
        const float t = std::clamp(progress, 0.f, 1.f);
        for (auto &e : entries_)
            e.current = e.from + (e.target - e.from) * t;
    }
    void rebase() {
        for (auto &e : entries_) e.from = e.current;
    }
    float height() const {
        float sum = 0;
        for (auto &e : entries_) sum += e.current;
        return sum;
    }
    float target_height() const {
        float sum = 0;
        for (auto &e : entries_) sum += e.target;
        return sum;
    }
    LayoutSlot slot(int key, float start = 198) const {
        for (auto &e : entries_) {
            if (e.key == key) return {key, start, e.current, e.full};
            start += e.current;
        }
        return {key, start, 0, 0};
    }
};
struct MainSections {
    float sources{}, bluetooth{}, media{}, footer{}, content{};
    explicit MainSections(float details) {
        sources = 198 + details;
        bluetooth = sources + 147;
        media = bluetooth + 99;
        footer = media + 218;
        content = footer + 58;
    }
};
inline float layout_ease(float t) {
    t = std::clamp(t, 0.f, 1.f);
    return t * t * t * (t * (t * 6 - 15) + 10);
}
} // namespace sonyconnect::ui
