#include "bayer_demosaic.h"

// RCD is an adapted scalar C++20 port of RawTherapee commit 498f62378,
// rtengine/rcd_demosaic.cc (RCD 2.3 by Luis Sanz Rodriguez, tiled by Ingo
// Weyrich), GPL-3.0-or-later. PPG follows that checkout's bundled LibRaw
// rtengine/libraw/src/demosaic/misc_demosaic.cpp implementation by Alain
// Desbiolles, Copyright 2019-2025 LibRaw LLC, used under LGPL-2.1.
// Modified for Ravo on 2026-08-30: application globals, OpenMP, callbacks/UI,
// integer clipping and the implicit IGV fallback were removed; bounds,
// cancellation, allocation failure, source ownership, float headroom and
// same-CFA preview reduction are Ravo-owned. See DevDocs/THIRD_PARTY_NOTICES.md.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "dng_opcodes.h"
#include "gpu_adapter.h"
#include "parallel_rows.h"

namespace ravo
{
namespace
{

struct PreparedBayer
{
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::array<std::uint8_t, 4> cfa{};
    std::vector<float> samples;
};

// RCD tile geometry (RawTherapee-adapted). Window demosaic must expand onto this
// absolute sensor grid so owned pixels match full-frame demosaic+crop.
constexpr int kRcdTileBorder = 9;
constexpr int kRcdAlgorithmBorder = 9;
constexpr int kRcdTileSize = 194;
constexpr int kRcdTileStep = kRcdTileSize - 2 * kRcdTileBorder;
// Apron beyond the owned ROI: must cover algorithm/tile border so border_interpolate
// and incomplete edge tiles cannot touch owned pixels after crop.
constexpr std::uint32_t kBayerWindowApron = 12U;
static_assert(kRcdTileBorder == kRcdAlgorithmBorder);
static_assert(static_cast<int>(kBayerWindowApron) >= kRcdAlgorithmBorder);

struct BayerWindowBounds
{
    std::uint32_t x0 = 0U;
    std::uint32_t y0 = 0U;
    std::uint32_t x1 = 0U;
    std::uint32_t y1 = 0U;
};

[[nodiscard]] BayerWindowBounds
bayer_window_demosaic_bounds(const std::uint32_t raw_width, const std::uint32_t raw_height,
                             const std::uint32_t origin_x, const std::uint32_t origin_y,
                             const std::uint32_t width, const std::uint32_t height,
                             const bool align_rcd_tiles) noexcept
{
    BayerWindowBounds bounds;
    bounds.x0 = origin_x > kBayerWindowApron ? origin_x - kBayerWindowApron : 0U;
    bounds.y0 = origin_y > kBayerWindowApron ? origin_y - kBayerWindowApron : 0U;
    bounds.x1 = std::min(raw_width, origin_x + width + kBayerWindowApron);
    bounds.y1 = std::min(raw_height, origin_y + height + kBayerWindowApron);
    if (!align_rcd_tiles || kRcdTileStep <= 0)
        return bounds;
    // Snap the prepared origin down onto the full-frame RCD tile grid so local
    // tile 0 == global tile at sensor (x0,y0). Expand the far edge to include
    // complete tiles that write into the owned ROI (matching full-frame clip).
    bounds.x0 = (bounds.x0 / static_cast<std::uint32_t>(kRcdTileStep)) *
                static_cast<std::uint32_t>(kRcdTileStep);
    bounds.y0 = (bounds.y0 / static_cast<std::uint32_t>(kRcdTileStep)) *
                static_cast<std::uint32_t>(kRcdTileStep);
    const std::uint32_t owned_last_x = origin_x + width - 1U;
    const std::uint32_t owned_last_y = origin_y + height - 1U;
    const std::uint32_t last_tile_x = (owned_last_x / static_cast<std::uint32_t>(kRcdTileStep)) *
                                      static_cast<std::uint32_t>(kRcdTileStep);
    const std::uint32_t last_tile_y = (owned_last_y / static_cast<std::uint32_t>(kRcdTileStep)) *
                                      static_cast<std::uint32_t>(kRcdTileStep);
    bounds.x1 = std::max(
        bounds.x1, std::min(raw_width, last_tile_x + static_cast<std::uint32_t>(kRcdTileSize)));
    bounds.y1 = std::max(
        bounds.y1, std::min(raw_height, last_tile_y + static_cast<std::uint32_t>(kRcdTileSize)));
    return bounds;
}

class FloatBuffer
{
public:
    FloatBuffer(const std::size_t size, const float value)
        : values_(size, value)
    {
    }

    [[nodiscard]] float &operator[](const int index) noexcept
    {
        return values_[static_cast<std::size_t>(index)];
    }

    [[nodiscard]] const float &operator[](const int index) const noexcept
    {
        return values_[static_cast<std::size_t>(index)];
    }

