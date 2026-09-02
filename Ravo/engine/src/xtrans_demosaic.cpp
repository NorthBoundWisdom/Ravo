#include "xtrans_demosaic.h"

// Markesteijn is an adapted scalar C++20 port of the frozen darktable
// implementation in legacy/src/iop/demosaicing/xtrans.c and RawTherapee commit
// 498f623784e33fd9a7077fcd8937fe0734033366, rtengine/xtrans_demosaic.cc,
// GPL-3.0-or-later. The algorithm is by Frank Markesteijn; the RawTherapee
// adaptation is by Ingo Weyrich. Modified for Ravo on 2026-08-30: global
// state, OpenMP, callbacks/UI, unsafe multidimensional pointer arithmetic and
// implicit algorithm fallback were removed; CFA phase, preview reduction,
// mirrored borders, cancellation, allocation bounds and output ownership are
// Ravo-owned. See DevDocs/THIRD_PARTY_NOTICES.md.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include "dng_opcodes.h"
#include "parallel_rows.h"

namespace ravo
{
namespace
{

constexpr int kTileSize = 122;
constexpr int kPadGreenMinMax = 3;
constexpr int kPadGreenInterpolation = 3;
constexpr int kPadGreenRecalculation = 6;

struct PreparedXTrans
{
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::array<std::uint8_t, 36> cfa{};
    std::vector<float> samples;
};

[[nodiscard]] constexpr std::size_t pixel_index(const std::uint32_t width, const std::uint32_t x,
                                                const std::uint32_t y) noexcept
{
    return static_cast<std::size_t>(y) * width + x;
}

[[nodiscard]] constexpr int positive_mod(const int value, const int divisor) noexcept
{
    const int remainder = value % divisor;
    return remainder < 0 ? remainder + divisor : remainder;
}

[[nodiscard]] constexpr std::uint8_t cfa_at(const std::array<std::uint8_t, 36> &cfa, const int row,
                                            const int column) noexcept
{
    return cfa[static_cast<std::size_t>(positive_mod(row, 6) * 6 + positive_mod(column, 6))];
}

[[nodiscard]] Result<void> validate_xtrans(const DecodedRaw &raw)
{
    if (raw.width < 6U || raw.height < 6U || raw.cfa_width != 6U || raw.cfa_height != 6U ||
        raw.cfa_channels.size() != 36U ||
        raw.width > std::numeric_limits<std::size_t>::max() / raw.height ||
        raw.pixels.size() != static_cast<std::size_t>(raw.width) * raw.height)
    {
        return make_error(ErrorCode::kUnsupported,
                          "X-Trans demosaic requires one complete 6x6 CFA frame",
                          {{"reason", "unsupported_raw_sensor"}, {"sensor", "non_xtrans"}});
    }
    std::array<std::uint32_t, 3> counts{};
    for (const std::uint8_t channel : raw.cfa_channels)
    {
        if (channel > 2U)
        {
            return make_error(ErrorCode::kUnsupported, "X-Trans demosaic requires RGB CFA channels",
                              {{"reason", "unsupported_xtrans_cfa"}});
        }
        ++counts[channel];
    }
    if (counts != std::array<std::uint32_t, 3>{8U, 20U, 8U})
    {
        return make_error(ErrorCode::kUnsupported,
                          "X-Trans demosaic requires the standard 8/20/8 RGB layout",
                          {{"reason", "unsupported_xtrans_cfa"}});
    }
    return {};
}

[[nodiscard]] Result<PreparedXTrans>
prepare_xtrans(const DecodedRaw &raw, const std::uint32_t width, const std::uint32_t height,
               const std::array<float, 4> &white_balance, const CancellationToken &cancellation)
try
{
    auto valid = validate_xtrans(raw);
    if (!valid)
    {
        return valid.error();
    }
    if (width == 0U || height == 0U || width > std::numeric_limits<std::size_t>::max() / height)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "X-Trans demosaic dimensions must be non-zero and bounded",
                          {{"reason", "invalid_demosaic_dimensions"}});
    }
    for (std::size_t channel = 0U; channel < white_balance.size(); ++channel)
    {
        if (!std::isfinite(white_balance[channel]) || white_balance[channel] <= 0.0F ||
            white_balance[channel] > 8.0F)
        {
            return make_error(ErrorCode::kValidation,
                              "RAW temperature coefficient is outside (0, 8]",
                              {{"channel", std::to_string(channel)}});
        }
    }

