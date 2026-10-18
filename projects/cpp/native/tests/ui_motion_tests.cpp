#include "sonyconnect/ui_motion.hpp"
#include "sonyconnect/ui_layout.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace sonyconnect::ui::motion;
namespace {
void require(bool v, const char *message) {
    if (!v)
        throw std::runtime_error(message);
}
void near(double a, double b, double e, const char *m) {
    require(std::isfinite(a) && std::abs(a - b) <= e, m);
}
template <class T> void advance(T &t, double seconds, int fps) {
    int n = static_cast<int>(std::round(seconds * fps));
    for (int i = 0; i < n; ++i)
        t.advance(seconds / n);
}
} // namespace
int main() {
    try {
        using sonyconnect::ui::ModuleFlow;
        using sonyconnect::ui::MainSections;
        ModuleFlow flow;
        require(flow.retarget({{1, 60}, {2, 42}}), "New module list must start a layout transition");
        near(flow.height(), 0, 0, "New modules begin collapsed");
        flow.sample(.5f);
        near(flow.height(), 51, 0, "Module extents share the displayed transition progress");
        near(flow.slot(2).top, 228, 0, "Later rows use the animated height of earlier rows");
        require(!flow.retarget({{1, 60}, {2, 42}}), "Identical snapshots must not restart layout motion");
        flow.sample(1);
        near(MainSections(flow.height()).content, 822, 0, "Existing Pen layout remains unchanged");
        // Generic extra rows represent future controls only; no device feature
        // or Sony protocol support is asserted by this fixture.
        flow.retarget({{1, 60}, {99, 42}, {2, 42}});
        near(flow.height(), 102, 0, "Inserting an optional control must not jump existing positions");
        flow.sample(.5f);
        near(flow.slot(2).top, 279, 0, "Optional rows move subsequent controls progressively");
        flow.sample(1);
        near(MainSections(flow.height()).content, 864, 0, "Window height derives from all measured rows");
        flow.retarget({{1, 60}, {2, 42}});
        near(flow.slot(2).top, 300, 0, "Departing rows retain their space until animated out");
        flow.sample(.5f);
        const auto flow_interrupted = flow.height();
        flow.retarget({{1, 60}, {99, 42}, {2, 42}});
        near(flow.height(), flow_interrupted, 0, "Interrupted insertion/removal continues at the displayed layout");
        flow.sample(1);
        flow.retarget({});
        flow.sample(1);
        near(flow.height(), 0, 0, "Removing all optional rows collapses the detail section");
        require(!flow.retarget({{1, -1}, {2, std::numeric_limits<float>::quiet_NaN()}}),
                "Invalid measured rows must not trigger perpetual retargeting");
        Wake wake;
        wake.reset(29);
        require(!wake.active(), "Stationary Wake must not schedule frames");
        wake.set_target(100);
        wake.advance(.05);
        require(wake.head() > 29 && wake.head() < 100, "Wake fill must tween toward its requested value");
        for (std::size_t i = 0; i < 32; ++i)
            near(wake.bar_height(i, 32), 12, 0, "New Wake must never raise a wave");
        wake.advance(.3);
        near(wake.head(), 100, 0, "Wake must settle exactly");
        require(!wake.active(), "Idle Wake stops");
        Wake a, b, c;
        a.reset(0);
        b.reset(0);
        c.reset(0);
        a.set_target(100);
        b.set_target(100);
        c.set_target(100);
        advance(a, .1, 30);
        advance(b, .1, 60);
        advance(c, .1, 144);
        near(a.head(), b.head(), 1e-7, "Wake motion must be frame-rate independent");
        near(a.head(), c.head(), 1e-7, "144Hz must preserve the same elapsed-time tween");
        double interrupted = b.head();
        b.set_target(0);
        near(b.head(), interrupted, 0, "Reversal starts at the visible value");
        for (int i = 0; i < 16; ++i) {
            b.advance(.016);
            require(b.head() >= 0 && b.head() <= interrupted, "Reversal must not overshoot");
        }
        b.reset(77);
        near(b.head(), 77, 0, "Drag follows pointer directly");
        require(!b.advance(-1) && !b.advance(std::numeric_limits<double>::quiet_NaN()),
                "Invalid frame intervals must be ignored");
        near(wake.bar_height(32, 32), 0, 0, "Invalid bar bounds are safe");
        for (int bars : {12, 20, 32}) {
            const double width = (352.0 - 4 * (bars - 1)) / bars;
            const auto corner = wake_corners(width);
            near(corner.x, std::min(2.0, std::min(width, 12.0) * .18), 1e-8,
                 "Wake keeps small corners and straight sides at every bar count");
            near(corner.y, corner.x, 0, "Wake corners remain circular without vertical distortion");
        }
        near(wake_corners(-1).x, 0, 0, "Invalid Wake geometry is safe");

        Segment segment;
        segment.set_target(2);
        double last = 0;
        for (int i = 0; i < 20; ++i) {
            segment.advance(.016);
            near(segment.right() - segment.left(), 132, 1e-8, "Segment must never stretch or squash");
            require(segment.left() >= last && segment.left() <= 272,
                    "Segment must move monotonically with no bounce");
            last = segment.left();
        }
        near(segment.left(), 272, 0, "Segment aligns at 220ms");
        require(!segment.active(), "Idle segment stops");
        segment.set_target(0);
        segment.advance(.05);
        double start = segment.left();
        segment.set_target(1);
        near(segment.left(), start, 0, "Segment interruption must not jump");
        segment.advance(1);
        near(segment.left(), 136, 0, "Last target wins");
        Segment s30, s144;
        s30.set_target(2);
        s144.set_target(2);
        advance(s30, .2, 30);
        advance(s144, .2, 144);
        near(s30.left(), s144.left(), 1e-7, "Segment timing is frame independent");

        Switch toggle;
        toggle.reset(false);
        toggle.set_target(true);
        toggle.advance(.05);
        require(toggle.position() > 0 && toggle.position() < 1 && toggle.tint() > 0 && toggle.tint() < 1,
                "Switch thumb and color must interpolate");
        toggle.advance(.3);
        near(toggle.position(), 1, 0, "Switch arrives at on");
        near(toggle.tint(), 1, 0, "Switch color arrives at on");
        toggle.drag(.2);
        near(toggle.position(), .2, 0, "Switch drag follows the pointer");
        toggle.set_target(false);
        toggle.advance(1);
        near(toggle.position(), 0, 0, "Cancelled/off switch returns smoothly");

        Elastic elastic;
        elastic.reset(25);
        near(elastic.thickness(), 6, 0, "Elastic resting thickness");
        elastic.set_active(true);
        elastic.set_target(75);
        elastic.advance(.08);
        require(elastic.thickness() > 6 && elastic.thickness() < 12, "Hover thickness interpolates");
        require(elastic.head() > 25 && elastic.head() < 75, "Elastic fill interpolates without rubber");
        elastic.jump(90);
        near(elastic.head(), 90, 0, "Elastic drag is direct");
        elastic.advance(.3);
        near(elastic.thickness(), 12, 0, "Elastic active thickness");
        elastic.set_active(false);
        elastic.advance(1);
        near(elastic.thickness(), 6, 0, "Elastic leaves at rest thickness");

        WheelAccumulator wheel, other;
        require(wheel.consume(30) == 0 && wheel.consume(30) == 0 && wheel.consume(60) == 1,
                "Partial wheel deltas accumulate to one notch");
        require(wheel.consume(-60) == 0 && wheel.consume(30) == 0 && wheel.consume(-90) == -1,
                "Signed wheel reversal keeps its remainder");
        require(other.consume(119) == 0 && wheel.remainder() == 0, "Each slider has independent precision");
        require(other.consume(1) == 1, "Each EQ band keeps its own remainder");
        require(wheel.consume(1200) == 10, "Multiple notches retain their actual units");

        require(outline_ccw(412, 40, 5, 0).empty(), "Released hold has no border");
        auto early = outline_ccw(412, 40, 5, .1);
        near(early.front().x, 206, 1e-8, "Enclosure begins at top centre");
        near(early.front().y, 0, 1e-8, "Enclosure is visible on top immediately");
        require(early[1].x < early[0].x, "Counterclockwise travel starts left along top");
        auto all = outline_ccw(412, 40, 5, 1);
        near(all.front().x, all.back().x, 1e-7, "Completed enclosure closes x");
        near(all.front().y, all.back().y, 1e-7, "Completed enclosure closes y");
        double area = 0;
        bool top = false, bottom = false, left = false, right = false;
        for (std::size_t i = 1; i < all.size(); ++i) {
            auto p = all[i - 1], q = all[i];
            require(std::hypot(q.x - p.x, q.y - p.y) <= 2.000001, "Enclosure samples stay smooth");
            area += p.x * q.y - q.x * p.y;
            top |= q.y < .01;
            bottom |= q.y > 39.99;
            left |= q.x < .01;
            right |= q.x > 411.99;
        }
        require(area < 0 && top && bottom && left && right, "All four sides close counterclockwise");
        std::cout << "Smooth native control scenarios passed: fixed-width segment, flat Wake, switch, "
                     "Elastic, wheel, enclosure\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
