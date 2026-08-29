#include "retouch.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "d50_lab.h"
#include "mask_evaluator.h"
#include "ravo/recipe/color_input.h"

namespace ravo
{
namespace
{

struct Bounds
{
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
};

struct GaussianCoefficients
{
    float a0 = 0.0F;
    float a1 = 0.0F;
    float a2 = 0.0F;
    float a3 = 0.0F;
    float b1 = 0.0F;
    float b2 = 0.0F;
    float coefp = 0.0F;
    float coefn = 0.0F;
};

struct BilateralGrid
{
    std::size_t size_x = 0U;
    std::size_t size_y = 0U;
    std::size_t size_z = 0U;
    float sigma_s = 0.0F;
    float sigma_r = 0.0F;
    float sigma_s_inv = 0.0F;
    float sigma_r_inv = 0.0F;
    std::vector<float> values;
};

[[nodiscard]] std::uint64_t saturating_multiply(const std::uint64_t left,
                                                const std::uint64_t right) noexcept
{
    if (left == 0U || right == 0U)
        return 0U;
    return left > std::numeric_limits<std::uint64_t>::max() / right ?
               std::numeric_limits<std::uint64_t>::max() :
               left * right;
}

[[nodiscard]] std::uint64_t saturating_add(const std::uint64_t left,
                                           const std::uint64_t right) noexcept
{
    return left > std::numeric_limits<std::uint64_t>::max() - right ?
               std::numeric_limits<std::uint64_t>::max() :
               left + right;
}

[[nodiscard]] const Mask *find_mask(const std::vector<Mask> &masks,
                                    const std::string_view id) noexcept
{
    const auto found =
        std::find_if(masks.begin(), masks.end(), [id](const Mask &mask) { return mask.id == id; });
    return found == masks.end() ? nullptr : &*found;
}

[[nodiscard]] Result<std::array<double, 2>> destination_anchor(const Mask &mask)
{
    if (const auto *circle = std::get_if<CircleMask>(&mask.payload); circle != nullptr)
        return std::array<double, 2>{circle->center_x, circle->center_y};
    if (const auto *ellipse = std::get_if<EllipseMask>(&mask.payload); ellipse != nullptr)
        return std::array<double, 2>{ellipse->center_x, ellipse->center_y};
    if (const auto *path = std::get_if<PathMask>(&mask.payload);
        path != nullptr && !path->points.empty())
        return std::array<double, 2>{path->points.front().x, path->points.front().y};
    if (const auto *brush = std::get_if<BrushMask>(&mask.payload);
        brush != nullptr && !brush->points.empty())
        return std::array<double, 2>{brush->points.front().x, brush->points.front().y};
    return make_error(ErrorCode::kUnsupported, "Retouch mask has no source anchor",
                      {{"mask_id", mask.id}, {"reason", "unsupported_retouch_mask_anchor"}});
}

[[nodiscard]] Result<void> validate_image(const WorkingImage &image)
{
    const std::uint64_t pixels = static_cast<std::uint64_t>(image.width) * image.height;
    if (image.width == 0U || image.height == 0U ||
        pixels > std::numeric_limits<std::size_t>::max() / 3U ||
        image.rgb.size() != static_cast<std::size_t>(pixels) * 3U)
    {
        return make_error(ErrorCode::kValidation, "Retouch working buffer is invalid",
                          {{"reason", "invalid_retouch_buffer"}});
    }
    if (image.color_profile.identifier != kInputProfileLinearRec709)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Retouch requires the canonical linear Rec.709 D50 working space",
                          {{"reason", "unsupported_retouch_working_profile"},
                           {"profile", image.color_profile.identifier}});
    }
    return {};
}

[[nodiscard]] Result<AlphaPlane> evaluate_region_mask(const WorkingImage &image,
                                                      const Recipe &recipe,
                                                      const std::string_view mask_id,
                                                      const CancellationToken &cancellation)
{
    if (image.width > std::numeric_limits<std::uint32_t>::max() / 3U)
    {
        return make_error(ErrorCode::kValidation, "Retouch mask row stride overflows",
                          {{"reason", "retouch_mask_stride_overflow"}});
    }
    const std::uint32_t stride = image.width * 3U;
    return evaluate_canonical_mask(
        recipe.masks, mask_id,
        MaskEvaluationRequest{.full_width = image.width,
                              .full_height = image.height,
                              .roi_x = 0U,
                              .roi_y = 0U,
                              .roi_width = image.width,
                              .roi_height = image.height,
                              .input = MaskRgbPlaneView{image.rgb, stride},
                              .operation_output = std::nullopt,
                              .attached_frame = image.mask_attached_frame,
                              .cancellation = cancellation});
}