    PreparedXTrans prepared;
    prepared.width = width;
    prepared.height = height;
    std::copy_n(raw.cfa_channels.begin(), prepared.cfa.size(), prepared.cfa.begin());
    prepared.samples.resize(static_cast<std::size_t>(width) * height);
    const float denominator = static_cast<float>(
        std::max<std::int64_t>(1, static_cast<std::int64_t>(raw.white_level) - raw.black_level));
    const bool defer_white_balance = dng_list3_requires_deferred_white_balance(raw.dng_opcodes);
    std::atomic_bool invalid_sample{false};
    const auto rows = detail::for_each_row(
        height, cancellation,
        [&](const std::uint32_t output_y)
        {
            if (invalid_sample.load(std::memory_order_relaxed))
            {
                return;
            }
            const std::uint32_t source_top = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(output_y) * raw.height / height);
            const std::uint32_t source_bottom = std::max(
                source_top + 1U,
                static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(output_y + 1U) * raw.height + height - 1U) /
                    height));
            for (std::uint32_t output_x = 0U; output_x < width; ++output_x)
            {
                const std::uint8_t wanted =
                    cfa_at(prepared.cfa, static_cast<int>(output_y), static_cast<int>(output_x));
                const std::uint32_t source_left = static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(output_x) * raw.width / width);
                const std::uint32_t source_right = std::max(
                    source_left + 1U,
                    static_cast<std::uint32_t>(
                        (static_cast<std::uint64_t>(output_x + 1U) * raw.width + width - 1U) /
                        width));
                double sum = 0.0;
                std::uint32_t count = 0U;
                for (std::uint32_t source_y = source_top;
                     source_y < std::min(source_bottom, raw.height); ++source_y)
                {
                    for (std::uint32_t source_x = source_left;
                         source_x < std::min(source_right, raw.width); ++source_x)
                    {
                        if (cfa_at(prepared.cfa, static_cast<int>(source_y),
                                   static_cast<int>(source_x)) != wanted)
                        {
                            continue;
                        }
                        float sample = std::max(
                            0.0F, (static_cast<float>(
                                       raw.pixels[pixel_index(raw.width, source_x, source_y)]) -
                                   static_cast<float>(raw.black_level)) /
                                      denominator);
                        if (raw.dng_opcodes)
                        {
                            sample =
                                apply_dng_opcode_list2_sample(*raw.dng_opcodes, source_x, source_y,
                                                              raw.width, raw.height, sample);
                        }
                        if (!defer_white_balance)
                        {
                            sample *= white_balance[wanted];
                        }
                        if (!std::isfinite(sample))
                        {
                            invalid_sample.store(true, std::memory_order_relaxed);
                            return;
                        }
                        sum += sample;
                        ++count;
                    }
                }
                if (count == 0U)
                {
                    std::uint32_t source_x =
                        std::min(raw.width - 1U,
                                 static_cast<std::uint32_t>(static_cast<std::uint64_t>(output_x) *
                                                            raw.width / width));
                    std::uint32_t source_y =
                        std::min(raw.height - 1U,
                                 static_cast<std::uint32_t>(static_cast<std::uint64_t>(output_y) *
                                                            raw.height / height));
                    bool found = false;
                    for (int radius = 0; radius <= 6 && !found; ++radius)
                    {
                        for (int offset_y = -radius; offset_y <= radius && !found; ++offset_y)
                        {
                            for (int offset_x = -radius; offset_x <= radius; ++offset_x)
                            {
                                const auto candidate_x = static_cast<std::uint32_t>(
                                    std::clamp(static_cast<int>(source_x) + offset_x, 0,
                                               static_cast<int>(raw.width) - 1));
                                const auto candidate_y = static_cast<std::uint32_t>(
                                    std::clamp(static_cast<int>(source_y) + offset_y, 0,
                                               static_cast<int>(raw.height) - 1));
                                if (cfa_at(prepared.cfa, static_cast<int>(candidate_y),
                                           static_cast<int>(candidate_x)) != wanted)
                                {
                                    continue;
                                }
                                source_x = candidate_x;
                                source_y = candidate_y;
                                found = true;
                                break;
                            }
                        }
                    }
                    if (!found)
                    {
                        invalid_sample.store(true, std::memory_order_relaxed);
                        return;
                    }
                    float sample = std::max(
                        0.0F, (static_cast<float>(
                                   raw.pixels[pixel_index(raw.width, source_x, source_y)]) -
                               static_cast<float>(raw.black_level)) /
                                  denominator);
                    if (raw.dng_opcodes)
                    {
                        sample = apply_dng_opcode_list2_sample(*raw.dng_opcodes, source_x, source_y,
                                                               raw.width, raw.height, sample);
                    }
                    if (!defer_white_balance)
                    {
                        sample *= white_balance[wanted];
                    }
                    sum = sample;
                    count = 1U;
                }
                prepared.samples[pixel_index(width, output_x, output_y)] =
                    static_cast<float>(sum / count);
            }
        });
    if (!rows)
    {
        return rows.error();
    }
    if (invalid_sample.load(std::memory_order_relaxed))
    {
        return make_error(ErrorCode::kValidation,
                          "X-Trans preparation could not produce a finite CFA sample",
                          {{"reason", "invalid_xtrans_sample"}});
    }
    return prepared;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "X-Trans preparation allocation failed",
                      {{"reason", "allocation_failed"}});
}

[[nodiscard]] int reflected_index(int value, const int size) noexcept
{
    if (size <= 1)
    {
        return 0;
    }
    const int period = 2 * size - 2;
    value = positive_mod(value, period);
    return value < size ? value : period - value;
}

