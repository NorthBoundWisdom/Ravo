#pragma once

#include <cstdint>
#include <map>
#include <string_view>

#include "ravo/foundation/error.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr std::string_view kTextureOperationId = "ravo.detail.texture";
inline constexpr std::int64_t kTextureOperationSchemaVersion = 1;
inline constexpr std::string_view kTextureWorkingSpaceLinearRec709 = "linear_rec709";
inline constexpr std::string_view kTextureAlgorithmGuidedLuminanceTwoBandV1 =
    "guided_luminance_two_band_v1";

inline constexpr double kTextureStrengthMin = -2.0;
inline constexpr double kTextureStrengthMax = 2.0;
inline constexpr double kTextureDetailThresholdMin = 0.01;
inline constexpr double kTextureDetailThresholdMax = 100.0;
inline constexpr std::int64_t kTextureIterationsMin = 1;
inline constexpr std::int64_t kTextureIterationsMax = 5;

struct TextureParams
{
    double strength = 0.0;
    double detail_threshold = 0.2;
    std::int64_t iterations = 1;

    [[nodiscard]] bool operator==(const TextureParams &) const noexcept = default;
};

[[nodiscard]] Result<TextureParams>
texture_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<std::map<std::string, ParameterValue, std::less<>>>
texture_to_parameters(const TextureParams &params);
[[nodiscard]] Result<void>
validate_texture_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);

} // namespace ravo
