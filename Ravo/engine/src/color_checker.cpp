#include "color_checker.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "d50_lab.h"
#include "ravo/recipe/color_input.h"

namespace ravo
{
namespace
{

struct ColorCheckerFit
{
    std::vector<std::array<float, 3>> sources;
    std::array<std::vector<float>, 3> coefficients;
};

[[nodiscard]] Result<void> validate_typed_params(const ColorCheckerParams &params)
{
    if (params.patches.size() > kColorCheckerMaxPatchCount)
    {
        return make_error(ErrorCode::kValidation,
                          "Color checker supports at most 49 ordered patches",
                          {{"patch_count", std::to_string(params.patches.size())}});
    }
    for (std::size_t patch = 0U; patch < params.patches.size(); ++patch)
    {
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            for (const double component : {params.patches[patch].source_lab[channel],
                                           params.patches[patch].target_lab[channel]})
            {
                if (!std::isfinite(component) || !std::isfinite(static_cast<float>(component)))
                {
                    return make_error(
                        ErrorCode::kValidation,
                        "Color checker Lab component must be finite and representable as float",
                        {{"patch_index", std::to_string(patch)},
                         {"component_index", std::to_string(channel)}});
                }
            }
        }
    }
    return {};
}

[[nodiscard]] float frozen_fastlog2(const float value) noexcept
{
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const float mantissa = std::bit_cast<float>((bits & 0x007fffffU) | 0x3f000000U);
    float exponent = static_cast<float>(bits);
    exponent *= 1.1920928955078125e-7F;
    return exponent - 124.22551499F - 1.498030302F * mantissa -
           1.72587999F / (0.3520887068F + mantissa);
}

[[nodiscard]] float frozen_fastlog(const float value) noexcept
{
    return 0.69314718055994530942F * frozen_fastlog2(value);
}

[[nodiscard]] bool gauss_make_triangular(std::vector<double> &matrix, std::vector<int> &pivots,
                                         const std::size_t size) noexcept
{
    pivots[size - 1U] = static_cast<int>(size - 1U);
    for (std::size_t k = 0; k < size; ++k)
    {
        std::size_t pivot = k;
        for (std::size_t row = k + 1U; row < size; ++row)
        {
            if (std::fabs(matrix[k + size * row]) > std::fabs(matrix[k + size * pivot]))
            {
                pivot = row;
            }
        }
        pivots[k] = static_cast<int>(pivot);
        const double diagonal = matrix[k + size * pivot];
        matrix[k + size * pivot] = matrix[k + size * k];
        matrix[k + size * k] = diagonal;
        if (diagonal == 0.0)
        {
            return false;
        }
        for (std::size_t row = k + 1U; row < size; ++row)
        {
            matrix[k + size * row] /= -diagonal;
        }
        if (k != pivot)
        {
            for (std::size_t column = k + 1U; column < size; ++column)
            {
                std::swap(matrix[column + size * pivot], matrix[column + size * k]);
            }
        }
        for (std::size_t row = k + 1U; row < size; ++row)
        {
            for (std::size_t column = k + 1U; column < size; ++column)
            {
                matrix[column + size * row] += matrix[k + row * size] * matrix[column + k * size];
            }
        }
    }
    return true;
}

void gauss_solve_triangular(const std::vector<double> &matrix, const std::vector<int> &pivots,
                            std::vector<double> &right, const std::size_t size) noexcept
{
    for (std::size_t k = 0; k + 1U < size; ++k)
    {
        const std::size_t pivot = static_cast<std::size_t>(pivots[k]);
        const double value = right[pivot];
        right[pivot] = right[k];
        right[k] = value;
        for (std::size_t row = k + 1U; row < size; ++row)
        {
            right[row] += matrix[k + size * row] * value;
        }
    }
    for (std::size_t k = size - 1U; k > 0U; --k)
    {
        right[k] /= matrix[k + size * k];
        const double value = right[k];
        for (std::size_t row = 0U; row < k; ++row)
        {
            right[row] -= matrix[k + size * row] * value;
        }
    }
    right[0] /= matrix[0];
}

[[nodiscard]] bool gauss_solve(std::vector<double> &matrix, std::vector<double> &right,
                               const std::size_t size)
{
    std::vector<int> pivots(size);
    if (!gauss_make_triangular(matrix, pivots, size))
    {
        return false;
    }
    gauss_solve_triangular(matrix, pivots, right, size);
    return true;
}

[[nodiscard]] Result<void> store_solution(std::vector<float> &coefficients,
                                          const std::size_t offset,
                                          const std::vector<double> &solution,
                                          const std::string_view channel)
{
    for (std::size_t index = 0U; index < solution.size(); ++index)
    {
        const float value = static_cast<float>(solution[index]);
        if (!std::isfinite(solution[index]) || !std::isfinite(value))
        {
            return make_error(
                ErrorCode::kValidation, "Color checker fit produced a non-finite coefficient",
                {{"channel", std::string(channel)}, {"reason", "nonfinite_colorchecker_fit"}});
        }
        coefficients[offset + index] = value;
    }
    return {};
}

[[nodiscard]] Result<ColorCheckerFit> fit_color_checker(const ColorCheckerParams &params,
                                                        const CancellationToken &cancellation)
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    auto valid = validate_typed_params(params);
    if (!valid)
    {
        return valid.error();
    }