[[nodiscard]] float virtual_sample(const PreparedXTrans &input, const int row, const int column,
                                   const std::uint8_t wanted) noexcept
{
    const int reflected_row = reflected_index(row, static_cast<int>(input.height));
    const int reflected_column = reflected_index(column, static_cast<int>(input.width));
    if (cfa_at(input.cfa, reflected_row, reflected_column) == wanted)
    {
        return input.samples[pixel_index(input.width, static_cast<std::uint32_t>(reflected_column),
                                         static_cast<std::uint32_t>(reflected_row))];
    }
    for (int radius = 1; radius <= 6; ++radius)
    {
        float sum = 0.0F;
        std::uint32_t count = 0U;
        for (int offset_y = -radius; offset_y <= radius; ++offset_y)
        {
            for (int offset_x = -radius; offset_x <= radius; ++offset_x)
            {
                if (std::abs(offset_x) != radius && std::abs(offset_y) != radius)
                {
                    continue;
                }
                const int sample_row =
                    reflected_index(row + offset_y, static_cast<int>(input.height));
                const int sample_column =
                    reflected_index(column + offset_x, static_cast<int>(input.width));
                if (cfa_at(input.cfa, sample_row, sample_column) != wanted)
                {
                    continue;
                }
                sum +=
                    input
                        .samples[pixel_index(input.width, static_cast<std::uint32_t>(sample_column),
                                             static_cast<std::uint32_t>(sample_row))];
                ++count;
            }
        }
        if (count != 0U)
        {
            return sum / static_cast<float>(count);
        }
    }
    return input.samples[pixel_index(input.width, static_cast<std::uint32_t>(reflected_column),
                                     static_cast<std::uint32_t>(reflected_row))];
}

[[nodiscard]] Result<void> interpolate_border(const PreparedXTrans &input, WorkingImage &output,
                                              const std::uint32_t requested_border,
                                              const CancellationToken &cancellation)
{
    const std::uint32_t border = std::min({requested_border, input.width, input.height});
    constexpr float weights[3][3] = {
        {0.25F, 0.5F, 0.25F}, {0.5F, 0.0F, 0.5F}, {0.25F, 0.5F, 0.25F}};
    for (std::uint32_t row = 0U; row < input.height; ++row)
    {
        auto active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (std::uint32_t column = 0U; column < input.width; ++column)
        {
            if (row >= border && row < input.height - border && column >= border &&
                column < input.width - border)
            {
                continue;
            }
            std::array<float, 3> sum{};
            std::array<float, 3> weight_sum{};
            for (int offset_y = -1; offset_y <= 1; ++offset_y)
            {
                const int sample_y = static_cast<int>(row) + offset_y;
                if (sample_y < 0 || sample_y >= static_cast<int>(input.height))
                {
                    continue;
                }
                for (int offset_x = -1; offset_x <= 1; ++offset_x)
                {
                    const int sample_x = static_cast<int>(column) + offset_x;
                    if (sample_x < 0 || sample_x >= static_cast<int>(input.width))
                    {
                        continue;
                    }
                    const std::uint8_t channel = cfa_at(input.cfa, sample_y, sample_x);
                    const float weight = weights[offset_y + 1][offset_x + 1];
                    sum[channel] +=
                        input.samples[pixel_index(input.width, static_cast<std::uint32_t>(sample_x),
                                                  static_cast<std::uint32_t>(sample_y))] *
                        weight;
                    weight_sum[channel] += weight;
                }
            }
            const std::uint8_t known =
                cfa_at(input.cfa, static_cast<int>(row), static_cast<int>(column));
            const std::size_t base = pixel_index(input.width, column, row) * 3U;
            for (std::uint8_t channel = 0U; channel < 3U; ++channel)
            {
                if (channel == known)
                {
                    output.rgb[base + channel] =
                        input.samples[pixel_index(input.width, column, row)];
                }
                else if (weight_sum[channel] > 0.0F)
                {
                    output.rgb[base + channel] = sum[channel] / weight_sum[channel];
                }
                else
                {
                    output.rgb[base + channel] = virtual_sample(input, static_cast<int>(row),
                                                                static_cast<int>(column), channel);
                }
            }
        }
    }
    return {};
}

struct TileScratch
{
    explicit TileScratch(const unsigned directions)
        : rgb(static_cast<std::size_t>(directions) * kTileSize * kTileSize * 3U)
        , yuv(3U * kTileSize * kTileSize)
        , derivative(static_cast<std::size_t>(directions) * kTileSize * kTileSize)
        , green_min(static_cast<std::size_t>(kTileSize) * kTileSize)
        , green_max(static_cast<std::size_t>(kTileSize) * kTileSize)
        , homogeneous(static_cast<std::size_t>(directions) * kTileSize * kTileSize)
        , homogeneous_sum(static_cast<std::size_t>(directions) * kTileSize * kTileSize)
    {
    }

    std::vector<float> rgb;
    std::vector<float> yuv;
    std::vector<float> derivative;
    std::vector<float> green_min;
    std::vector<float> green_max;
    std::vector<std::uint8_t> homogeneous;
    std::vector<std::uint8_t> homogeneous_sum;
};

[[nodiscard]] Result<WorkingImage> markesteijn(const PreparedXTrans &input,
                                               const ColorProfileState &profile, const int passes,
                                               const CancellationToken &cancellation)
