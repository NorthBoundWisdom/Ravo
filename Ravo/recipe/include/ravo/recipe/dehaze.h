#pragma once

#include <cstdint>
#include <map>
#include <string_view>

#include "ravo/foundation/error.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr std::string_view kDehazeOperationId = "ravo.effect.dehaze";
inline constexpr std::int64_t kDehazeOperationSchemaVersion = 2;
inline constexpr std::string_view kDehazeWorkingSpaceSourceLinearRgb = "source_linear_rgb";
inline constexpr std::string_view kDehazeAlgorithmDarkChannelGuidedV4 = "dark_channel_guided_v4";
inline constexpr double kDehazeStrengthMin = -1.0;
inline constexpr double kDehazeStrengthMax = 1.0;
inline constexpr double kDehazeDistanceMin = 0.0;
inline constexpr double kDehazeDistanceMax = 1.0;

struct DehazeParams
{
    double strength = 0.2;
    double distance = 0.2;
    bool adaptive = true;

    [[nodiscard]] bool operator==(const DehazeParams &) const noexcept = default;
};

[[nodiscard]] Result<DehazeParams>
dehaze_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<std::map<std::string, ParameterValue, std::less<>>>
dehaze_to_parameters(const DehazeParams &params);
[[nodiscard]] Result<void>
validate_dehaze_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<void> upgrade_dehaze_operation(OperationInstance &operation);

} // namespace ravo
