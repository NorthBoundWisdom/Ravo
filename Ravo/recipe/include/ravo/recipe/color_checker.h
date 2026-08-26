#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/foundation/error.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr std::string_view kColorCheckerOperationId = "ravo.color.colorchecker";
inline constexpr std::int64_t kColorCheckerOperationSchemaVersion = 1;
inline constexpr std::string_view kColorCheckerWorkingSpaceLabD50 = "lab_d50";
inline constexpr std::string_view kColorCheckerAlgorithmThinPlateRbfV2 = "thin_plate_rbf_v2";
inline constexpr std::size_t kColorCheckerDefaultPatchCount = 24;
inline constexpr std::size_t kColorCheckerMaxPatchCount = 49;

struct ColorCheckerPatch
{
    std::array<double, 3> source_lab{};
    std::array<double, 3> target_lab{};

    [[nodiscard]] bool operator==(const ColorCheckerPatch &) const noexcept = default;
};

struct ColorCheckerParams
{
    ColorCheckerParams();
    explicit ColorCheckerParams(std::vector<ColorCheckerPatch> patches_value);

    std::vector<ColorCheckerPatch> patches;

    [[nodiscard]] bool operator==(const ColorCheckerParams &) const noexcept = default;
};

struct ColorCheckerPresetDescriptor
{
    std::string_view id;
    std::string_view display_name;
    std::size_t patch_count = 0;
};

[[nodiscard]] std::span<const ColorCheckerPresetDescriptor> color_checker_presets() noexcept;
[[nodiscard]] Result<ColorCheckerParams>
color_checker_params_for_preset(std::string_view preset_id);
[[nodiscard]] Result<ColorCheckerParams>
color_checker_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<std::map<std::string, ParameterValue, std::less<>>>
color_checker_to_parameters(const ColorCheckerParams &params);
[[nodiscard]] Result<void> validate_color_checker_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters);

} // namespace ravo
