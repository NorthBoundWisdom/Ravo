#include "engine_color_test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <numeric>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <png.h>

#include <QBuffer>
#include <QColor>
#include <QFile>
#include <QImage>
#include <zlib.h>

#include "ravo/domain/types.h"
#include "ravo/engine/engine.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"

#include "color_balance_fixture.h"
#include "capability_ops.h"
#include "capture_metadata_test_support.h"
#include "color_balance_rgb.h"
#include "color_checker.h"
#include "color_harmonizer.h"
#include "color_contrast.h"
#include "d50_lab.h"
#include "dt_ucs.h"
#include "harmony_geometry.h"
#include "image_ops.h"
#include "input_color.h"
#include "primaries.h"
#include "raw_pipeline.h"
#include "raw_temperature.h"
#include "recursive_gaussian.h"
#include "temperature_fixture.h"
#include "engine_test_support.h"
#include "test_support.h"

namespace ravo::engine_color_test_support
{

// Independent scalar oracle transcribed from the frozen
// common/colorspaces_inline_conversions.h owner. In particular, it preserves
// the transposed-matrix addition order, the pre-rounded D50 reciprocals, and
// the Lab inverse scale/add order instead of calling the production seam.
[[nodiscard]] FrozenD50Triplet frozen_linear_rec709_to_xyz_d50(const FrozenD50Triplet &rgb) noexcept
{
    return {0.4360747F * rgb[0] + 0.3850649F * rgb[1] + 0.1430804F * rgb[2],
            0.2225045F * rgb[0] + 0.7168786F * rgb[1] + 0.0606169F * rgb[2],
            0.0139322F * rgb[0] + 0.0971045F * rgb[1] + 0.7141733F * rgb[2]};
}

[[nodiscard]] FrozenD50Triplet frozen_xyz_d50_to_linear_rec709(const FrozenD50Triplet &xyz) noexcept
{
    // Keep every negative coefficient as an added product, matching the frozen
    // dt_apply_transposed_color_matrix expression order rather than subtraction.
    return {3.1338561F * xyz[0] + (-1.6168667F) * xyz[1] + (-0.4906146F) * xyz[2],
            (-0.9787684F) * xyz[0] + 1.9161415F * xyz[1] + 0.0334540F * xyz[2],
            0.0719453F * xyz[0] + (-0.2289914F) * xyz[1] + 1.4052427F * xyz[2]};
}

[[nodiscard]] FrozenD50Triplet frozen_xyz_d50_to_lab(const FrozenD50Triplet &xyz) noexcept
{
    constexpr FrozenD50Triplet d50_inverse{1.0F / 0.9642F, 1.0F, 1.0F / 0.8249F};
    constexpr float epsilon = 216.0F / 24389.0F;
    constexpr float kappa = 24389.0F / 27.0F;
    FrozenD50Triplet transformed{};
    for (std::size_t channel = 0U; channel < transformed.size(); ++channel)
    {
        const float normalized = xyz[channel] * d50_inverse[channel];
        transformed[channel] =
            normalized > epsilon ? std::cbrt(normalized) : (kappa * normalized + 16.0F) / 116.0F;
    }
    return {116.0F * transformed[1] - 16.0F, 500.0F * (transformed[0] - transformed[1]),
            -200.0F * (transformed[2] - transformed[1])};
}

[[nodiscard]] FrozenD50Triplet frozen_lab_to_xyz_d50(const FrozenD50Triplet &lab) noexcept
{
    constexpr FrozenD50Triplet d50{0.9642F, 1.0F, 0.8249F};
    constexpr FrozenD50Triplet offset{0.0F, 16.0F, 0.0F};
    constexpr FrozenD50Triplet coefficient{1.0F / 500.0F, 1.0F / 116.0F, -1.0F / 200.0F};
    constexpr FrozenD50Triplet add_coefficient{1.0F, 0.0F, 1.0F};
    constexpr float epsilon = 0.20689655172413796F;
    constexpr float kappa = 24389.0F / 27.0F;
    const FrozenD50Triplet reordered{lab[1], lab[0], lab[2]};
    FrozenD50Triplet scaled{};
    for (std::size_t channel = 0U; channel < scaled.size(); ++channel)
    {
        scaled[channel] = (reordered[channel] + offset[channel]) * coefficient[channel];
    }
    FrozenD50Triplet xyz{};
    for (std::size_t channel = 0U; channel < xyz.size(); ++channel)
    {
        const float value = scaled[channel] + scaled[1] * add_coefficient[channel];
        const float inverse =
            value > epsilon ? value * value * value : (116.0F * value - 16.0F) / kappa;
        xyz[channel] = d50[channel] * inverse;
    }
    return xyz;
}

// Independent scalar oracle transcribed from frozen colorcontrast.c v2. It
// intentionally calls neither the production Color Contrast helper nor the
// production D50 bridge, preserving the source multiply/add and CLAMPS order.
[[nodiscard]] FrozenD50Triplet frozen_color_contrast_lab(const ColorContrastParams &params,
                                                         const FrozenD50Triplet &lab) noexcept
{
    const float a_steepness = static_cast<float>(params.a_steepness);
    const float a_offset = static_cast<float>(params.a_offset);
    const float b_steepness = static_cast<float>(params.b_steepness);
    const float b_offset = static_cast<float>(params.b_offset);
    float a = lab[1] * a_steepness + a_offset;
    float b = lab[2] * b_steepness + b_offset;
    if (!params.unbound)
    {
        a = a > -128.0F ? (a < 128.0F ? a : 128.0F) : -128.0F;
        b = b > -128.0F ? (b < 128.0F ? b : 128.0F) : -128.0F;
    }
    return {lab[0], a, b};
}

[[nodiscard]] FrozenD50Triplet frozen_color_contrast_rgb(const ColorContrastParams &params,
                                                         const FrozenD50Triplet &rgb) noexcept
{
    const auto lab = frozen_xyz_d50_to_lab(frozen_linear_rec709_to_xyz_d50(rgb));
    return frozen_xyz_d50_to_linear_rec709(
        frozen_lab_to_xyz_d50(frozen_color_contrast_lab(params, lab)));
}

[[nodiscard]] std::array<std::uint32_t, 3>
d50_triplet_bits(const FrozenD50Triplet &triplet) noexcept
{
    return {std::bit_cast<std::uint32_t>(triplet[0]), std::bit_cast<std::uint32_t>(triplet[1]),
            std::bit_cast<std::uint32_t>(triplet[2])};
}

void expect_frozen_d50_bits(const FrozenD50Triplet &actual, const FrozenD50Triplet &oracle,
                            const std::array<std::uint32_t, 3> &golden)
{
    EXPECT_EQ(d50_triplet_bits(oracle), golden);
    EXPECT_EQ(d50_triplet_bits(actual), golden);
}

void expect_frozen_d50_cbrt_reference(const FrozenD50Triplet &actual,
                                      const FrozenD50Triplet &oracle,
                                      const std::array<std::uint32_t, 3> &reference)
{
    EXPECT_EQ(d50_triplet_bits(actual), d50_triplet_bits(oracle));
    for (std::size_t channel = 0U; channel < reference.size(); ++channel)
    {
        // cbrtf is platform libm code. Preserve exact host-local source-order
        // agreement while retaining a tight, recorded cross-platform envelope.
        EXPECT_NEAR(actual[channel], std::bit_cast<float>(reference[channel]),
                    kPlatformLibmReferenceTolerance);
    }
}

// Independent scalar oracle transcribed from the frozen colorbalance.c
// commit_params(), _process_sop(), and _process_lgg() paths. It deliberately
// calls no production Color Balance helper, so the fixed goldens below do not
// merely restate apply_color_balance().
[[nodiscard]] std::vector<float>
frozen_legacy_color_balance_reference(const WorkingImage &input, const ColorBalanceParams &params)
{
    const auto linear_to_xyz = [](const std::array<float, 3> &rgb)
    {
        return std::array<float, 3>{0.4360747F * rgb[0] + 0.3850649F * rgb[1] + 0.1430804F * rgb[2],
                                    0.2225045F * rgb[0] + 0.7168786F * rgb[1] + 0.0606169F * rgb[2],
                                    0.0139322F * rgb[0] + 0.0971045F * rgb[1] +
                                        0.7141733F * rgb[2]};
    };
    const auto xyz_to_linear = [](const std::array<float, 3> &xyz)
    {
        return std::array<float, 3>{
            3.1338561F * xyz[0] - 1.6168667F * xyz[1] - 0.4906146F * xyz[2],
            -0.9787684F * xyz[0] + 1.9161415F * xyz[1] + 0.0334540F * xyz[2],
            0.0719453F * xyz[0] - 0.2289914F * xyz[1] + 1.4052427F * xyz[2]};
    };
    const auto xyz_to_lab = [](const std::array<float, 3> &xyz)
    {
        constexpr std::array<float, 3> d50{0.9642F, 1.0F, 0.8249F};
        constexpr float epsilon = 216.0F / 24389.0F;
        constexpr float kappa = 24389.0F / 27.0F;
        std::array<float, 3> f{};
        for (std::size_t channel = 0U; channel < f.size(); ++channel)
        {
            const float value = xyz[channel] / d50[channel];
            f[channel] = value > epsilon ? std::cbrt(value) : (kappa * value + 16.0F) / 116.0F;
        }
        return std::array<float, 3>{116.0F * f[1] - 16.0F, 500.0F * (f[0] - f[1]),
                                    200.0F * (f[1] - f[2])};
    };
    const auto lab_to_xyz = [](const std::array<float, 3> &lab)
    {
        constexpr std::array<float, 3> d50{0.9642F, 1.0F, 0.8249F};
        constexpr float epsilon = 0.20689655172413796F;
        constexpr float kappa = 24389.0F / 27.0F;
        const float fy = (lab[0] + 16.0F) / 116.0F;
        const std::array<float, 3> f{fy + lab[1] / 500.0F, fy, fy - lab[2] / 200.0F};
        std::array<float, 3> xyz{};
        for (std::size_t channel = 0U; channel < xyz.size(); ++channel)
        {
            const float value = f[channel] > epsilon ? f[channel] * f[channel] * f[channel] :
                                                       (116.0F * f[channel] - 16.0F) / kappa;
            xyz[channel] = d50[channel] * value;
        }
        return xyz;
    };
    const auto xyz_to_prophoto = [](const std::array<float, 3> &xyz)
    {
        return std::array<float, 3>{
            1.3459433F * xyz[0] - 0.2556075F * xyz[1] - 0.0511118F * xyz[2],
            -0.5445989F * xyz[0] + 1.5081673F * xyz[1] + 0.0205351F * xyz[2], 1.2118128F * xyz[2]};
    };
    const auto prophoto_to_xyz = [](const std::array<float, 3> &rgb)
    {
        return std::array<float, 3>{0.7976749F * rgb[0] + 0.1351917F * rgb[1] + 0.0313534F * rgb[2],
                                    0.2880402F * rgb[0] + 0.7118741F * rgb[1] + 0.0000857F * rgb[2],
                                    0.8252100F * rgb[2]};
    };
    const auto corrected = [&](const std::array<double, 4> &values)
    {
        const float red = static_cast<float>(values[1]);
        const float green = static_cast<float>(values[2]);
        const float blue = static_cast<float>(values[3]);
        const float luma = 0.2880402F * red + 0.7118741F * green + 0.0000857F * blue;
        return std::array<float, 4>{static_cast<float>(values[0]), red - luma + 1.0F,
                                    green - luma + 1.0F, blue - luma + 1.0F};
    };

    const auto lift = corrected(params.lift);
    const auto gamma = corrected(params.gamma);
    const auto gain = corrected(params.gain);
    const bool lgg = params.mode == kColorBalanceModeLiftGammaGain;
    std::array<float, 3> effective_lift{};
    std::array<float, 3> effective_gain{};
    std::array<float, 3> effective_power{};
    for (std::size_t channel = 0U; channel < 3U; ++channel)
    {
        effective_gain[channel] = gain[channel + 1U] * gain[0];
        if (lgg)
        {
            effective_lift[channel] = 2.0F - lift[channel + 1U] * lift[0];
            const float denominator = gamma[channel + 1U] * gamma[0];
            effective_power[channel] =
                2.2F * (denominator != 0.0F ? 1.0F / denominator : 1000000.0F);
        }
        else
        {
            effective_lift[channel] = lift[channel + 1U] + lift[0] - 2.0F;
            effective_power[channel] = (2.0F - gamma[channel + 1U]) * (2.0F - gamma[0]);
        }
    }
    const float input_saturation = static_cast<float>(params.input_saturation);
    const float output_saturation = static_cast<float>(params.output_saturation);
    const float contrast = static_cast<float>(params.contrast);
    const float contrast_power = 1.0F / contrast;
    const float grey = static_cast<float>(params.grey_fulcrum_percent / 100.0);
    const bool run_input_saturation = std::abs(input_saturation - 1.0F) > 1.0e-6F;
    const bool run_output_saturation = std::abs(output_saturation - 1.0F) > 1.0e-6F;
    const bool run_contrast = std::abs((lgg ? contrast_power : contrast) - 1.0F) > 1.0e-6F;

    std::vector<float> result(input.rgb.size());
    for (std::size_t index = 0U; index < input.rgb.size(); index += 3U)
    {
        const std::array<float, 3> source{input.rgb[index], input.rgb[index + 1U],
                                          input.rgb[index + 2U]};
        auto xyz = lab_to_xyz(xyz_to_lab(linear_to_xyz(source)));
        auto rgb = xyz_to_prophoto(xyz);
        if (run_input_saturation)
        {
            for (float &sample : rgb)
            {
                sample = xyz[1] + input_saturation * (sample - xyz[1]);
            }
        }
        for (std::size_t channel = 0U; channel < rgb.size(); ++channel)
        {
            if (lgg)
            {
                float value = std::pow(std::max(rgb[channel], 0.0F), 1.0F / 2.2F);
                value = ((value - 1.0F) * effective_lift[channel] + 1.0F) * effective_gain[channel];
                rgb[channel] = std::pow(std::max(value, 0.0F), effective_power[channel]);
            }
            else
            {
                rgb[channel] = std::pow(
                    std::max(effective_gain[channel] * rgb[channel] + effective_lift[channel],
                             0.0F),
                    effective_power[channel]);
            }
        }
        if (run_output_saturation)
        {
            const float luma = prophoto_to_xyz(rgb)[1];
            for (float &sample : rgb)
            {
                sample = luma + output_saturation * (sample - luma);
            }
        }
        if (run_contrast)
        {
            for (float &sample : rgb)
            {
                sample = std::pow(std::max(sample, 0.0F) / grey, contrast_power) * grey;
            }
        }
        const auto output = xyz_to_linear(lab_to_xyz(xyz_to_lab(prophoto_to_xyz(rgb))));
        std::copy(output.begin(), output.end(),
                  result.begin() + static_cast<std::ptrdiff_t>(index));
    }
    return result;
}
struct FrozenColorCheckerFit
{
    std::vector<std::array<float, 3>> sources;
    std::array<std::vector<float>, 3> coefficients;
};

[[nodiscard]] float frozen_color_checker_kernel_oracle(const std::array<float, 3> &left,
                                                       const std::array<float, 3> &right,
                                                       const bool use_libm)
{
    std::array<float, 3> squared{};
    for (std::size_t channel = 0U; channel < squared.size(); ++channel)
    {
        squared[channel] = left[channel] - right[channel];
        squared[channel] *= squared[channel];
    }
    const float radius_squared = squared[0] + squared[1] + squared[2];
    if (use_libm)
    {
        return radius_squared * std::log(std::max(1.0e-8F, radius_squared));
    }
    const float argument = std::max(1.0e-8F, radius_squared);
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(argument);
    const float mantissa = std::bit_cast<float>((bits & 0x007fffffU) | 0x3f000000U);
    float exponent = static_cast<float>(bits);
    exponent *= 1.1920928955078125e-7F;
    const float log2 = exponent - 124.22551499F - 1.498030302F * mantissa -
                       1.72587999F / (0.3520887068F + mantissa);
    return radius_squared * (0.69314718055994530942F * log2);
}

[[nodiscard]] bool frozen_color_checker_triangular(std::vector<double> &matrix,
                                                   std::vector<int> &pivots, const std::size_t size)
{
    pivots[size - 1U] = static_cast<int>(size - 1U);
    for (std::size_t column = 0U; column < size; ++column)
    {
        std::size_t best_row = column;
        for (std::size_t row = column + 1U; row < size; ++row)
        {
            if (std::fabs(matrix[row * size + column]) >
                std::fabs(matrix[best_row * size + column]))
            {
                best_row = row;
            }
        }
        pivots[column] = static_cast<int>(best_row);
        const double pivot = matrix[best_row * size + column];
        std::swap(matrix[best_row * size + column], matrix[column * size + column]);
        if (pivot == 0.0)
        {
            return false;
        }
        for (std::size_t row = column + 1U; row < size; ++row)
        {
            matrix[row * size + column] /= -pivot;
        }
        if (best_row != column)
        {
            for (std::size_t remaining = column + 1U; remaining < size; ++remaining)
            {
                std::swap(matrix[best_row * size + remaining], matrix[column * size + remaining]);
            }
        }
        for (std::size_t row = column + 1U; row < size; ++row)
        {
            for (std::size_t remaining = column + 1U; remaining < size; ++remaining)
            {
                matrix[row * size + remaining] +=
                    matrix[row * size + column] * matrix[column * size + remaining];
            }
        }
    }
    return true;
}

void frozen_color_checker_back_substitute(const std::vector<double> &matrix,
                                          const std::vector<int> &pivots,
                                          std::vector<double> &right, const std::size_t size)
{
    for (std::size_t column = 0U; column + 1U < size; ++column)
    {
        const std::size_t pivot = static_cast<std::size_t>(pivots[column]);
        const double value = right[pivot];
        std::swap(right[pivot], right[column]);
        for (std::size_t row = column + 1U; row < size; ++row)
        {
            right[row] += matrix[row * size + column] * value;
        }
    }
    for (std::size_t column = size - 1U; column > 0U; --column)
    {
        right[column] /= matrix[column * size + column];
        for (std::size_t row = 0U; row < column; ++row)
        {
            right[row] -= matrix[row * size + column] * right[column];
        }
    }
    right[0] /= matrix[0];
}

[[nodiscard]] bool frozen_color_checker_solve(std::vector<double> matrix,
                                              std::vector<double> &right)
{
    std::vector<int> pivots(right.size());
    if (!frozen_color_checker_triangular(matrix, pivots, right.size()))
    {
        return false;
    }
    frozen_color_checker_back_substitute(matrix, pivots, right, right.size());
    return true;
}

[[nodiscard]] FrozenColorCheckerFit
frozen_color_checker_fit_oracle(const ColorCheckerParams &params, const bool use_libm,
                                const bool promote_n3_sum)
{
    FrozenColorCheckerFit fit;
    fit.sources.reserve(params.patches.size());
    for (const auto &patch : params.patches)
    {
        fit.sources.push_back({static_cast<float>(patch.source_lab[0]),
                               static_cast<float>(patch.source_lab[1]),
                               static_cast<float>(patch.source_lab[2])});
    }
    const std::size_t count = fit.sources.size();
    for (auto &coefficients : fit.coefficients)
    {
        coefficients.assign(count + 4U, 0.0F);
    }
    fit.coefficients[0][count + 1U] = 1.0F;
    fit.coefficients[1][count + 2U] = 1.0F;
    fit.coefficients[2][count + 3U] = 1.0F;
    const auto target = [&](const std::size_t patch, const std::size_t channel)
    { return static_cast<float>(params.patches[patch].target_lab[channel]); };

    if (count == 0U)
    {
        return fit;
    }
    if (count == 1U)
    {
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            fit.coefficients[channel][count + channel + 1U] =
                target(0U, channel) / fit.sources[0][channel];
        }
        return fit;
    }
    if (count == 2U)
    {
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            std::vector<double> right{target(0U, channel), target(1U, channel)};
            if (!frozen_color_checker_solve(
                    {1.0, fit.sources[0][channel], 1.0, fit.sources[1][channel]}, right))
            {
                return fit;
            }
            fit.coefficients[channel][count] = static_cast<float>(right[0]);
            fit.coefficients[channel][count + channel + 1U] = static_cast<float>(right[1]);
        }
        return fit;
    }
    if (count == 3U)
    {
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            std::vector<double> matrix;
            for (std::size_t patch = 0U; patch < count; ++patch)
            {
                const std::size_t other0 = (channel + 1U) % 3U;
                const std::size_t other1 = (channel + 2U) % 3U;
                const double other_sum = promote_n3_sum ?
                                             static_cast<double>(fit.sources[patch][other0]) +
                                                 fit.sources[patch][other1] :
                                             static_cast<double>(fit.sources[patch][other0] +
                                                                 fit.sources[patch][other1]);
                matrix.insert(matrix.end(), {1.0, fit.sources[patch][channel], other_sum});
            }
            std::vector<double> right{target(0U, channel), target(1U, channel),
                                      target(2U, channel)};
            if (!frozen_color_checker_solve(std::move(matrix), right))
            {
                return fit;
            }
            fit.coefficients[channel][count] = static_cast<float>(right[0]);
            fit.coefficients[channel][count + channel + 1U] = static_cast<float>(right[1]);
            for (std::size_t input = 0U; input < 3U; ++input)
            {
                if (input != channel)
                {
                    fit.coefficients[channel][count + input + 1U] = static_cast<float>(right[2]);
                }
            }
        }
        return fit;
    }

    const std::size_t fit_size = count == 4U ? 4U : count + 4U;
    std::vector<double> matrix(fit_size * fit_size, 0.0);
    if (count == 4U)
    {
        for (std::size_t patch = 0U; patch < count; ++patch)
        {
            matrix[patch * fit_size] = 1.0;
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                matrix[patch * fit_size + channel + 1U] = fit.sources[patch][channel];
            }
        }
    }
    else
    {
        for (std::size_t row = 0U; row < count; ++row)
        {
            for (std::size_t column = 0U; column < count; ++column)
            {
                matrix[row * fit_size + column] = frozen_color_checker_kernel_oracle(
                    fit.sources[row], fit.sources[column], use_libm);
            }
            matrix[row * fit_size + count] = matrix[count * fit_size + row] = 1.0;
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                matrix[row * fit_size + count + channel + 1U] =
                    matrix[(count + channel + 1U) * fit_size + row] = fit.sources[row][channel];
            }
        }
    }
    std::vector<int> pivots(fit_size);
    if (!frozen_color_checker_triangular(matrix, pivots, fit_size))
    {
        return fit;
    }
    for (std::size_t channel = 0U; channel < 3U; ++channel)
    {
        std::vector<double> right(fit_size, 0.0);
        for (std::size_t patch = 0U; patch < count; ++patch)
        {
            right[patch] = target(patch, channel);
        }
        frozen_color_checker_back_substitute(matrix, pivots, right, fit_size);
        const std::size_t offset = count == 4U ? count : 0U;
        for (std::size_t index = 0U; index < fit_size; ++index)
        {
            fit.coefficients[channel][offset + index] = static_cast<float>(right[index]);
        }
    }
    return fit;
}

[[nodiscard]] std::array<float, 3>
frozen_color_checker_lab_reference(const ColorCheckerParams &params,
                                   const std::array<float, 3> &lab, const bool use_libm,
                                   const bool promote_n3_sum)
{
    const auto fit = frozen_color_checker_fit_oracle(params, use_libm, promote_n3_sum);
    const std::size_t count = fit.sources.size();
    std::array<float, 3> result{};
    for (std::size_t channel = 0U; channel < result.size(); ++channel)
    {
        const float term_l = fit.coefficients[channel][count + 1U] * lab[0];
        const float term_a = fit.coefficients[channel][count + 2U] * lab[1];
        const float term_b = fit.coefficients[channel][count + 3U] * lab[2];
        result[channel] = fit.coefficients[channel][count] + (term_l + term_a + term_b);
    }
    for (std::size_t patch = 0U; patch < count; ++patch)
    {
        const float phi = frozen_color_checker_kernel_oracle(lab, fit.sources[patch], use_libm);
        for (std::size_t channel = 0U; channel < result.size(); ++channel)
        {
            result[channel] += fit.coefficients[channel][patch] * phi;
        }
    }
    return result;
}

} // namespace ravo::engine_color_test_support