    ColorCheckerFit fit;
    const std::size_t count = params.patches.size();
    const std::size_t fit_size = count + 4U;
    fit.sources.reserve(count);
    for (const auto &patch : params.patches)
    {
        fit.sources.push_back({static_cast<float>(patch.source_lab[0]),
                               static_cast<float>(patch.source_lab[1]),
                               static_cast<float>(patch.source_lab[2])});
    }
    for (auto &coefficients : fit.coefficients)
    {
        coefficients.assign(fit_size, 0.0F);
    }
    fit.coefficients[0][count + 1U] = 1.0F;
    fit.coefficients[1][count + 2U] = 1.0F;
    fit.coefficients[2][count + 3U] = 1.0F;

    const auto target = [&](const std::size_t patch, const std::size_t channel)
    { return static_cast<float>(params.patches[patch].target_lab[channel]); };
    const auto store = [&](const std::size_t channel, const std::size_t offset,
                           const std::vector<double> &solution) -> Result<void>
    {
        constexpr std::array<std::string_view, 3> names{"L", "a", "b"};
        return store_solution(fit.coefficients[channel], offset, solution, names[channel]);
    };

    switch (count)
    {
    case 0U:
        return fit;
    case 1U:
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            const float denominator = fit.sources[0][channel];
            if (denominator == 0.0F)
            {
                return make_error(ErrorCode::kValidation,
                                  "Color checker one-patch denominator is zero",
                                  {{"channel", std::to_string(channel)},
                                   {"reason", "invalid_colorchecker_denominator"}});
            }
            const float coefficient = target(0U, channel) / denominator;
            if (!std::isfinite(coefficient))
            {
                return make_error(ErrorCode::kValidation,
                                  "Color checker one-patch coefficient is not finite",
                                  {{"channel", std::to_string(channel)},
                                   {"reason", "nonfinite_colorchecker_fit"}});
            }
            fit.coefficients[channel][count + channel + 1U] = coefficient;
        }
        return fit;
    case 2U:
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            std::vector<double> matrix{1.0, fit.sources[0][channel], 1.0, fit.sources[1][channel]};
            std::vector<double> right{target(0U, channel), target(1U, channel)};
            if (!gauss_solve(matrix, right, 2U))
            {
                return fit;
            }
            auto stored = store_solution(fit.coefficients[channel], count, right,
                                         channel == 0U ? "L" :
                                         channel == 1U ? "a" :
                                                         "b");
            if (!stored)
            {
                return stored.error();
            }
            // The slope belongs to the channel-matched polynomial slot.
            if (channel != 0U)
            {
                fit.coefficients[channel][count + channel + 1U] =
                    fit.coefficients[channel][count + 1U];
                fit.coefficients[channel][count + 1U] = 0.0F;
            }
        }
        return fit;
    case 3U:
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            std::vector<double> matrix;
            matrix.reserve(9U);
            for (std::size_t patch = 0U; patch < 3U; ++patch)
            {
                const std::size_t other0 = (channel + 1U) % 3U;
                const std::size_t other1 = (channel + 2U) % 3U;
                const float other_sum = fit.sources[patch][other0] + fit.sources[patch][other1];
                matrix.push_back(1.0);
                matrix.push_back(fit.sources[patch][channel]);
                matrix.push_back(other_sum);
            }
            std::vector<double> right{target(0U, channel), target(1U, channel),
                                      target(2U, channel)};
            if (!gauss_solve(matrix, right, 3U))
            {
                return fit;
            }
            for (const double value : right)
            {
                if (!std::isfinite(value) || !std::isfinite(static_cast<float>(value)))
                {
                    return make_error(ErrorCode::kValidation,
                                      "Color checker fit produced a non-finite coefficient",
                                      {{"channel", std::to_string(channel)},
                                       {"reason", "nonfinite_colorchecker_fit"}});
                }
            }
            fit.coefficients[channel][count] = static_cast<float>(right[0]);
            fit.coefficients[channel][count + channel + 1U] = static_cast<float>(right[1]);
            const float shared = static_cast<float>(right[2]);
            for (std::size_t input_channel = 0U; input_channel < 3U; ++input_channel)
            {
                if (input_channel != channel)
                {
                    fit.coefficients[channel][count + input_channel + 1U] = shared;
                }
            }
        }
        return fit;
    case 4U:
    {
        std::vector<double> matrix;
        matrix.reserve(16U);
        for (const auto &source : fit.sources)
        {
            matrix.insert(matrix.end(), {1.0, source[0], source[1], source[2]});
        }
        std::vector<int> pivots(4U);
        if (!gauss_make_triangular(matrix, pivots, 4U))
        {
            return fit;
        }
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            std::vector<double> right{target(0U, channel), target(1U, channel), target(2U, channel),
                                      target(3U, channel)};
            gauss_solve_triangular(matrix, pivots, right, 4U);
            auto stored = store(channel, count, right);
            if (!stored)
            {
                return stored.error();
            }
        }
        return fit;
    }
    default:
        break;
    }

    std::vector<double> matrix(fit_size * fit_size);
    for (std::size_t column = 0U; column < count; ++column)
    {
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (std::size_t row = column; row < count; ++row)
        {
            const float value =
                color_checker_thin_plate_kernel(fit.sources[row], fit.sources[column]);
            if (!std::isfinite(value))
            {
                return make_error(ErrorCode::kValidation,
                                  "Color checker kernel produced a non-finite coefficient",
                                  {{"reason", "nonfinite_colorchecker_kernel"}});
            }
            matrix[column * fit_size + row] = value;
            matrix[row * fit_size + column] = value;
        }
    }
    for (std::size_t row = 0U; row < count; ++row)
    {
        matrix[row * fit_size + count] = 1.0;
        matrix[count * fit_size + row] = 1.0;
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            matrix[row * fit_size + count + channel + 1U] = fit.sources[row][channel];
            matrix[(count + channel + 1U) * fit_size + row] = fit.sources[row][channel];
        }
    }
    for (std::size_t row = count; row < fit_size; ++row)
    {
        for (std::size_t column = count; column < fit_size; ++column)
        {
            matrix[row * fit_size + column] = 0.0;
        }
    }
    std::vector<int> pivots(fit_size);
    if (!gauss_make_triangular(matrix, pivots, fit_size))
    {
        return fit;
    }
    for (std::size_t channel = 0U; channel < 3U; ++channel)
    {
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        std::vector<double> right(fit_size, 0.0);
        for (std::size_t patch = 0U; patch < count; ++patch)
        {
            right[patch] = target(patch, channel);
        }
        gauss_solve_triangular(matrix, pivots, right, fit_size);
        auto stored = store(channel, 0U, right);
        if (!stored)
        {
            return stored.error();
        }
    }
    return fit;
}