    [[nodiscard]] float &operator[](const std::size_t index) noexcept
    {
        return values_[index];
    }
    [[nodiscard]] const float &operator[](const std::size_t index) const noexcept
    {
        return values_[index];
    }
    [[nodiscard]] auto begin() noexcept
    {
        return values_.begin();
    }
    [[nodiscard]] auto end() noexcept
    {
        return values_.end();
    }
    [[nodiscard]] std::size_t size() const noexcept
    {
        return values_.size();
    }

private:
    std::vector<float> values_;
};

[[nodiscard]] constexpr std::size_t pixel_index(const std::uint32_t width, const std::uint32_t x,
                                                const std::uint32_t y) noexcept
{
    return static_cast<std::size_t>(y) * width + x;
}

[[nodiscard]] constexpr std::uint8_t cfa_at(const std::array<std::uint8_t, 4> &cfa, const int y,
                                            const int x) noexcept
{
    return cfa[(static_cast<unsigned>(y) & 1U) * 2U + (static_cast<unsigned>(x) & 1U)];
}

[[nodiscard]] constexpr std::uint8_t cfa_at(const std::array<std::uint8_t, 4> &cfa,
                                            const std::uint32_t y, const std::uint32_t x) noexcept
{
    return cfa[(y & 1U) * 2U + (x & 1U)];
}

[[nodiscard]] Result<void> validate_bayer(const DecodedRaw &raw)
{
    if (raw.width == 0U || raw.height == 0U || raw.cfa_width != 2U || raw.cfa_height != 2U ||
        raw.cfa_channels.size() != 4U ||
        raw.width > std::numeric_limits<std::size_t>::max() / raw.height ||
        raw.pixels.size() != static_cast<std::size_t>(raw.width) * raw.height)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Bayer demosaic requires one complete 2x2 CFA frame",
                          {{"reason", "unsupported_raw_sensor"}, {"sensor", "non_bayer"}});
    }
    std::array<std::uint32_t, 3> counts{};
    for (const std::uint8_t channel : raw.cfa_channels)
    {
        if (channel > 2U)
        {
            return make_error(ErrorCode::kUnsupported, "Bayer demosaic requires RGB CFA channels",
                              {{"reason", "unsupported_bayer_cfa"}});
        }
        ++counts[channel];
    }
    if (counts != std::array<std::uint32_t, 3>{1U, 2U, 1U})
    {
        return make_error(ErrorCode::kUnsupported,
                          "Bayer demosaic requires one red, two green and one blue site",
                          {{"reason", "unsupported_bayer_cfa"}});
    }
    return {};
}

[[nodiscard]] Result<PreparedBayer> prepare_bayer(const DecodedRaw &raw, const std::uint32_t width,
                                                  const std::uint32_t height,
                                                  const std::array<float, 4> &white_balance,
                                                  const CancellationToken &cancellation)
try
{
    auto valid = validate_bayer(raw);
    if (!valid)
    {
        return valid.error();
    }
    if (width == 0U || height == 0U || width > std::numeric_limits<std::size_t>::max() / height)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Bayer demosaic dimensions must be non-zero and bounded",
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

    PreparedBayer prepared;
    prepared.width = width;
    prepared.height = height;
    std::copy_n(raw.cfa_channels.begin(), 4U, prepared.cfa.begin());
    prepared.samples.resize(static_cast<std::size_t>(width) * height);
    const float denominator = static_cast<float>(
        std::max<std::int64_t>(1, static_cast<std::int64_t>(raw.white_level) - raw.black_level));
    const bool defer_white_balance = dng_list3_requires_deferred_white_balance(raw.dng_opcodes);
    std::atomic_bool invalid_sample{false};
    const auto convert_sample = [&](const std::uint32_t source_x, const std::uint32_t source_y,
                                    const std::uint8_t channel) -> float
    {
        float sample = std::max(
            0.0F, (static_cast<float>(raw.pixels[pixel_index(raw.width, source_x, source_y)]) -
                   static_cast<float>(raw.black_level)) /
                      denominator);
        if (raw.dng_opcodes)
        {
            sample = apply_dng_opcode_list2_sample(*raw.dng_opcodes, source_x, source_y, raw.width,
                                                   raw.height, sample);
        }
        if (!defer_white_balance)
        {
            sample *= white_balance[channel];
        }
        return sample;
    };
    Result<void> rows{};
    if (width == raw.width && height == raw.height)
    {
        rows = detail::for_each_row(
            height, cancellation,
            [&](const std::uint32_t output_y)
            {
                if (invalid_sample.load(std::memory_order_relaxed))
                {
                    return;
                }
                for (std::uint32_t output_x = 0U; output_x < width; ++output_x)
                {
                    const std::uint8_t wanted = cfa_at(prepared.cfa, output_y, output_x);
                    const float sample = convert_sample(output_x, output_y, wanted);
                    if (!std::isfinite(sample))
                    {
                        invalid_sample.store(true, std::memory_order_relaxed);
                        return;
                    }
                    prepared.samples[pixel_index(width, output_x, output_y)] = sample;
                }
            });
    }
    else
    {
        rows = detail::for_each_row(
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
                    const std::uint8_t wanted = cfa_at(prepared.cfa, output_y, output_x);
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
                            const float sample = convert_sample(source_x, source_y, wanted);
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
                        std::uint32_t source_x = std::min(
                            raw.width - 1U,
                            static_cast<std::uint32_t>(
                                (static_cast<std::uint64_t>(output_x) * raw.width) / width));
                        std::uint32_t source_y = std::min(
                            raw.height - 1U,
                            static_cast<std::uint32_t>(
                                (static_cast<std::uint64_t>(output_y) * raw.height) / height));
                        bool found = false;
                        for (int radius = 0; radius <= 2 && !found; ++radius)
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
                                    if (cfa_at(prepared.cfa, candidate_y, candidate_x) != wanted)
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
                        const float sample = convert_sample(source_x, source_y, wanted);
                        if (!std::isfinite(sample))
                        {
                            invalid_sample.store(true, std::memory_order_relaxed);
                            return;
                        }
                        sum = sample;
                        count = 1U;
                    }
                    prepared.samples[pixel_index(width, output_x, output_y)] =
                        static_cast<float>(sum / count);
                }
            });
    }
    if (!rows)
    {
        return rows.error();
    }
    if (invalid_sample.load(std::memory_order_relaxed))
    {
        return make_error(ErrorCode::kValidation, "Bayer preparation produced a non-finite sample",
                          {{"reason", "non_finite_bayer_sample"}});
    }
    return prepared;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Bayer preparation allocation failed",
                      {{"reason", "allocation_failed"}});
}

void seed_known_samples(const PreparedBayer &input, WorkingImage &output)
{
    for (std::uint32_t y = 0U; y < input.height; ++y)
    {
        for (std::uint32_t x = 0U; x < input.width; ++x)
        {
            const std::size_t index = pixel_index(input.width, x, y);
            output.rgb[index * 3U + cfa_at(input.cfa, y, x)] = input.samples[index];
        }
    }
}

