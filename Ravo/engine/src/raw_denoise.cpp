#include "raw_denoise.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

#include "ravo/recipe/develop.h"

namespace ravo
{
namespace
{

constexpr int kRawDenoiseBands = 5;
constexpr std::array<float, kRawDenoiseBands> kRawDenoiseNoiseAll{0.8002F, 0.2735F, 0.1202F,
                                                                  0.0585F, 0.0291F};

[[nodiscard]] double parameter(const OperationInstance &operation, const std::string_view name,
                               const double fallback)
{
    const auto found = operation.parameters.find(std::string(name));
    if (found == operation.parameters.end())
    {
        return fallback;
    }
    if (const auto *number = std::get_if<double>(&found->second.value); number != nullptr)
    {
        return *number;
    }
    if (const auto *integer = std::get_if<std::int64_t>(&found->second.value); integer != nullptr)
    {
        return static_cast<double>(*integer);
    }
    return fallback;
}

[[nodiscard]] int dwt_interleave_rows(const int rowid, const int height, const int stride) noexcept
{
    if (height <= stride)
    {
        return rowid;
    }
    const int per_pass = (height + stride - 1) / stride;
    const int long_passes = height % stride;
    if (long_passes == 0 || rowid < long_passes * per_pass)
    {
        return (rowid / per_pass) + stride * (rowid % per_pass);
    }
    const int rowid2 = rowid - long_passes * per_pass;
    return long_passes + (rowid2 / (per_pass - 1)) + stride * (rowid2 % (per_pass - 1));
}

[[nodiscard]] Result<void> dwt_denoise_vert(std::vector<float> &out,
                                            const std::span<const float> in, const int height,
                                            const int width, const int lev,
                                            const CancellationToken &cancellation)
{
    const int vscale = std::min(1 << lev, height - 1);
    for (int rowid = 0; rowid < height; ++rowid)
    {
        if ((rowid & 31) == 0)
        {
            auto active = cancellation.check();
            if (!active)
            {
                return active.error();
            }
        }
        const int row = dwt_interleave_rows(rowid, height, vscale);
        const int below_row =
            (row + vscale < height) ? (row + vscale) : 2 * (height - 1) - (row + vscale);
        const auto width_sz = static_cast<std::size_t>(width);
        const float *center = in.data() + static_cast<std::size_t>(row) * width_sz;
        const float *above =
            in.data() + static_cast<std::size_t>(std::abs(row - vscale)) * width_sz;
        const float *below = in.data() + static_cast<std::size_t>(below_row) * width_sz;
        float *outrow = out.data() + static_cast<std::size_t>(row) * width_sz;
        for (int col = 0; col < width; ++col)
        {
            outrow[col] = 2.0F * center[col] + above[col] + below[col];
        }
    }
    return {};
}

[[nodiscard]] Result<void> dwt_denoise_horiz(std::vector<float> &coarse, const std::span<float> img,
                                             std::vector<float> &accum, const int height,
                                             const int width, const int lev, const float thold,
                                             const bool last, const CancellationToken &cancellation)
{
    const int hscale = std::min(1 << lev, width - 1);
    for (int row = 0; row < height; ++row)
    {
        if ((row & 31) == 0)
        {
            auto active = cancellation.check();
            if (!active)
            {
                return active.error();
            }
        }
        const std::size_t rowindex =
            static_cast<std::size_t>(row) * static_cast<std::size_t>(width);
        float *detail_row = img.data() + rowindex;
        float *coarse_row = coarse.data() + rowindex;
        float *accum_row = accum.data() + rowindex;
        for (int col = 0; col < hscale; ++col)
        {
            const float hat =
                (2.0F * coarse_row[col] + coarse_row[hscale - col] + coarse_row[col + hscale]) /
                16.0F;
            const float diff = detail_row[col] - hat;
            detail_row[col] = hat;
            accum_row[col] +=
                diff < 0.0F ? std::min(diff + thold, 0.0F) : std::max(diff - thold, 0.0F);
        }
        for (int col = hscale; col < width - hscale; ++col)
        {
            const float hat =
                (2.0F * coarse_row[col] + coarse_row[col - hscale] + coarse_row[col + hscale]) /
                16.0F;
            const float diff = detail_row[col] - hat;
            detail_row[col] = hat;
            accum_row[col] +=
                diff < 0.0F ? std::min(diff + thold, 0.0F) : std::max(diff - thold, 0.0F);
        }
        for (int col = width - hscale; col < width; ++col)
        {
            const float right = coarse_row[2 * width - 2 - (col + hscale)];
            const float hat = (2.0F * coarse_row[col] + coarse_row[col - hscale] + right) / 16.0F;
            const float diff = detail_row[col] - hat;
            detail_row[col] = hat;
            accum_row[col] += std::max(diff - thold, 0.0F) + std::min(diff + thold, 0.0F);
        }
        if (last)
        {
            for (int col = 0; col < width; ++col)
            {
                detail_row[col] += accum_row[col];
            }
        }
    }
    return {};
}

[[nodiscard]] Result<void> dwt_denoise(const std::span<float> img, const int width,
                                       const int height,
                                       const std::array<float, kRawDenoiseBands> &noise,
                                       const CancellationToken &cancellation)
{
    if (width <= 1 || height <= 1 ||
        img.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height))
    {
        return make_error(ErrorCode::kValidation, "RAW denoise plane is empty");
    }
    const std::size_t plane = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<float> accum(plane, 0.0F);
    std::vector<float> interm(plane);
    for (int lev = 0; lev < kRawDenoiseBands; ++lev)
    {
        const bool last = lev + 1 == kRawDenoiseBands;
        auto vertical = dwt_denoise_vert(interm, img, height, width, lev, cancellation);
        if (!vertical)
        {
            return vertical.error();
        }
        auto horizontal =
            dwt_denoise_horiz(interm, img, accum, height, width, lev,
                              noise[static_cast<std::size_t>(lev)], last, cancellation);
        if (!horizontal)
        {
            return horizontal.error();
        }
    }
    return {};
}

[[nodiscard]] constexpr int positive_mod(const int value, const int divisor) noexcept
{
    const int remainder = value % divisor;
    return remainder < 0 ? remainder + divisor : remainder;
}

[[nodiscard]] std::uint8_t raw_cfa_at(const DecodedRaw &raw, const int row,
                                      const int column) noexcept
{
    return raw.cfa_channels
        [static_cast<std::size_t>(positive_mod(row, static_cast<int>(raw.cfa_height))) *
             raw.cfa_width +
         static_cast<std::size_t>(positive_mod(column, static_cast<int>(raw.cfa_width)))];
}

void compute_channel_noise(std::array<float, kRawDenoiseBands> &noise, const int color,
                           const float threshold, const std::array<std::array<float, 5>, 4> &force)
{
    const int channel = color == 0 ? 1 : color == 2 ? 3 : 2;
    for (int i = 0; i < kRawDenoiseBands; ++i)
    {
        float chan = force[static_cast<std::size_t>(channel)]
                          [static_cast<std::size_t>(kRawDenoiseBands - i - 1)];
        chan *= chan;
        chan *= chan;
        float all = force[0][static_cast<std::size_t>(kRawDenoiseBands - i - 1)];
        all *= all;
        all *= all;
        noise[static_cast<std::size_t>(i)] = kRawDenoiseNoiseAll[static_cast<std::size_t>(i)] *
                                             all * chan * 16.0F * 16.0F * threshold;
    }
}

} // namespace