[[nodiscard]] Result<std::array<float, 3>> evaluate_fit(const ColorCheckerFit &fit,
                                                        const std::array<float, 3> &lab,
                                                        const CancellationToken &cancellation)
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    for (std::size_t channel = 0U; channel < lab.size(); ++channel)
    {
        if (!std::isfinite(lab[channel]))
        {
            return make_error(ErrorCode::kValidation,
                              "Color checker input contains a non-finite Lab sample",
                              {{"channel", std::to_string(channel)},
                               {"reason", "nonfinite_colorchecker_lab_input"}});
        }
    }
    const std::size_t count = fit.sources.size();
    std::array<float, 3> result{};
    for (std::size_t channel = 0U; channel < result.size(); ++channel)
    {
        const float term_l = fit.coefficients[channel][count + 1U] * lab[0];
        const float term_a = fit.coefficients[channel][count + 2U] * lab[1];
        const float term_b = fit.coefficients[channel][count + 3U] * lab[2];
        const float polynomial_sum = term_l + term_a + term_b;
        result[channel] = fit.coefficients[channel][count] + polynomial_sum;
    }
    for (std::size_t patch = 0U; patch < count; ++patch)
    {
        const float phi = color_checker_thin_plate_kernel(lab, fit.sources[patch]);
        if (!std::isfinite(phi))
        {
            return make_error(ErrorCode::kValidation,
                              "Color checker kernel produced a non-finite sample",
                              {{"patch_index", std::to_string(patch)},
                               {"reason", "nonfinite_colorchecker_kernel"}});
        }
        for (std::size_t channel = 0U; channel < result.size(); ++channel)
        {
            result[channel] += fit.coefficients[channel][patch] * phi;
        }
    }
    for (std::size_t channel = 0U; channel < result.size(); ++channel)
    {
        if (!std::isfinite(result[channel]))
        {
            return make_error(ErrorCode::kValidation,
                              "Color checker produced a non-finite Lab sample",
                              {{"channel", std::to_string(channel)},
                               {"reason", "nonfinite_colorchecker_lab_output"}});
        }
    }
    return result;
}

} // namespace