[[nodiscard]] std::optional<Bounds> nonzero_bounds(const AlphaPlane &alpha) noexcept
{
    std::uint32_t left = alpha.width;
    std::uint32_t top = alpha.height;
    std::uint32_t right = 0U;
    std::uint32_t bottom = 0U;
    bool found = false;
    for (std::uint32_t y = 0U; y < alpha.height; ++y)
    {
        for (std::uint32_t x = 0U; x < alpha.width; ++x)
        {
            if (alpha.alpha[static_cast<std::size_t>(y) * alpha.width + x] <= 0.0F)
                continue;
            found = true;
            left = std::min(left, x);
            top = std::min(top, y);
            right = std::max(right, x);
            bottom = std::max(bottom, y);
        }
    }
    if (!found)
        return std::nullopt;
    // Frozen shape areas retain an outer zero/falloff sample. Keep one pixel
    // around the non-zero raster while clipping to the attached frame.
    left = left == 0U ? 0U : left - 1U;
    top = top == 0U ? 0U : top - 1U;
    right = std::min(alpha.width - 1U, right + 1U);
    bottom = std::min(alpha.height - 1U, bottom + 1U);
    return Bounds{left, top, right - left + 1U, bottom - top + 1U};
}

[[nodiscard]] Result<void> copy_bounds(const WorkingImage &image, const Bounds bounds,
                                       std::vector<float> &result,
                                       const CancellationToken &cancellation)
{
    result.resize(static_cast<std::size_t>(bounds.width) * bounds.height * 3U);
    for (std::uint32_t y = 0U; y < bounds.height; ++y)
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();
        const std::size_t source =
            (static_cast<std::size_t>(bounds.y + y) * image.width + bounds.x) * 3U;
        const std::size_t destination = static_cast<std::size_t>(y) * bounds.width * 3U;
        std::copy_n(image.rgb.begin() + static_cast<std::ptrdiff_t>(source),
                    static_cast<std::size_t>(bounds.width) * 3U,
                    result.begin() + static_cast<std::ptrdiff_t>(destination));
    }
    return {};
}

[[nodiscard]] GaussianCoefficients gaussian_coefficients(const float sigma) noexcept
{
    const float alpha = 1.695F / sigma;
    const float ema = std::exp(-alpha);
    const float ema2 = std::exp(-2.0F * alpha);
    GaussianCoefficients result;
    result.b1 = -2.0F * ema;
    result.b2 = ema2;
    const float k = (1.0F - ema) * (1.0F - ema) / (1.0F + 2.0F * alpha * ema - ema2);
    result.a0 = k;
    result.a1 = k * (alpha - 1.0F) * ema;
    result.a2 = k * (alpha + 1.0F) * ema;
    result.a3 = -k * ema2;
    result.coefp = (result.a0 + result.a1) / (1.0F + result.b1 + result.b2);
    result.coefn = (result.a2 + result.a3) / (1.0F + result.b1 + result.b2);
    return result;
}

[[nodiscard]] Result<void> recursive_gaussian_zero_3c(std::vector<float> &signal,
                                                      const std::uint32_t width,
                                                      const std::uint32_t height, const float sigma,
                                                      const CancellationToken &cancellation)
{
    const GaussianCoefficients c = gaussian_coefficients(sigma);
    std::vector<float> scratch(signal.size());
    const auto filter_line = [&](const auto read, const auto write,
                                 const std::uint32_t count) -> Result<void>
    {
        std::array<float, 3> xp{};
        std::array<float, 3> yb{};
        std::array<float, 3> yp{};
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            xp[channel] = read(0U, channel);
            yb[channel] = xp[channel] * c.coefp;
            yp[channel] = yb[channel];
        }
        for (std::uint32_t position = 0U; position < count; ++position)
        {
            if ((position & 63U) == 0U)
            {
                auto active = cancellation.check();
                if (!active)
                    return active.error();
            }
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                const float xc = read(position, channel);
                const float yc =
                    c.a0 * xc + c.a1 * xp[channel] - c.b1 * yp[channel] - c.b2 * yb[channel];
                write(position, channel, yc, false);
                xp[channel] = xc;
                yb[channel] = yp[channel];
                yp[channel] = yc;
            }
        }
        std::array<float, 3> xn{};
        std::array<float, 3> xa{};
        std::array<float, 3> yn{};
        std::array<float, 3> ya{};
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            xn[channel] = read(count - 1U, channel);
            xa[channel] = xn[channel];
            yn[channel] = xn[channel] * c.coefn;
            ya[channel] = yn[channel];
        }
        for (std::uint32_t position = count; position > 0U; --position)
        {
            const std::uint32_t current = position - 1U;
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                const float xc = read(current, channel);
                const float yc = c.a2 * xn[channel] + c.a3 * xa[channel] - c.b1 * yn[channel] -
                                 c.b2 * ya[channel];
                write(current, channel, yc, true);
                xa[channel] = xn[channel];
                xn[channel] = xc;
                ya[channel] = yn[channel];
                yn[channel] = yc;
            }
        }
        return {};
    };

    for (std::uint32_t x = 0U; x < width; ++x)
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();
        auto filtered = filter_line(
            [&](const std::uint32_t y, const std::size_t channel)
            { return signal[(static_cast<std::size_t>(y) * width + x) * 3U + channel]; },
            [&](const std::uint32_t y, const std::size_t channel, const float value, const bool add)
            {
                float &sample = scratch[(static_cast<std::size_t>(y) * width + x) * 3U + channel];
                sample = add ? sample + value : value;
            },
            height);
        if (!filtered)
            return filtered.error();
    }
    for (std::uint32_t y = 0U; y < height; ++y)
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();
        auto filtered = filter_line(
            [&](const std::uint32_t x, const std::size_t channel)
            { return scratch[(static_cast<std::size_t>(y) * width + x) * 3U + channel]; },
            [&](const std::uint32_t x, const std::size_t channel, const float value, const bool add)
            {
                float &sample = signal[(static_cast<std::size_t>(y) * width + x) * 3U + channel];
                sample = add ? sample + value : value;
            },
            width);
        if (!filtered)
            return filtered.error();
    }
    return {};
}

