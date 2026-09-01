#include "ravo/recipe/develop.h"
#include "ravo/recipe/develop_mask.h"

#include "develop_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <iomanip>
#include <map>
#include <new>
#include <numbers>
#include <set>
#include <sstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ravo
{
using namespace develop_internal;

bool apply_crop_aspect(DevelopParams &params, const std::string_view aspect)
{
    if (aspect == "free")
    {
        return true;
    }
    double ratio = 0.0;
    if (aspect == "1:1")
    {
        ratio = 1.0;
    }
    else if (aspect == "3:2")
    {
        ratio = 3.0 / 2.0;
    }
    else if (aspect == "4:3")
    {
        ratio = 4.0 / 3.0;
    }
    else if (aspect == "5:4")
    {
        ratio = 5.0 / 4.0;
    }
    else if (aspect == "16:9")
    {
        ratio = 16.0 / 9.0;
    }
    else
    {
        return false;
    }

    const double box_w = params.crop_width;
    const double box_h = params.crop_height;
    const double box_x = params.crop_x;
    const double box_y = params.crop_y;
    double new_w = box_w;
    double new_h = box_h;
    if (box_w / std::max(box_h, kEpsilon) > ratio)
    {
        new_w = box_h * ratio;
        new_h = box_h;
    }
    else
    {
        new_w = box_w;
        new_h = box_w / ratio;
    }
    params.crop_width = new_w;
    params.crop_height = new_h;
    params.crop_x = box_x + (box_w - new_w) * 0.5;
    params.crop_y = box_y + (box_h - new_h) * 0.5;
    clamp_develop(params);
    return true;
}

double develop_crop_min_short_edge_pixels(const double source_width,
                                          const double source_height) noexcept
{
    const double short_edge = std::min(source_width, source_height);
    if (!(short_edge > 0.0))
    {
        return 0.0;
    }
    return std::min(kDevelopCropMinShortEdgePixels, short_edge * kDevelopCropMinShortEdgeFraction);
}

void clamp_develop_crop_min_extent(DevelopParams &params, const double source_width,
                                   const double source_height) noexcept
{
    clamp_develop(params);
    const double min_px = develop_crop_min_short_edge_pixels(source_width, source_height);
    if (!(min_px > 0.0) || !(source_width > 0.0) || !(source_height > 0.0))
    {
        return;
    }
    const double pixel_width = params.crop_width * source_width;
    const double pixel_height = params.crop_height * source_height;
    const double short_px = std::min(pixel_width, pixel_height);
    if (short_px + kEpsilon >= min_px)
    {
        return;
    }
    const double scale = min_px / std::max(short_px, kEpsilon);
    const double center_x = params.crop_x + params.crop_width * 0.5;
    const double center_y = params.crop_y + params.crop_height * 0.5;
    params.crop_width = std::min(1.0, params.crop_width * scale);
    params.crop_height = std::min(1.0, params.crop_height * scale);
    params.crop_x = std::clamp(center_x - params.crop_width * 0.5, 0.0, 1.0 - params.crop_width);
    params.crop_y = std::clamp(center_y - params.crop_height * 0.5, 0.0, 1.0 - params.crop_height);
    const double min_w = std::min(1.0, min_px / source_width);
    const double min_h = std::min(1.0, min_px / source_height);
    if (params.crop_width + kEpsilon < min_w)
    {
        const double cx = params.crop_x + params.crop_width * 0.5;
        params.crop_width = min_w;
        params.crop_x = std::clamp(cx - params.crop_width * 0.5, 0.0, 1.0 - params.crop_width);
    }
    if (params.crop_height + kEpsilon < min_h)
    {
        const double cy = params.crop_y + params.crop_height * 0.5;
        params.crop_height = min_h;
        params.crop_y = std::clamp(cy - params.crop_height * 0.5, 0.0, 1.0 - params.crop_height);
    }
    clamp_develop(params);
}

void transform_crop_for_quarter_turns(DevelopParams &params, int turns_cw) noexcept
{
    turns_cw = ((turns_cw % 4) + 4) % 4;
    for (int turn = 0; turn < turns_cw; ++turn)
    {
        const double x = params.crop_x;
        const double y = params.crop_y;
        const double width = params.crop_width;
        const double height = params.crop_height;
        params.crop_x = 1.0 - y - height;
        params.crop_y = x;
        params.crop_width = height;
        params.crop_height = width;
    }
    clamp_develop(params);
}

void transform_crop_for_flip(DevelopParams &params, const bool horizontal,
                             const bool vertical) noexcept
{
    if (horizontal)
    {
        params.crop_x = 1.0 - params.crop_x - params.crop_width;
    }
    if (vertical)
    {
        params.crop_y = 1.0 - params.crop_y - params.crop_height;
    }
    clamp_develop(params);
}

double working_image_aspect(const std::int64_t rotate_quarters, const double source_aspect) noexcept
{
    const double aspect = source_aspect > kEpsilon ? source_aspect : 1.5;
    if (((rotate_quarters % 4) + 4) % 4 % 2 != 0)
    {
        return 1.0 / aspect;
    }
    return aspect;
}

namespace
{

[[nodiscard]] double max_inscribed_normalized_height(const double degrees,
                                                     const double image_aspect,
                                                     const double crop_aspect) noexcept
{
    const double ratio = std::max(crop_aspect, kEpsilon);
    const double aspect = std::max(image_aspect, kEpsilon);
    double height = std::min(1.0, 1.0 / ratio);
    if (std::abs(degrees) < 1e-4)
    {
        return height;
    }
    const double rad = -degrees * std::numbers::pi / 180.0;
    const double inv_c = std::cos(rad);
    const double inv_s = std::sin(rad);
    const double terms[4] = {
        std::abs(inv_c * ratio - inv_s / aspect), std::abs(inv_c * ratio + inv_s / aspect),
        std::abs(inv_s * ratio * aspect + inv_c), std::abs(inv_s * ratio * aspect - inv_c)};
    for (const double term : terms)
    {
        if (term > kEpsilon)
        {
            height = std::min(height, 1.0 / term);
        }
    }
    return clamp_value(height, 0.01, 1.0);
}

} // namespace

void map_straighten_normalized(const double x, const double y, const double straighten_degrees,
                               const double working_aspect, const bool inverse, double &ox,
                               double &oy) noexcept
{
    const double rad =
        (inverse ? -straighten_degrees : straighten_degrees) * std::numbers::pi / 180.0;
    const double cosine = std::cos(rad);
    const double sine = std::sin(rad);
    const double aspect = std::max(working_aspect, kEpsilon);
    const double dx = (x - 0.5) * aspect;
    const double dy = y - 0.5;
    ox = (cosine * dx - sine * dy) / aspect + 0.5;
    oy = sine * dx + cosine * dy + 0.5;
}

void straightened_source_quad(const double straighten_degrees, const double working_aspect,
                              std::array<double, 8> &corners) noexcept
{
    constexpr double kSource[8] = {0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0};
    for (int index = 0; index < 4; ++index)
    {
        map_straighten_normalized(kSource[index * 2], kSource[index * 2 + 1], straighten_degrees,
                                  working_aspect, false,
                                  corners[static_cast<std::size_t>(index * 2)],
                                  corners[static_cast<std::size_t>(index * 2 + 1)]);
    }
}

void inscribed_crop_for_straighten(const double straighten_degrees, const double working_aspect,
                                   const double crop_aspect_norm, double &x, double &y,
                                   double &width, double &height) noexcept
{
    height = max_inscribed_normalized_height(straighten_degrees, working_aspect, crop_aspect_norm);
    width = crop_aspect_norm * height;
    if (width > 1.0)
    {
        width = 1.0;
        height = width / std::max(crop_aspect_norm, kEpsilon);
    }
    x = (1.0 - width) * 0.5;
    y = (1.0 - height) * 0.5;
}

void constrain_crop_to_straighten(DevelopParams &params, const double working_aspect) noexcept
{
    const double ratio = params.crop_width / std::max(params.crop_height, kEpsilon);
    double limit_x = 0.0;
    double limit_y = 0.0;
    double limit_w = 1.0;
    double limit_h = 1.0;
    inscribed_crop_for_straighten(params.straighten_degrees, working_aspect, ratio, limit_x,
                                  limit_y, limit_w, limit_h);
    double width = std::min(params.crop_width, limit_w);
    double height = std::min(params.crop_height, limit_h);
    if (width / std::max(height, kEpsilon) > ratio)
    {
        width = height * ratio;
    }
    else
    {
        height = width / std::max(ratio, kEpsilon);
    }
    const double center_x = params.crop_x + params.crop_width * 0.5;
    const double center_y = params.crop_y + params.crop_height * 0.5;
    params.crop_width = width;
    params.crop_height = height;
    params.crop_x = clamp_value(center_x - width * 0.5, limit_x, limit_x + limit_w - width);
    params.crop_y = clamp_value(center_y - height * 0.5, limit_y, limit_y + limit_h - height);
    clamp_develop(params);
}

void fit_crop_to_straighten(DevelopParams &params, const double working_aspect) noexcept
{
    const double ratio = params.crop_width / std::max(params.crop_height, kEpsilon);
    inscribed_crop_for_straighten(params.straighten_degrees, working_aspect, ratio, params.crop_x,
                                  params.crop_y, params.crop_width, params.crop_height);
    clamp_develop(params);
}

void strip_crop_operations(Recipe &recipe)
{
    recipe.operations.erase(std::remove_if(recipe.operations.begin(), recipe.operations.end(),
                                           [](const OperationInstance &operation)
                                           { return operation.id == "ravo.geometry.crop"; }),
                            recipe.operations.end());
}

void strip_straighten_operations(Recipe &recipe)
{
    recipe.operations.erase(std::remove_if(recipe.operations.begin(), recipe.operations.end(),
                                           [](const OperationInstance &operation)
                                           {
                                               return operation.id == "ravo.geometry.straighten" ||
                                                      operation.id == kPerspectiveOperationId;
                                           }),
                            recipe.operations.end());
}

} // namespace ravo