try
{
    constexpr short orth[12] = {1, 0, 0, 1, -1, 0, 0, -1, 1, 0, 0, 1};
    constexpr short pattern[2][16] = {{0, 1, 0, -1, 2, 0, -1, 0, 1, 1, 1, -1, 0, 0, 0, 0},
                                      {0, 1, 0, -2, 1, 0, -2, 0, 1, 1, -2, -2, 1, -1, -1, 1}};
    constexpr std::array<int, 4> directions{1, kTileSize, kTileSize + 1, kTileSize - 1};
    const unsigned direction_count = passes > 1 ? 8U : 4U;
    const int tile_border = passes == 1 ? 12 : 17;
    const int tile_step = kTileSize - 2 * tile_border;
    const int tile_rows = (static_cast<int>(input.height) + tile_step - 1) / tile_step;

    WorkingImage output;
    output.width = input.width;
    output.height = input.height;
    output.rgb.assign(static_cast<std::size_t>(input.width) * input.height * 3U, 0.0F);
    output.color_profile = profile;

    auto border =
        interpolate_border(input, output, static_cast<std::uint32_t>(tile_border), cancellation);
    if (!border)
    {
        return border.error();
    }
    if (input.width <= static_cast<std::uint32_t>(2 * tile_border) ||
        input.height <= static_cast<std::uint32_t>(2 * tile_border))
    {
        return output;
    }

    short all_hex[3][3][8]{};
    int solitary_green_row = 0;
    int solitary_green_column = 0;
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            for (int non_green = 0, direction = 0; direction < 10; direction += 2)
            {
                const int green = cfa_at(input.cfa, row, column) == 1U;
                if (cfa_at(input.cfa, row + orth[direction], column + orth[direction + 2]) == 1U)
                {
                    non_green = 0;
                }
                else
                {
                    ++non_green;
                }
                if (non_green == 4)
                {
                    solitary_green_row = row;
                    solitary_green_column = column;
                }
                if (non_green == green + 1)
                {
                    for (int index = 0; index < 8; ++index)
                    {
                        const int vertical = orth[direction] * pattern[green][index * 2] +
                                             orth[direction + 1] * pattern[green][index * 2 + 1];
                        const int horizontal = orth[direction + 2] * pattern[green][index * 2] +
                                               orth[direction + 3] * pattern[green][index * 2 + 1];
                        all_hex[row][column][index ^ (green * 2 & direction)] =
                            static_cast<short>(horizontal + vertical * kTileSize);
                    }
                }
            }
        }
    }
    const auto hex_map = [&](const int row, const int column) -> const short *
    { return all_hex[positive_mod(row, 3)][positive_mod(column, 3)]; };

    const unsigned worker_count =
        detail::parallel_row_workers(static_cast<std::uint32_t>(tile_rows));
    std::vector<TileScratch> scratch;
    scratch.reserve(worker_count);
    for (unsigned worker = 0U; worker < worker_count; ++worker)
    {
        scratch.emplace_back(direction_count);
    }
    std::atomic_bool stopped{false};
    std::atomic_bool invalid_output{false};
    const std::size_t tile_pixels = static_cast<std::size_t>(kTileSize) * kTileSize;
    const auto tiles = detail::for_each_row(
        static_cast<std::uint32_t>(tile_rows), cancellation,
        [&](const std::uint32_t tile_row_value, const unsigned worker)
        {
            TileScratch &buffers = scratch[worker];
            const int top = -tile_border + static_cast<int>(tile_row_value) * tile_step;
            const int global_row_end =
                std::min(top + kTileSize, static_cast<int>(input.height) + tile_border);
            const int local_rows = global_row_end - top;
            for (int left = -tile_border; left < static_cast<int>(input.width) - tile_border;
                 left += tile_step)
            {
                if (!cancellation.check())
                {
                    stopped.store(true, std::memory_order_relaxed);
                    return;
                }
                const int global_column_end =
                    std::min(left + kTileSize, static_cast<int>(input.width) + tile_border);
                const int local_columns = global_column_end - left;
                std::fill(buffers.rgb.begin(), buffers.rgb.end(), 0.0F);
                std::fill(buffers.yuv.begin(), buffers.yuv.end(), 0.0F);
                std::fill(buffers.derivative.begin(), buffers.derivative.end(), 0.0F);
                std::fill(buffers.green_min.begin(), buffers.green_min.end(), 0.0F);
                std::fill(buffers.green_max.begin(), buffers.green_max.end(), 0.0F);
                std::fill(buffers.homogeneous.begin(), buffers.homogeneous.end(), 0U);
                std::fill(buffers.homogeneous_sum.begin(), buffers.homogeneous_sum.end(), 0U);

                const auto local_index = [](const int row, const int column) noexcept
                { return row * kTileSize + column; };
                const auto rgb = [&](const unsigned direction, const int index,
                                     const int channel) -> float &
                {
                    return buffers.rgb[(static_cast<std::size_t>(direction) * tile_pixels +
                                        static_cast<std::size_t>(index)) *
                                           3U +
                                       static_cast<std::size_t>(channel)];
                };
                const auto yuv = [&](const int channel, const int index) -> float &
                {
                    return buffers.yuv[static_cast<std::size_t>(channel) * tile_pixels +
                                       static_cast<std::size_t>(index)];
                };
                const auto derivative = [&](const unsigned direction, const int index) -> float &
                {
                    return buffers.derivative[static_cast<std::size_t>(direction) * tile_pixels +
                                              static_cast<std::size_t>(index)];
                };
                const auto homogeneous = [&](const unsigned direction,
                                             const int index) -> std::uint8_t &
                {
                    return buffers.homogeneous[static_cast<std::size_t>(direction) * tile_pixels +
                                               static_cast<std::size_t>(index)];
                };
                const auto homogeneous_sum = [&](const unsigned direction,
                                                 const int index) -> std::uint8_t &
                {
                    return buffers
                        .homogeneous_sum[static_cast<std::size_t>(direction) * tile_pixels +
                                         static_cast<std::size_t>(index)];
                };

                for (int row = top; row < global_row_end; ++row)
                {
                    for (int column = left; column < global_column_end; ++column)
                    {
                        const int index = local_index(row - top, column - left);
                        const std::uint8_t channel = cfa_at(input.cfa, row, column);
                        rgb(0U, index, channel) =
                            std::max(0.0F, virtual_sample(input, row, column, channel));
                    }
                }
                for (unsigned direction = 1U; direction < 4U; ++direction)
                {
                    std::copy_n(buffers.rgb.begin(), tile_pixels * 3U,
                                buffers.rgb.begin() +
                                    static_cast<std::ptrdiff_t>(direction * tile_pixels * 3U));
                }

                for (int row = top + kPadGreenMinMax; row < global_row_end - kPadGreenMinMax; ++row)
                {
                    float minimum = std::numeric_limits<float>::max();
                    float maximum = 0.0F;
                    for (int column = left + kPadGreenMinMax;
                         column < global_column_end - kPadGreenMinMax; ++column)
                    {
                        if (cfa_at(input.cfa, row, column) == 1U)
                        {
                            minimum = std::numeric_limits<float>::max();
                            maximum = 0.0F;
                            continue;
                        }
                        const int index = local_index(row - top, column - left);
                        if (maximum == 0.0F)
                        {
                            const short *hex = hex_map(row, column);
                            for (int candidate = 0; candidate < 6; ++candidate)
                            {
                                const float value = rgb(0U, index + hex[candidate], 1);
                                minimum = std::min(minimum, value);
                                maximum = std::max(maximum, value);
                            }
                        }
                        buffers.green_min[static_cast<std::size_t>(index)] = minimum;
                        buffers.green_max[static_cast<std::size_t>(index)] = maximum;
                        switch (positive_mod(row - solitary_green_row, 3))
                        {
                        case 1:
                            if (row < global_row_end - 4)
                            {
                                ++row;
                                --column;
                            }
                            break;
                        case 2:
                            minimum = std::numeric_limits<float>::max();
                            maximum = 0.0F;
                            column += 2;
                            if (column < global_column_end - 4 && row > top + 3)
                            {
                                --row;
                            }
                            break;
                        default:
                            break;
                        }
                    }
                }

                for (int row = top + kPadGreenInterpolation;
                     row < global_row_end - kPadGreenInterpolation; ++row)
                {
                    for (int column = left + kPadGreenInterpolation;
                         column < global_column_end - kPadGreenInterpolation; ++column)
                    {
                        const int channel = cfa_at(input.cfa, row, column);
                        if (channel == 1)
                        {
                            continue;
                        }
                        const int index = local_index(row - top, column - left);
                        const short *hex = hex_map(row, column);
                        std::array<float, 4> color{};
                        color[0] =
                            0.6796875F * (rgb(0U, index + hex[1], 1) + rgb(0U, index + hex[0], 1)) -
                            0.1796875F *
                                (rgb(0U, index + 2 * hex[1], 1) + rgb(0U, index + 2 * hex[0], 1));
                        color[1] = 0.87109375F * rgb(0U, index + hex[3], 1) +
                                   0.13F * rgb(0U, index + hex[2], 1) +
                                   0.359375F *
                                       (rgb(0U, index, channel) - rgb(0U, index - hex[2], channel));
                        for (int candidate = 0; candidate < 2; ++candidate)
                        {
                            color[static_cast<std::size_t>(2 + candidate)] =
                                0.640625F * rgb(0U, index + hex[4 + candidate], 1) +
                                0.359375F * rgb(0U, index - 2 * hex[4 + candidate], 1) +
                                0.12890625F * (2.0F * rgb(0U, index, channel) -
                                               rgb(0U, index + 3 * hex[4 + candidate], channel) -
                                               rgb(0U, index - 3 * hex[4 + candidate], channel));
                        }
                        for (int candidate = 0; candidate < 4; ++candidate)
                        {
                            const unsigned direction = static_cast<unsigned>(
                                candidate ^ !positive_mod(row - solitary_green_row, 3));
                            rgb(direction, index, 1) =
                                std::clamp(color[static_cast<std::size_t>(candidate)],
                                           buffers.green_min[static_cast<std::size_t>(index)],
                                           buffers.green_max[static_cast<std::size_t>(index)]);
                        }
                    }
                }

                unsigned direction_base = 0U;
                for (int pass = 0; pass < passes; ++pass)
                {
                    if (pass == 1)
                    {
                        std::copy_n(buffers.rgb.begin(), 4U * tile_pixels * 3U,
                                    buffers.rgb.begin() +
                                        static_cast<std::ptrdiff_t>(4U * tile_pixels * 3U));
                        direction_base = 4U;
                    }
                    if (pass != 0)
                    {
                        for (int row = top + kPadGreenRecalculation;
                             row < global_row_end - kPadGreenRecalculation; ++row)
                        {
                            for (int column = left + kPadGreenRecalculation;
                                 column < global_column_end - kPadGreenRecalculation; ++column)
                            {
                                const int channel = cfa_at(input.cfa, row, column);
                                if (channel == 1)
                                {
                                    continue;
                                }
                                const int index = local_index(row - top, column - left);
                                const short *hex = hex_map(row, column);
                                for (int direction = 3; direction < 6; ++direction)
                                {
                                    const unsigned plane =
                                        direction_base +
                                        static_cast<unsigned>(
                                            (direction - 2) ^
                                            !positive_mod(row - solitary_green_row, 3));
                                    const float value =
                                        rgb(plane, index - 2 * hex[direction], 1) +
                                        2.0F * rgb(plane, index + hex[direction], 1) -
                                        rgb(plane, index - 2 * hex[direction], channel) -
                                        2.0F * rgb(plane, index + hex[direction], channel) +
                                        3.0F * rgb(plane, index, channel);
                                    rgb(plane, index, 1) = std::clamp(
                                        value / 3.0F,
                                        buffers.green_min[static_cast<std::size_t>(index)],
                                        buffers.green_max[static_cast<std::size_t>(index)]);
                                }
                            }
                        }
                    }

                    const int green_padding = passes == 1 ? 6 : 5;
                    for (int row = (top - solitary_green_row + green_padding + 2) / 3 * 3 +
                                   solitary_green_row;
                         row < global_row_end - green_padding; row += 3)
                    {
                        for (int column =
                                 (left - solitary_green_column + green_padding + 2) / 3 * 3 +
                                 solitary_green_column;
                             column < global_column_end - green_padding; column += 3)
                        {
                            int plane_index = local_index(row - top, column - left);
                            int channel = cfa_at(input.cfa, row, column + 1);
                            std::array<float, 6> difference{};
                            float color[2][6]{};
                            unsigned output_direction = direction_base;
                            for (int axis = 1, direction = 0; direction < 6;
                                 ++direction, axis ^= kTileSize ^ 1, channel ^= 2)
                            {
                                for (int distance = 0; distance < 2; ++distance, channel ^= 2)
                                {
                                    const int offset = axis << distance;
                                    const float green =
                                        2.0F * rgb(output_direction, plane_index, 1) -
                                        rgb(output_direction, plane_index + offset, 1) -
                                        rgb(output_direction, plane_index - offset, 1);
                                    color[channel != 0][direction] =
                                        green +
                                        rgb(output_direction, plane_index + offset, channel) +
                                        rgb(output_direction, plane_index - offset, channel);
                                    if (direction > 1)
                                    {
                                        const float slope =
                                            rgb(output_direction, plane_index + offset, 1) -
                                            rgb(output_direction, plane_index - offset, 1) -
                                            rgb(output_direction, plane_index + offset, channel) +
                                            rgb(output_direction, plane_index - offset, channel);
                                        difference[static_cast<std::size_t>(direction)] +=
                                            slope * slope + green * green;
                                    }
                                }
                                if (direction < 2 || (direction & 1) != 0)
                                {
                                    const int selected =
                                        direction -
                                        ((direction > 1 &&
                                          difference[static_cast<std::size_t>(direction - 1)] <
                                              difference[static_cast<std::size_t>(direction)]) ?
                                             1 :
                                             0);
                                    rgb(output_direction, plane_index, 0) =
                                        color[0][selected] * 0.5F;
                                    rgb(output_direction, plane_index, 2) =
                                        color[1][selected] * 0.5F;
                                    ++output_direction;
                                }
                            }
                        }
                    }

                    const int red_blue_padding = passes == 1 ? 6 : 5;
                    for (int row = top + red_blue_padding; row < global_row_end - red_blue_padding;
                         ++row)
                    {
                        for (int column = left + red_blue_padding;
                             column < global_column_end - red_blue_padding; ++column)
                        {
                            const int missing = 2 - cfa_at(input.cfa, row, column);
                            if (missing == 1)
                            {
                                continue;
                            }
                            const int index = local_index(row - top, column - left);
                            const int cardinal =
                                positive_mod(row - solitary_green_row, 3) != 0 ? kTileSize : 1;
                            const int alternate = 3 * (cardinal ^ kTileSize ^ 1);
                            for (unsigned direction = 0U; direction < 4U; ++direction)
                            {
                                const unsigned plane = direction_base + direction;
                                const int selected =
                                    direction > 1U ||
                                            ((static_cast<int>(direction) ^ cardinal) & 1) ||
                                            (std::abs(rgb(plane, index, 1) -
                                                      rgb(plane, index + cardinal, 1)) +
                                                 std::abs(rgb(plane, index, 1) -
                                                          rgb(plane, index - cardinal, 1)) <
                                             2.0F * (std::abs(rgb(plane, index, 1) -
                                                              rgb(plane, index + alternate, 1)) +
                                                     std::abs(rgb(plane, index, 1) -
                                                              rgb(plane, index - alternate, 1)))) ?
                                        cardinal :
                                        alternate;
                                rgb(plane, index, missing) =
                                    (rgb(plane, index + selected, missing) +
                                     rgb(plane, index - selected, missing) +
                                     2.0F * rgb(plane, index, 1) - rgb(plane, index + selected, 1) -
                                     rgb(plane, index - selected, 1)) *
                                    0.5F;
                            }
                        }
                    }

                    const int block_padding = passes == 1 ? 8 : 4;
                    for (int row = top + block_padding; row < global_row_end - block_padding; ++row)
                    {
                        if (positive_mod(row - solitary_green_row, 3) == 0)
                        {
                            continue;
                        }
                        for (int column = left + block_padding;
                             column < global_column_end - block_padding; ++column)
                        {
                            if (positive_mod(column - solitary_green_column, 3) == 0)
                            {
                                continue;
                            }
                            const int index = local_index(row - top, column - left);
                            const short *hex = hex_map(row, column);
                            for (unsigned direction = 0U; direction < direction_count;
                                 direction += 2U)
                            {
                                const unsigned plane = direction_base + direction / 2U;
                                if (hex[direction] + hex[direction + 1U] != 0)
                                {
                                    const float green =
                                        3.0F * rgb(plane, index, 1) -
                                        2.0F * rgb(plane, index + hex[direction], 1) -
                                        rgb(plane, index + hex[direction + 1U], 1);
                                    for (int channel = 0; channel < 3; channel += 2)
                                    {
                                        rgb(plane, index, channel) =
                                            (green +
                                             2.0F * rgb(plane, index + hex[direction], channel) +
                                             rgb(plane, index + hex[direction + 1U], channel)) /
                                            3.0F;
                                    }
                                }
                                else
                                {
                                    const float green = 2.0F * rgb(plane, index, 1) -
                                                        rgb(plane, index + hex[direction], 1) -
                                                        rgb(plane, index + hex[direction + 1U], 1);
                                    for (int channel = 0; channel < 3; channel += 2)
                                    {
                                        rgb(plane, index, channel) =
                                            (green + rgb(plane, index + hex[direction], channel) +
                                             rgb(plane, index + hex[direction + 1U], channel)) *
                                            0.5F;
                                    }
                                }
                            }
                        }
                    }
                }

                for (unsigned direction = 0U; direction < direction_count; ++direction)
                {
                    const int yuv_padding = passes == 1 ? 8 : 13;
                    for (int row = yuv_padding; row < local_rows - yuv_padding; ++row)
                    {
                        for (int column = yuv_padding; column < local_columns - yuv_padding;
                             ++column)
                        {
                            const int index = local_index(row, column);
                            const float red = rgb(direction, index, 0);
                            const float green = rgb(direction, index, 1);
                            const float blue = rgb(direction, index, 2);
                            const float luma = 0.2627F * red + 0.6780F * green + 0.0593F * blue;
                            yuv(0, index) = luma;
                            yuv(1, index) = (blue - luma) * 0.56433F;
                            yuv(2, index) = (red - luma) * 0.67815F;
                        }
                    }
                    const int offset = directions[direction & 3U];
                    const int derivative_padding = passes == 1 ? 9 : 14;
                    for (int row = derivative_padding; row < local_rows - derivative_padding; ++row)
                    {
                        for (int column = derivative_padding;
                             column < local_columns - derivative_padding; ++column)
                        {
                            const int index = local_index(row, column);
                            float value = 0.0F;
                            for (int channel = 0; channel < 3; ++channel)
                            {
                                const float second = 2.0F * yuv(channel, index) -
                                                     yuv(channel, index + offset) -
                                                     yuv(channel, index - offset);
                                value += second * second;
                            }
                            derivative(direction, index) = value;
                        }
                    }
                }

                const int homogeneity_padding = passes == 1 ? 10 : 15;
                for (int row = homogeneity_padding; row < local_rows - homogeneity_padding; ++row)
                {
                    for (int column = homogeneity_padding;
                         column < local_columns - homogeneity_padding; ++column)
                    {
                        const int index = local_index(row, column);
                        float threshold = std::numeric_limits<float>::max();
                        for (unsigned direction = 0U; direction < direction_count; ++direction)
                        {
                            threshold = std::min(threshold, derivative(direction, index));
                        }
                        threshold *= 8.0F;
                        for (unsigned direction = 0U; direction < direction_count; ++direction)
                        {
                            std::uint8_t count = 0U;
                            for (int vertical = -1; vertical <= 1; ++vertical)
                            {
                                for (int horizontal = -1; horizontal <= 1; ++horizontal)
                                {
                                    count += derivative(direction, index + vertical * kTileSize +
                                                                       horizontal) <= threshold ?
                                                 1U :
                                                 0U;
                                }
                            }
                            homogeneous(direction, index) = count;
                        }
                    }
                }

                for (unsigned direction = 0U; direction < direction_count; ++direction)
                {
                    for (int row = tile_border; row < local_rows - tile_border; ++row)
                    {
                        int column = tile_border - 5;
                        std::array<int, 5> vertical_sum{};
                        homogeneous_sum(direction, local_index(row, column)) = 0U;
                        for (++column; column < local_columns - tile_border; ++column)
                        {
                            int column_sum = 0;
                            for (int vertical = -2; vertical <= 2; ++vertical)
                            {
                                column_sum +=
                                    homogeneous(direction, local_index(row + vertical, column + 2));
                            }
                            const int value =
                                static_cast<int>(
                                    homogeneous_sum(direction, local_index(row, column - 1))) -
                                vertical_sum[static_cast<std::size_t>(column % 5)] + column_sum;
                            homogeneous_sum(direction, local_index(row, column)) =
                                static_cast<std::uint8_t>(std::clamp(value, 0, 255));
                            vertical_sum[static_cast<std::size_t>(column % 5)] = column_sum;
                        }
                    }
                }

                for (int row = tile_border; row < local_rows - tile_border; ++row)
                {
                    for (int column = tile_border; column < local_columns - tile_border; ++column)
                    {
                        const int index = local_index(row, column);
                        std::array<std::uint8_t, 8> scores{};
                        std::uint8_t maximum = 0U;
                        for (unsigned direction = 0U; direction < direction_count; ++direction)
                        {
                            scores[direction] = homogeneous_sum(direction, index);
                            maximum = std::max(maximum, scores[direction]);
                        }
                        maximum = static_cast<std::uint8_t>(maximum - (maximum >> 3U));
                        for (unsigned direction = 0U; direction + 4U < direction_count; ++direction)
                        {
                            if (scores[direction] < scores[direction + 4U])
                            {
                                scores[direction] = 0U;
                            }
                            else if (scores[direction] > scores[direction + 4U])
                            {
                                scores[direction + 4U] = 0U;
                            }
                        }
                        std::array<float, 3> average{};
                        unsigned count = 0U;
                        for (unsigned direction = 0U; direction < direction_count; ++direction)
                        {
                            if (scores[direction] < maximum)
                            {
                                continue;
                            }
                            for (int channel = 0; channel < 3; ++channel)
                            {
                                average[static_cast<std::size_t>(channel)] +=
                                    rgb(direction, index, channel);
                            }
                            ++count;
                        }
                        if (count == 0U)
                        {
                            invalid_output.store(true, std::memory_order_relaxed);
                            return;
                        }
                        const int output_row = row + top;
                        const int output_column = column + left;
                        const std::size_t output_base =
                            pixel_index(input.width, static_cast<std::uint32_t>(output_column),
                                        static_cast<std::uint32_t>(output_row)) *
                            3U;
                        for (int channel = 0; channel < 3; ++channel)
                        {
                            const float value =
                                std::max(0.0F, average[static_cast<std::size_t>(channel)] /
                                                   static_cast<float>(count));
                            if (!std::isfinite(value))
                            {
                                invalid_output.store(true, std::memory_order_relaxed);
                                return;
                            }
                            output.rgb[output_base + static_cast<std::size_t>(channel)] = value;
                        }
                    }
                }
            }
        });
    if (!tiles)
    {
        return tiles.error();
    }
    if (stopped.load(std::memory_order_relaxed))
    {
        auto active = cancellation.check();
        return active ? make_error(ErrorCode::kCancelled, "X-Trans demosaic was cancelled") :
                        active.error();
    }
    if (invalid_output.load(std::memory_order_relaxed) ||
        std::any_of(output.rgb.begin(), output.rgb.end(),
                    [](const float sample) { return !std::isfinite(sample); }))
    {
        return make_error(ErrorCode::kValidation, "Markesteijn demosaic produced an invalid sample",
                          {{"reason", "non_finite_demosaic_output"}});
    }
    return output;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Markesteijn demosaic allocation failed",
                      {{"reason", "allocation_failed"}});
}

} // namespace

