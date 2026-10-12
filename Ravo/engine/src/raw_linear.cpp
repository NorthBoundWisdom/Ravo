#include "raw_linear.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <numbers>

#include "capability_ops.h"
#include "ravo/recipe/develop.h"
#include "capability_ops_internal.h"
#include "dng_opcodes.h"
#include "image_ops_internal.h"

namespace ravo
{
namespace
{
Result<void> validate_linear_raw(const DecodedRaw &raw)
{
    const auto count = static_cast<std::uint64_t>(raw.width) * raw.height * 3U;
    if (raw.pixel_layout != RawPixelLayout::kLinearRgb16 || raw.width == 0U || raw.height == 0U ||
        raw.width > 65535U || raw.height > 65535U || count > 256ULL * 1024ULL * 1024ULL ||
        count > std::numeric_limits<std::size_t>::max() || raw.pixels.size() != count ||
        raw.cfa_width != 0U || raw.cfa_height != 0U || !raw.cfa_channels.empty() ||
        raw.black_level < 0 || raw.white_level <= static_cast<std::uint32_t>(raw.black_level) ||
        raw.white_level > 65535U || !std::isfinite(raw.linear_baseline_exposure_ev) ||
        raw.linear_baseline_exposure_ev < -16.0 || raw.linear_baseline_exposure_ev > 16.0)
        return make_error(ErrorCode::kValidation, "Linear RAW buffer or sensor range is invalid",
                          {{"reason", "invalid_linear_raw_buffer"}});
    return {};
}

Result<WorkingImage> prepare_linear_raw(const DecodedRaw &raw,
                                        const std::array<float, 4> &white_balance,
                                        const CancellationToken &cancellation)
{
    auto valid = validate_linear_raw(raw);
    if (!valid)
        return valid.error();
    for (const float coefficient : white_balance)
        if (!std::isfinite(coefficient) || coefficient <= 0.0F || coefficient > 8.0F)
            return make_error(ErrorCode::kValidation, "Linear RAW white balance is invalid");
    WorkingImage image;
    image.width = raw.width;
    image.height = raw.height;
    image.color_profile = raw.color_profile;
    image.canonical_roi_scale =
        CanonicalRoiScale::from_scaled_dimensions(raw.width, raw.height, raw.width, raw.height);
    image.rgb.resize(raw.pixels.size());
    const float black = static_cast<float>(raw.black_level);
    const float range = static_cast<float>(raw.white_level) - black;
    const float baseline_gain = static_cast<float>(std::exp2(raw.linear_baseline_exposure_ev));
    for (std::uint32_t y = 0U; y < raw.height; ++y)
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();
        for (std::uint32_t x = 0U; x < raw.width; ++x)
            for (std::size_t c = 0U; c < 3U; ++c)
            {
                const auto index = (static_cast<std::size_t>(y) * raw.width + x) * 3U + c;
                float value =
                    std::max(0.0F, (static_cast<float>(raw.pixels[index]) - black) / range);
                if (raw.dng_opcodes && !raw.dng_opcodes->list2_gain_maps.empty())
                    value = apply_dng_opcode_list2_sample(*raw.dng_opcodes, x, y, raw.width,
                                                          raw.height, value);
                image.rgb[index] = value;
            }
    }
    if (raw.dng_opcodes)
    {
        auto corrected = apply_dng_opcode_list3(std::move(image), *raw.dng_opcodes, cancellation);
        if (!corrected)
            return corrected.error();
        image = std::move(corrected).value();
    }
    for (std::uint32_t y = 0U; y < image.height; ++y)
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();
        const auto begin = static_cast<std::size_t>(y) * image.width * 3U;
        for (std::size_t i = begin; i < begin + image.width * 3U; ++i)
            image.rgb[i] *= white_balance[i % 3U] * baseline_gain;
    }
    return image;
}
} // namespace

Result<WorkingImage> working_from_linear_raw(const DecodedRaw &raw, const std::uint32_t width,
                                             const std::uint32_t height,
                                             const std::array<float, 4> &white_balance,
                                             const CancellationToken &cancellation)