void bilateral_grid_size(BilateralGrid &grid, const std::uint32_t width, const std::uint32_t height,
                         float sigma_s, const float requested_sigma_r) noexcept
{
    sigma_s = std::max(sigma_s, 0.5F);
    const float x = static_cast<float>(
        std::clamp(static_cast<int>(std::round(static_cast<float>(width) / sigma_s)), 4, 3000));
    const float y = static_cast<float>(
        std::clamp(static_cast<int>(std::round(static_cast<float>(height) / sigma_s)), 4, 3000));
    const float z = static_cast<float>(
        std::clamp(static_cast<int>(std::round(100.0F / requested_sigma_r)), 4, 50));
    grid.sigma_s = std::max(static_cast<float>(height) / y, static_cast<float>(width) / x);
    grid.sigma_r = 100.0F / z;
    grid.sigma_s_inv = 1.0F / grid.sigma_s;
    grid.sigma_r_inv = 1.0F / grid.sigma_r;
    grid.size_x =
        static_cast<std::size_t>(std::ceil(static_cast<float>(width) * grid.sigma_s_inv)) + 1U;
    grid.size_y =
        static_cast<std::size_t>(std::ceil(static_cast<float>(height) * grid.sigma_s_inv)) + 1U;
    grid.size_z = static_cast<std::size_t>(std::ceil(100.0F * grid.sigma_r_inv)) + 1U;
}

[[nodiscard]] std::size_t grid_index(const BilateralGrid &grid, const std::uint32_t x,
                                     const std::uint32_t y, const float lightness, float &xf,
                                     float &yf, float &zf) noexcept
{
    const float gx = std::clamp(static_cast<float>(x) * grid.sigma_s_inv, 0.0F,
                                static_cast<float>(grid.size_x - 1U));
    const float gy = std::clamp(static_cast<float>(y) * grid.sigma_s_inv, 0.0F,
                                static_cast<float>(grid.size_y - 1U));
    const float gz =
        std::clamp(lightness * grid.sigma_r_inv, 0.0F, static_cast<float>(grid.size_z - 1U));
    const std::size_t xi = std::min(static_cast<std::size_t>(gx), grid.size_x - 2U);
    const std::size_t yi = std::min(static_cast<std::size_t>(gy), grid.size_y - 2U);
    const std::size_t zi = std::min(static_cast<std::size_t>(gz), grid.size_z - 2U);
    xf = gx - static_cast<float>(xi);
    yf = gy - static_cast<float>(yi);
    zf = gz - static_cast<float>(zi);
    return (xi + yi * grid.size_x) * grid.size_z + zi;
}

void blur_grid_line(std::vector<float> &values, const std::size_t offset1,
                    const std::size_t offset2, const std::size_t offset3, const std::size_t size1,
                    const std::size_t size2, const std::size_t size3,
                    const bool derivative) noexcept
{
    const float w0 = derivative ? 0.0F : 6.0F / 16.0F;
    const float w1 = 4.0F / 16.0F;
    const float w2 = derivative ? 2.0F / 16.0F : 1.0F / 16.0F;
    for (std::size_t k = 0U; k < size1; ++k)
    {
        std::size_t index = k * offset1;
        for (std::size_t j = 0U; j < size2; ++j)
        {
            float tmp1 = values[index];
            values[index] = derivative ?
                                w1 * values[index + offset3] + w2 * values[index + 2U * offset3] :
                                values[index] * w0 + w1 * values[index + offset3] +
                                    w2 * values[index + 2U * offset3];
            index += offset3;
            float tmp2 = values[index];
            values[index] =
                derivative ?
                    w1 * (values[index + offset3] - tmp1) + w2 * values[index + 2U * offset3] :
                    values[index] * w0 + w1 * (values[index + offset3] + tmp1) +
                        w2 * values[index + 2U * offset3];
            index += offset3;
            for (std::size_t i = 2U; i + 2U < size3; ++i)
            {
                const float tmp3 = values[index];
                values[index] = derivative ?
                                    w1 * (values[index + offset3] - tmp2) +
                                        w2 * (values[index + 2U * offset3] - tmp1) :
                                    values[index] * w0 + w1 * (values[index + offset3] + tmp2) +
                                        w2 * (values[index + 2U * offset3] + tmp1);
                index += offset3;
                tmp1 = tmp2;
                tmp2 = tmp3;
            }
            const float tmp3 = values[index];
            values[index] =
                derivative ? w1 * (values[index + offset3] - tmp2) - w2 * tmp1 :
                             values[index] * w0 + w1 * (values[index + offset3] + tmp2) + w2 * tmp1;
            index += offset3;
            values[index] =
                derivative ? -w1 * tmp3 - w2 * tmp2 : values[index] * w0 + w1 * tmp3 + w2 * tmp2;
            index += offset3;
            index += offset2 - offset3 * size3;
        }
    }
}

