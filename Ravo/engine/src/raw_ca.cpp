#include "raw_ca.h"

// C++20 port of the GPLv3 RawTherapee-derived CPU kernel frozen in
// legacy/src/iop/cacorrect.c. UI, OpenMP ownership and dynamic IOP state are intentionally absent.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <vector>

// The frozen kernel uses signed tile coordinates because the first tile starts at -8. Every
// conversion to an owning buffer occurs after the corresponding mirrored-border or image bounds
// check. Keeping that arithmetic signed makes the port auditable against cacorrect.c and avoids
// unsigned wraparound at the border.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif

namespace ravo
{
namespace
{

constexpr int kTileSize = 128;
constexpr int kTileHalf = kTileSize / 2;
constexpr int kBorder = 8;
constexpr int kBorder2 = 2 * kBorder;
constexpr int kBorderHalf = kBorder / 2;
constexpr int kTileStep = kTileSize - kBorder2;
constexpr float kEpsilon = 1.0e-5F;
constexpr float kEpsilon2 = 1.0e-10F;
constexpr float kAutoStrength = 4.0F;

using TilePlane = std::array<float, kTileSize * kTileSize>;
using HalfPlane = std::array<float, kTileSize * kTileHalf>;
using BlockShift = std::array<std::array<float, 2>, 2>;
using FitParameters = std::array<std::array<std::array<double, 16>, 2>, 2>;

struct TileBounds
{
    int top = 0;
    int left = 0;
    int bottom = 0;
    int right = 0;
    int rows = 0;
    int columns = 0;
    int row_min = 0;
    int row_max = 0;
    int column_min = 0;
    int column_max = 0;
};

struct TileBuffers
{
    std::array<TilePlane, 3> rgb{};
    HalfPlane rb_high_horizontal{};
    HalfPlane rb_high_vertical{};
    HalfPlane rb_low_horizontal{};
    HalfPlane rb_low_vertical{};
    HalfPlane grb_low_horizontal{};
    HalfPlane grb_low_vertical{};
};

[[nodiscard]] double number_parameter(const OperationInstance &operation,
                                      const std::string_view name, const double fallback)
{
    const auto found = operation.parameters.find(std::string(name));
    if (found == operation.parameters.end())
    {
        return fallback;
    }
    if (const auto *integer = std::get_if<std::int64_t>(&found->second.value); integer != nullptr)
    {
        return static_cast<double>(*integer);
    }
    if (const auto *number = std::get_if<double>(&found->second.value); number != nullptr)
    {
        return *number;
    }
    return fallback;
}

[[nodiscard]] bool bool_parameter(const OperationInstance &operation, const std::string_view name,
                                  const bool fallback)
{
    const auto found = operation.parameters.find(std::string(name));
    if (found == operation.parameters.end())
    {
        return fallback;
    }
    if (const auto *flag = std::get_if<bool>(&found->second.value); flag != nullptr)
    {
        return *flag;
    }
    return fallback;
}

[[nodiscard]] std::uint8_t cfa_channel(const DecodedRaw &raw, const int row,
                                       const int column) noexcept
{
    const int y = ((row % static_cast<int>(raw.cfa_height)) +
                   static_cast<int>(raw.cfa_height)) %
                  static_cast<int>(raw.cfa_height);
    const int x = ((column % static_cast<int>(raw.cfa_width)) +
                   static_cast<int>(raw.cfa_width)) %
                  static_cast<int>(raw.cfa_width);
    return raw.cfa_channels[static_cast<std::size_t>(y) * raw.cfa_width +
                            static_cast<std::size_t>(x)];
}

[[nodiscard]] float square(const float value) noexcept
{
    return value * value;
}

[[nodiscard]] float interpolate(const float amount, const float next,
                                const float previous) noexcept
{
    return std::fma(amount, next - previous, previous);
}

[[nodiscard]] float median9(std::array<float, 9> values)
{
    std::sort(values.begin(), values.end());
    return values[4];
}

[[nodiscard]] bool solve_linear(std::vector<double> matrix, std::vector<double> vector,
                                const std::size_t dimension,
                                std::array<double, 16> &solution) noexcept
{
    for (std::size_t column = 0; column + 1U < dimension; ++column)
    {
        std::size_t pivot = column;
        double maximum = std::abs(matrix[column * dimension + column]);
        for (std::size_t row = column + 1U; row < dimension; ++row)
        {
            const double candidate = std::abs(matrix[row * dimension + column]);
            if (candidate > maximum)
            {
                maximum = candidate;
                pivot = row;
            }
        }
        if (pivot != column)
        {
            for (std::size_t index = column; index < dimension; ++index)
            {
                std::swap(matrix[column * dimension + index],
                          matrix[pivot * dimension + index]);
            }
            std::swap(vector[column], vector[pivot]);
        }
        const double divisor = matrix[column * dimension + column];
        if (!std::isfinite(divisor) || divisor == 0.0)
        {
            return false;
        }
        for (std::size_t row = column + 1U; row < dimension; ++row)
        {
            const double factor = -matrix[row * dimension + column] / divisor;
            for (std::size_t index = column; index < dimension; ++index)
            {
                matrix[row * dimension + index] +=
                    factor * matrix[column * dimension + index];
            }
            vector[row] += factor * vector[column];
        }
    }
    for (std::size_t reverse = dimension; reverse-- > 0;)
    {
        double value = vector[reverse];
        for (std::size_t column = reverse + 1U; column < dimension; ++column)
        {
            value -= matrix[reverse * dimension + column] * solution[column];
        }
        const double divisor = matrix[reverse * dimension + reverse];
        if (!std::isfinite(divisor) || divisor == 0.0)
        {
            return false;
        }
        solution[reverse] = value / divisor;
        if (!std::isfinite(solution[reverse]))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Result<void> gaussian_blur(std::vector<float> &plane, const int width,
                                         const int height, const float sigma,
                                         const CancellationToken &cancellation)
{
    const int radius = static_cast<int>(std::ceil(3.0F * sigma));
    std::vector<float> kernel(static_cast<std::size_t>(2 * radius + 1));
    float sum = 0.0F;
    for (int offset = -radius; offset <= radius; ++offset)
    {
        const float weight = std::exp(-static_cast<float>(offset * offset) /
                                      (2.0F * sigma * sigma));
        kernel[static_cast<std::size_t>(offset + radius)] = weight;
        sum += weight;
    }
    for (float &weight : kernel)
    {
        weight /= sum;
    }
    std::vector<float> temporary(plane.size());
    for (int row = 0; row < height; ++row)
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        for (int column = 0; column < width; ++column)
        {
            float value = 0.0F;
            for (int offset = -radius; offset <= radius; ++offset)
            {
                const int sample = std::clamp(column + offset, 0, width - 1);
                value += plane[static_cast<std::size_t>(row) * width + sample] *
                         kernel[static_cast<std::size_t>(offset + radius)];
            }
            temporary[static_cast<std::size_t>(row) * width + column] = value;
        }
    }
    for (int row = 0; row < height; ++row)
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        for (int column = 0; column < width; ++column)
        {
            float value = 0.0F;
            for (int offset = -radius; offset <= radius; ++offset)
            {
                const int sample = std::clamp(row + offset, 0, height - 1);
                value += temporary[static_cast<std::size_t>(sample) * width + column] *
                         kernel[static_cast<std::size_t>(offset + radius)];
            }
            plane[static_cast<std::size_t>(row) * width + column] =
                std::clamp(value, 0.1F, 10.0F);
        }
    }
    return {};
}

[[nodiscard]] TileBounds tile_bounds(const int top, const int left, const int width,
                                     const int height) noexcept
{
    TileBounds bounds;
    bounds.top = top;
    bounds.left = left;
    bounds.bottom = std::min(top + kTileSize, height + kBorder);
    bounds.right = std::min(left + kTileSize, width + kBorder);
    bounds.rows = bounds.bottom - top;
    bounds.columns = bounds.right - left;
    bounds.row_min = top < 0 ? kBorder : 0;
    bounds.row_max = bounds.bottom > height ? height - top : bounds.rows;
    bounds.column_min = left < 0 ? kBorder : 0;
    bounds.column_max = bounds.right > width ? width - left : bounds.columns;
    return bounds;
}

void clear_tile(TileBuffers &buffers)
{
    for (auto &plane : buffers.rgb)
    {
        plane.fill(0.0F);
    }
    buffers.rb_high_horizontal.fill(0.0F);
    buffers.rb_high_vertical.fill(0.0F);
    buffers.rb_low_horizontal.fill(0.0F);
    buffers.rb_low_vertical.fill(0.0F);
    buffers.grb_low_horizontal.fill(0.0F);
    buffers.grb_low_vertical.fill(0.0F);
}

void load_tile(TileBuffers &buffers, const TileBounds &bounds, const DecodedRaw &raw,
               const std::vector<float> &input, const std::vector<float> &green,
               const bool use_interpolated_green)
{
    const int width = static_cast<int>(raw.width);
    const int height = static_cast<int>(raw.height);
    const auto copy_sample = [&](const int local_row, const int local_column,
                                 const int source_row, const int source_column)
    {
        const std::size_t local = static_cast<std::size_t>(local_row) * kTileSize + local_column;
        const std::size_t source = static_cast<std::size_t>(source_row) * width + source_column;
        const auto color = cfa_channel(raw, local_row, local_column);
        buffers.rgb[color][local] = input[source];
        if (use_interpolated_green)
        {
            buffers.rgb[1][local] = green[source];
        }
    };

    for (int row = bounds.row_min; row < bounds.row_max; ++row)
    {
        for (int column = bounds.column_min; column < bounds.column_max; ++column)
        {
            copy_sample(row, column, row + bounds.top, column + bounds.left);
        }
    }
    if (bounds.row_min > 0)
    {
        for (int row = 0; row < kBorder; ++row)
        {
            for (int column = bounds.column_min; column < bounds.column_max; ++column)
            {
                const std::size_t target = static_cast<std::size_t>(row) * kTileSize + column;
                const std::size_t source =
                    static_cast<std::size_t>(kBorder2 - row) * kTileSize + column;
                const auto color = cfa_channel(raw, row, column);
                buffers.rgb[color][target] = buffers.rgb[color][source];
                if (use_interpolated_green)
                {
                    buffers.rgb[1][target] = buffers.rgb[1][source];
                }
            }
        }
    }
    if (bounds.row_max < bounds.rows)
    {
        for (int row = 0; row < std::min(kBorder, bounds.rows - bounds.row_max); ++row)
        {
            for (int column = bounds.column_min; column < bounds.column_max; ++column)
            {
                copy_sample(bounds.row_max + row, column, height - row - 2,
                            bounds.left + column);
            }
        }
    }
    if (bounds.column_min > 0)
    {
        for (int row = bounds.row_min; row < bounds.row_max; ++row)
        {
            for (int column = 0; column < kBorder; ++column)
            {
                const std::size_t target = static_cast<std::size_t>(row) * kTileSize + column;
                const std::size_t source =
                    static_cast<std::size_t>(row) * kTileSize + kBorder2 - column;
                const auto color = cfa_channel(raw, row, column);
                buffers.rgb[color][target] = buffers.rgb[color][source];
                if (use_interpolated_green)
                {
                    buffers.rgb[1][target] = buffers.rgb[1][source];
                }
            }
        }
    }
    if (bounds.column_max < bounds.columns)
    {
        for (int row = bounds.row_min; row < bounds.row_max; ++row)
        {
            for (int column = 0;
                 column < std::min(kBorder, bounds.columns - bounds.column_max); ++column)
            {
                copy_sample(row, bounds.column_max + column, bounds.top + row,
                            width - column - 2);
            }
        }
    }
    if (bounds.row_min > 0 && bounds.column_min > 0)
    {
        for (int row = 0; row < kBorder; ++row)
        {
            for (int column = 0; column < kBorder; ++column)
            {
                copy_sample(row, column, kBorder2 - row, kBorder2 - column);
            }
        }
    }
    if (bounds.row_max < bounds.rows && bounds.column_max < bounds.columns)
    {
        for (int row = 0; row < std::min(kBorder, bounds.rows - bounds.row_max); ++row)
        {
            for (int column = 0;
                 column < std::min(kBorder, bounds.columns - bounds.column_max); ++column)
            {
                copy_sample(bounds.row_max + row, bounds.column_max + column, height - row - 2,
                            width - column - 2);
            }
        }
    }
    if (bounds.row_min > 0 && bounds.column_max < bounds.columns)
    {
        for (int row = 0; row < kBorder; ++row)
        {
            for (int column = 0;
                 column < std::min(kBorder, bounds.columns - bounds.column_max); ++column)
            {
                copy_sample(row, bounds.column_max + column, kBorder2 - row,
                            width - column - 2);
            }
        }
    }
    if (bounds.row_max < bounds.rows && bounds.column_min > 0)
    {
        for (int row = 0; row < std::min(kBorder, bounds.rows - bounds.row_max); ++row)
        {
            for (int column = 0; column < kBorder; ++column)
            {
                copy_sample(bounds.row_max + row, column, height - row - 2,
                            kBorder2 - column);
            }
        }
    }
}

[[nodiscard]] Result<void> process_cacorrect(DecodedRaw &raw, const int iterations,
                                             const bool avoid_shift,
                                             const std::array<float, 4> &white_balance,
                                             const CancellationToken &cancellation)
{
    const int width = static_cast<int>(raw.width);
    const int height = static_cast<int>(raw.height);
    const std::size_t count = static_cast<std::size_t>(width) * height;
    if (width < 32 || height < 32 || raw.pixels.size() != count)
    {
        return make_error(ErrorCode::kUnsupported,
                          "RAW chromatic aberration correction requires a complete Bayer frame of at least 32x32");
    }
    const float range = static_cast<float>(static_cast<std::int64_t>(raw.white_level) -
                                           raw.black_level);
    if (!(range > 0.0F))
    {
        return make_error(ErrorCode::kValidation,
                          "RAW chromatic aberration correction requires white above black level");
    }
    float scaler = 1.0F;
    for (const float coefficient : white_balance)
    {
        if (!std::isfinite(coefficient) || coefficient <= 0.0F)
        {
            return make_error(ErrorCode::kValidation,
                              "RAW chromatic aberration correction requires finite positive white balance");
        }
        scaler = std::max(scaler, coefficient);
    }
    std::vector<float> output(count);
    for (int row = 0; row < height; ++row)
    {
        for (int column = 0; column < width; ++column)
        {
            const std::size_t index = static_cast<std::size_t>(row) * width + column;
            const auto channel = cfa_channel(raw, row, column);
            const float normalized =
                std::max(static_cast<float>(static_cast<std::int64_t>(raw.pixels[index]) -
                                            raw.black_level),
                         0.0F) /
                range;
            output[index] = normalized * white_balance[channel] / scaler;
        }
    }
    const int half_width = (width + 1) / 2;
    const int half_height = (height + 1) / 2;
    std::vector<float> red_factor;
    std::vector<float> blue_factor;
    std::vector<float> old_raw;
    if (avoid_shift)
    {
        red_factor.assign(static_cast<std::size_t>(half_width) * half_height, 0.0F);
        blue_factor.assign(static_cast<std::size_t>(half_width) * half_height, 0.0F);
        old_raw.assign(static_cast<std::size_t>(height) * half_width, 0.0F);
        for (int row = 0; row < height; ++row)
        {
            const int first_column = cfa_channel(raw, row, 0) & 1U;
            for (int column = first_column; column < width; column += 2)
            {
                old_raw[static_cast<std::size_t>(row) * half_width + column / 2] =
                    output[static_cast<std::size_t>(row) * width + column];
            }
        }
    }

    const int vertical_extra = (height + kBorder2) % kTileStep == 0 ? 1 : 0;
    const int horizontal_extra = (width + kBorder2) % kTileStep == 0 ? 1 : 0;
    const int vertical_tiles = static_cast<int>(
        std::ceil(static_cast<float>(height + kBorder2) / static_cast<float>(kTileStep) + 2.0F +
                  static_cast<float>(vertical_extra)));
    const int horizontal_tiles = static_cast<int>(
        std::ceil(static_cast<float>(width + kBorder2) / static_cast<float>(kTileStep) + 2.0F +
                  static_cast<float>(horizontal_extra)));
    const std::size_t block_count =
        static_cast<std::size_t>(vertical_tiles) * horizontal_tiles;
    std::vector<float> block_weight(block_count, 0.0F);
    std::vector<BlockShift> block_shifts(block_count);
    std::array<std::array<float, 2>, 2> block_average{};
    std::array<std::array<float, 2>, 2> block_square_average{};
    std::array<std::array<float, 2>, 2> block_denominator{};
    std::array<std::array<float, 2>, 2> block_variance{};
    FitParameters fit_parameters{};
    std::vector<float> green(count, 0.0F);
    std::vector<float> corrected_non_green(count, 0.0F);
    int polynomial_order = 4;
    int parameter_count = 16;
    TileBuffers tile;

    for (int iteration = 0; iteration < iterations; ++iteration)
    {
        for (int top = -kBorder; top < height; top += kTileStep)
        {
            for (int left = -kBorder; left < width; left += kTileStep)
            {
                auto cancelled = cancellation.check();
                if (!cancelled)
                {
                    return cancelled.error();
                }
                clear_tile(tile);
                const TileBounds bounds = tile_bounds(top, left, width, height);
                load_tile(tile, bounds, raw, output, green, false);
                const int vertical_block = ((top + kBorder) / kTileStep) + 1;
                const int horizontal_block = ((left + kBorder) / kTileStep) + 1;
                const std::size_t block_index =
                    static_cast<std::size_t>(vertical_block) * horizontal_tiles +
                    horizontal_block;

                for (int row = 3; row < bounds.rows - 3; ++row)
                {
                    const int global_row = row + top;
                    for (int column = 3 + (cfa_channel(raw, row, 3) & 1U);
                         column < bounds.columns - 3; column += 2)
                    {
                        const int index = row * kTileSize + column;
                        const int color = cfa_channel(raw, row, column);
                        const float weight_up =
                            1.0F /
                            square(kEpsilon +
                                   std::abs(tile.rgb[1][index + kTileSize] -
                                            tile.rgb[1][index - kTileSize]) +
                                   std::abs(tile.rgb[color][index] -
                                            tile.rgb[color][index - 2 * kTileSize]) +
                                   std::abs(tile.rgb[1][index - kTileSize] -
                                            tile.rgb[1][index - 3 * kTileSize]));
                        const float weight_down =
                            1.0F /
                            square(kEpsilon +
                                   std::abs(tile.rgb[1][index - kTileSize] -
                                            tile.rgb[1][index + kTileSize]) +
                                   std::abs(tile.rgb[color][index] -
                                            tile.rgb[color][index + 2 * kTileSize]) +
                                   std::abs(tile.rgb[1][index + kTileSize] -
                                            tile.rgb[1][index + 3 * kTileSize]));
                        const float weight_left =
                            1.0F /
                            square(kEpsilon +
                                   std::abs(tile.rgb[1][index + 1] - tile.rgb[1][index - 1]) +
                                   std::abs(tile.rgb[color][index] - tile.rgb[color][index - 2]) +
                                   std::abs(tile.rgb[1][index - 1] - tile.rgb[1][index - 3]));
                        const float weight_right =
                            1.0F /
                            square(kEpsilon +
                                   std::abs(tile.rgb[1][index - 1] - tile.rgb[1][index + 1]) +
                                   std::abs(tile.rgb[color][index] - tile.rgb[color][index + 2]) +
                                   std::abs(tile.rgb[1][index + 1] - tile.rgb[1][index + 3]));
                        tile.rgb[1][index] =
                            (weight_up * tile.rgb[1][index - kTileSize] +
                             weight_down * tile.rgb[1][index + kTileSize] +
                             weight_left * tile.rgb[1][index - 1] +
                             weight_right * tile.rgb[1][index + 1]) /
                            (weight_up + weight_down + weight_left + weight_right);
                    }
                    if (global_row >= 0 && global_row < height)
                    {
                        int local_index =
                            row * kTileSize + 3 - (left < 0 ? left + 3 : 0);
                        for (int column = std::max(left + 3, 0);
                             column < std::min(bounds.columns + left - 3, width);
                             ++column, ++local_index)
                        {
                            green[static_cast<std::size_t>(global_row) * width + column] =
                                tile.rgb[1][local_index];
                        }
                    }
                }

                for (int row = kBorderHalf; row < bounds.rows - kBorderHalf; ++row)
                {
                    for (int column = kBorderHalf + (cfa_channel(raw, row, 2) & 1U);
                         column < bounds.columns - kBorderHalf; column += 2)
                    {
                        const int index = row * kTileSize + column;
                        const int color = cfa_channel(raw, row, column);
                        const std::size_t half = static_cast<std::size_t>(index >> 1);
                        tile.rb_high_vertical[half] =
                            std::abs(std::abs((tile.rgb[1][index] - tile.rgb[color][index]) -
                                              (tile.rgb[1][index + 4 * kTileSize] -
                                               tile.rgb[color][index + 4 * kTileSize])) +
                                     std::abs((tile.rgb[1][index - 4 * kTileSize] -
                                               tile.rgb[color][index - 4 * kTileSize]) -
                                              (tile.rgb[1][index] - tile.rgb[color][index])) -
                                     std::abs((tile.rgb[1][index - 4 * kTileSize] -
                                               tile.rgb[color][index - 4 * kTileSize]) -
                                              (tile.rgb[1][index + 4 * kTileSize] -
                                               tile.rgb[color][index + 4 * kTileSize])));
                        tile.rb_high_horizontal[half] =
                            std::abs(std::abs((tile.rgb[1][index] - tile.rgb[color][index]) -
                                              (tile.rgb[1][index + 4] -
                                               tile.rgb[color][index + 4])) +
                                     std::abs((tile.rgb[1][index - 4] -
                                               tile.rgb[color][index - 4]) -
                                              (tile.rgb[1][index] - tile.rgb[color][index])) -
                                     std::abs((tile.rgb[1][index - 4] -
                                               tile.rgb[color][index - 4]) -
                                              (tile.rgb[1][index + 4] -
                                               tile.rgb[color][index + 4])));
                        const float green_low_vertical =
                            0.25F * (2.0F * tile.rgb[1][index] +
                                     tile.rgb[1][index + 2 * kTileSize] +
                                     tile.rgb[1][index - 2 * kTileSize]);
                        const float green_low_horizontal =
                            0.25F * (2.0F * tile.rgb[1][index] + tile.rgb[1][index + 2] +
                                     tile.rgb[1][index - 2]);
                        tile.rb_low_vertical[half] =
                            kEpsilon +
                            std::abs(green_low_vertical -
                                     0.25F * (2.0F * tile.rgb[color][index] +
                                              tile.rgb[color][index + 2 * kTileSize] +
                                              tile.rgb[color][index - 2 * kTileSize]));
                        tile.rb_low_horizontal[half] =
                            kEpsilon +
                            std::abs(green_low_horizontal -
                                     0.25F * (2.0F * tile.rgb[color][index] +
                                              tile.rgb[color][index + 2] +
                                              tile.rgb[color][index - 2]));
                        tile.grb_low_vertical[half] =
                            green_low_vertical +
                            0.25F * (2.0F * tile.rgb[color][index] +
                                     tile.rgb[color][index + 2 * kTileSize] +
                                     tile.rgb[color][index - 2 * kTileSize]);
                        tile.grb_low_horizontal[half] =
                            green_low_horizontal +
                            0.25F * (2.0F * tile.rgb[color][index] +
                                     tile.rgb[color][index + 2] +
                                     tile.rgb[color][index - 2]);
                    }
                }

                float coefficients[2][3][2]{};
                for (int row = kBorder; row < bounds.rows - kBorder; ++row)
                {
                    for (int column = kBorder + (cfa_channel(raw, row, 2) & 1U);
                         column < bounds.columns - kBorder; column += 2)
                    {
                        const int index = row * kTileSize + column;
                        const int color = cfa_channel(raw, row, column);
                        const int color_index = color >> 1;
                        const std::size_t half = static_cast<std::size_t>(index >> 1);
                        float green_difference =
                            0.3125F * (tile.rgb[1][index + kTileSize] -
                                       tile.rgb[1][index - kTileSize]) +
                            0.09375F *
                                (tile.rgb[1][index + kTileSize + 1] -
                                     tile.rgb[1][index - kTileSize + 1] +
                                 tile.rgb[1][index + kTileSize - 1] -
                                     tile.rgb[1][index - kTileSize - 1]);
                        const float delta = tile.rgb[color][index] - tile.rgb[1][index];
                        float gradient_weight =
                            std::abs(0.25F * tile.rb_high_vertical[half] +
                                     0.125F * (tile.rb_high_vertical[half + 1] +
                                               tile.rb_high_vertical[half - 1])) *
                            (tile.grb_low_vertical[half - kTileHalf] +
                             tile.grb_low_vertical[half + kTileHalf]) /
                            (kEpsilon +
                             0.1F * (tile.grb_low_vertical[half - kTileHalf] +
                                     tile.grb_low_vertical[half + kTileHalf]) +
                             tile.rb_low_vertical[half - kTileHalf] +
                             tile.rb_low_vertical[half + kTileHalf]);
                        coefficients[0][0][color_index] += gradient_weight * delta * delta;
                        coefficients[0][1][color_index] += gradient_weight * green_difference * delta;
                        coefficients[0][2][color_index] +=
                            gradient_weight * green_difference * green_difference;
                        green_difference =
                            0.3125F * (tile.rgb[1][index + 1] - tile.rgb[1][index - 1]) +
                            0.09375F *
                                (tile.rgb[1][index + 1 + kTileSize] -
                                     tile.rgb[1][index - 1 + kTileSize] +
                                 tile.rgb[1][index + 1 - kTileSize] -
                                     tile.rgb[1][index - 1 - kTileSize]);
                        gradient_weight =
                            std::abs(0.25F * tile.rb_high_horizontal[half] +
                                     0.125F * (tile.rb_high_horizontal[half + kTileHalf] +
                                               tile.rb_high_horizontal[half - kTileHalf])) *
                            (tile.grb_low_horizontal[half - 1] +
                             tile.grb_low_horizontal[half + 1]) /
                            (kEpsilon +
                             0.1F * (tile.grb_low_horizontal[half - 1] +
                                     tile.grb_low_horizontal[half + 1]) +
                             tile.rb_low_horizontal[half - 1] +
                             tile.rb_low_horizontal[half + 1]);
                        coefficients[1][0][color_index] += gradient_weight * delta * delta;
                        coefficients[1][1][color_index] += gradient_weight * green_difference * delta;
                        coefficients[1][2][color_index] +=
                            gradient_weight * green_difference * green_difference;
                    }
                }
                for (int color = 0; color < 2; ++color)
                {
                    for (int direction = 0; direction < 2; ++direction)
                    {
                        float shift = 17.0F;
                        if (coefficients[direction][2][color] > kEpsilon2)
                        {
                            shift = coefficients[direction][1][color] /
                                    coefficients[direction][2][color];
                            block_weight[block_index] =
                                coefficients[direction][2][color] /
                                (kEpsilon + coefficients[direction][0][color]);
                        }
                        if (std::abs(shift) < 2.0F)
                        {
                            block_average[direction][color] += shift;
                            block_square_average[direction][color] += shift * shift;
                            block_denominator[direction][color] += 1.0F;
                        }
                        block_shifts[block_index][color][direction] = shift;
                    }
                }
            }
        }

        for (int direction = 0; direction < 2; ++direction)
        {
            for (int color = 0; color < 2; ++color)
            {
                if (block_denominator[direction][color] <= 0.0F)
                {
                    return make_error(ErrorCode::kUnsupported,
                                      "RAW chromatic aberration fit has insufficient signal");
                }
                block_variance[direction][color] =
                    block_square_average[direction][color] /
                        block_denominator[direction][color] -
                    square(block_average[direction][color] /
                           block_denominator[direction][color]);
            }
        }
        for (int vertical = 1; vertical < vertical_tiles - 1; ++vertical)
        {
            for (int color = 0; color < 2; ++color)
            {
                for (int direction = 0; direction < 2; ++direction)
                {
                    block_shifts[static_cast<std::size_t>(vertical) * horizontal_tiles][color]
                                [direction] =
                        block_shifts[static_cast<std::size_t>(vertical) * horizontal_tiles + 2]
                                    [color][direction];
                    block_shifts[static_cast<std::size_t>(vertical) * horizontal_tiles +
                                 horizontal_tiles - 1][color][direction] =
                        block_shifts[static_cast<std::size_t>(vertical) * horizontal_tiles +
                                     horizontal_tiles - 3][color][direction];
                }
            }
        }
        for (int horizontal = 0; horizontal < horizontal_tiles; ++horizontal)
        {
            for (int color = 0; color < 2; ++color)
            {
                for (int direction = 0; direction < 2; ++direction)
                {
                    block_shifts[horizontal][color][direction] =
                        block_shifts[2U * horizontal_tiles + horizontal][color][direction];
                    block_shifts[static_cast<std::size_t>(vertical_tiles - 1) * horizontal_tiles +
                                 horizontal][color][direction] =
                        block_shifts[static_cast<std::size_t>(vertical_tiles - 3) * horizontal_tiles +
                                     horizontal][color][direction];
                }
            }
        }

        std::array<std::array<std::array<double, 256>, 2>, 2> polynomial_matrix{};
        std::array<std::array<std::array<double, 16>, 2>, 2> shift_matrix{};
        std::array<int, 2> usable_blocks{};
        for (int vertical = 1; vertical < vertical_tiles - 1; ++vertical)
        {
            for (int horizontal = 1; horizontal < horizontal_tiles - 1; ++horizontal)
            {
                const std::size_t block =
                    static_cast<std::size_t>(vertical) * horizontal_tiles + horizontal;
                for (int color = 0; color < 2; ++color)
                {
                    std::array<float, 2> median{};
                    for (int direction = 0; direction < 2; ++direction)
                    {
                        std::array<float, 9> values{};
                        std::size_t index = 0;
                        for (int row = -1; row <= 1; ++row)
                        {
                            for (int column = -1; column <= 1; ++column)
                            {
                                values[index++] =
                                    block_shifts[static_cast<std::size_t>(vertical + row) *
                                                     horizontal_tiles +
                                                 horizontal + column][color][direction];
                            }
                        }
                        median[direction] = median9(values);
                    }
                    if (square(median[0]) > kAutoStrength * block_variance[0][color] ||
                        square(median[1]) > kAutoStrength * block_variance[1][color])
                    {
                        continue;
                    }
                    ++usable_blocks[color];
                    double vertical_power_initial = 1.0;
                    for (int i = 0; i < polynomial_order; ++i)
                    {
                        double horizontal_power_initial = 1.0;
                        for (int j = 0; j < polynomial_order; ++j)
                        {
                            double vertical_power = vertical_power_initial;
                            for (int m = 0; m < polynomial_order; ++m)
                            {
                                double horizontal_power = horizontal_power_initial;
                                for (int n = 0; n < polynomial_order; ++n)
                                {
                                    const double increment =
                                        vertical_power * horizontal_power * block_weight[block];
                                    const std::size_t matrix_index =
                                        static_cast<std::size_t>(parameter_count) *
                                            (polynomial_order * i + j) +
                                        (polynomial_order * m + n);
                                    polynomial_matrix[color][0][matrix_index] += increment;
                                    polynomial_matrix[color][1][matrix_index] += increment;
                                    horizontal_power *= horizontal;
                                }
                                vertical_power *= vertical;
                            }
                            const double block_increment = vertical_power_initial *
                                                           horizontal_power_initial *
                                                           block_weight[block];
                            const std::size_t vector_index =
                                static_cast<std::size_t>(polynomial_order * i + j);
                            shift_matrix[color][0][vector_index] +=
                                block_increment * median[0];
                            shift_matrix[color][1][vector_index] +=
                                block_increment * median[1];
                            horizontal_power_initial *= horizontal;
                        }
                        vertical_power_initial *= vertical;
                    }
                }
            }
        }
        usable_blocks[1] = std::min(usable_blocks[0], usable_blocks[1]);
        if (usable_blocks[1] < 32)
        {
            polynomial_order = 2;
            parameter_count = 4;
            if (usable_blocks[1] < 10)
            {
                return make_error(ErrorCode::kUnsupported,
                                  "RAW chromatic aberration fit has fewer than 10 usable blocks",
                                  {{"usable_blocks", std::to_string(usable_blocks[1])}});
            }
        }
        for (int color = 0; color < 2; ++color)
        {
            for (int direction = 0; direction < 2; ++direction)
            {
                std::vector<double> matrix(
                    polynomial_matrix[color][direction].begin(),
                    polynomial_matrix[color][direction].begin() + parameter_count * parameter_count);
                std::vector<double> vector(shift_matrix[color][direction].begin(),
                                           shift_matrix[color][direction].begin() + parameter_count);
                fit_parameters[color][direction].fill(0.0);
                if (!solve_linear(std::move(matrix), std::move(vector), parameter_count,
                                  fit_parameters[color][direction]))
                {
                    return make_error(ErrorCode::kUnsupported,
                                      "RAW chromatic aberration polynomial fit is singular");
                }
            }
        }

        corrected_non_green.assign(count, 0.0F);
        for (int top = -kBorder; top < height; top += kTileStep)
        {
            for (int left = -kBorder; left < width; left += kTileStep)
            {
                auto cancelled = cancellation.check();
                if (!cancelled)
                {
                    return cancelled.error();
                }
                clear_tile(tile);
                const TileBounds bounds = tile_bounds(top, left, width, height);
                load_tile(tile, bounds, raw, output, green, true);
                const int vertical_block = ((top + kBorder) / kTileStep) + 1;
                const int horizontal_block = ((left + kBorder) / kTileStep) + 1;
                float local_shift[2][2]{};
                double vertical_power = 1.0;
                for (int i = 0; i < polynomial_order; ++i)
                {
                    double horizontal_power = vertical_power;
                    for (int j = 0; j < polynomial_order; ++j)
                    {
                        const int parameter = polynomial_order * i + j;
                        local_shift[0][0] += static_cast<float>(
                            horizontal_power * fit_parameters[0][0][parameter]);
                        local_shift[0][1] += static_cast<float>(
                            horizontal_power * fit_parameters[0][1][parameter]);
                        local_shift[1][0] += static_cast<float>(
                            horizontal_power * fit_parameters[1][0][parameter]);
                        local_shift[1][1] += static_cast<float>(
                            horizontal_power * fit_parameters[1][1][parameter]);
                        horizontal_power *= horizontal_block;
                    }
                    vertical_power *= vertical_block;
                }
                for (auto &color : local_shift)
                {
                    for (float &direction : color)
                    {
                        direction = std::clamp(direction, -3.99F, 3.99F);
                    }
                }

                std::array<int, 3> vertical_floor{};
                std::array<int, 3> vertical_ceil{};
                std::array<int, 3> horizontal_floor{};
                std::array<int, 3> horizontal_ceil{};
                std::array<float, 3> vertical_fraction{};
                std::array<float, 3> horizontal_fraction{};
                int direction[2][3]{};
                for (int color = 0; color < 3; color += 2)
                {
                    const int color_index = color >> 1;
                    vertical_floor[color] = static_cast<int>(std::floor(local_shift[color_index][0]));
                    vertical_ceil[color] = static_cast<int>(std::ceil(local_shift[color_index][0]));
                    if (local_shift[color_index][0] < 0.0F)
                    {
                        std::swap(vertical_floor[color], vertical_ceil[color]);
                    }
                    vertical_fraction[color] =
                        std::abs(local_shift[color_index][0] -
                                 static_cast<float>(vertical_floor[color]));
                    horizontal_floor[color] =
                        static_cast<int>(std::floor(local_shift[color_index][1]));
                    horizontal_ceil[color] =
                        static_cast<int>(std::ceil(local_shift[color_index][1]));
                    if (local_shift[color_index][1] < 0.0F)
                    {
                        std::swap(horizontal_floor[color], horizontal_ceil[color]);
                    }
                    horizontal_fraction[color] =
                        std::abs(local_shift[color_index][1] -
                                 static_cast<float>(horizontal_floor[color]));
                    direction[0][color] = local_shift[color_index][0] > 0.0F ? 2 : -2;
                    direction[1][color] = local_shift[color_index][1] > 0.0F ? 2 : -2;
                }
                HalfPlane &green_red_blue_difference = tile.rb_high_horizontal;
                HalfPlane &green_shift = tile.rb_high_vertical;
                for (int row = kBorderHalf; row < bounds.rows - kBorderHalf; ++row)
                {
                    for (int column = kBorderHalf + (cfa_channel(raw, row, 2) & 1U);
                         column < bounds.columns - kBorderHalf; column += 2)
                    {
                        const int color = cfa_channel(raw, row, column);
                        const float green_horizontal_floor = interpolate(
                            horizontal_fraction[color],
                            tile.rgb[1][(row + vertical_floor[color]) * kTileSize + column +
                                        horizontal_ceil[color]],
                            tile.rgb[1][(row + vertical_floor[color]) * kTileSize + column +
                                        horizontal_floor[color]]);
                        const float green_horizontal_ceil = interpolate(
                            horizontal_fraction[color],
                            tile.rgb[1][(row + vertical_ceil[color]) * kTileSize + column +
                                        horizontal_ceil[color]],
                            tile.rgb[1][(row + vertical_ceil[color]) * kTileSize + column +
                                        horizontal_floor[color]]);
                        const float interpolated_green =
                            interpolate(vertical_fraction[color], green_horizontal_ceil,
                                        green_horizontal_floor);
                        const int index = row * kTileSize + column;
                        green_red_blue_difference[static_cast<std::size_t>(index >> 1)] =
                            interpolated_green - tile.rgb[color][index];
                        green_shift[static_cast<std::size_t>(index >> 1)] = interpolated_green;
                    }
                }
                horizontal_fraction[0] *= 0.5F;
                horizontal_fraction[2] *= 0.5F;
                vertical_fraction[0] *= 0.5F;
                vertical_fraction[2] *= 0.5F;
                for (int row = kBorder; row < bounds.rows - kBorder; ++row)
                {
                    for (int column = kBorder + (cfa_channel(raw, row, 2) & 1U);
                         column < bounds.columns - kBorder; column += 2)
                    {
                        const int color = cfa_channel(raw, row, column);
                        const int index = row * kTileSize + column;
                        const std::size_t half = static_cast<std::size_t>(index >> 1);
                        const float original_difference =
                            tile.rgb[1][index] - tile.rgb[color][index];
                        const float horizontal_floor_difference = interpolate(
                            horizontal_fraction[color],
                            green_red_blue_difference[static_cast<std::size_t>(
                                (index - direction[1][color]) >> 1)],
                            green_red_blue_difference[half]);
                        const float horizontal_ceil_difference = interpolate(
                            horizontal_fraction[color],
                            green_red_blue_difference[static_cast<std::size_t>(
                                ((row - direction[0][color]) * kTileSize + column -
                                 direction[1][color]) >>
                                1)],
                            green_red_blue_difference[static_cast<std::size_t>(
                                ((row - direction[0][color]) * kTileSize + column) >> 1)]);
                        float interpolated_difference =
                            interpolate(vertical_fraction[color], horizontal_ceil_difference,
                                        horizontal_floor_difference);
                        const float interpolated_rb = tile.rgb[1][index] - interpolated_difference;
                        if (std::abs(interpolated_rb - tile.rgb[color][index]) <
                            0.25F * (interpolated_rb + tile.rgb[color][index]))
                        {
                            if (std::abs(original_difference) > std::abs(interpolated_difference))
                            {
                                tile.rgb[color][index] = interpolated_rb;
                            }
                        }
                        else
                        {
                            const float p0 =
                                1.0F / (kEpsilon + std::abs(tile.rgb[1][index] - green_shift[half]));
                            const float p1 =
                                1.0F /
                                (kEpsilon +
                                 std::abs(tile.rgb[1][index] -
                                          green_shift[static_cast<std::size_t>(
                                              (index - direction[1][color]) >> 1)]));
                            const float p2 =
                                1.0F /
                                (kEpsilon +
                                 std::abs(tile.rgb[1][index] -
                                          green_shift[static_cast<std::size_t>(
                                              ((row - direction[0][color]) * kTileSize + column) >>
                                              1)]));
                            const float p3 =
                                1.0F /
                                (kEpsilon +
                                 std::abs(tile.rgb[1][index] -
                                          green_shift[static_cast<std::size_t>(
                                              ((row - direction[0][color]) * kTileSize + column -
                                               direction[1][color]) >>
                                              1)]));
                            interpolated_difference =
                                (p0 * green_red_blue_difference[half] +
                                 p1 * green_red_blue_difference[static_cast<std::size_t>(
                                          (index - direction[1][color]) >> 1)] +
                                 p2 * green_red_blue_difference[static_cast<std::size_t>(
                                          ((row - direction[0][color]) * kTileSize + column) >> 1)] +
                                 p3 * green_red_blue_difference[static_cast<std::size_t>(
                                          ((row - direction[0][color]) * kTileSize + column -
                                           direction[1][color]) >>
                                          1)]) /
                                (p0 + p1 + p2 + p3);
                            if (std::abs(original_difference) > std::abs(interpolated_difference))
                            {
                                tile.rgb[color][index] =
                                    tile.rgb[1][index] - interpolated_difference;
                            }
                        }
                        if (original_difference * interpolated_difference < 0.0F)
                        {
                            tile.rgb[color][index] =
                                tile.rgb[1][index] -
                                0.5F * (original_difference + interpolated_difference);
                        }
                    }
                }
                for (int row = kBorder; row < bounds.rows - kBorder; ++row)
                {
                    for (int column = kBorder + (cfa_channel(raw, row, 2) & 1U);
                         column < bounds.columns - kBorder; column += 2)
                    {
                        const int global_row = row + top;
                        const int global_column = column + left;
                        if (global_row >= 0 && global_row < height && global_column >= 0 &&
                            global_column < width)
                        {
                            const int color = cfa_channel(raw, row, column);
                            corrected_non_green[static_cast<std::size_t>(global_row) * width +
                                                global_column] =
                                tile.rgb[color][row * kTileSize + column];
                        }
                    }
                }
            }
        }
        for (int row = 0; row < height; ++row)
        {
            const int first_column = cfa_channel(raw, row, 0) & 1U;
            for (int column = first_column; column < width; column += 2)
            {
                output[static_cast<std::size_t>(row) * width + column] =
                    corrected_non_green[static_cast<std::size_t>(row) * width + column];
            }
        }
    }

    if (avoid_shift)
    {
        for (int row = 0; row < height; ++row)
        {
            const int first_column = cfa_channel(raw, row, 0) & 1U;
            const int color = cfa_channel(raw, row, first_column);
            auto &factor = color == 0 ? red_factor : blue_factor;
            for (int column = first_column; column < width; column += 2)
            {
                const std::size_t input_index = static_cast<std::size_t>(row) * width + column;
                const std::size_t old_index = static_cast<std::size_t>(row) * half_width + column / 2;
                const std::size_t factor_index =
                    static_cast<std::size_t>(row / 2) * half_width + column / 2;
                const float ratio = old_raw[old_index] / output[input_index];
                factor[factor_index] = ratio >= 0.5F ? std::min(ratio, 2.0F) : 0.5F;
            }
        }
        if ((height & 1) != 0)
        {
            for (int column = 0; column < half_width; ++column)
            {
                red_factor[static_cast<std::size_t>(half_height - 1) * half_width + column] =
                    red_factor[static_cast<std::size_t>(half_height - 2) * half_width + column];
                blue_factor[static_cast<std::size_t>(half_height - 1) * half_width + column] =
                    blue_factor[static_cast<std::size_t>(half_height - 2) * half_width + column];
            }
        }
        if ((width & 1) != 0)
        {
            const int row = 1 - (cfa_channel(raw, 0, 0) & 1U);
            const int column = cfa_channel(raw, row, 0) & 1U;
            const int color = cfa_channel(raw, row, column);
            auto &factor = color == 0 ? red_factor : blue_factor;
            for (int factor_row = 0; factor_row < half_height; ++factor_row)
            {
                factor[static_cast<std::size_t>(factor_row) * half_width + half_width - 1] =
                    factor[static_cast<std::size_t>(factor_row) * half_width + half_width - 2];
            }
        }
        auto blurred = gaussian_blur(red_factor, half_width, half_height, 30.0F, cancellation);
        if (!blurred)
        {
            return blurred.error();
        }
        blurred = gaussian_blur(blue_factor, half_width, half_height, 30.0F, cancellation);
        if (!blurred)
        {
            return blurred.error();
        }
        for (int row = 2; row < height - 2; ++row)
        {
            const int first_column = cfa_channel(raw, row, 0) & 1U;
            const int color = cfa_channel(raw, row, first_column);
            const auto &factor = color == 0 ? red_factor : blue_factor;
            for (int column = first_column; column < width - 2; column += 2)
            {
                output[static_cast<std::size_t>(row) * width + column] *=
                    factor[static_cast<std::size_t>(row / 2) * half_width + column / 2];
            }
        }
    }

    std::vector<std::uint16_t> published(raw.pixels.size());
    for (int row = 0; row < height; ++row)
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        for (int column = 0; column < width; ++column)
        {
            const std::size_t index = static_cast<std::size_t>(row) * width + column;
            const auto channel = cfa_channel(raw, row, column);
            const float normalized = output[index] * scaler / white_balance[channel];
            if (!std::isfinite(normalized))
            {
                return make_error(ErrorCode::kValidation,
                                  "RAW chromatic aberration correction produced a non-finite sample",
                                  {{"sample_index", std::to_string(index)}});
            }
            const auto value = static_cast<std::int64_t>(std::lround(
                static_cast<double>(raw.black_level) + static_cast<double>(normalized * range)));
            published[index] = static_cast<std::uint16_t>(std::clamp<std::int64_t>(
                value, 0, std::numeric_limits<std::uint16_t>::max()));
        }
    }
    raw.pixels = std::move(published);
    return {};
}

} // namespace