try
{
    auto image = prepare_linear_raw(raw, white_balance, cancellation);
    if (!image)
        return image.error();
    const int turns = normalized_rotate_quarters(raw.rotate_quarters);
    if (turns != 0)
    {
        auto rotated = image_ops_internal::rotate_working(std::move(image).value(), turns);
        if (!rotated)
            return rotated.error();
        image = std::move(rotated);
    }
    const auto source_width = image.value().width;
    const auto source_height = image.value().height;
    if (width == source_width && height == source_height)
        return image;
    return scale_working_image(image.value(), width, height, source_width, source_height,
                               cancellation);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Linear RAW preparation allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<WorkingImage> working_from_linear_raw_window(const DecodedRaw &raw, const std::uint32_t x,
                                                    const std::uint32_t y,
                                                    const std::uint32_t width,
                                                    const std::uint32_t height,
                                                    const std::array<float, 4> &white_balance,
                                                    const CancellationToken &cancellation)
try
{
    if (width == 0U || height == 0U || x >= raw.width || y >= raw.height || width > raw.width - x ||
        height > raw.height - y)
        return make_error(ErrorCode::kInvalidArgument, "Linear RAW ROI is outside the source",
                          {{"reason", "invalid_preview_roi"}});
    // Correct the complete native frame before cropping so file correction
    // coordinates and every overlapping pixel agree with full-resolution export.
    auto full = prepare_linear_raw(raw, white_balance, cancellation);
    if (!full)
        return full.error();
    WorkingImage output;
    output.width = width;
    output.height = height;
    output.color_profile = full.value().color_profile;
    output.canonical_roi_scale = full.value().canonical_roi_scale;
    output.rgb.resize(static_cast<std::size_t>(width) * height * 3U);
    for (std::uint32_t row = 0U; row < height; ++row)
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();
        const auto source = (static_cast<std::size_t>(y + row) * raw.width + x) * 3U;
        std::copy_n(full.value().rgb.data() + source, static_cast<std::size_t>(width) * 3U,
                    output.rgb.data() + static_cast<std::size_t>(row) * width * 3U);
    }
    return image_ops_internal::rotate_working(std::move(output), raw.rotate_quarters);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Linear RAW ROI allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<void> apply_linear_raw_highlights(DecodedRaw &raw, const OperationInstance &operation,
                                         const std::array<float, 4> &white_balance,
                                         const CancellationToken &cancellation)
try
{
    auto valid = validate_linear_raw(raw);
    if (!valid)
        return valid.error();
    using namespace capability_internal;
    const auto mode = parameter_string(operation, "mode", std::string(kRawHighlightsModeOpposed));
    if (mode != kRawHighlightsModeOpposed && mode != kRawHighlightsModeClip)
        return make_error(ErrorCode::kUnsupported, "Linear RGB highlight mode is unavailable",
                          {{"mode", mode}, {"sensor", "linear_rgb"}});
    for (const float coefficient : white_balance)
        if (!std::isfinite(coefficient) || coefficient <= 0.0F || coefficient > 8.0F)
            return make_error(ErrorCode::kValidation, "Linear RAW white balance is invalid");
    const float amount = static_cast<float>(parameter(operation, "amount", 1.0));
    const float clip = static_cast<float>(parameter(operation, "clip", 1.0));
    if (!std::isfinite(amount) || amount < 0.0F || amount > 1.0F || !std::isfinite(clip) ||
        clip <= 0.0F || clip > 2.0F)
        return make_error(ErrorCode::kValidation, "Linear RAW highlight parameters are invalid");
    const float black = static_cast<float>(raw.black_level);
    const float range = static_cast<float>(raw.white_level) - black;
    std::array<float, 3> limits{};
    for (std::size_t c = 0U; c < 3U; ++c)
    {
        const auto limit = raw.linear_response_limits[c];
        if (limit != 0U &&
            (limit <= static_cast<std::uint32_t>(raw.black_level) || limit > raw.white_level))
            return make_error(ErrorCode::kValidation, "Linear RAW response limit is invalid");
        limits[c] = clip * (mode == kRawHighlightsModeOpposed ? 0.987F : 1.0F);
        if (mode == kRawHighlightsModeOpposed && limit != 0U)
            limits[c] = std::min(limits[c], (static_cast<float>(limit) - black) / range);
        limits[c] *= white_balance[c];
    }
    auto output = raw.pixels;
    const auto sample = [&](const std::uint32_t x, const std::uint32_t y, const std::size_t c)
    {
        return std::max(0.0F, (raw.pixels[(static_cast<std::size_t>(y) * raw.width + x) * 3U + c] -
                               black) /
                                  range) *
               white_balance[c];
    };
    for (std::uint32_t y = 0U; y < raw.height; ++y)
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();
        for (std::uint32_t x = 0U; x < raw.width; ++x)
            for (std::size_t c = 0U; c < 3U; ++c)
            {
                const float input = sample(x, y, c);
                if (input < limits[c] || amount == 0.0F)
                    continue;
                float candidate = std::min(input, limits[c]);
                if (mode == kRawHighlightsModeOpposed)
                {
                    const auto a = (c + 1U) % 3U;
                    const auto b = (c + 2U) % 3U;
                    float chroma = 0.0F;
                    std::uint32_t count = 0U;
                    for (std::uint32_t ny = y > 0U ? y - 1U : 0U;
                         ny <= std::min(y + 1U, raw.height - 1U); ++ny)
                        for (std::uint32_t nx = x > 0U ? x - 1U : 0U;
                             nx <= std::min(x + 1U, raw.width - 1U); ++nx)
                        {
                            const float nc = sample(nx, ny, c);
                            const float na = sample(nx, ny, a);
                            const float nb = sample(nx, ny, b);
                            if (nc < limits[c] && na < limits[a] && nb < limits[b])
                            {
                                chroma += nc - (na + nb) * 0.5F;
                                ++count;
                            }
                        }
                    const float ref = (sample(x, y, a) + sample(x, y, b)) * 0.5F;
                    candidate = std::max(
                        input, ref + (count == 0U ? 0.0F : chroma / static_cast<float>(count)));
                }
                const float mixed = input + amount * (candidate - input);
                // Bound in float before rounding: extreme valid white balance
                // can exceed a 32-bit long during opponent reconstruction.
                const float code =
                    std::clamp(black + mixed / white_balance[c] * range, 0.0F, 65535.0F);
                output[(static_cast<std::size_t>(y) * raw.width + x) * 3U + c] =
                    static_cast<std::uint16_t>(std::lround(code));
            }
    }
    auto active = cancellation.check();
    if (!active)
        return active.error();
    raw.pixels = std::move(output);
    return {};
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Linear RAW highlight allocation failed",
                      {{"reason", "allocation_failed"}});
}
} // namespace ravo
