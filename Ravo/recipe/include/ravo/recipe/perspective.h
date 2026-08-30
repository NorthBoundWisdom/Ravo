#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "ravo/foundation/error.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr std::string_view kPerspectiveOperationId = "ravo.geometry.perspective";
inline constexpr std::int64_t kPerspectiveOperationSchemaVersion = 1;
inline constexpr std::string_view kPerspectiveWorkingSpace = "linear_rgb";
inline constexpr std::string_view kPerspectiveAlgorithm = "shift_n_homography_v1";
inline constexpr std::string_view kPerspectiveInterpolationBilinear = "bilinear";
inline constexpr std::string_view kPerspectiveInterpolationLanczos2 = "lanczos2";
inline constexpr std::string_view kPerspectiveInterpolationLanczos3 = "lanczos3";
inline constexpr double kPerspectiveRotationMin = -45.0;
inline constexpr double kPerspectiveRotationMax = 45.0;
inline constexpr double kPerspectiveShiftMin = -2.0;
inline constexpr double kPerspectiveShiftMax = 2.0;
inline constexpr double kPerspectiveShearMin = -0.5;
inline constexpr double kPerspectiveShearMax = 0.5;

struct PerspectiveParams
{
    double rotation_degrees = 0.0;
    double vertical_shift = 0.0;
    double horizontal_shift = 0.0;
    double shear = 0.0;
    bool constrain_crop = true;
    std::string interpolation{std::string(kPerspectiveInterpolationLanczos3)};

    [[nodiscard]] bool is_identity() const noexcept;
    [[nodiscard]] bool operator==(const PerspectiveParams &) const noexcept = default;
};

[[nodiscard]] bool perspective_interpolation_is_supported(std::string_view interpolation) noexcept;
[[nodiscard]] Result<PerspectiveParams>
perspective_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<std::map<std::string, ParameterValue, std::less<>>>
perspective_to_parameters(const PerspectiveParams &params);

} // namespace ravo
