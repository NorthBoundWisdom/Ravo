#include "preview_roi.h"

#include "ravo/engine/engine.h"

#include <algorithm>

namespace ravo
{
namespace
{

void display_to_cfa(const DecodedRaw &raw, const std::uint32_t dx, const std::uint32_t dy,
                    std::uint32_t &cx, std::uint32_t &cy) noexcept
{
    const int turns = normalized_rotate_quarters(raw.rotate_quarters);
    if (turns == 0)
    {
        cx = dx;
        cy = dy;
        return;
    }
    if (turns == 1)
    {
        cx = dy;
        cy = raw.height - 1U - dx;
        return;
    }
    if (turns == 2)
    {
        cx = raw.width - 1U - dx;
        cy = raw.height - 1U - dy;
        return;
    }
    cx = raw.width - 1U - dy;
    cy = dx;
}

} // namespace

Result<SensorPixelRect> map_display_rect_to_cfa(const DecodedRaw &raw, const std::uint32_t display_x,
                                                const std::uint32_t display_y,
                                                const std::uint32_t display_width,
                                                const std::uint32_t display_height)
{
    std::uint32_t display_w = raw.width;
    std::uint32_t display_h = raw.height;
    apply_display_rotation_to_size(display_w, display_h, raw.rotate_quarters);
    if (display_width == 0U || display_height == 0U || display_x >= display_w ||
        display_y >= display_h || display_width > display_w - display_x ||
        display_height > display_h - display_y)
    {
        return make_error(ErrorCode::kInvalidArgument, "Display ROI is outside the photo",
                          {{"reason", "invalid_preview_roi"}});
    }
    const std::uint32_t x1 = display_x + display_width - 1U;
    const std::uint32_t y1 = display_y + display_height - 1U;
    std::uint32_t cxs[4]{};
    std::uint32_t cys[4]{};
    display_to_cfa(raw, display_x, display_y, cxs[0], cys[0]);
    display_to_cfa(raw, x1, display_y, cxs[1], cys[1]);
    display_to_cfa(raw, display_x, y1, cxs[2], cys[2]);
    display_to_cfa(raw, x1, y1, cxs[3], cys[3]);
    SensorPixelRect window;
    window.x = *std::min_element(cxs, cxs + 4);
    window.y = *std::min_element(cys, cys + 4);
    const auto max_x = *std::max_element(cxs, cxs + 4);
    const auto max_y = *std::max_element(cys, cys + 4);
    window.width = max_x - window.x + 1U;
    window.height = max_y - window.y + 1U;
    return window;
}

} // namespace ravo