Result<void> apply_raw_denoise(DecodedRaw &raw, const OperationInstance &operation,
                               const CancellationToken &cancellation)
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    const float threshold = static_cast<float>(parameter(operation, "threshold", 0.01));
    if (!std::isfinite(threshold) || threshold < 0.0F || threshold > 1.0F)
    {
        return make_error(ErrorCode::kValidation, "RAW denoise threshold must be finite in [0, 1]");
    }
    if (!(threshold > 0.0F))
    {
        return {};
    }
    const bool bayer = raw.cfa_width == 2U && raw.cfa_height == 2U && raw.cfa_channels.size() == 4U;
    const bool xtrans =
        raw.cfa_width == 6U && raw.cfa_height == 6U && raw.cfa_channels.size() == 36U;
    if (!bayer && !xtrans)
    {
        return make_error(ErrorCode::kUnsupported,
                          "RAW denoise requires a Bayer 2x2 or X-Trans 6x6 CFA",
                          {{"reason", "unsupported_raw_sensor"}});
    }
    if (raw.width < 4U || raw.height < 4U ||
        raw.pixels.size() != static_cast<std::size_t>(raw.width) * raw.height)
    {
        return make_error(ErrorCode::kUnsupported, "RAW denoise requires a complete CFA frame");
    }
    if (raw.white_level <= static_cast<std::uint32_t>(std::max(raw.black_level, 0)))
    {
        return make_error(ErrorCode::kValidation,
                          "RAW denoise requires white level above black level");
    }
    std::array<std::array<float, 5>, 4> force{};
    const char *names[4] = {"all", "red", "green", "blue"};
    for (int channel = 0; channel < 4; ++channel)
    {
        for (int band = 0; band < kRawDenoiseBands; ++band)
        {
            const std::string key = std::string("y_") + names[channel] + std::to_string(band);
            const float value = static_cast<float>(parameter(operation, key, 0.5));
            if (!std::isfinite(value) || value < 0.0F || value > 16.0F)
            {
                return make_error(ErrorCode::kValidation, "RAW denoise band is out of range",
                                  {{"band", key}});
            }
            force[static_cast<std::size_t>(channel)][static_cast<std::size_t>(band)] = value;
        }
    }
    const std::int64_t black = raw.black_level;
    const float range = static_cast<float>(static_cast<std::int64_t>(raw.white_level) - black);
    std::vector<float> plane(raw.pixels.size());
    for (std::size_t index = 0; index < plane.size(); ++index)
    {
        plane[index] =
            std::max(static_cast<float>(static_cast<std::int64_t>(raw.pixels[index]) - black),
                     0.0F) /
            range;
    }
    const int width = static_cast<int>(raw.width);
    const int height = static_cast<int>(raw.height);
    if (xtrans)
    {
        const std::array<std::uint32_t, 3> counts{
            static_cast<std::uint32_t>(
                std::count(raw.cfa_channels.begin(), raw.cfa_channels.end(), 0U)),
            static_cast<std::uint32_t>(
                std::count(raw.cfa_channels.begin(), raw.cfa_channels.end(), 1U)),
            static_cast<std::uint32_t>(
                std::count(raw.cfa_channels.begin(), raw.cfa_channels.end(), 2U))};
        if (counts != std::array<std::uint32_t, 3>{8U, 20U, 8U})
        {
            return make_error(ErrorCode::kUnsupported,
                              "RAW denoise requires the standard X-Trans RGB layout",
                              {{"reason", "unsupported_xtrans_cfa"}});
        }
        const std::size_t width_size = static_cast<std::size_t>(width);
        std::vector<float> padded(width_size * static_cast<std::size_t>(height + 2), 0.5F);
        const auto normalized = [&](const std::size_t index) noexcept
        { return std::sqrt(std::max(0.0F, plane[index])); };
        for (int color = 0; color < 3; ++color)
        {
            cancelled = cancellation.check();
            if (!cancelled)
            {
                return cancelled.error();
            }
            std::fill(padded.begin(), padded.end(), 0.5F);
            float *const image = padded.data() + width_size;
            for (int row = 0; row < height; ++row)
            {
                if ((row & 31) == 0)
                {
                    cancelled = cancellation.check();
                    if (!cancelled)
                    {
                        return cancelled.error();
                    }
                }
                const std::size_t row_offset = static_cast<std::size_t>(row) * width_size;
                float *const image_row = image + row_offset;
                if (color != 1 && raw_cfa_at(raw, row, 0) == color)
                {
                    const float value = normalized(row_offset);
                    image_row[0] = value;
                    image_row[-width] = value;
                    image_row[-width + 1] = value;
                }
                for (int column = color != 1 ? 1 : 0; column < width - 1; ++column)
                {
                    if (raw_cfa_at(raw, row, column) != color)
                    {
                        continue;
                    }
                    const float value = normalized(row_offset + static_cast<std::size_t>(column));
                    image_row[column] = value;
                    if (color == 1)
                    {
                        image_row[column + 1] = value;
                        image_row[column + width] = value;
                    }
                    else
                    {
                        image_row[column - width - 1] = value;
                        image_row[column - width] = value;
                        image_row[column - width + 1] = value;
                        image_row[column - 1] = value;
                        image_row[column + 1] = value;
                        if (row + 1 < height)
                        {
                            image_row[column + width - 1] = value;
                            image_row[column + width] = value;
                            image_row[column + width + 1] = value;
                        }
                    }
                }
                if (raw_cfa_at(raw, row, 0) != color)
                {
                    std::size_t source = row_offset;
                    if (row > 1 && raw_cfa_at(raw, row - 1, 0) == color)
                    {
                        source -= width_size;
                    }
                    else if (raw_cfa_at(raw, row, 1) == color)
                    {
                        ++source;
                    }
                    else if (row > 1 && raw_cfa_at(raw, row - 1, 1) == color)
                    {
                        source = source - width_size + 1U;
                    }
                    image_row[0] = normalized(source);
                }
                if (color != 1 && raw_cfa_at(raw, row, width - 1) == color)
                {
                    const float value = normalized(row_offset + width_size - 1U);
                    image_row[width - 2] = value;
                    image_row[width - 1] = value;
                    image_row[-1] = value;
                }
                else if (raw_cfa_at(raw, row, width - 1) != color)
                {
                    std::size_t source = row_offset + width_size - 1U;
                    if (raw_cfa_at(raw, row, width - 2) == color)
                    {
                        --source;
                    }
                    else if (row > 1 && raw_cfa_at(raw, row - 1, width - 1) == color)
                    {
                        source -= width_size;
                    }
                    else if (row > 1 && raw_cfa_at(raw, row - 1, width - 2) == color)
                    {
                        source -= width_size + 1U;
                    }
                    image_row[width - 1] = normalized(source);
                }
            }
            std::array<float, kRawDenoiseBands> noise{};
            compute_channel_noise(noise, color, threshold, force);
            auto denoised =
                dwt_denoise(std::span<float>(image, static_cast<std::size_t>(width) *
                                                        static_cast<std::size_t>(height)),
                            width, height, noise, cancellation);
            if (!denoised)
            {
                return denoised.error();
            }
            for (int row = 0; row < height; ++row)
            {
                const std::size_t row_offset = static_cast<std::size_t>(row) * width_size;
                for (int column = 0; column < width; ++column)
                {
                    if (raw_cfa_at(raw, row, column) == color)
                    {
                        const float value = image[row_offset + static_cast<std::size_t>(column)];
                        plane[row_offset + static_cast<std::size_t>(column)] = value * value;
                    }
                }
            }
        }
    }
    else
    {
        for (int c = 0; c < 4; ++c)
        {
            cancelled = cancellation.check();
            if (!cancelled)
            {
                return cancelled.error();
            }
            const std::uint8_t color = raw.cfa_channels[static_cast<std::size_t>(c % 2) * 2U +
                                                        static_cast<std::size_t>(c / 2)];
            std::array<float, kRawDenoiseBands> noise{};
            compute_channel_noise(noise, color, threshold, force);
            const int halfwidth = width / 2 + (width & (~(c >> 1)) & 1);
            const int halfheight = height / 2 + (height & (~c) & 1);
            std::vector<float> mono(
                static_cast<std::size_t>(halfwidth) * static_cast<std::size_t>(halfheight), 0.0F);
            const int offset = (c & 2) >> 1;
            const auto halfwidth_sz = static_cast<std::size_t>(halfwidth);
            const auto width_sz = static_cast<std::size_t>(width);
            for (int row = c & 1; row < height; row += 2)
            {
                float *row_out = mono.data() + static_cast<std::size_t>(row / 2) * halfwidth_sz;
                const float *row_in =
                    plane.data() + static_cast<std::size_t>(row) * width_sz + offset;
                const int senselwidth = (width - offset + 1) / 2;
                for (int col = 0; col < senselwidth; ++col)
                {
                    row_out[col] = std::sqrt(std::max(0.0F, row_in[2 * col]));
                }
            }
            auto denoised =
                dwt_denoise(std::span<float>(mono), halfwidth, halfheight, noise, cancellation);
            if (!denoised)
            {
                return denoised.error();
            }
            for (int row = c & 1; row < height; row += 2)
            {
                const float *row_in =
                    mono.data() + static_cast<std::size_t>(row / 2) * halfwidth_sz;
                float *row_out = plane.data() + static_cast<std::size_t>(row) * width_sz + offset;
                const int senselwidth = (width - offset + 1) / 2;
                for (int col = 0; col < senselwidth; ++col)
                {
                    const float value = row_in[col];
                    row_out[2 * col] = value * value;
                }
            }
        }
    }
    for (std::size_t index = 0; index < plane.size(); ++index)
    {
        const float restored = static_cast<float>(black) + plane[index] * range;
        const auto rounded = static_cast<std::int64_t>(std::lround(restored));
        raw.pixels[index] =
            static_cast<std::uint16_t>(std::clamp(rounded, std::int64_t{0}, std::int64_t{65535}));
    }
    return {};
}

} // namespace ravo