[[nodiscard]] Result<void> border_interpolate(const PreparedBayer &input, WorkingImage &output,
                                              const std::uint32_t requested_border,
                                              const CancellationToken &cancellation)
{
    const std::uint32_t border = std::min({requested_border, input.width, input.height});
    for (std::uint32_t y = 0U; y < input.height; ++y)
    {
        auto active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (std::uint32_t x = 0U; x < input.width; ++x)
        {
            if (y >= border && y < input.height - border && x >= border && x < input.width - border)
            {
                continue;
            }
            std::array<float, 3> sum{};
            std::array<std::uint32_t, 3> count{};
            for (int offset_y = -1; offset_y <= 1; ++offset_y)
            {
                const int sample_y = static_cast<int>(y) + offset_y;
                if (sample_y < 0 || sample_y >= static_cast<int>(input.height))
                {
                    continue;
                }
                for (int offset_x = -1; offset_x <= 1; ++offset_x)
                {
                    const int sample_x = static_cast<int>(x) + offset_x;
                    if (sample_x < 0 || sample_x >= static_cast<int>(input.width))
                    {
                        continue;
                    }
                    const std::uint8_t channel = cfa_at(input.cfa, sample_y, sample_x);
                    sum[channel] +=
                        input.samples[pixel_index(input.width, static_cast<std::uint32_t>(sample_x),
                                                  static_cast<std::uint32_t>(sample_y))];
                    ++count[channel];
                }
            }
            const std::uint8_t known = cfa_at(input.cfa, y, x);
            const std::size_t base = pixel_index(input.width, x, y) * 3U;
            for (std::uint8_t channel = 0U; channel < 3U; ++channel)
            {
                if (channel == known)
                {
                    output.rgb[base + channel] = input.samples[pixel_index(input.width, x, y)];
                }
                else if (count[channel] != 0U)
                {
                    output.rgb[base + channel] = sum[channel] / static_cast<float>(count[channel]);
                }
            }
        }
    }
    return {};
}

[[nodiscard]] constexpr float interpolate(const float weight, const float first,
                                          const float second) noexcept
{
    return weight * (first - second) + second;
}

[[nodiscard]] Result<WorkingImage> demosaic_rcd(const PreparedBayer &input,
                                                const ColorProfileState &profile,
                                                const CancellationToken &cancellation)