[[nodiscard]] Result<void> bilateral_filter_lightness_impl(const std::span<float> lightness,
                                                           const std::uint32_t width,
                                                           const std::uint32_t height,
                                                           const float sigma_s, const float sigma_r,
                                                           const CancellationToken &cancellation)
{
    if (width == 0U || height == 0U ||
        static_cast<std::uint64_t>(width) * height != lightness.size() || !std::isfinite(sigma_s) ||
        sigma_s <= 0.0F || !std::isfinite(sigma_r) || sigma_r <= 0.0F)
        return make_error(ErrorCode::kValidation, "Bilateral lightness input is invalid",
                          {{"reason", "invalid_bilateral_lightness_input"}});
    for (std::size_t index = 0U; index < lightness.size(); ++index)
    {
        if (!std::isfinite(lightness[index]))
            return make_error(ErrorCode::kValidation,
                              "Bilateral lightness contains a non-finite sample",
                              {{"sample_index", std::to_string(index)},
                               {"reason", "nonfinite_bilateral_lightness_input"}});
    }
    BilateralGrid grid;
    bilateral_grid_size(grid, width, height, sigma_s, sigma_r);
    const std::uint64_t grid_elements =
        static_cast<std::uint64_t>(grid.size_x) * grid.size_y * grid.size_z;
    if (grid_elements > std::vector<float>{}.max_size())
    {
        return make_error(ErrorCode::kValidation, "Bilateral lightness grid is too large",
                          {{"reason", "bilateral_lightness_grid_overflow"}});
    }
    grid.values.assign(static_cast<std::size_t>(grid_elements), 0.0F);
    const std::size_t ox = grid.size_z;
    const std::size_t oy = grid.size_x * grid.size_z;
    constexpr std::size_t oz = 1U;
    const std::array<std::size_t, 8> offsets{0U, ox,      oy,      ox + oy,
                                             oz, oz + ox, oz + oy, oz + oy + ox};
    const float sigma_squared = grid.sigma_s * grid.sigma_s;
    for (std::uint32_t y = 0U; y < height; ++y)
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();
        for (std::uint32_t x = 0U; x < width; ++x)
        {
            float xf = 0.0F;
            float yf = 0.0F;
            float zf = 0.0F;
            const std::size_t pixel = static_cast<std::size_t>(y) * width + x;
            const std::size_t base = grid_index(grid, x, y, lightness[pixel], xf, yf, zf);
            const std::array<float, 4> contribution{
                (1.0F - xf) * (1.0F - yf) * 100.0F / sigma_squared,
                xf * (1.0F - yf) * 100.0F / sigma_squared,
                (1.0F - xf) * yf * 100.0F / sigma_squared, xf * yf * 100.0F / sigma_squared};
            for (std::size_t corner = 0U; corner < 4U; ++corner)
            {
                grid.values[base + offsets[corner]] += contribution[corner] * (1.0F - zf);
                grid.values[base + offsets[corner + 4U]] += contribution[corner] * zf;
            }
        }
    }
    blur_grid_line(grid.values, oz, oy, ox, grid.size_z, grid.size_y, grid.size_x, false);
    blur_grid_line(grid.values, oz, ox, oy, grid.size_z, grid.size_x, grid.size_y, false);
    blur_grid_line(grid.values, ox, oy, oz, grid.size_x, grid.size_y, grid.size_z, true);
    const float norm = grid.sigma_r * 0.04F;
    for (std::uint32_t y = 0U; y < height; ++y)
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();
        for (std::uint32_t x = 0U; x < width; ++x)
        {
            const std::size_t pixel = static_cast<std::size_t>(y) * width + x;
            float xf = 0.0F;
            float yf = 0.0F;
            float zf = 0.0F;
            const std::size_t base = grid_index(grid, x, y, lightness[pixel], xf, yf, zf);
            const float value = grid.values[base] * (1.0F - xf) * (1.0F - yf) * (1.0F - zf) +
                                grid.values[base + ox] * xf * (1.0F - yf) * (1.0F - zf) +
                                grid.values[base + oy] * (1.0F - xf) * yf * (1.0F - zf) +
                                grid.values[base + ox + oy] * xf * yf * (1.0F - zf) +
                                grid.values[base + oz] * (1.0F - xf) * (1.0F - yf) * zf +
                                grid.values[base + ox + oz] * xf * (1.0F - yf) * zf +
                                grid.values[base + oy + oz] * (1.0F - xf) * yf * zf +
                                grid.values[base + ox + oy + oz] * xf * yf * zf;
            lightness[pixel] = std::max(0.0F, lightness[pixel] + norm * value);
        }
    }
    return {};
}