Result<void> apply_raw_cacorrect(DecodedRaw &raw, const OperationInstance &operation,
                                 const std::array<float, 4> &white_balance,
                                 const CancellationToken &cancellation)
{
    const double iterations_value = number_parameter(operation, "iterations", 2.0);
    const auto iterations = static_cast<int>(std::llround(iterations_value));
    const bool avoid_shift = bool_parameter(operation, "avoid_color_shift", false);
    if (!std::isfinite(iterations_value) || iterations < 1 || iterations > 5 ||
        std::abs(iterations_value - static_cast<double>(iterations)) > 1.0e-9)
    {
        return make_error(ErrorCode::kValidation,
                          "RAW chromatic aberration iterations must be an integer in [1, 5]");
    }
    if (raw.cfa_width != 2U || raw.cfa_height != 2U || raw.cfa_channels.size() != 4U)
    {
        return make_error(ErrorCode::kUnsupported,
                          "RAW chromatic aberration correction requires a Bayer 2x2 CFA");
    }
    const bool has_red = std::find(raw.cfa_channels.begin(), raw.cfa_channels.end(), 0U) !=
                         raw.cfa_channels.end();
    const bool has_green = std::find(raw.cfa_channels.begin(), raw.cfa_channels.end(), 1U) !=
                           raw.cfa_channels.end();
    const bool has_blue = std::find(raw.cfa_channels.begin(), raw.cfa_channels.end(), 2U) !=
                          raw.cfa_channels.end();
    if (!has_red || !has_green || !has_blue)
    {
        return make_error(ErrorCode::kUnsupported,
                          "RAW chromatic aberration correction requires a three-color Bayer CFA");
    }
    try
    {
        return process_cacorrect(raw, iterations, avoid_shift, white_balance, cancellation);
    }
    catch (const std::bad_alloc &)
    {
        return make_error(ErrorCode::kInternal,
                          "RAW chromatic aberration correction ran out of memory");
    }
}

} // namespace ravo

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