try
{
    constexpr int tile_border = kRcdTileBorder;
    constexpr int algorithm_border = kRcdAlgorithmBorder;
    constexpr int tile_size = kRcdTileSize;
    constexpr int tile_step = kRcdTileStep;
    constexpr float epsilon = 1.0e-5F;
    constexpr float epsilon_squared = 1.0e-10F;
    constexpr int w1 = tile_size;
    constexpr int w2 = 2 * tile_size;
    constexpr int w3 = 3 * tile_size;
    constexpr int w4 = 4 * tile_size;

    WorkingImage output;
    output.width = input.width;
    output.height = input.height;
    output.rgb.assign(static_cast<std::size_t>(input.width) * input.height * 3U, 0.0F);
    output.color_profile = profile;
    seed_known_samples(input, output);
    const int tile_rows = (static_cast<int>(input.height) + tile_step - 1) / tile_step;
    const int tile_columns = (static_cast<int>(input.width) + tile_step - 1) / tile_step;
    std::atomic_bool cancelled{false};

    const auto tiles = detail::for_each_row(
        static_cast<std::uint32_t>(tile_rows), cancellation,
        [&](const std::uint32_t tile_row_value)
        {
            FloatBuffer cfa(static_cast<std::size_t>(tile_size) * tile_size, 0.0F);
            FloatBuffer rgb(static_cast<std::size_t>(tile_size) * tile_size * 3U, 0.0F);
            FloatBuffer vh(static_cast<std::size_t>(tile_size) * tile_size, 0.0F);
            FloatBuffer pq(static_cast<std::size_t>(tile_size) * tile_size / 2U, 0.0F);
            FloatBuffer p_high(pq.size(), 0.0F);
            FloatBuffer q_high(pq.size(), 0.0F);
            const auto channel = [&](const int c, const int index) -> float &
            {
                const auto offset = static_cast<std::size_t>(c) * tile_size * tile_size;
                return rgb[offset + static_cast<std::size_t>(index)];
            };
            const int tile_row = static_cast<int>(tile_row_value);
            for (int tile_column = 0; tile_column < tile_columns; ++tile_column)
            {
                if (!cancellation.check())
                {
                    cancelled.store(true, std::memory_order_relaxed);
                    return;
                }
                std::fill(cfa.begin(), cfa.end(), 0.0F);
                std::fill(rgb.begin(), rgb.end(), 0.0F);
                std::fill(vh.begin(), vh.end(), 0.0F);
                std::fill(pq.begin(), pq.end(), 0.0F);
                std::fill(p_high.begin(), p_high.end(), 0.0F);
                std::fill(q_high.begin(), q_high.end(), 0.0F);
                const int row_start = tile_row * tile_step;
                const int row_end = std::min(row_start + tile_size, static_cast<int>(input.height));
                const int column_start = tile_column * tile_step;
                const int column_end =
                    std::min(column_start + tile_size, static_cast<int>(input.width));
                if (row_start + tile_border >= row_end - tile_border ||
                    column_start + tile_border >= column_end - tile_border)
                {
                    continue;
                }
                const int rows = row_end - row_start;
                const int columns = column_end - column_start;
                for (int row = row_start; row < row_end; ++row)
                {
                    if (!cancellation.check())
                    {
                        cancelled.store(true, std::memory_order_relaxed);
                        return;
                    }
                    const int c0 = cfa_at(input.cfa, row, column_start);
                    const int c1 = cfa_at(input.cfa, row, column_start + 1);
                    for (int column = column_start, index = (row - row_start) * tile_size;
                         column < column_end; ++column, ++index)
                    {
                        const float value = input.samples[pixel_index(
                            input.width, static_cast<std::uint32_t>(column),
                            static_cast<std::uint32_t>(row))];
                        cfa[index] = value;
                        channel(c0, index) = value;
                        channel(c1, index) = value;
                    }
                }

                std::array<std::array<float, tile_size - 8>, 3> vertical_buffer{};
                for (int row = 3; row < std::min(rows - 3, 5); ++row)
                {
                    for (int column = 4, index = row * tile_size + column; column < columns - 4;
                         ++column, ++index)
                    {
                        const float high = (cfa[index - w3] - cfa[index - w1] - cfa[index + w1] +
                                            cfa[index + w3]) -
                                           3.0F * (cfa[index - w2] + cfa[index + w2]) +
                                           6.0F * cfa[index];
                        vertical_buffer[static_cast<std::size_t>(row - 3)]
                                       [static_cast<std::size_t>(column - 4)] = high * high;
                    }
                }
                std::array<float, tile_size - 6> horizontal_buffer{};
                float *vertical0 = vertical_buffer[0].data();
                float *vertical1 = vertical_buffer[1].data();
                float *vertical2 = vertical_buffer[2].data();
                for (int row = 4; row < rows - 4; ++row)
                {
                    for (int column = 3, index = row * tile_size + column; column < columns - 3;
                         ++column, ++index)
                    {
                        const float high =
                            (cfa[index - 3] - cfa[index - 1] - cfa[index + 1] + cfa[index + 3]) -
                            3.0F * (cfa[index - 2] + cfa[index + 2]) + 6.0F * cfa[index];
                        horizontal_buffer[static_cast<std::size_t>(column - 3)] = high * high;
                    }
                    for (int column = 4, index = (row + 1) * tile_size + column;
                         column < columns - 4; ++column, ++index)
                    {
                        const float high = (cfa[index - w3] - cfa[index - w1] - cfa[index + w1] +
                                            cfa[index + w3]) -
                                           3.0F * (cfa[index - w2] + cfa[index + w2]) +
                                           6.0F * cfa[index];
                        vertical2[column - 4] = high * high;
                    }
                    for (int column = 4, index = row * tile_size + column; column < columns - 4;
                         ++column, ++index)
                    {
                        const float vertical = std::max(epsilon_squared, vertical0[column - 4] +
                                                                             vertical1[column - 4] +
                                                                             vertical2[column - 4]);
                        const float horizontal =
                            std::max(epsilon_squared,
                                     horizontal_buffer[static_cast<std::size_t>(column - 4)] +
                                         horizontal_buffer[static_cast<std::size_t>(column - 3)] +
                                         horizontal_buffer[static_cast<std::size_t>(column - 2)]);
                        vh[index] = vertical / (vertical + horizontal);
                    }
                    std::swap(vertical0, vertical2);
                    std::swap(vertical0, vertical1);
                }

                for (int row = 2; row < rows - 2; ++row)
                {
                    for (int column = 2 + (cfa_at(input.cfa, row, 0) & 1),
                             index = row * tile_size + column, low_index = index / 2;
                         column < columns - 2; column += 2, index += 2, ++low_index)
                    {
                        pq[low_index] = cfa[index] +
                                        0.5F * (cfa[index - w1] + cfa[index + w1] + cfa[index - 1] +
                                                cfa[index + 1]) +
                                        0.25F * (cfa[index - w1 - 1] + cfa[index - w1 + 1] +
                                                 cfa[index + w1 - 1] + cfa[index + w1 + 1]);
                    }
                }
                for (int row = 4; row < rows - 4; ++row)
                {
                    for (int column = 4 + (cfa_at(input.cfa, row, 0) & 1),
                             index = row * tile_size + column, low_index = index / 2;
                         column < columns - 4; column += 2, index += 2, ++low_index)
                    {
                        const float center = cfa[index];
                        const float north_gradient = epsilon +
                                                     std::abs(cfa[index - w1] - cfa[index + w1]) +
                                                     std::abs(center - cfa[index - w2]) +
                                                     std::abs(cfa[index - w1] - cfa[index - w3]) +
                                                     std::abs(cfa[index - w2] - cfa[index - w4]);
                        const float south_gradient = epsilon +
                                                     std::abs(cfa[index - w1] - cfa[index + w1]) +
                                                     std::abs(center - cfa[index + w2]) +
                                                     std::abs(cfa[index + w1] - cfa[index + w3]) +
                                                     std::abs(cfa[index + w2] - cfa[index + w4]);
                        const float west_gradient = epsilon +
                                                    std::abs(cfa[index - 1] - cfa[index + 1]) +
                                                    std::abs(center - cfa[index - 2]) +
                                                    std::abs(cfa[index - 1] - cfa[index - 3]) +
                                                    std::abs(cfa[index - 2] - cfa[index - 4]);
                        const float east_gradient = epsilon +
                                                    std::abs(cfa[index - 1] - cfa[index + 1]) +
                                                    std::abs(center - cfa[index + 2]) +
                                                    std::abs(cfa[index + 1] - cfa[index + 3]) +
                                                    std::abs(cfa[index + 2] - cfa[index + 4]);
                        const float low = pq[low_index];
                        const float north =
                            cfa[index - w1] * (low + low) / (epsilon + low + pq[low_index - w1]);
                        const float south =
                            cfa[index + w1] * (low + low) / (epsilon + low + pq[low_index + w1]);
                        const float west =
                            cfa[index - 1] * (low + low) / (epsilon + low + pq[low_index - 1]);
                        const float east =
                            cfa[index + 1] * (low + low) / (epsilon + low + pq[low_index + 1]);
                        const float vertical = (south_gradient * north + north_gradient * south) /
                                               (north_gradient + south_gradient);
                        const float horizontal = (west_gradient * east + east_gradient * west) /
                                                 (east_gradient + west_gradient);
                        const float neighborhood =
                            0.25F * (vh[index - w1 - 1] + vh[index - w1 + 1] + vh[index + w1 - 1] +
                                     vh[index + w1 + 1]);
                        const float direction =
                            std::abs(0.5F - vh[index]) < std::abs(0.5F - neighborhood) ?
                                neighborhood :
                                vh[index];
                        channel(1, index) = interpolate(direction, horizontal, vertical);
                    }
                }

                for (int row = 3; row < rows - 3; ++row)
                {
                    for (int column = 3, index = row * tile_size + column, half_index = index / 2;
                         column < columns - 3; column += 2, index += 2, ++half_index)
                    {
                        const float p = (cfa[index - w3 - 3] - cfa[index - w1 - 1] -
                                         cfa[index + w1 + 1] + cfa[index + w3 + 3]) -
                                        3.0F * (cfa[index - w2 - 2] + cfa[index + w2 + 2]) +
                                        6.0F * cfa[index];
                        const float q = (cfa[index - w3 + 3] - cfa[index - w1 + 1] -
                                         cfa[index + w1 - 1] + cfa[index + w3 - 3]) -
                                        3.0F * (cfa[index - w2 + 2] + cfa[index + w2 - 2]) +
                                        6.0F * cfa[index];
                        p_high[half_index] = p * p;
                        q_high[half_index] = q * q;
                    }
                }
                for (int row = 4; row < rows - 4; ++row)
                {
                    for (int column = 4 + (cfa_at(input.cfa, row, 0) & 1),
                             index = row * tile_size + column, half = index / 2,
                             north_west = (index - w1 - 1) / 2, south_west = (index + w1 - 1) / 2;
                         column < columns - 4;
                         column += 2, index += 2, ++half, ++north_west, ++south_west)
                    {
                        const float p =
                            std::max(epsilon_squared,
                                     p_high[north_west] + p_high[half] + p_high[south_west + 1]);
                        const float q =
                            std::max(epsilon_squared,
                                     q_high[north_west + 1] + q_high[half] + q_high[south_west]);
                        pq[half] = p / (p + q);
                    }
                }
                for (int row = 4; row < rows - 4; ++row)
                {
                    for (int column = 4 + (cfa_at(input.cfa, row, 0) & 1),
                             index = row * tile_size + column,
                             opposite = 2 - cfa_at(input.cfa, row, column), half = index / 2,
                             north_west = (index - w1 - 1) / 2, south_west = (index + w1 - 1) / 2;
                         column < columns - 4;
                         column += 2, index += 2, ++half, ++north_west, ++south_west)
                    {
                        const float neighborhood = 0.25F * (pq[north_west] + pq[north_west + 1] +
                                                            pq[south_west] + pq[south_west + 1]);
                        const float direction =
                            std::abs(0.5F - pq[half]) < std::abs(0.5F - neighborhood) ?
                                neighborhood :
                                pq[half];
                        const float north_west_gradient =
                            epsilon +
                            std::abs(channel(opposite, index - w1 - 1) -
                                     channel(opposite, index + w1 + 1)) +
                            std::abs(channel(opposite, index - w1 - 1) -
                                     channel(opposite, index - w3 - 3)) +
                            std::abs(channel(1, index) - channel(1, index - w2 - 2));
                        const float north_east_gradient =
                            epsilon +
                            std::abs(channel(opposite, index - w1 + 1) -
                                     channel(opposite, index + w1 - 1)) +
                            std::abs(channel(opposite, index - w1 + 1) -
                                     channel(opposite, index - w3 + 3)) +
                            std::abs(channel(1, index) - channel(1, index - w2 + 2));
                        const float south_west_gradient =
                            epsilon +
                            std::abs(channel(opposite, index - w1 + 1) -
                                     channel(opposite, index + w1 - 1)) +
                            std::abs(channel(opposite, index + w1 - 1) -
                                     channel(opposite, index + w3 - 3)) +
                            std::abs(channel(1, index) - channel(1, index + w2 - 2));
                        const float south_east_gradient =
                            epsilon +
                            std::abs(channel(opposite, index - w1 - 1) -
                                     channel(opposite, index + w1 + 1)) +
                            std::abs(channel(opposite, index + w1 + 1) -
                                     channel(opposite, index + w3 + 3)) +
                            std::abs(channel(1, index) - channel(1, index + w2 + 2));
                        const float north_west_difference =
                            channel(opposite, index - w1 - 1) - channel(1, index - w1 - 1);
                        const float north_east_difference =
                            channel(opposite, index - w1 + 1) - channel(1, index - w1 + 1);
                        const float south_west_difference =
                            channel(opposite, index + w1 - 1) - channel(1, index + w1 - 1);
                        const float south_east_difference =
                            channel(opposite, index + w1 + 1) - channel(1, index + w1 + 1);
                        const float p = (north_west_gradient * south_east_difference +
                                         south_east_gradient * north_west_difference) /
                                        (north_west_gradient + south_east_gradient);
                        const float q = (north_east_gradient * south_west_difference +
                                         south_west_gradient * north_east_difference) /
                                        (north_east_gradient + south_west_gradient);
                        channel(opposite, index) = channel(1, index) + interpolate(direction, q, p);
                    }
                }
                for (int row = 4; row < rows - 4; ++row)
                {
                    for (int column = 4 + (cfa_at(input.cfa, row, 1) & 1),
                             index = row * tile_size + column;
                         column < columns - 4; column += 2, index += 2)
                    {
                        const float neighborhood =
                            0.25F * (vh[index - w1 - 1] + vh[index - w1 + 1] + vh[index + w1 - 1] +
                                     vh[index + w1 + 1]);
                        const float direction =
                            std::abs(0.5F - vh[index]) < std::abs(0.5F - neighborhood) ?
                                neighborhood :
                                vh[index];
                        const float green = channel(1, index);
                        const float north_green = channel(1, index - w1);
                        const float south_green = channel(1, index + w1);
                        const float west_green = channel(1, index - 1);
                        const float east_green = channel(1, index + 1);
                        for (int color = 0; color <= 2; color += 2)
                        {
                            const float north_base =
                                epsilon + std::abs(green - channel(1, index - w2));
                            const float south_base =
                                epsilon + std::abs(green - channel(1, index + w2));
                            const float west_base =
                                epsilon + std::abs(green - channel(1, index - 2));
                            const float east_base =
                                epsilon + std::abs(green - channel(1, index + 2));
                            const float vertical_difference =
                                std::abs(channel(color, index - w1) - channel(color, index + w1));
                            const float horizontal_difference =
                                std::abs(channel(color, index - 1) - channel(color, index + 1));
                            const float north_gradient =
                                north_base + vertical_difference +
                                std::abs(channel(color, index - w1) - channel(color, index - w3));
                            const float south_gradient =
                                south_base + vertical_difference +
                                std::abs(channel(color, index + w1) - channel(color, index + w3));
                            const float west_gradient =
                                west_base + horizontal_difference +
                                std::abs(channel(color, index - 1) - channel(color, index - 3));
                            const float east_gradient =
                                east_base + horizontal_difference +
                                std::abs(channel(color, index + 1) - channel(color, index + 3));
                            const float north = channel(color, index - w1) - north_green;
                            const float south = channel(color, index + w1) - south_green;
                            const float west = channel(color, index - 1) - west_green;
                            const float east = channel(color, index + 1) - east_green;
                            const float vertical =
                                (north_gradient * south + south_gradient * north) /
                                (north_gradient + south_gradient);
                            const float horizontal = (east_gradient * west + west_gradient * east) /
                                                     (east_gradient + west_gradient);
                            channel(color, index) =
                                green + interpolate(direction, horizontal, vertical);
                        }
                    }
                }

                const int first_row = row_start + (tile_row == 0 ? algorithm_border : tile_border);
                const int last_row =
                    row_end - (tile_row == tile_rows - 1 ? algorithm_border : tile_border);
                const int first_column =
                    column_start + (tile_column == 0 ? algorithm_border : tile_border);
                const int last_column =
                    column_end - (tile_column == tile_columns - 1 ? algorithm_border : tile_border);
                for (int row = first_row; row < last_row; ++row)
                {
                    for (int column = first_column; column < last_column; ++column)
                    {
                        const int local = (row - row_start) * tile_size + column - column_start;
                        const std::size_t destination =
                            pixel_index(input.width, static_cast<std::uint32_t>(column),
                                        static_cast<std::uint32_t>(row)) *
                            3U;
                        output.rgb[destination] = std::max(0.0F, channel(0, local));
                        output.rgb[destination + 1U] = std::max(0.0F, channel(1, local));
                        output.rgb[destination + 2U] = std::max(0.0F, channel(2, local));
                    }
                }
            }
        });
    if (!tiles)
    {
        return tiles.error();
    }
    if (cancelled.load(std::memory_order_relaxed))
    {
        auto active = cancellation.check();
        return active ? make_error(ErrorCode::kCancelled, "RCD demosaic cancelled") :
                        active.error();
    }
    auto border = border_interpolate(input, output, algorithm_border, cancellation);
    if (!border)
    {
        return border.error();
    }
    return output;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "RCD demosaic allocation failed",
                      {{"reason", "allocation_failed"}});
}