Result<XTransDemosaicMode> parse_xtrans_demosaic_mode(const std::string_view mode)
{
    if (mode == kXTransDemosaicModeMarkesteijn1)
    {
        return XTransDemosaicMode::kMarkesteijn1;
    }
    if (mode == kXTransDemosaicModeMarkesteijn3)
    {
        return XTransDemosaicMode::kMarkesteijn3;
    }
    return make_error(
        ErrorCode::kUnsupported, "X-Trans demosaic mode is unsupported",
        {{"demosaic_mode", std::string(mode)}, {"reason", "unsupported_demosaic_mode"}});
}

Result<WorkingImage> demosaic_xtrans(const DecodedRaw &raw, const std::uint32_t width,
                                     const std::uint32_t height,
                                     const std::array<float, 4> &white_balance,
                                     const XTransDemosaicMode mode,
                                     const CancellationToken &cancellation)
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    auto prepared = prepare_xtrans(raw, width, height, white_balance, cancellation);
    if (!prepared)
    {
        return prepared.error();
    }
    return markesteijn(prepared.value(), raw.color_profile,
                       mode == XTransDemosaicMode::kMarkesteijn1 ? 1 : 3, cancellation);
}

std::uint64_t estimate_xtrans_demosaic_memory(const std::uint32_t width, const std::uint32_t height,
                                              const XTransDemosaicMode mode) noexcept
{
    constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    if (width != 0U && height > maximum / width)
    {
        return maximum;
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    constexpr std::uint64_t per_pixel = 4U * sizeof(float);
    if (pixels > maximum / per_pixel)
    {
        return maximum;
    }
    std::uint64_t bytes = pixels * per_pixel;
    const std::uint64_t directions = mode == XTransDemosaicMode::kMarkesteijn1 ? 4U : 8U;
    const std::uint64_t floats_per_worker =
        static_cast<std::uint64_t>(kTileSize) * kTileSize * (directions * 4U + 5U);
    const std::uint64_t bytes_per_worker =
        floats_per_worker > maximum / sizeof(float) ? maximum : floats_per_worker * sizeof(float);
    constexpr std::uint64_t byte_planes_per_direction = 2U;
    const std::uint64_t map_bytes =
        static_cast<std::uint64_t>(kTileSize) * kTileSize * directions * byte_planes_per_direction;
    const int tile_border = mode == XTransDemosaicMode::kMarkesteijn1 ? 12 : 17;
    const int tile_step = kTileSize - 2 * tile_border;
    const std::uint64_t tile_rows =
        (static_cast<std::uint64_t>(height) + static_cast<std::uint64_t>(tile_step) - 1U) /
        static_cast<std::uint64_t>(tile_step);
    const std::uint64_t workers = std::min<std::uint64_t>(detail::kParallelRowWorkerLimit,
                                                          std::max<std::uint64_t>(1U, tile_rows));
    const std::uint64_t scratch =
        bytes_per_worker == maximum || map_bytes > maximum - bytes_per_worker ?
            maximum :
            (bytes_per_worker + map_bytes > maximum / workers ?
                 maximum :
                 (bytes_per_worker + map_bytes) * workers);
    return scratch == maximum || scratch > maximum - bytes ? maximum : bytes + scratch;
}

} // namespace ravo
