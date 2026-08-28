#include "profile_gamma.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <tuple>

namespace ravo
{
namespace
{

constexpr float kProfileGammaNoiseFloor = 1.0F / 65536.0F;

static_assert(sizeof(float) == sizeof(std::uint32_t));
static_assert(std::numeric_limits<float>::is_iec559);

[[nodiscard]] Result<void> validate_bounded(const double value, const double minimum,
                                            const double maximum, const std::string_view name)
{
    if (!std::isfinite(value))
    {
        return make_error(ErrorCode::kValidation, "Profile gamma parameter must be finite",
                          {{"parameter", std::string(name)}});
    }
    if (value < minimum || value > maximum)
    {
        return make_error(ErrorCode::kValidation,
                          "Profile gamma parameter is outside the permitted range",
                          {{"parameter", std::string(name)}});
    }
    return {};
}

[[nodiscard]] Result<ProfileGammaRenderMode> parse_mode(const std::string_view mode)
{
    if (mode == kProfileGammaModeLogarithmic)
    {
        return ProfileGammaRenderMode::kLogarithmic;
    }
    if (mode == kProfileGammaModeGamma)
    {
        return ProfileGammaRenderMode::kGamma;
    }
    return make_error(ErrorCode::kValidation, "Profile gamma mode is unsupported",
                      {{"mode", std::string(mode)}});
}

[[nodiscard]] Result<void> validate_params(const ProfileGammaParams &params)
{
    auto mode = parse_mode(params.mode);
    if (!mode)
    {
        return mode.error();
    }
    const std::array<std::tuple<double, double, double, std::string_view>, 6> fields{
        {{params.linear, kProfileGammaLinearMin, kProfileGammaLinearMax, "linear"},
         {params.gamma, kProfileGammaGammaMin, kProfileGammaGammaMax, "gamma"},
         {params.dynamic_range, kProfileGammaDynamicRangeMin, kProfileGammaDynamicRangeMax,
          "dynamic_range"},
         {params.grey_point, kProfileGammaGreyPointMin, kProfileGammaGreyPointMax, "grey_point"},
         {params.shadows_range, kProfileGammaShadowsRangeMin, kProfileGammaShadowsRangeMax,
          "shadows_range"},
         {params.security_factor, kProfileGammaSecurityFactorMin, kProfileGammaSecurityFactorMax,
          "security_factor"}}};
    for (const auto &[value, minimum, maximum, name] : fields)
    {
        auto valid = validate_bounded(value, minimum, maximum, name);
        if (!valid)
        {
            return valid.error();
        }
    }
    return {};
}

[[nodiscard]] Result<void> check_cancelled(const CancellationToken &cancellation)
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    return {};
}

[[nodiscard]] Result<void> validate_profiled_input(const ProfiledColorBuffer &input,
                                                   const CancellationToken &cancellation)
{
    const std::uint64_t pixels = static_cast<std::uint64_t>(input.width) * input.height;
    if (input.width == 0 || input.height == 0 ||
        pixels > std::numeric_limits<std::size_t>::max() / 3U ||
        input.channels.size() != static_cast<std::size_t>(pixels * 3U))
    {
        return make_error(ErrorCode::kValidation,
                          "Profile gamma input buffer does not match its dimensions");
    }
    if (input.color_profile.model != ColorModel::kRgb)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Profile gamma requires an RGB source colour model");
    }
    for (std::uint32_t row = 0; row < input.height; ++row)
    {
        auto active = check_cancelled(cancellation);
        if (!active)
        {
            return active.error();
        }
        const std::size_t begin = static_cast<std::size_t>(row) * input.width * 3U;
        const std::size_t end = begin + static_cast<std::size_t>(input.width) * 3U;
        for (std::size_t index = begin; index < end; ++index)
        {
            if (!std::isfinite(input.channels[index]))
            {
                return make_error(ErrorCode::kValidation,
                                  "Profile gamma input contains a non-finite sample",
                                  {{"sample_index", std::to_string(index)}});
            }
        }
    }
    return {};
}