[[nodiscard]] float median3(const float first, const float second, const float third) noexcept
{
    return std::max(std::min(first, second), std::min(std::max(first, second), third));
}

[[nodiscard]] Result<WorkingImage> demosaic_ppg(const PreparedBayer &input,
                                                const ColorProfileState &profile,
                                                const CancellationToken &cancellation)
try
{
    WorkingImage output;
    output.width = input.width;
    output.height = input.height;
    output.rgb.assign(static_cast<std::size_t>(input.width) * input.height * 3U, 0.0F);
    output.color_profile = profile;
    seed_known_samples(input, output);
    auto border = border_interpolate(input, output, 3U, cancellation);
    if (!border)
    {
        return border.error();
    }
    const int width = static_cast<int>(input.width);
    const int height = static_cast<int>(input.height);
    const int direction[5]{1, width, -1, -width, 1};
    const auto value = [&](const int index, const int channel) -> float &
    {
        return output.rgb[static_cast<std::size_t>(index) * 3U + static_cast<std::size_t>(channel)];
    };

    for (int row = 3; row < height - 3; ++row)
    {
        auto active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (int column = 3 + (cfa_at(input.cfa, row, 3) & 1),
                 color = cfa_at(input.cfa, row, column);
             column < width - 3; column += 2)
        {
            const int index = row * width + column;
            float difference[2]{};
            float guess[2]{};
            for (int axis = 0; axis < 2; ++axis)
            {
                const int distance = direction[axis];
                guess[axis] = (value(index - distance, 1) + value(index, color) +
                               value(index + distance, 1)) *
                                  2.0F -
                              value(index - 2 * distance, color) -
                              value(index + 2 * distance, color);
                difference[axis] =
                    (std::abs(value(index - 2 * distance, color) - value(index, color)) +
                     std::abs(value(index + 2 * distance, color) - value(index, color)) +
                     std::abs(value(index - distance, 1) - value(index + distance, 1))) *
                        3.0F +
                    (std::abs(value(index + 3 * distance, 1) - value(index + distance, 1)) +
                     std::abs(value(index - 3 * distance, 1) - value(index - distance, 1))) *
                        2.0F;
            }
            const int axis = difference[0] > difference[1] ? 1 : 0;
            const int distance = direction[axis];
            value(index, 1) = median3(guess[axis] * 0.25F, value(index + distance, 1),
                                      value(index - distance, 1));
        }
    }
    for (int row = 1; row < height - 1; ++row)
    {
        auto active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (int column = 1 + (cfa_at(input.cfa, row, 2) & 1),
                 color = cfa_at(input.cfa, row, column + 1);
             column < width - 1; column += 2)
        {
            const int index = row * width + column;
            for (int axis = 0; axis < 2; color = 2 - color, ++axis)
            {
                const int distance = direction[axis];
                value(index, color) = std::max(
                    0.0F, 0.5F * (value(index - distance, color) + value(index + distance, color) +
                                  2.0F * value(index, 1) - value(index - distance, 1) -
                                  value(index + distance, 1)));
            }
        }
    }
    for (int row = 1; row < height - 1; ++row)
    {
        auto active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (int column = 1 + (cfa_at(input.cfa, row, 1) & 1),
                 color = 2 - cfa_at(input.cfa, row, column);
             column < width - 1; column += 2)
        {
            const int index = row * width + column;
            float difference[2]{};
            float guess[2]{};
            for (int diagonal = 0; diagonal < 2; ++diagonal)
            {
                const int distance = direction[diagonal] + direction[diagonal + 1];
                difference[diagonal] =
                    std::abs(value(index - distance, color) - value(index + distance, color)) +
                    std::abs(value(index - distance, 1) - value(index, 1)) +
                    std::abs(value(index + distance, 1) - value(index, 1));
                guess[diagonal] = value(index - distance, color) + value(index + distance, color) +
                                  2.0F * value(index, 1) - value(index - distance, 1) -
                                  value(index + distance, 1);
            }
            value(index, color) =
                difference[0] == difference[1] ?
                    std::max(0.0F, (guess[0] + guess[1]) * 0.25F) :
                    std::max(0.0F, guess[difference[0] > difference[1] ? 1 : 0] * 0.5F);
        }
    }
    return output;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "PPG demosaic allocation failed",
                      {{"reason", "allocation_failed"}});
}

