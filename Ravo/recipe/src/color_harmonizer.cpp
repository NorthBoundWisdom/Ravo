#include "ravo/recipe/color_harmonizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace ravo
{
namespace
{

constexpr std::array<std::string_view, 17> kParameterNames{
    "working_space",     "algorithm",         "rule",
    "anchor_hue",        "pull_strength",     "neutral_protection",
    "pull_width",        "custom_hue_0",      "custom_hue_1",
    "custom_hue_2",      "custom_hue_3",      "num_custom_nodes",
    "node_saturation_0", "node_saturation_1", "node_saturation_2",
    "node_saturation_3", "smoothing"};

constexpr std::array<std::string_view, 10> kRuleNames{"monochromatic",
                                                      "analogous",
                                                      "analogous_complementary",
                                                      "complementary",
                                                      "split_complementary",
                                                      "dyad",
                                                      "triad",
                                                      "tetrad",
                                                      "square",
                                                      "custom"};

static_assert(kRuleNames.size() == kColorHarmonizerRuleCount);
static_assert(kColorHarmonizerPredefinedNodeCounts.size() + 1U == kColorHarmonizerRuleCount);

[[nodiscard]] TaskError invalid_parameter(const std::string_view message,
                                          const std::string_view parameter = {})
{
    std::map<std::string, std::string, std::less<>> context{
        {"reason", "invalid_colorharmonizer_parameters"}};
    if (!parameter.empty())
    {
        context.emplace("parameter", parameter);
    }
    return make_error(ErrorCode::kValidation, std::string(message), std::move(context));
}

[[nodiscard]] Result<const ParameterValue *>
required(const std::map<std::string, ParameterValue, std::less<>> &parameters,
         const std::string_view name)
{
    const auto found = parameters.find(std::string(name));
    if (found == parameters.end())
    {
        return invalid_parameter("Color Harmonizer parameter is required", name);
    }
    return &found->second;
}

[[nodiscard]] Result<std::string>
text(const std::map<std::string, ParameterValue, std::less<>> &parameters,
     const std::string_view name)
{
    auto value = required(parameters, name);
    if (!value)
    {
        return value.error();
    }
    const auto *parsed = std::get_if<std::string>(&value.value()->value);
    if (parsed == nullptr)
    {
        return invalid_parameter("Color Harmonizer parameter must be a string", name);
    }
    return *parsed;
}

[[nodiscard]] Result<double>
number(const std::map<std::string, ParameterValue, std::less<>> &parameters,
       const std::string_view name, const double minimum, const double maximum)
{
    auto value = required(parameters, name);
    if (!value)
    {
        return value.error();
    }
    double parsed = std::numeric_limits<double>::quiet_NaN();
    if (const auto *floating = std::get_if<double>(&value.value()->value); floating != nullptr)
    {
        parsed = *floating;
    }
    else if (const auto *integer = std::get_if<std::int64_t>(&value.value()->value);
             integer != nullptr)
    {
        parsed = static_cast<double>(*integer);
    }
    const float narrowed = static_cast<float>(parsed);
    if (!std::isfinite(parsed) || !std::isfinite(narrowed))
    {
        return invalid_parameter(
            "Color Harmonizer parameter must be finite and representable as float", name);
    }
    if (parsed < minimum || parsed > maximum)
    {
        return invalid_parameter("Color Harmonizer parameter is outside its supported range", name);
    }
    return parsed;
}

[[nodiscard]] Result<std::int64_t>
integer(const std::map<std::string, ParameterValue, std::less<>> &parameters,
        const std::string_view name, const std::int64_t minimum, const std::int64_t maximum)
{
    auto value = required(parameters, name);
    if (!value)
    {
        return value.error();
    }
    const auto *parsed = std::get_if<std::int64_t>(&value.value()->value);
    if (parsed == nullptr)
    {
        return invalid_parameter("Color Harmonizer parameter must be an integer", name);
    }
    if (*parsed < minimum || *parsed > maximum)
    {
        return invalid_parameter("Color Harmonizer parameter is outside its supported range", name);
    }
    return *parsed;
}

[[nodiscard]] Result<ColorHarmonizerRule> rule_from_name(const std::string_view name)
{
    const auto found = std::find(kRuleNames.begin(), kRuleNames.end(), name);
    if (found == kRuleNames.end())
    {
        return invalid_parameter("Color Harmonizer rule is unsupported", "rule");
    }
    return static_cast<ColorHarmonizerRule>(std::distance(kRuleNames.begin(), found));
}

[[nodiscard]] Result<std::string_view> name_from_rule(const ColorHarmonizerRule rule)
{
    const auto index = static_cast<std::size_t>(rule);
    if (index >= kRuleNames.size())
    {
        return invalid_parameter("Color Harmonizer rule is unsupported", "rule");
    }
    return kRuleNames[index];
}

} // namespace

Result<ColorHarmonizerParams> color_harmonizer_from_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    for (const auto &[name, ignored] : parameters)
    {
        static_cast<void>(ignored);
        if (std::find(kParameterNames.begin(), kParameterNames.end(), name) ==
            kParameterNames.end())
        {
            return invalid_parameter("Color Harmonizer parameter is unknown", name);
        }
    }
    if (parameters.size() != kParameterNames.size())
    {
        return invalid_parameter("Color Harmonizer parameters must contain exactly 17 fields");
    }

    auto working_space = text(parameters, "working_space");
    auto algorithm = text(parameters, "algorithm");
    auto rule_name = text(parameters, "rule");
    if (!working_space || !algorithm || !rule_name)
    {
        return !working_space ? working_space.error() :
               !algorithm     ? algorithm.error() :
                                rule_name.error();
    }
    if (working_space.value() != kColorHarmonizerWorkingSpace)
    {
        return invalid_parameter("Color Harmonizer working space is unsupported", "working_space");
    }
    if (algorithm.value() != kColorHarmonizerAlgorithm)
    {
        return invalid_parameter("Color Harmonizer algorithm is unsupported", "algorithm");
    }
    auto rule = rule_from_name(rule_name.value());
    auto anchor = number(parameters, "anchor_hue", kColorHarmonizerHueMin, kColorHarmonizerHueMax);
    auto strength = number(parameters, "pull_strength", kColorHarmonizerPullStrengthMin,
                           kColorHarmonizerPullStrengthMax);
    auto neutral = number(parameters, "neutral_protection", kColorHarmonizerNeutralProtectionMin,
                          kColorHarmonizerNeutralProtectionMax);
    auto width = number(parameters, "pull_width", kColorHarmonizerPullWidthMin,
                        kColorHarmonizerPullWidthMax);
    std::array<Result<double>, 4> custom{
        number(parameters, "custom_hue_0", kColorHarmonizerHueMin, kColorHarmonizerHueMax),
        number(parameters, "custom_hue_1", kColorHarmonizerHueMin, kColorHarmonizerHueMax),
        number(parameters, "custom_hue_2", kColorHarmonizerHueMin, kColorHarmonizerHueMax),
        number(parameters, "custom_hue_3", kColorHarmonizerHueMin, kColorHarmonizerHueMax)};
    auto custom_nodes = integer(parameters, "num_custom_nodes", kColorHarmonizerCustomNodesMin,
                                kColorHarmonizerCustomNodesMax);
    std::array<Result<double>, 4> saturation{
        number(parameters, "node_saturation_0", kColorHarmonizerNodeSaturationMin,
               kColorHarmonizerNodeSaturationMax),
        number(parameters, "node_saturation_1", kColorHarmonizerNodeSaturationMin,
               kColorHarmonizerNodeSaturationMax),
        number(parameters, "node_saturation_2", kColorHarmonizerNodeSaturationMin,
               kColorHarmonizerNodeSaturationMax),
        number(parameters, "node_saturation_3", kColorHarmonizerNodeSaturationMin,
               kColorHarmonizerNodeSaturationMax)};
    auto smoothing =
        number(parameters, "smoothing", kColorHarmonizerSmoothingMin, kColorHarmonizerSmoothingMax);

    if (!rule || !anchor || !strength || !neutral || !width || !custom_nodes || !smoothing)
    {
        return !rule         ? rule.error() :
               !anchor       ? anchor.error() :
               !strength     ? strength.error() :
               !neutral      ? neutral.error() :
               !width        ? width.error() :
               !custom_nodes ? custom_nodes.error() :
                               smoothing.error();
    }
    for (std::size_t index = 0U; index < custom.size(); ++index)
    {
        if (!custom[index])
        {
            return custom[index].error();
        }
        if (!saturation[index])
        {
            return saturation[index].error();
        }
    }

    return ColorHarmonizerParams{
        rule.value(),
        anchor.value(),
        strength.value(),
        neutral.value(),
        width.value(),
        {custom[0].value(), custom[1].value(), custom[2].value(), custom[3].value()},
        custom_nodes.value(),
        {saturation[0].value(), saturation[1].value(), saturation[2].value(),
         saturation[3].value()},
        smoothing.value()};
}

