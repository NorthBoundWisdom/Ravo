#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string_view>

#include "ravo/foundation/error.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr std::string_view kColorHarmonizerOperationId = "ravo.color.colorharmonizer";
inline constexpr std::int64_t kColorHarmonizerOperationSchemaVersion = 1;
inline constexpr std::string_view kColorHarmonizerWorkingSpace = "profile_linear_rgb_d50";
inline constexpr std::string_view kColorHarmonizerAlgorithm = "dt_ucs_harmony_v1";
inline constexpr double kColorHarmonizerHueMin = 0.0;
inline constexpr double kColorHarmonizerHueMax = 1.0;
inline constexpr double kColorHarmonizerPullStrengthMin = 0.0;
inline constexpr double kColorHarmonizerPullStrengthMax = 1.0;
inline constexpr double kColorHarmonizerNeutralProtectionMin = 0.0;
inline constexpr double kColorHarmonizerNeutralProtectionMax = 1.0;
inline constexpr double kColorHarmonizerPullWidthMin = 0.25;
inline constexpr double kColorHarmonizerPullWidthMax = 4.0;
inline constexpr std::int64_t kColorHarmonizerCustomNodesMin = 2;
inline constexpr std::int64_t kColorHarmonizerCustomNodesMax = 4;
inline constexpr double kColorHarmonizerNodeSaturationMin = 0.0;
inline constexpr double kColorHarmonizerNodeSaturationMax = 2.0;
inline constexpr double kColorHarmonizerSmoothingMin = 0.0;
inline constexpr double kColorHarmonizerSmoothingMax = 2.0;
inline constexpr double kColorHarmonizerHueDegreesMin = 0.0;
inline constexpr double kColorHarmonizerHueDegreesMax = 360.0;
inline constexpr std::size_t kColorHarmonizerRuleCount = 10;
inline constexpr std::size_t kColorHarmonizerNodeSlotCount = 4;
// Canonical active-node counts for predefined rules 0..8. Engine geometry and
// product consumers share this semantic table rather than copying its arity.
inline constexpr std::array<std::int64_t, 9> kColorHarmonizerPredefinedNodeCounts{1, 3, 4, 2, 3,
                                                                                  2, 3, 4, 4};

enum class ColorHarmonizerRule : std::uint8_t
{
    kMonochromatic = 0U,
    kAnalogous,
    kAnalogousComplementary,
    kComplementary,
    kSplitComplementary,
    kDyad,
    kTriad,
    kTetrad,
    kSquare,
    kCustom,
};

struct ColorHarmonizerParams
{
    ColorHarmonizerRule rule = ColorHarmonizerRule::kComplementary;
    double anchor_hue = 0.1;
    double pull_strength = 0.0;
    double neutral_protection = 0.5;
    double pull_width = 1.0;
    std::array<double, 4> custom_hue{0.0, 0.25, 0.5, 0.75};
    std::int64_t num_custom_nodes = 4;
    std::array<double, 4> node_saturation{1.0, 1.0, 1.0, 1.0};
    double smoothing = 0.0;

    [[nodiscard]] bool operator==(const ColorHarmonizerParams &) const noexcept = default;
};

[[nodiscard]] Result<ColorHarmonizerParams> color_harmonizer_from_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<std::map<std::string, ParameterValue, std::less<>>>
color_harmonizer_to_parameters(const ColorHarmonizerParams &params);
[[nodiscard]] Result<void> validate_color_harmonizer_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] std::string_view color_harmonizer_rule_name(ColorHarmonizerRule rule) noexcept;
[[nodiscard]] std::int64_t color_harmonizer_rule_index(ColorHarmonizerRule rule) noexcept;
[[nodiscard]] Result<ColorHarmonizerRule> color_harmonizer_rule_from_index(std::int64_t index);
[[nodiscard]] Result<double> color_harmonizer_hue_degrees_to_turns(double degrees);
[[nodiscard]] double color_harmonizer_hue_turns_to_degrees(double turns) noexcept;
[[nodiscard]] std::int64_t
color_harmonizer_active_node_count(const ColorHarmonizerParams &params) noexcept;
[[nodiscard]] bool color_harmonizer_uses_anchor_hue(ColorHarmonizerRule rule) noexcept;
[[nodiscard]] bool color_harmonizer_uses_custom_hue(const ColorHarmonizerParams &params,
                                                    std::size_t index) noexcept;
[[nodiscard]] bool color_harmonizer_uses_node_saturation(const ColorHarmonizerParams &params,
                                                         std::size_t index) noexcept;

} // namespace ravo
