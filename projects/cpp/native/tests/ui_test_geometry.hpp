#pragma once
#include "sonyconnect/ui_layout.hpp"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace ui_test {
// Independent product expectation, not a value read back from the renderer.
inline constexpr float expected_zoom = 0.85f;
static_assert(sonyconnect::ui::interface_scale == expected_zoom);
inline int pixel(float logical, unsigned dpi = 96) {
    return static_cast<int>(std::lround(logical * dpi / 96.f * expected_zoom));
}
inline RECT physical(RECT logical, unsigned dpi = 96) {
    return {pixel(static_cast<float>(logical.left), dpi), pixel(static_cast<float>(logical.top), dpi),
            pixel(static_cast<float>(logical.right), dpi), pixel(static_cast<float>(logical.bottom), dpi)};
}
inline int main_height(int logical, int work_height, unsigned dpi = 96) {
    const float scale = dpi / 96.f * expected_zoom;
    const int available = static_cast<int>((work_height - pixel(16, dpi)) / scale);
    return pixel(static_cast<float>(std::min(logical, std::max(180, available))), dpi);
}
// Keep the diagnostic PNG's logical sampling grid while sampling actual new
// physical pixels. Window size/position are asserted separately.
inline std::vector<DWORD> logical_sample(const DWORD *data, int width, int height, RECT region) {
    std::vector<DWORD> result;
    for (int y = region.top; y < region.bottom; ++y)
        for (int x = region.left; x < region.right; ++x) {
            const int sx = pixel(static_cast<float>(x)), sy = pixel(static_cast<float>(y));
            if (sx < 0 || sy < 0 || sx >= width || sy >= height)
                throw std::runtime_error("Pixel probe is outside the scaled fixture");
            result.push_back(data[sy * width + sx] & 0x00ffffff);
        }
    return result;
}
} // namespace ui_test
