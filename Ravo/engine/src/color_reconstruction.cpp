#include "color_reconstruction.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <numbers>
#include <string>
#include <vector>

#include "d50_lab.h"
#include "ravo/recipe/color_input.h"

namespace ravo
{
namespace
{

constexpr std::uint32_t kMaximumSpatialResolution = 500U;
constexpr std::uint32_t kMaximumRangeResolution = 100U;

struct GridCell
{
    float lightness = 0.0F;
    float a = 0.0F;
    float b = 0.0F;
    float weight = 0.0F;
};

struct GridShape
{
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    std::uint32_t z = 0U;
    float sigma_spatial = 0.0F;
    float sigma_range = 0.0F;
};

struct FrozenColorReconstructionData
{
    float threshold = 100.0F;
    float spatial = 400.0F;
    float range = 10.0F;
    float hue = 0.66F;
    ColorReconstructionPrecedence precedence = ColorReconstructionPrecedence::kNone;
};

[[nodiscard]] float clamp_source(const float value, const float minimum,
                                 const float maximum) noexcept
{
    return value > minimum ? (value < maximum ? value : maximum) : minimum;
}

[[nodiscard]] std::uint32_t clamped_grid_resolution(const float value,
                                                    const std::uint32_t maximum) noexcept
{
    if (!std::isfinite(value))
    {
        return 0U;
    }
    const float rounded = std::round(value);
    const float bounded = clamp_source(rounded, 4.0F, static_cast<float>(maximum));
    return static_cast<std::uint32_t>(bounded) + 1U;
}

[[nodiscard]] GridShape grid_shape(const std::uint32_t width, const std::uint32_t height,
                                   const float canonical_scale,
                                   const FrozenColorReconstructionData &data) noexcept
{
    if (width == 0U || height == 0U || !std::isfinite(canonical_scale) || canonical_scale <= 0.0F)
    {
        return {};
    }
    const float requested_sigma_spatial = std::fmax(data.spatial, 1.0F) * canonical_scale;
    const float requested_sigma_range = std::fmax(data.range, 0.1F);
    if (!std::isfinite(requested_sigma_spatial) || requested_sigma_spatial <= 0.0F ||
        !std::isfinite(requested_sigma_range) || requested_sigma_range <= 0.0F)
    {
        return {};
    }
    GridShape shape;
    shape.x = clamped_grid_resolution(static_cast<float>(width) / requested_sigma_spatial,
                                      kMaximumSpatialResolution);
    shape.y = clamped_grid_resolution(static_cast<float>(height) / requested_sigma_spatial,
                                      kMaximumSpatialResolution);
    shape.z = clamped_grid_resolution(100.0F / requested_sigma_range, kMaximumRangeResolution);
    if (shape.x < 5U || shape.y < 5U || shape.z < 5U)
    {
        return {};
    }
    shape.sigma_spatial = std::fmax(static_cast<float>(height) / static_cast<float>(shape.y - 1U),
                                    static_cast<float>(width) / static_cast<float>(shape.x - 1U));
    shape.sigma_range = 100.0F / static_cast<float>(shape.z - 1U);
    if (!std::isfinite(shape.sigma_spatial) || shape.sigma_spatial <= 0.0F ||
        !std::isfinite(shape.sigma_range) || shape.sigma_range <= 0.0F)
    {
        return {};
    }
    return shape;
}

[[nodiscard]] std::uint64_t grid_cell_count(const GridShape &shape) noexcept
{
    if (shape.x == 0U || shape.y == 0U || shape.z == 0U)
    {
        return std::numeric_limits<std::uint64_t>::max();
    }
    const std::uint64_t xy = static_cast<std::uint64_t>(shape.x) * shape.y;
    if (xy > std::numeric_limits<std::uint64_t>::max() / shape.z)
    {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return xy * shape.z;
}

[[nodiscard]] FrozenColorReconstructionData
commit_color_reconstruction(const ColorReconstructionParams &params) noexcept
{
    return {static_cast<float>(params.threshold), static_cast<float>(params.spatial),
            static_cast<float>(params.range), static_cast<float>(params.hue), params.precedence};
}

void checkpoint(const detail::ColorReconstructionControl &control,
                const detail::ColorReconstructionCheckpoint stage,
                const std::uint32_t progress) noexcept
{
    if (control.checkpoint_callback != nullptr)
    {
        control.checkpoint_callback(control.context, stage, progress);
    }
}

[[nodiscard]] float hue_to_rgb(const float m1, const float m2, const float hue) noexcept
{
    if (hue < 1.0F)
    {
        return m1 + (m2 - m1) * hue;
    }
    if (hue < 3.0F)
    {
        return m2;
    }
    return hue < 4.0F ? m1 + (m2 - m1) * (4.0F - hue) : m1;
}

[[nodiscard]] float legacy_hue_to_lab_hue(float hue) noexcept
{
    constexpr float saturation = 1.0F;
    constexpr float lightness = 0.5F;
    const float m2 = lightness < 0.5F ? lightness * (1.0F + saturation) :
                                        lightness + saturation - lightness * saturation;
    const float m1 = 2.0F * lightness - m2;
    hue *= 6.0F;
    const std::array<float, 3> rgb{
        hue_to_rgb(m1, m2, hue < 4.0F ? hue + 2.0F : hue - 4.0F),
        hue_to_rgb(m1, m2, hue),
        hue_to_rgb(m1, m2, hue > 2.0F ? hue - 2.0F : hue + 4.0F),
    };
    const auto lab = d50_lab::xyz_to_lab(d50_lab::linear_rec709_to_xyz(rgb));
    return std::atan2(lab[2], lab[1]);
}

void image_to_grid(const GridShape &shape, const float image_x, const float image_y,
                   const float lightness, float &grid_x, float &grid_y, float &grid_z) noexcept
{
    grid_x = clamp_source(image_x / shape.sigma_spatial, 0.0F, static_cast<float>(shape.x - 1U));
    grid_y = clamp_source(image_y / shape.sigma_spatial, 0.0F, static_cast<float>(shape.y - 1U));
    grid_z = clamp_source(lightness / shape.sigma_range, 0.0F, static_cast<float>(shape.z - 1U));
}

[[nodiscard]] Result<void> blur_lines(std::vector<GridCell> &grid, const std::size_t offset1,
                                      const std::size_t offset2, const std::size_t offset3,
                                      const std::uint32_t size1, const std::uint32_t size2,
                                      const std::uint32_t size3, const std::uint32_t pass,
                                      const CancellationToken &cancellation,
                                      const detail::ColorReconstructionControl &control)
{
    constexpr float w0 = 6.0F / 16.0F;
    constexpr float w1 = 4.0F / 16.0F;
    constexpr float w2 = 1.0F / 16.0F;
    for (std::uint32_t k = 0U; k < size1; ++k)
    {
        checkpoint(control, detail::ColorReconstructionCheckpoint::kBlurLine,
                   pass * kMaximumRangeResolution + k);
        auto active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        std::size_t index = static_cast<std::size_t>(k) * offset1;
        for (std::uint32_t j = 0U; j < size2; ++j)
        {
            GridCell previous2 = grid[index];
            grid[index].lightness = grid[index].lightness * w0 +
                                    w1 * grid[index + offset3].lightness +
                                    w2 * grid[index + 2U * offset3].lightness;
            grid[index].a = grid[index].a * w0 + w1 * grid[index + offset3].a +
                            w2 * grid[index + 2U * offset3].a;
            grid[index].b = grid[index].b * w0 + w1 * grid[index + offset3].b +
                            w2 * grid[index + 2U * offset3].b;
            grid[index].weight = grid[index].weight * w0 + w1 * grid[index + offset3].weight +
                                 w2 * grid[index + 2U * offset3].weight;
            index += offset3;
            GridCell previous1 = grid[index];
            grid[index].lightness = grid[index].lightness * w0 +
                                    w1 * (grid[index + offset3].lightness + previous2.lightness) +
                                    w2 * grid[index + 2U * offset3].lightness;
            grid[index].a = grid[index].a * w0 + w1 * (grid[index + offset3].a + previous2.a) +
                            w2 * grid[index + 2U * offset3].a;
            grid[index].b = grid[index].b * w0 + w1 * (grid[index + offset3].b + previous2.b) +
                            w2 * grid[index + 2U * offset3].b;
            grid[index].weight = grid[index].weight * w0 +
                                 w1 * (grid[index + offset3].weight + previous2.weight) +
                                 w2 * grid[index + 2U * offset3].weight;
            index += offset3;
            for (std::uint32_t i = 2U; i < size3 - 2U; ++i)
            {
                const GridCell current = grid[index];
                grid[index].lightness =
                    grid[index].lightness * w0 +
                    w1 * (grid[index + offset3].lightness + previous1.lightness) +
                    w2 * (grid[index + 2U * offset3].lightness + previous2.lightness);
                grid[index].a = grid[index].a * w0 + w1 * (grid[index + offset3].a + previous1.a) +
                                w2 * (grid[index + 2U * offset3].a + previous2.a);
                grid[index].b = grid[index].b * w0 + w1 * (grid[index + offset3].b + previous1.b) +
                                w2 * (grid[index + 2U * offset3].b + previous2.b);
                grid[index].weight = grid[index].weight * w0 +
                                     w1 * (grid[index + offset3].weight + previous1.weight) +
                                     w2 * (grid[index + 2U * offset3].weight + previous2.weight);
                index += offset3;
                previous2 = previous1;
                previous1 = current;
            }
            const GridCell current = grid[index];
            grid[index].lightness = grid[index].lightness * w0 +
                                    w1 * (grid[index + offset3].lightness + previous1.lightness) +
                                    w2 * previous2.lightness;
            grid[index].a = grid[index].a * w0 + w1 * (grid[index + offset3].a + previous1.a) +
                            w2 * previous2.a;
            grid[index].b = grid[index].b * w0 + w1 * (grid[index + offset3].b + previous1.b) +
                            w2 * previous2.b;
            grid[index].weight = grid[index].weight * w0 +
                                 w1 * (grid[index + offset3].weight + previous1.weight) +
                                 w2 * previous2.weight;
            index += offset3;
            grid[index].lightness =
                grid[index].lightness * w0 + w1 * current.lightness + w2 * previous1.lightness;
            grid[index].a = grid[index].a * w0 + w1 * current.a + w2 * previous1.a;
            grid[index].b = grid[index].b * w0 + w1 * current.b + w2 * previous1.b;
            grid[index].weight =
                grid[index].weight * w0 + w1 * current.weight + w2 * previous1.weight;
            index += offset3;
            index += offset2 - offset3 * size3;
        }
    }
    return {};
}

[[nodiscard]] float interpolate(const std::vector<GridCell> &grid, const std::size_t index,
                                const std::size_t offset_x, const std::size_t offset_y,
                                const std::size_t offset_z, const float fraction_x,
                                const float fraction_y, const float fraction_z,
                                float GridCell::*member) noexcept
{
    return grid[index].*member * (1.0F - fraction_x) * (1.0F - fraction_y) * (1.0F - fraction_z) +
           grid[index + offset_x].*member * fraction_x * (1.0F - fraction_y) * (1.0F - fraction_z) +
           grid[index + offset_y].*member * (1.0F - fraction_x) * fraction_y * (1.0F - fraction_z) +
           grid[index + offset_x + offset_y].*member * fraction_x * fraction_y *
               (1.0F - fraction_z) +
           grid[index + offset_z].*member * (1.0F - fraction_x) * (1.0F - fraction_y) * fraction_z +
           grid[index + offset_x + offset_z].*member * fraction_x * (1.0F - fraction_y) *
               fraction_z +
           grid[index + offset_y + offset_z].*member * (1.0F - fraction_x) * fraction_y *
               fraction_z +
           grid[index + offset_x + offset_y + offset_z].*member * fraction_x * fraction_y *
               fraction_z;
}

[[nodiscard]] Result<void> validate_input(const WorkingImage &input)
{
    if (input.width == 0U || input.height == 0U)
    {
        return make_error(ErrorCode::kValidation,
                          "Color Reconstruction input dimensions must be non-zero",
                          {{"reason", "invalid_colorreconstruct_dimensions"}});
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(input.width) * input.height;
    if (pixels > std::numeric_limits<std::size_t>::max() / 3U ||
        input.rgb.size() != static_cast<std::size_t>(pixels * 3U))
    {
        return make_error(ErrorCode::kValidation,
                          "Color Reconstruction input buffer does not match its dimensions",
                          {{"reason", "invalid_colorreconstruct_buffer"}});
    }
    if (input.color_profile.model != ColorModel::kRgb ||
        input.color_profile.identifier != kInputProfileLinearRec709)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Color Reconstruction requires declared linear Rec709 working pixels",
                          {{"profile", input.color_profile.identifier},
                           {"reason", "unsupported_colorreconstruct_working_space"}});
    }
    if (!input.canonical_roi_scale.valid())
    {
        return make_error(ErrorCode::kValidation,
                          "Color Reconstruction requires a canonical full-frame ROI scale",
                          {{"reason", "invalid_colorreconstruct_roi_scale"}});
    }
    return {};
}

} // namespace

std::uint64_t
detail::color_reconstruction_grid_bytes(const std::uint32_t width, const std::uint32_t height,
                                        const float canonical_scale,
                                        const ColorReconstructionParams &params) noexcept
{
    const auto bounded = [](const double value, const double minimum, const double maximum) noexcept
    {
        return std::isfinite(value) && std::isfinite(static_cast<float>(value)) &&
               value >= minimum && value <= maximum;
    };
    if (!bounded(params.threshold, kColorReconstructionThresholdMin,
                 kColorReconstructionThresholdMax) ||
        !bounded(params.spatial, kColorReconstructionSpatialMin, kColorReconstructionSpatialMax) ||
        !bounded(params.range, kColorReconstructionRangeMin, kColorReconstructionRangeMax) ||
        !bounded(params.hue, kColorReconstructionHueMin, kColorReconstructionHueMax))
    {
        return std::numeric_limits<std::uint64_t>::max();
    }
    switch (params.precedence)
    {
    case ColorReconstructionPrecedence::kNone:
    case ColorReconstructionPrecedence::kChroma:
    case ColorReconstructionPrecedence::kHue:
        break;
    default:
        return std::numeric_limits<std::uint64_t>::max();
    }
    const GridShape shape =
        grid_shape(width, height, canonical_scale, commit_color_reconstruction(params));
    const std::uint64_t cells = grid_cell_count(shape);
    if (cells > std::numeric_limits<std::uint64_t>::max() / sizeof(GridCell))
    {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return cells * sizeof(GridCell);
}

Result<WorkingImage> detail::apply_color_reconstruction_controlled(
    const WorkingImage &input, const ColorReconstructionParams &params,
    const CancellationToken &cancellation, const ColorReconstructionControl control)
try
{
    checkpoint(control, ColorReconstructionCheckpoint::kBeforeValidation, 0U);
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    auto canonical = color_reconstruction_to_parameters(params);
    if (!canonical)
    {
        return canonical.error();
    }
    auto input_valid = validate_input(input);
    if (!input_valid)
    {
        return input_valid.error();
    }
    const FrozenColorReconstructionData data = commit_color_reconstruction(params);
    const GridShape shape =
        grid_shape(input.width, input.height, input.canonical_roi_scale.value(), data);
    const std::uint64_t cells = grid_cell_count(shape);
    if (cells == std::numeric_limits<std::uint64_t>::max() ||
        cells > std::numeric_limits<std::size_t>::max() / sizeof(GridCell))
    {
        return make_error(ErrorCode::kValidation, "Color Reconstruction grid dimensions overflow",
                          {{"reason", "colorreconstruct_grid_overflow"}});
    }

    for (std::uint32_t row = 0U; row < input.height; ++row)
    {
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        const std::size_t begin = static_cast<std::size_t>(row) * input.width * 3U;
        const std::size_t end = begin + static_cast<std::size_t>(input.width) * 3U;
        for (std::size_t index = begin; index < end; ++index)
        {
            if (!std::isfinite(input.rgb[index]))
            {
                return make_error(ErrorCode::kValidation,
                                  "Color Reconstruction input contains a non-finite RGB sample",
                                  {{"sample_index", std::to_string(index)},
                                   {"reason", "nonfinite_colorreconstruct_input"}});
            }
        }
    }

    std::vector<GridCell> grid(static_cast<std::size_t>(cells));
    const float selected_hue = legacy_hue_to_lab_hue(data.hue);
    constexpr float hue_variance = std::numbers::pi_v<float> * std::numbers::pi_v<float> / 8.0F;
    for (std::uint32_t row = 0U; row < input.height; ++row)
    {
        checkpoint(control, ColorReconstructionCheckpoint::kSplatRow, row);
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (std::uint32_t column = 0U; column < input.width; ++column)
        {
            const std::size_t input_index =
                (static_cast<std::size_t>(row) * input.width + column) * 3U;
            const std::array<float, 3> rgb{input.rgb[input_index], input.rgb[input_index + 1U],
                                           input.rgb[input_index + 2U]};
            const auto lab = d50_lab::xyz_to_lab(d50_lab::linear_rec709_to_xyz(rgb));
            if (!std::isfinite(lab[0]) || !std::isfinite(lab[1]) || !std::isfinite(lab[2]))
            {
                return make_error(ErrorCode::kValidation,
                                  "Color Reconstruction input produced non-finite Lab",
                                  {{"sample_index", std::to_string(input_index)},
                                   {"reason", "nonfinite_colorreconstruct_lab_input"}});
            }
            if (lab[0] > data.threshold)
            {
                continue;
            }
            float weight = 1.0F;
            if (data.precedence == ColorReconstructionPrecedence::kChroma)
            {
                weight = std::sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
            }
            else if (data.precedence == ColorReconstructionPrecedence::kHue)
            {
                float distance = std::atan2(lab[2], lab[1]) - selected_hue;
                distance = distance > std::numbers::pi_v<float> ?
                               distance - 2.0F * std::numbers::pi_v<float> :
                               (distance < -std::numbers::pi_v<float> ?
                                    distance + 2.0F * std::numbers::pi_v<float> :
                                    distance);
                weight = std::exp(-distance * distance / hue_variance);
            }
            if (!std::isfinite(weight))
            {
                return make_error(ErrorCode::kValidation,
                                  "Color Reconstruction precedence weight is non-finite",
                                  {{"sample_index", std::to_string(input_index)},
                                   {"reason", "nonfinite_colorreconstruct_weight"}});
            }
            float grid_x = 0.0F;
            float grid_y = 0.0F;
            float grid_z = 0.0F;
            image_to_grid(shape, static_cast<float>(column), static_cast<float>(row), lab[0],
                          grid_x, grid_y, grid_z);
            const auto x = static_cast<std::uint32_t>(std::round(grid_x));
            const auto y = static_cast<std::uint32_t>(std::round(grid_y));
            const auto z = static_cast<std::uint32_t>(std::round(grid_z));
            const std::size_t grid_index =
                static_cast<std::size_t>(x) +
                static_cast<std::size_t>(shape.x) *
                    (static_cast<std::size_t>(y) + static_cast<std::size_t>(shape.y) * z);
            grid[grid_index].lightness += lab[0] * weight;
            grid[grid_index].a += lab[1] * weight;
            grid[grid_index].b += lab[2] * weight;
            grid[grid_index].weight += weight;
        }
    }

    auto blurred = blur_lines(grid, static_cast<std::size_t>(shape.x) * shape.y, shape.x, 1U,
                              shape.z, shape.y, shape.x, 0U, cancellation, control);
    if (!blurred)
    {
        return blurred.error();
    }
    blurred = blur_lines(grid, static_cast<std::size_t>(shape.x) * shape.y, 1U, shape.x, shape.z,
                         shape.x, shape.y, 1U, cancellation, control);
    if (!blurred)
    {
        return blurred.error();
    }
    blurred = blur_lines(grid, 1U, shape.x, static_cast<std::size_t>(shape.x) * shape.y, shape.x,
                         shape.y, shape.z, 2U, cancellation, control);
    if (!blurred)
    {
        return blurred.error();
    }

    WorkingImage output;
    output.width = input.width;
    output.height = input.height;
    output.color_profile = input.color_profile;
    output.exposure_analysis = input.exposure_analysis;
    output.canonical_roi_scale = input.canonical_roi_scale;
    output.mask_attached_frame = input.mask_attached_frame;
    output.rgb.resize(input.rgb.size());
    const std::size_t offset_x = 1U;
    const std::size_t offset_y = shape.x;
    const std::size_t offset_z = static_cast<std::size_t>(shape.x) * shape.y;
    for (std::uint32_t row = 0U; row < input.height; ++row)
    {
        checkpoint(control, ColorReconstructionCheckpoint::kSliceRow, row);
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (std::uint32_t column = 0U; column < input.width; ++column)
        {
            const std::size_t input_index =
                (static_cast<std::size_t>(row) * input.width + column) * 3U;
            const std::array<float, 3> rgb{input.rgb[input_index], input.rgb[input_index + 1U],
                                           input.rgb[input_index + 2U]};
            const auto lab = d50_lab::xyz_to_lab(d50_lab::linear_rec709_to_xyz(rgb));
            const float blend = clamp_source(20.0F / data.threshold * lab[0] - 19.0F, 0.0F, 1.0F);
            std::array<float, 3> reconstructed = lab;
            if (blend != 0.0F)
            {
                float grid_x = 0.0F;
                float grid_y = 0.0F;
                float grid_z = 0.0F;
                image_to_grid(shape, static_cast<float>(column), static_cast<float>(row), lab[0],
                              grid_x, grid_y, grid_z);
                const auto x = std::min(static_cast<std::uint32_t>(grid_x), shape.x - 2U);
                const auto y = std::min(static_cast<std::uint32_t>(grid_y), shape.y - 2U);
                const auto z = std::min(static_cast<std::uint32_t>(grid_z), shape.z - 2U);
                const float fraction_x = grid_x - static_cast<float>(x);
                const float fraction_y = grid_y - static_cast<float>(y);
                const float fraction_z = grid_z - static_cast<float>(z);
                const std::size_t grid_index =
                    static_cast<std::size_t>(x) +
                    static_cast<std::size_t>(shape.x) *
                        (static_cast<std::size_t>(y) + static_cast<std::size_t>(shape.y) * z);
                const float lightness =
                    interpolate(grid, grid_index, offset_x, offset_y, offset_z, fraction_x,
                                fraction_y, fraction_z, &GridCell::lightness);
                const float a = interpolate(grid, grid_index, offset_x, offset_y, offset_z,
                                            fraction_x, fraction_y, fraction_z, &GridCell::a);
                const float b = interpolate(grid, grid_index, offset_x, offset_y, offset_z,
                                            fraction_x, fraction_y, fraction_z, &GridCell::b);
                const float weight =
                    interpolate(grid, grid_index, offset_x, offset_y, offset_z, fraction_x,
                                fraction_y, fraction_z, &GridCell::weight);
                const float safe_lightness = std::fmax(lightness, 0.01F);
                if (weight > 0.0F)
                {
                    reconstructed[1] =
                        lab[1] * (1.0F - blend) + a * lab[0] / safe_lightness * blend;
                    reconstructed[2] =
                        lab[2] * (1.0F - blend) + b * lab[0] / safe_lightness * blend;
                }
            }
            const auto reconstructed_rgb =
                d50_lab::xyz_to_linear_rec709(d50_lab::lab_to_xyz(reconstructed));
            for (std::size_t channel = 0U; channel < reconstructed_rgb.size(); ++channel)
            {
                if (!std::isfinite(reconstructed_rgb[channel]))
                {
                    return make_error(ErrorCode::kValidation,
                                      "Color Reconstruction produced a non-finite RGB sample",
                                      {{"sample_index", std::to_string(input_index + channel)},
                                       {"reason", "nonfinite_colorreconstruct_output"}});
                }
                output.rgb[input_index + channel] = reconstructed_rgb[channel];
            }
        }
    }
    checkpoint(control, ColorReconstructionCheckpoint::kBeforePublication, 0U);
    active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    return output;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Color Reconstruction allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<WorkingImage> apply_color_reconstruction(const WorkingImage &input,
                                                const ColorReconstructionParams &params,
                                                const CancellationToken &cancellation)
{
    return detail::apply_color_reconstruction_controlled(input, params, cancellation, {});
}

Result<WorkingImage> apply_color_reconstruction(const WorkingImage &input,
                                                const OperationInstance &operation,
                                                const CancellationToken &cancellation)
try
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    if (operation.id != kColorReconstructionOperationId)
    {
        return make_error(ErrorCode::kValidation, "Operation is not Color Reconstruction",
                          {{"operation_id", operation.id}});
    }
    if (operation.schema_version != kColorReconstructionOperationSchemaVersion)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Color Reconstruction operation schema version is unsupported",
                          {{"operation_id", operation.id},
                           {"schema_version", std::to_string(operation.schema_version)}});
    }
    if (operation.mask_id.has_value())
    {
        return make_error(ErrorCode::kUnsupported,
                          "Color Reconstruction mask evaluation is unavailable",
                          {{"operation_id", operation.id},
                           {"reason", "colorreconstruct_mask_graph_unavailable"}});
    }
    if (!operation.enabled)
    {
        return input;
    }
    auto params = color_reconstruction_from_parameters(operation.parameters);
    if (!params)
    {
        return params.error();
    }
    return apply_color_reconstruction(input, params.value(), cancellation);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Color Reconstruction operation allocation failed",
                      {{"reason", "allocation_failed"}});
}

} // namespace ravo
