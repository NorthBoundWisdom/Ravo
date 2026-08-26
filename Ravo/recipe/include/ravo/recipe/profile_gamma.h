#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "ravo/foundation/error.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr std::string_view kProfileGammaOperationId = "ravo.color.profilegamma";
inline constexpr std::int64_t kProfileGammaOperationSchemaVersion = 1;
inline constexpr std::string_view kProfileGammaModeLogarithmic = "logarithmic";
inline constexpr std::string_view kProfileGammaModeGamma = "gamma";
inline constexpr std::array<std::string_view, 2> kSelectableProfileGammaModes{
    kProfileGammaModeLogarithmic,
    kProfileGammaModeGamma,
};
inline constexpr double kProfileGammaLinearMin = 0.0;
inline constexpr double kProfileGammaLinearMax = 1.0;
inline constexpr double kProfileGammaLinearDefault = 0.1;
inline constexpr double kProfileGammaGammaMin = 0.0;
inline constexpr double kProfileGammaGammaMax = 1.0;
inline constexpr double kProfileGammaGammaDefault = 0.45;
inline constexpr double kProfileGammaDynamicRangeMin = 0.01;
inline constexpr double kProfileGammaDynamicRangeMax = 32.0;
inline constexpr double kProfileGammaDynamicRangeDefault = 10.0;
inline constexpr double kProfileGammaGreyPointMin = 0.1;
inline constexpr double kProfileGammaGreyPointMax = 100.0;
inline constexpr double kProfileGammaGreyPointDefault = 18.0;
inline constexpr double kProfileGammaShadowsRangeMin = -16.0;
inline constexpr double kProfileGammaShadowsRangeMax = 16.0;
inline constexpr double kProfileGammaShadowsRangeDefault = -5.0;
inline constexpr double kProfileGammaSecurityFactorMin = -100.0;
inline constexpr double kProfileGammaSecurityFactorMax = 100.0;
inline constexpr double kProfileGammaSecurityFactorDefault = 0.0;

// Frozen profile_gamma schema-v2 values, expressed in the Ravo v1 recipe contract.
struct ProfileGammaParams
{
    std::string mode{std::string(kProfileGammaModeLogarithmic)};
    double linear = kProfileGammaLinearDefault;
    double gamma = kProfileGammaGammaDefault;
    double dynamic_range = kProfileGammaDynamicRangeDefault;
    double grey_point = kProfileGammaGreyPointDefault;
    double shadows_range = kProfileGammaShadowsRangeDefault;
    double security_factor = kProfileGammaSecurityFactorDefault;

    [[nodiscard]] bool is_default() const noexcept;
    [[nodiscard]] bool operator==(const ProfileGammaParams &) const noexcept = default;
};

[[nodiscard]] Result<void> validate_profile_gamma_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<ProfileGammaParams>
profile_gamma_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<std::map<std::string, ParameterValue, std::less<>>>
profile_gamma_to_parameters(const ProfileGammaParams &params);

} // namespace ravo