[[nodiscard]] Result<void> validate_derived(const ProfileGammaDerived &derived)
{
    if (derived.mode == ProfileGammaRenderMode::kLogarithmic)
    {
        if (!std::isfinite(derived.dynamic_range) || derived.dynamic_range <= 0.0F ||
            !std::isfinite(derived.grey) || derived.grey <= 0.0F ||
            !std::isfinite(derived.shadows_range))
        {
            return make_error(ErrorCode::kValidation,
                              "Profile gamma logarithmic constants are invalid");
        }
        return {};
    }
    if (derived.table.size() != kProfileGammaLutEntries ||
        !std::all_of(derived.table.begin(), derived.table.end(),
                     [](const float value) { return std::isfinite(value); }) ||
        !std::all_of(derived.unbounded_coefficients.begin(), derived.unbounded_coefficients.end(),
                     [](const float value) { return std::isfinite(value); }))
    {
        return make_error(ErrorCode::kValidation, "Profile gamma LUT or extrapolation is invalid");
    }
    return {};
}

[[nodiscard]] Result<void> estimate_unbounded(ProfileGammaDerived &derived,
                                              const CancellationToken &cancellation)
{
    const std::array<float, 4> x{0.7F, 0.8F, 0.9F, 1.0F};
    std::array<float, 4> y{};
    for (std::size_t index = 0; index < x.size(); ++index)
    {
        const auto lut_index =
            std::clamp(static_cast<int>(x[index] * static_cast<float>(kProfileGammaLutEntries)), 0,
                       static_cast<int>(kProfileGammaLutEntries - 1U));
        y[index] = derived.table[static_cast<std::size_t>(lut_index)];
    }

    const float x0 = x.back();
    const float y0 = y.back();
    if (!std::isfinite(x0) || !std::isfinite(y0) || x0 <= 0.0F || y0 <= 0.0F)
    {
        return make_error(ErrorCode::kValidation, "Profile gamma extrapolation anchor is invalid");
    }
    float exponent = 0.0F;
    int count = 0;
    for (std::size_t index = 0; index + 1U < x.size(); ++index)
    {
        auto active = check_cancelled(cancellation);
        if (!active)
        {
            return active.error();
        }
        const float yy = y[index] / y0;
        const float xx = x[index] / x0;
        if (yy > 0.0F && xx > 0.0F)
        {
            const float term = std::log(y[index] / y0) / std::log(x[index] / x0);
            if (!std::isfinite(term))
            {
                return make_error(ErrorCode::kValidation,
                                  "Profile gamma extrapolation fit is non-finite");
            }
            exponent += term;
            ++count;
        }
    }
    if (count != 0)
    {
        exponent *= 1.0F / static_cast<float>(count);
    }
    else
    {
        exponent = 1.0F;
    }
    derived.unbounded_coefficients = {1.0F / x0, y0, exponent};
    return validate_derived(derived);
}