[[nodiscard]] Result<PreparedBayer>
prepare_bayer_window(const DecodedRaw &raw, const std::uint32_t origin_x,
                     const std::uint32_t origin_y, const std::uint32_t width,
                     const std::uint32_t height, const std::array<float, 4> &white_balance,
                     const CancellationToken &cancellation)
try
{
    auto valid = validate_bayer(raw);
    if (!valid)
    {
        return valid.error();
    }
    if (width == 0U || height == 0U || origin_x >= raw.width || origin_y >= raw.height ||
        width > raw.width - origin_x || height > raw.height - origin_y)
    {
        return make_error(ErrorCode::kInvalidArgument, "Bayer window is outside the CFA frame",
                          {{"reason", "invalid_bayer_window"},
                           {"origin_x", std::to_string(origin_x)},
                           {"origin_y", std::to_string(origin_y)},
                           {"width", std::to_string(width)},
                           {"height", std::to_string(height)}});
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

    PreparedBayer prepared;
    prepared.width = width;
    prepared.height = height;
    std::array<std::uint8_t, 4> source_cfa{};
    std::copy_n(raw.cfa_channels.begin(), 4U, source_cfa.begin());
    prepared.cfa[0] = cfa_at(source_cfa, origin_y, origin_x);
    prepared.cfa[1] = cfa_at(source_cfa, origin_y, origin_x + 1U);
    prepared.cfa[2] = cfa_at(source_cfa, origin_y + 1U, origin_x);
    prepared.cfa[3] = cfa_at(source_cfa, origin_y + 1U, origin_x + 1U);
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
            const std::uint32_t source_y = origin_y + output_y;
            for (std::uint32_t output_x = 0U; output_x < width; ++output_x)
            {
                const std::uint32_t source_x = origin_x + output_x;
                const std::uint8_t channel = cfa_at(source_cfa, source_y, source_x);
                float sample = std::max(
                    0.0F,
                    (static_cast<float>(raw.pixels[pixel_index(raw.width, source_x, source_y)]) -
                     static_cast<float>(raw.black_level)) /
                        denominator);
                if (raw.dng_opcodes)
                {
                    sample = apply_dng_opcode_list2_sample(*raw.dng_opcodes, source_x, source_y,
                                                           raw.width, raw.height, sample);
                }
                if (!defer_white_balance)
                {
                    sample *= white_balance[channel];
                }
                if (!std::isfinite(sample))
                {
                    invalid_sample.store(true, std::memory_order_relaxed);
                    return;
                }
                prepared.samples[pixel_index(width, output_x, output_y)] = sample;
            }
        });
    if (!rows)
    {
        return rows.error();
    }
    if (invalid_sample.load(std::memory_order_relaxed))
    {
        return make_error(ErrorCode::kValidation, "Bayer window produced a non-finite sample",
                          {{"reason", "non_finite_bayer_sample"}});
    }
    return prepared;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Bayer window allocation failed",
                      {{"reason", "allocation_failed"}});
}

} // namespace