Result<std::map<std::string, ParameterValue, std::less<>>>
color_harmonizer_to_parameters(const ColorHarmonizerParams &params)
{
    auto rule = name_from_rule(params.rule);
    if (!rule)
    {
        return rule.error();
    }
    std::map<std::string, ParameterValue, std::less<>> parameters{
        {"working_space", ParameterValue{std::string(kColorHarmonizerWorkingSpace)}},
        {"algorithm", ParameterValue{std::string(kColorHarmonizerAlgorithm)}},
        {"rule", ParameterValue{std::string(rule.value())}},
        {"anchor_hue", ParameterValue{params.anchor_hue}},
        {"pull_strength", ParameterValue{params.pull_strength}},
        {"neutral_protection", ParameterValue{params.neutral_protection}},
        {"pull_width", ParameterValue{params.pull_width}},
        {"custom_hue_0", ParameterValue{params.custom_hue[0]}},
        {"custom_hue_1", ParameterValue{params.custom_hue[1]}},
        {"custom_hue_2", ParameterValue{params.custom_hue[2]}},
        {"custom_hue_3", ParameterValue{params.custom_hue[3]}},
        {"num_custom_nodes", ParameterValue{params.num_custom_nodes}},
        {"node_saturation_0", ParameterValue{params.node_saturation[0]}},
        {"node_saturation_1", ParameterValue{params.node_saturation[1]}},
        {"node_saturation_2", ParameterValue{params.node_saturation[2]}},
        {"node_saturation_3", ParameterValue{params.node_saturation[3]}},
        {"smoothing", ParameterValue{params.smoothing}},
    };
    auto validated = color_harmonizer_from_parameters(parameters);
    if (!validated)
    {
        return validated.error();
    }
    return parameters;
}

