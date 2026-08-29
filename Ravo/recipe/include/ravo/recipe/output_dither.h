#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string_view>

#include "ravo/foundation/error.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr std::string_view kOutputDitherOperationId = "ravo.output.dither";
inline constexpr std::int64_t kOutputDitherOperationSchemaVersion = 1;
inline constexpr std::string_view kOutputDitherWorkingSpace = "encoded_output_rgb";
inline constexpr std::string_view kOutputDitherAlgorithm = "frozen_dither_v2";
inline constexpr double kOutputDitherDampingMin = -200.0;
inline constexpr double kOutputDitherDampingMax = 0.0;
inline constexpr std::size_t kOutputDitherMethodCount = 18U;

enum class OutputDitherMethod : std::uint8_t
{
    kRandom = 0,
    kFloydSteinberg1BitGray,
    kFloydSteinberg1BitRgb,
    kFloydSteinberg2BitGray,
    kFloydSteinberg2BitRgb,
    kFloydSteinberg4BitGray,
    kFloydSteinberg4BitRgb,
    kFloydSteinberg6BitGray,
    kFloydSteinberg8BitRgb,
    kFloydSteinberg16BitRgb,
    kFloydSteinbergAuto,
    kPosterize2,
    kPosterize3,
    kPosterize4,
    kPosterize5,
    kPosterize6,
    kPosterize7,
    kPosterize8,
};

struct OutputDitherParams
{
    OutputDitherMethod method = OutputDitherMethod::kFloydSteinbergAuto;
    double random_damping_db = -100.0;

    [[nodiscard]] bool operator==(const OutputDitherParams &) const noexcept = default;
};

[[nodiscard]] std::string_view output_dither_method_name(OutputDitherMethod method) noexcept;
[[nodiscard]] Result<OutputDitherMethod> parse_output_dither_method(std::string_view name);
[[nodiscard]] Result<OutputDitherMethod> output_dither_method_from_index(std::int64_t index);
[[nodiscard]] std::int64_t output_dither_method_index(OutputDitherMethod method) noexcept;

[[nodiscard]] Result<OutputDitherParams>
output_dither_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<std::map<std::string, ParameterValue, std::less<>>>
output_dither_to_parameters(const OutputDitherParams &params);
[[nodiscard]] Result<void> validate_output_dither_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters);

} // namespace ravo