Result<BayerDemosaicMode> parse_bayer_demosaic_mode(const std::string_view mode)
{
    if (mode == kBayerDemosaicModeRcd)
    {
        return BayerDemosaicMode::kRcd;
    }
    if (mode == kBayerDemosaicModePpg)
    {
        return BayerDemosaicMode::kPpg;
    }
    return make_error(
        ErrorCode::kUnsupported, "Bayer demosaic mode is unsupported",
        {{"demosaic_mode", std::string(mode)}, {"reason", "unsupported_demosaic_mode"}});
}

Result<WorkingImage> demosaic_bayer(const DecodedRaw &raw, const std::uint32_t width,
                                    const std::uint32_t height,
                                    const std::array<float, 4> &white_balance,
                                    const BayerDemosaicMode mode,
                                    const CancellationToken &cancellation)
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    auto prepared = prepare_bayer(raw, width, height, white_balance, cancellation);
    if (!prepared)
    {
        return prepared.error();
    }
    auto output = mode == BayerDemosaicMode::kRcd ?
                      demosaic_rcd(prepared.value(), raw.color_profile, cancellation) :
                      demosaic_ppg(prepared.value(), raw.color_profile, cancellation);
    if (!output)
    {
        return output.error();
    }
    if (std::any_of(output.value().rgb.begin(), output.value().rgb.end(),
                    [](const float sample) { return !std::isfinite(sample); }))
    {
        return make_error(ErrorCode::kValidation, "Bayer demosaic produced a non-finite sample",
                          {{"reason", "non_finite_demosaic_output"}});
    }
    return output;
}

