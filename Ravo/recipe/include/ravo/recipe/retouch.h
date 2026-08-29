#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr std::string_view kRetouchOperationId = "ravo.repair.retouch";
inline constexpr std::int64_t kRetouchOperationSchemaVersion = 1;
inline constexpr std::string_view kRetouchWorkingSpaceLinearRec709D50 = "linear_rec709_d50";
inline constexpr std::string_view kRetouchAlgorithmOrderedWaveletV1 =
    "ordered_regions_wavelet_v1";
inline constexpr std::size_t kRetouchMaxRegions = kCanonicalMaskMaxNodes;
inline constexpr std::int64_t kRetouchMaxScales = 15;
inline constexpr std::int64_t kRetouchDefaultHealIterations = 2000;
inline constexpr std::int64_t kRetouchMaxHealIterations = 20000;
inline constexpr double kRetouchBlurRadiusMin = 0.1;
inline constexpr double kRetouchBlurRadiusMax = 200.0;

enum class RetouchMode
{
    kClone,
    kHeal,
    kBlur,
    kFill,
};

enum class RetouchBlurType
{
    kGaussian,
    kBilateral,
};

enum class RetouchFillMode
{
    kErase,
    kColor,
};

struct RetouchRegion
{
    std::string mask_id;
    RetouchMode mode = RetouchMode::kHeal;
    std::int64_t scale = 0;
    double opacity = 1.0;
    double source_x = 0.5;
    double source_y = 0.5;
    RetouchBlurType blur_type = RetouchBlurType::kGaussian;
    double blur_radius = 10.0;
    RetouchFillMode fill_mode = RetouchFillMode::kErase;
    std::array<double, 3> fill_color{};
    double fill_brightness = 0.0;

    [[nodiscard]] bool operator==(const RetouchRegion &) const noexcept = default;
};

struct RetouchParams
{
    std::int64_t num_scales = 0;
    std::int64_t merge_from_scale = 0;
    std::int64_t max_heal_iterations = kRetouchDefaultHealIterations;
    std::vector<RetouchRegion> regions;

    [[nodiscard]] bool is_identity() const noexcept;
    [[nodiscard]] bool operator==(const RetouchParams &) const noexcept = default;
};

[[nodiscard]] std::string_view retouch_mode_name(RetouchMode mode) noexcept;
[[nodiscard]] std::string_view retouch_blur_type_name(RetouchBlurType type) noexcept;
[[nodiscard]] std::string_view retouch_fill_mode_name(RetouchFillMode mode) noexcept;

[[nodiscard]] Result<RetouchParams>
retouch_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] std::map<std::string, ParameterValue, std::less<>>
retouch_to_parameters(const RetouchParams &params);
[[nodiscard]] Result<void> validate_retouch_operation(const OperationInstance &operation,
                                                      const std::vector<Mask> &masks);

} // namespace ravo
