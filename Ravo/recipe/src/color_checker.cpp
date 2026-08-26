#include "ravo/recipe/color_checker.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace ravo
{
namespace
{

constexpr std::array<std::array<float, 3>, kColorCheckerDefaultPatchCount> kDefaultPatches{{
    {37.99F, 13.56F, 14.06F},  {65.71F, 18.13F, 17.81F},  {49.93F, -4.88F, -21.93F},
    {43.14F, -13.10F, 21.91F}, {55.11F, 8.84F, -25.40F},  {70.72F, -33.40F, -0.20F},
    {62.66F, 36.07F, 57.10F},  {40.02F, 10.41F, -45.96F}, {51.12F, 48.24F, 16.25F},
    {30.33F, 22.98F, -21.59F}, {72.53F, -23.71F, 57.26F}, {71.94F, 19.36F, 67.86F},
    {28.78F, 14.18F, -50.30F}, {55.26F, -38.34F, 31.37F}, {42.10F, 53.38F, 28.19F},
    {81.73F, 4.04F, 79.82F},   {51.94F, 49.99F, -14.57F}, {51.04F, -28.63F, -28.64F},
    {96.54F, -0.43F, 1.19F},   {81.26F, -0.64F, -0.34F},  {66.77F, -0.73F, -0.50F},
    {50.87F, -0.15F, -0.27F},  {35.66F, -0.42F, -1.23F},  {20.46F, -0.08F, -0.97F},
}};

constexpr std::array<ColorCheckerPresetDescriptor, 8> kPresetDescriptors{{
    {"it8_skin_tones", "IT8 skin tones", 24},
    {"expanded_color_checker", "Expanded color checker", 49},
    {"helmholtz_kohlrausch_monochrome", "Helmholtz/Kohlrausch monochrome", 24},
    {"fuji_astia", "Fuji Astia emulation", 49},
    {"fuji_classic_chrome", "Fuji Classic Chrome emulation", 49},
    {"fuji_monochrome", "Fuji Monochrome emulation", 49},
    {"fuji_provia", "Fuji Provia emulation", 49},
    {"fuji_velvia", "Fuji Velvia emulation", 49},
}};

#include "color_checker_presets.inc"

[[nodiscard]] std::vector<ColorCheckerPatch> make_default_patches()
{
    std::vector<ColorCheckerPatch> patches;
    patches.reserve(kDefaultPatches.size());
    for (const auto &lab : kDefaultPatches)
    {
        const std::array<double, 3> value{static_cast<double>(lab[0]), static_cast<double>(lab[1]),
                                          static_cast<double>(lab[2])};
        patches.push_back({value, value});
    }
    return patches;
}

[[nodiscard]] ColorCheckerParams
params_from_frozen_v2_words(const std::array<std::uint32_t, 295> &words,
                            const std::size_t patch_count)
{
    constexpr std::size_t stride = kColorCheckerMaxPatchCount;
    std::vector<ColorCheckerPatch> patches;
    patches.reserve(patch_count);
    for (std::size_t patch = 0U; patch < patch_count; ++patch)
    {
        const auto component = [&](const std::size_t plane)
        { return static_cast<double>(std::bit_cast<float>(words[plane * stride + patch])); };
        patches.push_back({{{component(0U), component(1U), component(2U)}},
                           {{component(3U), component(4U), component(5U)}}});
    }
    return ColorCheckerParams{std::move(patches)};
}

[[nodiscard]] Result<double> parse_component(const ParameterValue &value,
                                             const std::size_t patch_index,
                                             const std::string_view field,
                                             const std::size_t component_index)
{
    double parsed = std::numeric_limits<double>::quiet_NaN();
    if (const auto *floating = std::get_if<double>(&value.value); floating != nullptr)
    {
        parsed = *floating;
    }
    else if (const auto *integer = std::get_if<std::int64_t>(&value.value); integer != nullptr)
    {
        parsed = static_cast<double>(*integer);
    }
    const float narrowed = static_cast<float>(parsed);
    if (!std::isfinite(parsed) || !std::isfinite(narrowed))
    {
        return make_error(ErrorCode::kValidation,
                          "Color checker Lab component must be finite and representable as float",
                          {{"patch_index", std::to_string(patch_index)},
                           {"field", std::string(field)},
                           {"component_index", std::to_string(component_index)}});
    }
    return parsed;
}

[[nodiscard]] Result<std::array<double, 3>>
parse_lab(const ParameterValue &value, const std::size_t patch_index, const std::string_view field)
{
    const auto *array = std::get_if<ParameterValue::Array>(&value.value);
    if (array == nullptr || array->size() != 3U)
    {
        return make_error(
            ErrorCode::kValidation, "Color checker Lab value must be an array of exactly 3 numbers",
            {{"patch_index", std::to_string(patch_index)}, {"field", std::string(field)}});
    }
    std::array<double, 3> result{};
    for (std::size_t component = 0; component < result.size(); ++component)
    {
        auto parsed = parse_component((*array)[component], patch_index, field, component);
        if (!parsed)
        {
            return parsed.error();
        }
        result[component] = parsed.value();
    }
    return result;
}

[[nodiscard]] ParameterValue lab_to_parameter(const std::array<double, 3> &lab)
{
    return ParameterValue{ParameterValue::Array{ParameterValue{lab[0]}, ParameterValue{lab[1]},
                                                ParameterValue{lab[2]}}};
}

} // namespace

ColorCheckerParams::ColorCheckerParams()
    : patches(make_default_patches())
{
}

ColorCheckerParams::ColorCheckerParams(std::vector<ColorCheckerPatch> patches_value)
    : patches(std::move(patches_value))
{
}

std::span<const ColorCheckerPresetDescriptor> color_checker_presets() noexcept
{
    return kPresetDescriptors;
}

Result<ColorCheckerParams> color_checker_params_for_preset(const std::string_view preset_id)
{
    for (std::size_t index = 0U; index < kPresetDescriptors.size(); ++index)
    {
        if (kPresetDescriptors[index].id == preset_id)
        {
            return params_from_frozen_v2_words(*kPresetWords[index],
                                               kPresetDescriptors[index].patch_count);
        }
    }
    return make_error(ErrorCode::kUnsupported, "Color checker preset is unsupported",
                      {{"preset_id", std::string(preset_id)}});
}

Result<ColorCheckerParams>
color_checker_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    if (parameters.size() != 3U)
    {
        return make_error(
            ErrorCode::kValidation,
            "Color checker parameters must contain only working_space, algorithm, and patches");
    }
    const auto text = [&](const std::string_view name) -> Result<std::string>
    {
        const auto found = parameters.find(std::string(name));
        if (found == parameters.end())
        {
            return make_error(ErrorCode::kValidation, "Color checker parameter is required",
                              {{"parameter", std::string(name)}});
        }
        const auto *value = std::get_if<std::string>(&found->second.value);
        if (value == nullptr)
        {
            return make_error(ErrorCode::kValidation, "Color checker parameter must be a string",
                              {{"parameter", std::string(name)}});
        }
        return *value;
    };
    auto working_space = text("working_space");
    if (!working_space)
    {
        return working_space.error();
    }
    if (working_space.value() != kColorCheckerWorkingSpaceLabD50)
    {
        return make_error(
            ErrorCode::kValidation, "Color checker working space is unsupported",
            {{"parameter", "working_space"}, {"working_space", working_space.value()}});
    }
    auto algorithm = text("algorithm");
    if (!algorithm)
    {
        return algorithm.error();
    }
    if (algorithm.value() != kColorCheckerAlgorithmThinPlateRbfV2)
    {
        return make_error(ErrorCode::kValidation, "Color checker algorithm is unsupported",
                          {{"parameter", "algorithm"}, {"algorithm", algorithm.value()}});
    }
    const auto found = parameters.find("patches");
    if (found == parameters.end())
    {
        return make_error(ErrorCode::kValidation, "Color checker parameter is required",
                          {{"parameter", "patches"}});
    }
    const auto *array = std::get_if<ParameterValue::Array>(&found->second.value);
    if (array == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Color checker patches must be an array",
                          {{"parameter", "patches"}});
    }
    if (array->size() > kColorCheckerMaxPatchCount)
    {
        return make_error(ErrorCode::kValidation,
                          "Color checker supports at most 49 ordered patches",
                          {{"patch_count", std::to_string(array->size())}});
    }

    std::vector<ColorCheckerPatch> patches;
    patches.reserve(array->size());
    for (std::size_t index = 0; index < array->size(); ++index)
    {
        const auto *object = std::get_if<ParameterValue::Object>(&(*array)[index].value);
        if (object == nullptr || object->size() != 2U || !object->contains("source_lab") ||
            !object->contains("target_lab"))
        {
            return make_error(ErrorCode::kValidation,
                              "Color checker patch must contain only source_lab and target_lab",
                              {{"patch_index", std::to_string(index)}});
        }
        auto source = parse_lab(object->at("source_lab"), index, "source_lab");
        if (!source)
        {
            return source.error();
        }
        auto target = parse_lab(object->at("target_lab"), index, "target_lab");
        if (!target)
        {
            return target.error();
        }
        patches.push_back({source.value(), target.value()});
    }
    return ColorCheckerParams{std::move(patches)};
}

Result<std::map<std::string, ParameterValue, std::less<>>>
color_checker_to_parameters(const ColorCheckerParams &params)
{
    ParameterValue::Array patches;
    patches.reserve(params.patches.size());
    for (const auto &patch : params.patches)
    {
        patches.emplace_back(ParameterValue::Object{
            {"source_lab", lab_to_parameter(patch.source_lab)},
            {"target_lab", lab_to_parameter(patch.target_lab)},
        });
    }
    std::map<std::string, ParameterValue, std::less<>> parameters{
        {"working_space", ParameterValue{std::string(kColorCheckerWorkingSpaceLabD50)}},
        {"algorithm", ParameterValue{std::string(kColorCheckerAlgorithmThinPlateRbfV2)}},
        {"patches", ParameterValue{std::move(patches)}},
    };
    auto valid = color_checker_from_parameters(parameters);
    if (!valid)
    {
        return valid.error();
    }
    return parameters;
}

Result<void> validate_color_checker_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    auto parsed = color_checker_from_parameters(parameters);
    if (!parsed)
    {
        return parsed.error();
    }
    return {};
}

} // namespace ravo