[[nodiscard]] Result<void> bilateral_lab_base(std::vector<float> &rgb, const std::uint32_t width,
                                              const std::uint32_t height, const float sigma_s,
                                              const CancellationToken &cancellation)
{
    std::vector<d50_lab::Triplet> lab(static_cast<std::size_t>(width) * height);
    std::vector<float> lightness(lab.size());
    for (std::uint32_t y = 0U; y < height; ++y)
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();
        for (std::uint32_t x = 0U; x < width; ++x)
        {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            const d50_lab::Triplet value{rgb[index * 3U], rgb[index * 3U + 1U],
                                         rgb[index * 3U + 2U]};
            lab[index] = d50_lab::xyz_to_lab(d50_lab::linear_rec709_to_xyz(value));
            lightness[index] = lab[index][0];
        }
    }
    auto filtered =
        bilateral_filter_lightness_impl(lightness, width, height, sigma_s, 100.0F, cancellation);
    if (!filtered)
        return filtered.error();
    for (std::size_t index = 0U; index < lab.size(); ++index)
    {
        lab[index][0] = lightness[index];
        const auto converted = d50_lab::xyz_to_linear_rec709(d50_lab::lab_to_xyz(lab[index]));
        rgb[index * 3U] = converted[0];
        rgb[index * 3U + 1U] = converted[1];
        rgb[index * 3U + 2U] = converted[2];
    }
    return {};
}

[[nodiscard]] Result<void> mix_local(WorkingImage &image, const Bounds bounds,
                                     const std::vector<float> &source, const AlphaPlane &alpha,
                                     const float opacity, const CancellationToken &cancellation)
{
    for (std::uint32_t y = 0U; y < bounds.height; ++y)
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();
        for (std::uint32_t x = 0U; x < bounds.width; ++x)
        {
            const std::size_t full =
                static_cast<std::size_t>(bounds.y + y) * image.width + bounds.x + x;
            const std::size_t local = static_cast<std::size_t>(y) * bounds.width + x;
            const float factor = alpha.alpha[full] * opacity;
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                image.rgb[full * 3U + channel] = image.rgb[full * 3U + channel] * (1.0F - factor) +
                                                 source[local * 3U + channel] * factor;
            }
        }
    }
    return {};
}

[[nodiscard]] Result<void> apply_clone(WorkingImage &image, const RetouchRegion &region,
                                       const Mask &mask, const AlphaPlane &alpha,
                                       const Bounds bounds, const CancellationToken &cancellation)
{
    auto anchor = destination_anchor(mask);
    if (!anchor)
        return anchor.error();
    const int dx = static_cast<int>((anchor.value()[0] - region.source_x) * image.width);
    const int dy = static_cast<int>((anchor.value()[1] - region.source_y) * image.height);
    std::vector<float> source(static_cast<std::size_t>(bounds.width) * bounds.height * 3U, 0.0F);
    std::vector<float> local_alpha(static_cast<std::size_t>(bounds.width) * bounds.height, 0.0F);
    for (std::uint32_t y = 0U; y < bounds.height; ++y)
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();
        for (std::uint32_t x = 0U; x < bounds.width; ++x)
        {
            const int destination_x = static_cast<int>(bounds.x + x);
            const int destination_y = static_cast<int>(bounds.y + y);
            const int source_x = destination_x - dx;
            const int source_y = destination_y - dy;
            if (source_x < 0 || source_y < 0 || source_x >= static_cast<int>(image.width) ||
                source_y >= static_cast<int>(image.height))
                continue;
            const std::size_t source_index = static_cast<std::size_t>(source_y) * image.width +
                                             static_cast<std::size_t>(source_x);
            const std::size_t local = static_cast<std::size_t>(y) * bounds.width + x;
            std::copy_n(image.rgb.begin() + static_cast<std::ptrdiff_t>(source_index * 3U), 3U,
                        source.begin() + static_cast<std::ptrdiff_t>(local * 3U));
            local_alpha[local] = alpha.alpha[static_cast<std::size_t>(destination_y) * image.width +
                                             static_cast<std::size_t>(destination_x)];
        }
    }
    for (std::uint32_t y = 0U; y < bounds.height; ++y)
    {
        for (std::uint32_t x = 0U; x < bounds.width; ++x)
        {
            const std::size_t local = static_cast<std::size_t>(y) * bounds.width + x;
            const std::size_t full =
                static_cast<std::size_t>(bounds.y + y) * image.width + bounds.x + x;
            const float factor = local_alpha[local] * static_cast<float>(region.opacity);
            for (std::size_t channel = 0U; channel < 3U; ++channel)
                image.rgb[full * 3U + channel] = image.rgb[full * 3U + channel] * (1.0F - factor) +
                                                 source[local * 3U + channel] * factor;
        }
    }
    return {};
}