Result<WorkingImage> demosaic_bayer_window(const DecodedRaw &raw, const std::uint32_t origin_x,
                                           const std::uint32_t origin_y, const std::uint32_t width,
                                           const std::uint32_t height,
                                           const std::array<float, 4> &white_balance,
                                           const BayerDemosaicMode mode,
                                           const CancellationToken &cancellation,
                                           const GpuAdapter *gpu)
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    if (width == 0U || height == 0U || origin_x >= raw.width || origin_y >= raw.height ||
        width > raw.width - origin_x || height > raw.height - origin_y)
    {
        return make_error(ErrorCode::kInvalidArgument, "Bayer window is outside the CFA frame",
                          {{"reason", "invalid_bayer_window"}});
    }
    // RCD is tiled on an absolute sensor grid. A naive apron around the ROI
    // restarts tiles at the prepared origin and diverges from full-frame crop
    // on high-contrast content. Align the prepared window to kRcdTileStep and
    // include complete tiles that write owned pixels; PPG keeps the apron only.
    const bool align_rcd_tiles = mode == BayerDemosaicMode::kRcd;
    const BayerWindowBounds bounds = bayer_window_demosaic_bounds(
        raw.width, raw.height, origin_x, origin_y, width, height, align_rcd_tiles);
    const std::uint32_t x0 = bounds.x0;
    const std::uint32_t y0 = bounds.y0;
    const std::uint32_t x1 = bounds.x1;
    const std::uint32_t y1 = bounds.y1;
    auto prepared =
        prepare_bayer_window(raw, x0, y0, x1 - x0, y1 - y0, white_balance, cancellation);
    if (!prepared)
    {
        return prepared.error();
    }
    Result<WorkingImage> expanded =
        make_error(ErrorCode::kInternal, "Bayer window demosaic path was not selected");
    if (gpu != nullptr && mode == BayerDemosaicMode::kRcd)
    {
        const std::uint32_t crop_x = origin_x - x0;
        const std::uint32_t crop_y = origin_y - y0;
        WorkingImage gpu_image;
        gpu_image.width = width;
        gpu_image.height = height;
        gpu_image.color_profile = raw.color_profile;
        gpu_image.rgb.resize(static_cast<std::size_t>(width) * height * 3U);
        auto applied =
            gpu->demosaic_rcd(prepared.value().samples, gpu_image.rgb, prepared.value().width,
                              prepared.value().height, prepared.value().cfa, crop_x, crop_y, width,
                              height, cancellation);
        if (!applied)
        {
            return applied.error();
        }
        if (std::any_of(gpu_image.rgb.begin(), gpu_image.rgb.end(),
                        [](const float sample) { return !std::isfinite(sample); }))
        {
            return make_error(ErrorCode::kValidation,
                              "Bayer window demosaic produced a non-finite sample",
                              {{"reason", "non_finite_demosaic_output"}});
        }
        return gpu_image;
    }
    else
    {
        expanded = mode == BayerDemosaicMode::kRcd ?
                       demosaic_rcd(prepared.value(), raw.color_profile, cancellation) :
                       demosaic_ppg(prepared.value(), raw.color_profile, cancellation);
    }
    if (!expanded)
    {
        return expanded.error();
    }
    const std::uint32_t crop_x = origin_x - x0;
    const std::uint32_t crop_y = origin_y - y0;
    WorkingImage output;
    output.width = width;
    output.height = height;
    output.color_profile = expanded.value().color_profile;
    output.rgb.resize(static_cast<std::size_t>(width) * height * 3U);
    for (std::uint32_t y = 0U; y < height; ++y)
    {
        const std::size_t src =
            (static_cast<std::size_t>(crop_y + y) * expanded.value().width + crop_x) * 3U;
        const std::size_t dst = static_cast<std::size_t>(y) * width * 3U;
        std::copy_n(expanded.value().rgb.begin() + static_cast<std::ptrdiff_t>(src),
                    static_cast<std::size_t>(width) * 3U,
                    output.rgb.begin() + static_cast<std::ptrdiff_t>(dst));
    }
    if (std::any_of(output.rgb.begin(), output.rgb.end(),
                    [](const float sample) { return !std::isfinite(sample); }))
    {
        return make_error(ErrorCode::kValidation,
                          "Bayer window demosaic produced a non-finite sample",
                          {{"reason", "non_finite_demosaic_output"}});
    }
    if (gpu != nullptr)
    {
        auto retained = gpu->retain_source_rgb(output.rgb, width, height, cancellation);
        if (!retained)
        {
            return retained.error();
        }
    }
    return output;
}

std::uint64_t estimate_bayer_demosaic_memory(const std::uint32_t width, const std::uint32_t height,
                                             const BayerDemosaicMode mode) noexcept
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
    if (mode == BayerDemosaicMode::kRcd)
    {
        constexpr std::uint64_t tile_scratch =
            (194ULL * 194ULL * (1ULL + 3ULL + 1ULL) + 3ULL * (194ULL * 194ULL / 2ULL)) *
            sizeof(float);
        bytes = tile_scratch > maximum - bytes ? maximum : bytes + tile_scratch;
    }
    return bytes;
}

} // namespace ravo
