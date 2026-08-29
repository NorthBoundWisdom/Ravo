#pragma once

#include <cstdint>
#include <map>
#include <string_view>

#include "ravo/foundation/error.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr std::string_view kSharpenOperationId = "ravo.detail.sharpen";
inline constexpr std::int64_t kSharpenOperationSchemaVersion = 2;
inline constexpr std::string_view kSharpenWorkingSpaceLabD50 = "lab_d50";
inline constexpr std::string_view kSharpenAlgorithmSeparableGaussianUsmV1 =
    "separable_gaussian_usm_v1";

inline constexpr double kSharpenRadiusMin = 0.0;
inline constexpr double kSharpenRadiusMax = 99.0;
inline constexpr double kSharpenAmountMin = 0.0;
inline constexpr double kSharpenAmountMax = 2.0;
inline constexpr double kSharpenThresholdMin = 0.0;
inline constexpr double kSharpenThresholdMax = 100.0;

struct SharpenParams
{
    double radius = 2.0;
    double amount = 0.5;
    double threshold = 0.5;

    [[nodiscard]] bool operator==(const SharpenParams &) const noexcept = default;
};

[[nodiscard]] Result<SharpenParams>
sharpen_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<std::map<std::string, ParameterValue, std::less<>>>
sharpen_to_parameters(const SharpenParams &params);
[[nodiscard]] Result<void>
validate_sharpen_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<void> upgrade_sharpen_operation(OperationInstance &operation);

} // namespace ravo