[[nodiscard]] Result<ProfileGammaDerived>
derive_profile_gamma_impl(const ProfileGammaParams &params, const CancellationToken &cancellation)
{
    auto valid = validate_params(params);
    if (!valid)
    {
        return valid.error();
    }
    auto active = check_cancelled(cancellation);
    if (!active)
    {
        return active.error();
    }
    auto mode = parse_mode(params.mode);
    if (!mode)
    {
        return mode.error();
    }

    ProfileGammaDerived result;
    result.mode = mode.value();
    if (result.mode == ProfileGammaRenderMode::kLogarithmic)
    {
        result.dynamic_range = static_cast<float>(params.dynamic_range);
        result.grey = static_cast<float>(params.grey_point) / 100.0F;
        result.shadows_range = static_cast<float>(params.shadows_range);
        auto derived = validate_derived(result);
        if (!derived)
        {
            return derived.error();
        }
        return result;
    }

    const float linear = static_cast<float>(params.linear);
    const float gamma = static_cast<float>(params.gamma);
    if (!std::isfinite(linear) || !std::isfinite(gamma))
    {
        return make_error(ErrorCode::kValidation, "Profile gamma parameters are non-finite");
    }
    result.table.resize(kProfileGammaLutEntries);
    if (gamma == 1.0F)
    {
        for (std::size_t index = 0; index < result.table.size(); ++index)
        {
            if ((index & 0xffU) == 0U)
            {
                active = check_cancelled(cancellation);
                if (!active)
                {
                    return active.error();
                }
            }
            result.table[index] =
                static_cast<float>(index) / static_cast<float>(kProfileGammaLutEntries);
        }
    }
    else if (linear == 0.0F)
    {
        for (std::size_t index = 0; index < result.table.size(); ++index)
        {
            if ((index & 0xffU) == 0U)
            {
                active = check_cancelled(cancellation);
                if (!active)
                {
                    return active.error();
                }
            }
            const float sample =
                static_cast<float>(index) / static_cast<float>(kProfileGammaLutEntries);
            result.table[index] = std::pow(sample, gamma);
        }
    }
    else
    {
        float a = 0.0F;
        float b = 0.0F;
        float c = 1.0F;
        float exponent = 0.0F;
        if (linear < 1.0F)
        {
            // The two source expressions use double literals (1.0) before their
            // values are stored as floats. Preserve that conversion boundary.
            exponent = static_cast<float>(
                static_cast<double>(gamma) * (1.0 - static_cast<double>(linear)) /
                (1.0 - static_cast<double>(gamma) * static_cast<double>(linear)));
            a = static_cast<float>(
                1.0 / (1.0 + static_cast<double>(linear) * (static_cast<double>(exponent) - 1.0)));
            b = linear * (exponent - 1.0F) * a;
            c = std::pow(a * linear + b, exponent) / linear;
        }
        if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c) || !std::isfinite(exponent))
        {
            return make_error(ErrorCode::kValidation,
                              "Profile gamma piecewise constants are non-finite");
        }
        const float breakpoint = static_cast<float>(kProfileGammaLutEntries) * linear;
        for (std::size_t index = 0; index < result.table.size(); ++index)
        {
            if ((index & 0xffU) == 0U)
            {
                active = check_cancelled(cancellation);
                if (!active)
                {
                    return active.error();
                }
            }
            const float sample =
                static_cast<float>(index) / static_cast<float>(kProfileGammaLutEntries);
            result.table[index] = static_cast<float>(index) < breakpoint ?
                                      c * sample :
                                      std::pow(a * sample + b, exponent);
        }
    }

    auto extrapolation = estimate_unbounded(result, cancellation);
    if (!extrapolation)
    {
        return extrapolation.error();
    }
    return result;
}

[[nodiscard]] float gamma_sample(const ProfileGammaDerived &derived, const float input) noexcept
{
    if (input < 1.0F)
    {
        if (input <= 0.0F)
        {
            return derived.table.front();
        }
        const auto index = static_cast<std::size_t>(
            std::clamp(static_cast<int>(input * static_cast<float>(kProfileGammaLutEntries)), 0,
                       static_cast<int>(kProfileGammaLutEntries - 1U)));
        return derived.table[index];
    }
    return derived.unbounded_coefficients[1] *
           std::pow(input * derived.unbounded_coefficients[0], derived.unbounded_coefficients[2]);
}