[[nodiscard]] Result<void> apply_heal(WorkingImage &image, const RetouchRegion &region,
                                      const Mask &mask, const AlphaPlane &alpha,
                                      const Bounds bounds, const std::int64_t max_iterations,
                                      const CancellationToken &cancellation)
{
    auto anchor = destination_anchor(mask);
    if (!anchor)
        return anchor.error();
    const int dx = static_cast<int>((anchor.value()[0] - region.source_x) * image.width);
    const int dy = static_cast<int>((anchor.value()[1] - region.source_y) * image.height);
    const std::size_t pixels = static_cast<std::size_t>(bounds.width) * bounds.height;
    std::vector<float> source(pixels * 3U, 0.0F);
    std::vector<float> difference(pixels * 3U, 0.0F);
    std::vector<float> mask_values(pixels, 0.0F);
    std::size_t masked = 0U;
    for (std::uint32_t y = 0U; y < bounds.height; ++y)
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();
        for (std::uint32_t x = 0U; x < bounds.width; ++x)
        {
            const int destination_x = static_cast<int>(bounds.x + x);
            const int destination_y = static_cast<int>(bounds.y + y);
            const int source_x = destination_x - dx;
            const int source_y = destination_y - dy;
            if (source_x < 0 || source_y < 0 || source_x >= static_cast<int>(image.width) ||
                source_y >= static_cast<int>(image.height))
                continue;
            const std::size_t local = static_cast<std::size_t>(y) * bounds.width + x;
            const std::size_t full = static_cast<std::size_t>(destination_y) * image.width +
                                     static_cast<std::size_t>(destination_x);
            const std::size_t source_index = static_cast<std::size_t>(source_y) * image.width +
                                             static_cast<std::size_t>(source_x);
            mask_values[local] = alpha.alpha[full];
            if (mask_values[local] > 0.0F)
                ++masked;
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                source[local * 3U + channel] = image.rgb[source_index * 3U + channel];
                difference[local * 3U + channel] =
                    image.rgb[full * 3U + channel] - source[local * 3U + channel];
            }
        }
    }
    if (masked == 0U)
        return {};
    const float w =
        (2.0F - 1.0F / (0.1575F * std::sqrt(static_cast<float>(masked)) + 0.8F)) * 0.25F;
    const float epsilon = 0.1F / 255.0F;
    const float exit_error = epsilon * epsilon * w * w;
    for (std::int64_t iteration = 0; iteration < max_iterations; ++iteration)
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();
        float error = 0.0F;
        for (int parity = 0; parity < 2; ++parity)
        {
            for (std::uint32_t y = 0U; y < bounds.height; ++y)
            {
                for (std::uint32_t x = 0U; x < bounds.width; ++x)
                {
                    if (((x + y) & 1U) != static_cast<std::uint32_t>(parity))
                        continue;
                    const std::size_t pixel = static_cast<std::size_t>(y) * bounds.width + x;
                    if (mask_values[pixel] <= 0.0F)
                        continue;
                    float divisor = 0.0F;
                    std::array<float, 3> neighbor{};
                    const auto add = [&](const std::uint32_t nx, const std::uint32_t ny)
                    {
                        const std::size_t adjacent =
                            static_cast<std::size_t>(ny) * bounds.width + nx;
                        for (std::size_t channel = 0U; channel < 3U; ++channel)
                            neighbor[channel] += difference[adjacent * 3U + channel];
                        divisor += 1.0F;
                    };
                    if (x > 0U)
                        add(x - 1U, y);
                    if (x + 1U < bounds.width)
                        add(x + 1U, y);
                    if (y > 0U)
                        add(x, y - 1U);
                    if (y + 1U < bounds.height)
                        add(x, y + 1U);
                    for (std::size_t channel = 0U; channel < 3U; ++channel)
                    {
                        float &value = difference[pixel * 3U + channel];
                        const float delta = w * (divisor * value - neighbor[channel]);
                        value -= delta;
                        error += delta * delta;
                    }
                }
            }
        }
        if (error < exit_error)
            break;
    }
    std::vector<float> healed(source.size());
    for (std::size_t index = 0U; index < healed.size(); ++index)
        healed[index] = source[index] + difference[index];
    return mix_local(image, bounds, healed, alpha, static_cast<float>(region.opacity),
                     cancellation);
}

[[nodiscard]] Result<void> apply_blur(WorkingImage &image, const RetouchRegion &region,
                                      const AlphaPlane &alpha, const Bounds bounds,
                                      const CancellationToken &cancellation)
{
    if (!image.canonical_roi_scale.valid())
    {
        return make_error(ErrorCode::kUnsupported,
                          "Retouch blur requires canonical source scale metadata",
                          {{"reason", "retouch_scale_unavailable"}});
    }
    std::vector<float> local;
    auto copied = copy_bounds(image, bounds, local, cancellation);
    if (!copied)
        return copied.error();
    const float sigma = static_cast<float>(region.blur_radius) * image.canonical_roi_scale.value();
    Result<void> blurred;
    if (region.blur_type == RetouchBlurType::kGaussian)
        blurred =
            recursive_gaussian_zero_3c(local, bounds.width, bounds.height, sigma, cancellation);
    else
        blurred = bilateral_lab_base(local, bounds.width, bounds.height, sigma, cancellation);
    if (!blurred)
        return blurred.error();
    return mix_local(image, bounds, local, alpha, static_cast<float>(region.opacity), cancellation);
}

[[nodiscard]] Result<void> apply_fill(WorkingImage &image, const RetouchRegion &region,
                                      const AlphaPlane &alpha, const Bounds bounds,
                                      const CancellationToken &cancellation)
{
    std::array<float, 3> color{};
    for (std::size_t channel = 0U; channel < color.size(); ++channel)
    {
        const double base =
            region.fill_mode == RetouchFillMode::kErase ? 0.0 : region.fill_color[channel];
        color[channel] = static_cast<float>(base + region.fill_brightness);
    }
    for (std::uint32_t y = 0U; y < bounds.height; ++y)
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();
        for (std::uint32_t x = 0U; x < bounds.width; ++x)
        {
            const std::size_t pixel =
                static_cast<std::size_t>(bounds.y + y) * image.width + bounds.x + x;
            const float factor = alpha.alpha[pixel] * static_cast<float>(region.opacity);
            for (std::size_t channel = 0U; channel < 3U; ++channel)
                image.rgb[pixel * 3U + channel] =
                    image.rgb[pixel * 3U + channel] * (1.0F - factor) + color[channel] * factor;
        }
    }
    return {};
}