float color_checker_thin_plate_kernel(const std::array<float, 3> &left,
                                      const std::array<float, 3> &right) noexcept
{
    std::array<float, 3> squared{};
    for (std::size_t channel = 0U; channel < squared.size(); ++channel)
    {
        squared[channel] = left[channel] - right[channel];
        squared[channel] *= squared[channel];
    }
    const float radius_squared = squared[0] + squared[1] + squared[2];
    return radius_squared * frozen_fastlog(std::max(1.0e-8F, radius_squared));
}

Result<std::array<float, 3>> apply_color_checker_lab(const ColorCheckerParams &params,
                                                     const std::array<float, 3> &lab,
                                                     const CancellationToken &cancellation)
try
{
    auto fit = fit_color_checker(params, cancellation);
    if (!fit)
    {
        return fit.error();
    }
    return evaluate_fit(fit.value(), lab, cancellation);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Color checker fit allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<WorkingImage> apply_color_checker(const WorkingImage &input,
                                         const ColorCheckerParams &params,
                                         const CancellationToken &cancellation)
try
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    if (input.width == 0U || input.height == 0U)
    {
        return make_error(ErrorCode::kValidation, "Color checker input dimensions must be non-zero",
                          {{"reason", "invalid_colorchecker_dimensions"}});
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(input.width) * input.height;
    if (pixels > std::numeric_limits<std::size_t>::max() / 3U ||
        input.rgb.size() != static_cast<std::size_t>(pixels * 3U))
    {
        return make_error(ErrorCode::kValidation,
                          "Color checker input buffer does not match its dimensions",
                          {{"reason", "invalid_colorchecker_buffer"}});
    }
    if (input.color_profile.model != ColorModel::kRgb ||
        input.color_profile.identifier != kInputProfileLinearRec709)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Color checker requires linear sRGB D50 working pixels",
                          {{"profile", input.color_profile.identifier},
                           {"reason", "unsupported_colorchecker_working_space"}});
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
                                  "Color checker input contains a non-finite sample",
                                  {{"sample_index", std::to_string(index)},
                                   {"reason", "nonfinite_colorchecker_input"}});
            }
        }
    }

    auto fit = fit_color_checker(params, cancellation);
    if (!fit)
    {
        return fit.error();
    }
    active = cancellation.check();
    if (!active)
    {
        return active.error();
    }

    WorkingImage output;
    output.width = input.width;
    output.height = input.height;
    output.color_profile = input.color_profile;
    output.exposure_analysis = input.exposure_analysis;
    output.canonical_roi_scale = input.canonical_roi_scale;
    output.rgb.resize(input.rgb.size());
    for (std::uint32_t row = 0U; row < input.height; ++row)
    {
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (std::uint32_t column = 0U; column < input.width; ++column)
        {
            const std::size_t index = (static_cast<std::size_t>(row) * input.width + column) * 3U;
            const std::array<float, 3> rgb{input.rgb[index], input.rgb[index + 1U],
                                           input.rgb[index + 2U]};
            const auto lab = d50_lab::xyz_to_lab(d50_lab::linear_rec709_to_xyz(rgb));
            auto transformed = evaluate_fit(fit.value(), lab, cancellation);
            if (!transformed)
            {
                return transformed.error();
            }
            const auto transformed_rgb =
                d50_lab::xyz_to_linear_rec709(d50_lab::lab_to_xyz(transformed.value()));
            for (std::size_t channel = 0U; channel < transformed_rgb.size(); ++channel)
            {
                if (!std::isfinite(transformed_rgb[channel]))
                {
                    return make_error(ErrorCode::kValidation,
                                      "Color checker produced a non-finite RGB sample",
                                      {{"sample_index", std::to_string(index + channel)},
                                       {"reason", "nonfinite_colorchecker_output"}});
                }
                output.rgb[index + channel] = transformed_rgb[channel];
            }
        }
    }
    active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    return output;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Color checker output allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<WorkingImage> apply_color_checker(const WorkingImage &input,
                                         const OperationInstance &operation,
                                         const CancellationToken &cancellation)
try
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    if (operation.id != kColorCheckerOperationId)
    {
        return make_error(ErrorCode::kValidation, "Operation is not color checker",
                          {{"operation_id", operation.id}});
    }
    if (operation.schema_version != kColorCheckerOperationSchemaVersion)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Color checker operation schema version is unsupported",
                          {{"operation_id", operation.id},
                           {"schema_version", std::to_string(operation.schema_version)}});
    }
    if (operation.mask_id.has_value())
    {
        return make_error(
            ErrorCode::kUnsupported, "Color checker mask evaluation is unavailable",
            {{"operation_id", operation.id}, {"reason", "colorchecker_mask_graph_unavailable"}});
    }
    if (!operation.enabled)
    {
        return input;
    }
    auto params = color_checker_from_parameters(operation.parameters);
    if (!params)
    {
        return params.error();
    }
    return apply_color_checker(input, params.value(), cancellation);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Color checker operation allocation failed",
                      {{"reason", "allocation_failed"}});
}

} // namespace ravo
