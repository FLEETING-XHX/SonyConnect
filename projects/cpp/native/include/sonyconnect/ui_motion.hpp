#pragma once
// Native presentation adapters for the user-supplied smooth React controls.
// Sources: SonyConnect_4_Controls_Pen_4C5ED6_Wheel. See native/notices/React-Bits-NOTICE.txt.
// No device state, I/O, or request scheduling belongs in this module.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

namespace sonyconnect::ui::motion {
namespace detail {
inline double finite_clamp(double value, double low, double high) noexcept {
    return std::isfinite(value) ? std::clamp(value, low, high) : low;
}
// The supplied CSS/Motion cubic-bezier(0.23,1,0.32,1), evaluated in time space.
inline double ease_out(double progress) noexcept {
    const double x = std::clamp(progress, 0.0, 1.0);
    if (x == 0.0 || x == 1.0)
        return x;
    double low = 0, high = 1, t = x;
    for (int i = 0; i < 28; ++i) {
        double u = 1 - t;
        double at = 3 * u * u * t * .23 + 3 * u * t * t * .32 + t * t * t;
        if (at < x)
            low = t;
        else
            high = t;
        t = (low + high) * .5;
    }
    return 1 - std::pow(1 - t, 3);
}
} // namespace detail

class Tween {
  public:
    void reset(double value) noexcept {
        value_ = from_ = to_ = std::isfinite(value) ? value : 0;
        elapsed_ = duration_ = 0;
    }
    void set_target(double value, double duration = .22) noexcept {
        if (!std::isfinite(value) || value == to_)
            return;
        from_ = value_;
        to_ = value;
        elapsed_ = 0;
        duration_ = std::isfinite(duration) ? std::max(0.0, duration) : 0;
        if (duration_ == 0)
            reset(to_);
    }
    bool advance(double seconds) noexcept {
        if (!(seconds > 0) || !std::isfinite(seconds) || !active())
            return false;
        elapsed_ = std::min(duration_, elapsed_ + seconds);
        value_ = from_ + (to_ - from_) * detail::ease_out(elapsed_ / duration_);
        if (elapsed_ >= duration_)
            value_ = to_;
        return true;
    }
    double value() const noexcept { return value_; }
    double target() const noexcept { return to_; }
    bool active() const noexcept { return elapsed_ < duration_; }

  private:
    double value_{}, from_{}, to_{}, elapsed_{}, duration_{};
};

struct WakeCorners {
    double x{}, y{};
};
inline WakeCorners wake_corners(double width, double height = 12) noexcept {
    if (!(width > 0) || !(height > 0) || !std::isfinite(width + height))
        return {};
    // The updated design uses straight sides and small circular corners, with
    // no scaleY distortion or half-width capsule ends (including narrow bars).
    const double radius = std::min(2.0, std::min(width, height) * .18);
    return {radius, radius};
}
class Wake {
  public:
    void reset(double pct) noexcept { position_.reset(detail::finite_clamp(pct, 0, 100)); }
    void set_target(double pct, double duration = .22) noexcept {
        position_.set_target(detail::finite_clamp(pct, 0, 100), duration);
    }
    bool advance(double seconds) noexcept { return position_.advance(seconds); }
    double head() const noexcept { return position_.value(); }
    bool active() const noexcept { return position_.active(); }
    double bar_height(std::size_t index, std::size_t bars, double max_height = 24,
                      double rest_height = 12) const noexcept {
        if (!bars || index >= bars || !(max_height > 0) || !std::isfinite(max_height))
            return 0;
        return detail::finite_clamp(rest_height, 0, max_height);
    }

  private:
    Tween position_;
};

class Segment {
  public:
    explicit Segment(double stride = 136, double thumb_width = 132, int slots = 3) noexcept
        : stride_(stride > 0 && std::isfinite(stride) ? stride : 136),
          width_(thumb_width > 0 && std::isfinite(thumb_width) ? thumb_width : 132),
          slots_(std::max(1, slots)) {
        reset(0);
    }
    void reset(int index) noexcept {
        index_ = std::clamp(index, 0, slots_ - 1);
        left_.reset(index_ * stride_);
    }
    void set_target(int index) noexcept {
        index_ = std::clamp(index, 0, slots_ - 1);
        left_.set_target(index_ * stride_, .22);
    }
    bool advance(double seconds) noexcept { return left_.advance(seconds); }
    double left() const noexcept { return left_.value(); }
    double right() const noexcept { return left() + width_; }
    int target() const noexcept { return index_; }
    bool active() const noexcept { return left_.active(); }