[[nodiscard]] Result<void> process_regions(WorkingImage &layer, const Recipe &recipe,
                                           const RetouchParams &params, const std::int64_t scale,
                                           const CancellationToken &cancellation)
{
    for (const auto &region : params.regions)
    {
        if (region.scale != scale)
            continue;
        auto active = cancellation.check();
        if (!active)
            return active.error();
        const Mask *mask = find_mask(recipe.masks, region.mask_id);
        if (mask == nullptr)
        {
            return make_error(ErrorCode::kValidation, "Retouch region mask disappeared",
                              {{"mask_id", region.mask_id}, {"reason", "missing_retouch_mask"}});
        }
        auto alpha = evaluate_region_mask(layer, recipe, region.mask_id, cancellation);
        if (!alpha)
            return alpha.error();
        auto bounds = nonzero_bounds(alpha.value());
        if (!bounds)
            continue;
        Result<void> applied;
        switch (region.mode)
        {
        case RetouchMode::kClone:
            applied = apply_clone(layer, region, *mask, alpha.value(), *bounds, cancellation);
            break;
        case RetouchMode::kHeal:
            applied = apply_heal(layer, region, *mask, alpha.value(), *bounds,
                                 params.max_heal_iterations, cancellation);
            break;
        case RetouchMode::kBlur:
            applied = apply_blur(layer, region, alpha.value(), *bounds, cancellation);
            break;
        case RetouchMode::kFill:
            applied = apply_fill(layer, region, alpha.value(), *bounds, cancellation);
            break;
        }
        if (!applied)
            return applied.error();
    }
    return {};
}

[[nodiscard]] std::int64_t maximum_dwt_scales(const std::uint32_t width,
                                              const std::uint32_t height) noexcept
{
    const std::uint32_t size = std::min(width, height);
    std::int64_t result = 0;
    while (result < kRetouchMaxScales && (std::uint64_t{1} << (result + 1)) < size)
        ++result;
    return result;
}

[[nodiscard]] Result<void> decompose_layer(std::vector<float> &details, std::vector<float> &coarse,
                                           const std::uint32_t width, const std::uint32_t height,
                                           const std::int64_t level,
                                           const CancellationToken &cancellation)
{
    const std::uint32_t vertical_scale =
        std::min<std::uint32_t>(std::uint32_t{1} << level, height - 1U);
    std::vector<float> vertical(details.size());
    for (std::uint32_t y = 0U; y < height; ++y)
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();
        const std::uint32_t above = y > vertical_scale ? y - vertical_scale : vertical_scale - y;
        const std::uint32_t below = y + vertical_scale < height ?
                                        y + vertical_scale :
                                        2U * (height - 1U) - (y + vertical_scale);
        for (std::uint32_t x = 0U; x < width; ++x)
        {
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 3U + channel;
                const std::size_t upper =
                    (static_cast<std::size_t>(above) * width + x) * 3U + channel;
                const std::size_t lower =
                    (static_cast<std::size_t>(below) * width + x) * 3U + channel;
                vertical[index] = 2.0F * details[index] + details[upper] + details[lower];
            }
        }
    }
    const std::uint32_t horizontal_scale =
        std::min<std::uint32_t>(std::uint32_t{1} << level, width);
    for (std::uint32_t y = 0U; y < height; ++y)
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();
        for (std::uint32_t x = 0U; x < width; ++x)
        {
            const std::uint32_t left =
                x >= horizontal_scale ? x - horizontal_scale : horizontal_scale - x;
            const std::uint32_t right = x + horizontal_scale < width ?
                                            x + horizontal_scale :
                                            2U * width - 2U - (x + horizontal_scale);
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 3U + channel;
                const std::size_t lhs = (static_cast<std::size_t>(y) * width + left) * 3U + channel;
                const std::size_t rhs =
                    (static_cast<std::size_t>(y) * width + right) * 3U + channel;
                const float value =
                    (2.0F * vertical[index] + vertical[lhs] + vertical[rhs]) / 16.0F;
                coarse[index] = value;
                details[index] -= value;
            }
        }
    }
    return {};
}

void add_image(std::vector<float> &destination, const std::vector<float> &source) noexcept
{
    for (std::size_t index = 0U; index < destination.size(); ++index)
        destination[index] += source[index];
}

[[nodiscard]] WorkingImage layer_image(const WorkingImage &prototype, std::vector<float> rgb)
{
    WorkingImage result = prototype;
    result.rgb = std::move(rgb);
    return result;
}