[[nodiscard]] Result<ProfiledColorBuffer>
apply_profile_gamma_impl(const ProfiledColorBuffer &input, const ProfileGammaParams &params,
                         const CancellationToken &cancellation)
{
    auto valid = validate_profiled_input(input, cancellation);
    if (!valid)
    {
        return valid.error();
    }
    auto derived = derive_profile_gamma(params, cancellation);
    if (!derived)
    {
        return derived.error();
    }

    ProfiledColorBuffer output;
    output.width = input.width;
    output.height = input.height;
    output.color_profile = input.color_profile;
    output.canonical_roi_scale = input.canonical_roi_scale;
    output.channels.resize(input.channels.size());
    for (std::uint32_t row = 0; row < input.height; ++row)
    {
        auto active = check_cancelled(cancellation);
        if (!active)
        {
            return active.error();
        }
        const std::size_t begin = static_cast<std::size_t>(row) * input.width * 3U;
        const std::size_t end = begin + static_cast<std::size_t>(input.width) * 3U;
        for (std::size_t index = begin; index < end; ++index)
        {
            const float source = input.channels[index];
            float transformed = 0.0F;
            if (derived.value().mode == ProfileGammaRenderMode::kLogarithmic)
            {
                float normalized = source / derived.value().grey;
                if (normalized < kProfileGammaNoiseFloor)
                {
                    normalized = kProfileGammaNoiseFloor;
                }
                if (!std::isfinite(normalized))
                {
                    return make_error(ErrorCode::kValidation,
                                      "Profile gamma logarithmic normalization is non-finite",
                                      {{"sample_index", std::to_string(index)}});
                }
                normalized = (profile_gamma_fastlog2(normalized) - derived.value().shadows_range) /
                             derived.value().dynamic_range;
                transformed =
                    normalized < kProfileGammaNoiseFloor ? kProfileGammaNoiseFloor : normalized;
            }
            else
            {
                transformed = gamma_sample(derived.value(), source);
            }
            if (!std::isfinite(transformed))
            {
                return make_error(ErrorCode::kValidation,
                                  "Profile gamma produced a non-finite sample",
                                  {{"sample_index", std::to_string(index)}});
            }
            output.channels[index] = transformed;
        }
    }
    return output;
}

} // namespace

float profile_gamma_fastlog2(const float value) noexcept
{
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const float mantissa = std::bit_cast<float>((bits & 0x007fffffU) | 0x3f000000U);
    float exponent = static_cast<float>(bits);
    exponent *= 1.1920928955078125e-7F;
    return exponent - 124.22551499F - 1.498030302F * mantissa -
           1.72587999F / (0.3520887068F + mantissa);
}

Result<ProfileGammaDerived> derive_profile_gamma(const ProfileGammaParams &params,
                                                 const CancellationToken &cancellation)
try
{
    return derive_profile_gamma_impl(params, cancellation);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Profile gamma LUT allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<std::optional<ProfileGammaParams>> resolve_profile_gamma(const Recipe &recipe)
try
{
    std::optional<ProfileGammaParams> resolved;
    for (const auto &operation : recipe.operations)
    {
        if (!operation.enabled || operation.id != kProfileGammaOperationId)
        {
            continue;
        }
        if (resolved)
        {
            return make_error(ErrorCode::kConflict,
                              "Recipe contains more than one profile gamma operation");
        }
        auto params = profile_gamma_from_parameters(operation.parameters);
        if (!params)
        {
            return params.error();
        }
        resolved = std::move(params).value();
    }
    return resolved;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Profile gamma parameter allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<ProfiledColorBuffer> apply_profile_gamma(const ProfiledColorBuffer &input,
                                                const ProfileGammaParams &params,
                                                const CancellationToken &cancellation)
try
{
    return apply_profile_gamma_impl(input, params, cancellation);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Profile gamma output allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<ProfiledColorBuffer> apply_profile_gamma(const ProfiledColorBuffer &input,
                                                const OperationInstance &operation,
                                                const CancellationToken &cancellation)
try
{
    if (operation.id != kProfileGammaOperationId)
    {
        return make_error(ErrorCode::kValidation, "Operation is not profile gamma",
                          {{"operation_id", operation.id}});
    }
    if (operation.schema_version != kProfileGammaOperationSchemaVersion)
    {
        return make_error(ErrorCode::kValidation, "Profile gamma schema version is unsupported",
                          {{"schema_version", std::to_string(operation.schema_version)}});
    }
    if (operation.mask_id.has_value())
    {
        return make_error(ErrorCode::kUnsupported, "Profile gamma does not support masks");
    }
    if (!operation.enabled)
    {
        return input;
    }
    auto params = profile_gamma_from_parameters(operation.parameters);
    if (!params)
    {
        return params.error();
    }
    return apply_profile_gamma_impl(input, params.value(), cancellation);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Profile gamma operation allocation failed",
                      {{"reason", "allocation_failed"}});
}

} // namespace ravo