  private:
    double stride_, width_;
    int slots_, index_{};
    Tween left_;
};

class Switch {
  public:
    void reset(bool on) noexcept {
        target_ = on;
        position_.reset(on ? 1 : 0);
        tint_.reset(on ? 1 : 0);
    }
    void set_target(bool on) noexcept {
        target_ = on;
        position_.set_target(on ? 1 : 0, .22);
        tint_.set_target(on ? 1 : 0, .18);
    }
    void drag(double position) noexcept { position_.reset(detail::finite_clamp(position, 0, 1)); }
    bool advance(double seconds) noexcept {
        const bool a = position_.advance(seconds);
        return tint_.advance(seconds) || a;
    }
    double position() const noexcept { return position_.value(); }
    double tint() const noexcept { return tint_.value(); }
    bool target() const noexcept { return target_; }
    bool active() const noexcept { return position_.active() || tint_.active(); }

  private:
    bool target_{};
    Tween position_, tint_;
};

class Elastic {
  public:
    void reset(double pct) noexcept {
        position_.reset(detail::finite_clamp(pct, 0, 100));
        thickness_.reset(6);
    }
    void jump(double pct) noexcept { position_.reset(detail::finite_clamp(pct, 0, 100)); }
    void set_target(double pct, double duration = .22) noexcept {
        position_.set_target(detail::finite_clamp(pct, 0, 100), duration);
    }
    void set_active(bool active) noexcept { thickness_.set_target(active ? 12 : 6, .16); }
    bool advance(double seconds) noexcept {
        const bool a = position_.advance(seconds);
        return thickness_.advance(seconds) || a;
    }
    double head() const noexcept { return position_.value(); }
    double thickness() const noexcept { return thickness_.value(); }
    bool active() const noexcept { return position_.active() || thickness_.active(); }

  private:
    Tween position_, thickness_;
};

// One instance per numeric hit region. Signed high-resolution deltas survive
// between messages and cannot leak from one slider/EQ band into another.
class WheelAccumulator {
  public:
    int consume(int delta) noexcept {
        const std::int64_t sum = static_cast<std::int64_t>(remainder_) + delta;
        remainder_ = static_cast<int>(sum % 120);
        return static_cast<int>(sum / 120);
    }
    void reset() noexcept { remainder_ = 0; }
    int remainder() const noexcept { return remainder_; }

  private:
    int remainder_{};
};
struct Point {
    double x{}, y{};
};

// Start at the top centre and travel counterclockwise. Both the beginning and
// final part of the hold are visible above the label; completion closes the loop.
inline std::vector<Point> outline_ccw(double width, double height, double radius, double progress,
                                      double max_step = 2.0) {
    if (!(width > 0.0) || !(height > 0.0) || !std::isfinite(width) || !std::isfinite(height))
        return {};
    const double r = detail::finite_clamp(radius, 0.0, std::min(width, height) * 0.5);
    const double fraction = detail::finite_clamp(progress, 0.0, 1.0);
    if (fraction == 0.0)
        return {};
    const double vertical = height - 2.0 * r, horizontal = width - 2.0 * r;
    const double arc = r * std::numbers::pi * 0.5;
    const double perimeter = 2.0 * (vertical + horizontal) + 4.0 * arc;
    const double length = perimeter * fraction;
    const double spacing = detail::finite_clamp(max_step, 0.25, 32.0);
    const auto count = static_cast<std::size_t>(std::ceil(length / spacing));
    std::vector<Point> points;
    points.reserve(count + 1);
    const auto sample = [&](double d) -> Point {
        if (d <= vertical)
            return {0.0, r + d};
        d -= vertical;
        if (r > 0.0 && d <= arc) {
            const double angle = std::numbers::pi - d / r;
            return {r + r * std::cos(angle), height - r + r * std::sin(angle)};
        }
        d -= arc;
        if (d <= horizontal)
            return {r + d, height};
        d -= horizontal;
        if (r > 0.0 && d <= arc) {
            const double angle = std::numbers::pi * 0.5 - d / r;
            return {width - r + r * std::cos(angle), height - r + r * std::sin(angle)};
        }
        d -= arc;
        if (d <= vertical)
            return {width, height - r - d};
        d -= vertical;
        if (r > 0.0 && d <= arc) {
            const double angle = -d / r;
            return {width - r + r * std::cos(angle), r + r * std::sin(angle)};
        }
        d -= arc;
        if (d <= horizontal)
            return {width - r - d, 0.0};
        d -= horizontal;
        const double angle = -std::numbers::pi * 0.5 - (r > 0.0 ? d / r : 0.0);
        return {r + r * std::cos(angle), r + r * std::sin(angle)};
    };
    const double start = vertical + arc + horizontal + arc + vertical + arc + horizontal * .5;
    for (std::size_t i = 0; i <= count; ++i) {
        const double d =
            std::fmod(start + length * static_cast<double>(i) / static_cast<double>(count), perimeter);
        points.push_back(sample(d));
    }
    return points;
}
} // namespace sonyconnect::ui::motion