Result<void> validate_color_harmonizer_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    auto parsed = color_harmonizer_from_parameters(parameters);
    if (!parsed)
    {
        return parsed.error();
    }
    return {};
}

std::string_view color_harmonizer_rule_name(const ColorHarmonizerRule rule) noexcept
{
    const auto index = static_cast<std::size_t>(rule);
    if (index >= kRuleNames.size())
    {
        return {};
    }
    return kRuleNames[index];
}

std::int64_t color_harmonizer_rule_index(const ColorHarmonizerRule rule) noexcept
{
    return static_cast<std::int64_t>(rule);
}

Result<ColorHarmonizerRule> color_harmonizer_rule_from_index(const std::int64_t index)
{
    if (index < 0 || index >= static_cast<std::int64_t>(kRuleNames.size()))
    {
        return invalid_parameter("Color Harmonizer rule is unsupported", "rule");
    }
    return static_cast<ColorHarmonizerRule>(index);
}

Result<double> color_harmonizer_hue_degrees_to_turns(const double degrees)
{
    if (!std::isfinite(degrees) || !std::isfinite(static_cast<float>(degrees)))
    {
        return invalid_parameter(
            "Color Harmonizer hue degrees must be finite and representable as float", "anchor_hue");
    }
    if (degrees < kColorHarmonizerHueDegreesMin || degrees > kColorHarmonizerHueDegreesMax)
    {
        return invalid_parameter("Color Harmonizer hue degrees are outside 0..360", "anchor_hue");
    }
    const double turns = degrees / kColorHarmonizerHueDegreesMax;
    const float narrowed = static_cast<float>(turns);
    if (!std::isfinite(turns) || !std::isfinite(narrowed) || turns < kColorHarmonizerHueMin ||
        turns > kColorHarmonizerHueMax)
    {
        return invalid_parameter(
            "Color Harmonizer hue degrees cannot produce a valid canonical turn", "anchor_hue");
    }
    return turns;
}

double color_harmonizer_hue_turns_to_degrees(const double turns) noexcept
{
    return turns * kColorHarmonizerHueDegreesMax;
}

std::int64_t color_harmonizer_active_node_count(const ColorHarmonizerParams &params) noexcept
{
    if (params.rule == ColorHarmonizerRule::kCustom)
    {
        return params.num_custom_nodes;
    }
    const auto index = static_cast<std::size_t>(params.rule);
    if (index >= kColorHarmonizerPredefinedNodeCounts.size())
    {
        return 0;
    }
    return kColorHarmonizerPredefinedNodeCounts[index];
}

bool color_harmonizer_uses_anchor_hue(const ColorHarmonizerRule rule) noexcept
{
    return rule != ColorHarmonizerRule::kCustom;
}

bool color_harmonizer_uses_custom_hue(const ColorHarmonizerParams &params,
                                      const std::size_t index) noexcept
{
    return params.rule == ColorHarmonizerRule::kCustom &&
           color_harmonizer_uses_node_saturation(params, index);
}

bool color_harmonizer_uses_node_saturation(const ColorHarmonizerParams &params,
                                           const std::size_t index) noexcept
{
    const auto active = color_harmonizer_active_node_count(params);
    return active > 0 && index < kColorHarmonizerNodeSlotCount &&
           index < static_cast<std::size_t>(active);
}

} // namespace ravo