[[nodiscard]] Result<WorkingImage> apply_wavelet(WorkingImage image, const Recipe &recipe,
                                                 const RetouchParams &params,
                                                 const CancellationToken &cancellation)
{
    auto original = process_regions(image, recipe, params, 0, cancellation);
    if (!original)
        return original.error();
    if (params.num_scales == 0)
        return image;
    const std::int64_t scales =
        std::min(params.num_scales, maximum_dwt_scales(image.width, image.height));
    if (scales == 0)
    {
        // The residual of an image too small to decompose is the whole image.
        auto residual = process_regions(image, recipe, params, params.num_scales + 1, cancellation);
        if (!residual)
            return residual.error();
        return image;
    }
    std::vector<float> current = image.rgb;
    std::vector<float> coarse(image.rgb.size(), 0.0F);
    std::vector<float> layers(image.rgb.size(), 0.0F);
    std::vector<float> merged(image.rgb.size(), 0.0F);
    for (std::int64_t level = 0; level < scales; ++level)
    {
        auto decomposed =
            decompose_layer(current, coarse, image.width, image.height, level, cancellation);
        if (!decomposed)
            return decomposed.error();
        if (params.merge_from_scale == 0 || params.merge_from_scale > level + 1)
        {
            WorkingImage detail = layer_image(image, std::move(current));
            auto processed = process_regions(detail, recipe, params, level + 1, cancellation);
            if (!processed)
                return processed.error();
            add_image(layers, detail.rgb);
            current = std::move(coarse);
            coarse.assign(image.rgb.size(), 0.0F);
        }
        else
        {
            add_image(merged, current);
            WorkingImage merged_layer = layer_image(image, std::move(merged));
            auto processed = process_regions(merged_layer, recipe, params, level + 1, cancellation);
            if (!processed)
                return processed.error();
            merged = std::move(merged_layer.rgb);
            current = std::move(coarse);
            coarse.assign(image.rgb.size(), 0.0F);
        }
    }
    const std::int64_t residual_scale =
        scales < params.num_scales ? params.num_scales + 1 : scales + 1;
    WorkingImage residual = layer_image(image, std::move(current));
    auto processed_residual =
        process_regions(residual, recipe, params, residual_scale, cancellation);
    if (!processed_residual)
        return processed_residual.error();
    if (params.merge_from_scale > 0)
        add_image(layers, merged);
    add_image(layers, residual.rgb);
    image.rgb = std::move(layers);
    return image;
}

} // namespace

Result<void> detail::bilateral_filter_lightness(const std::span<float> lightness,
                                                const std::uint32_t width,
                                                const std::uint32_t height, const float sigma_s,
                                                const float sigma_r,
                                                const CancellationToken &cancellation)
try
{
    return bilateral_filter_lightness_impl(lightness, width, height, sigma_s, sigma_r,
                                           cancellation);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Bilateral lightness allocation failed",
                      {{"reason", "allocation_failed"}});
}

std::uint64_t detail::bilateral_filter_working_bytes(const std::uint32_t width,
                                                     const std::uint32_t height,
                                                     const float sigma_s,
                                                     const float sigma_r) noexcept
{
    if (width == 0U || height == 0U || !std::isfinite(sigma_s) || sigma_s <= 0.0F ||
        !std::isfinite(sigma_r) || sigma_r <= 0.0F)
        return std::numeric_limits<std::uint64_t>::max();
    BilateralGrid grid;
    bilateral_grid_size(grid, width, height, sigma_s, sigma_r);
    return saturating_multiply(
        saturating_multiply(static_cast<std::uint64_t>(grid.size_x), grid.size_y),
        saturating_multiply(grid.size_z, sizeof(float)));
}

Result<WorkingImage> apply_retouch(WorkingImage image, const Recipe &recipe,
                                   const OperationInstance &operation,
                                   const CancellationToken &cancellation)
try
{
    auto active = cancellation.check();
    if (!active)
        return active.error();
    auto valid_image = validate_image(image);
    if (!valid_image)
        return valid_image.error();
    auto valid_operation = validate_retouch_operation(operation, recipe.masks);
    if (!valid_operation)
        return valid_operation.error();
    auto params = retouch_from_parameters(operation.parameters);
    if (!params)
        return params.error();
    if (params.value().is_identity())
        return image;
    auto result = apply_wavelet(std::move(image), recipe, params.value(), cancellation);
    if (!result)
        return result.error();
    for (std::size_t index = 0U; index < result.value().rgb.size(); ++index)
    {
        if (!std::isfinite(result.value().rgb[index]))
        {
            return make_error(
                ErrorCode::kValidation, "Retouch produced a non-finite sample",
                {{"reason", "nonfinite_retouch_output"}, {"sample_index", std::to_string(index)}});
        }
    }
    active = cancellation.check();
    if (!active)
        return active.error();
    return result;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Retouch allocation failed",
                      {{"operation_id", operation.id}, {"reason", "allocation_failed"}});
}

std::uint64_t detail::retouch_working_bytes(const std::uint32_t width, const std::uint32_t height,
                                            const RetouchParams &params) noexcept
{
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    const std::uint64_t rgb = saturating_multiply(pixels, 3U * sizeof(float));
    const std::uint64_t alpha = saturating_multiply(pixels, sizeof(float));
    // Mutable image, current/coarse/vertical/layers/merged/local source/heal
    // source/heal difference plus one evaluator alpha. This intentionally
    // overestimates full-frame peaks so the scheduler fails before allocation.
    std::uint64_t result = saturating_multiply(rgb, params.num_scales > 0 ? 9U : 5U);
    result = saturating_add(result, alpha);
    return result;
}

} // namespace ravo
