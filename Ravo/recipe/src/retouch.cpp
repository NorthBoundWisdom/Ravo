#include "ravo/recipe/retouch.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <span>
#include <utility>

namespace ravo
{
namespace
{

constexpr std::array<std::string_view, 5> kOperationParameterNames{
    "working_space", "algorithm", "num_scales", "merge_from_scale", "max_heal_iterations"};
constexpr std::array<std::string_view, 11> kRegionNames{
    "mask_id",       "mode",        "scale",          "opacity",
    "source_x",      "source_y",    "blur_type",      "blur_radius",
    "fill_mode",     "fill_color",  "fill_brightness",
};

[[nodiscard]] TaskError parameter_error(const std::string_view message,
                                        const std::string_view parameter,
                                        const std::string_view reason)
{
    return make_error(ErrorCode::kValidation, std::string(message),
                      {{"operation_id", std::string(kRetouchOperationId)},
                       {"parameter", std::string(parameter)},
                       {"reason", std::string(reason)}});
}

[[nodiscard]] bool known_name(const std::string_view name,
                              const std::span<const std::string_view> names) noexcept
{
    return std::find(names.begin(), names.end(), name) != names.end();
}

[[nodiscard]] Result<const ParameterValue *> required(
    const std::map<std::string, ParameterValue, std::less<>> &parameters,
    const std::string_view name)
{
    const auto found = parameters.find(std::string(name));
    if (found == parameters.end())
    {
        return parameter_error("Retouch parameter is required", name, "missing_retouch_parameter");
    }
    return &found->second;
}

[[nodiscard]] Result<std::string> string_value(const ParameterValue &parameter,
                                               const std::string_view path)
{
    const auto *value = std::get_if<std::string>(&parameter.value);
    if (value == nullptr)
    {
        return parameter_error("Retouch parameter must be a string", path,
                               "invalid_retouch_parameter_type");
    }
    return *value;
}

[[nodiscard]] Result<double> number_value(const ParameterValue &parameter,
                                          const std::string_view path, const double minimum,
                                          const double maximum)
{
    const auto *floating = std::get_if<double>(&parameter.value);
    const auto *integer = std::get_if<std::int64_t>(&parameter.value);
    if (floating == nullptr && integer == nullptr)
    {
        return parameter_error("Retouch parameter must be numeric", path,
                               "invalid_retouch_parameter_type");
    }
    const double value = floating != nullptr ? *floating : static_cast<double>(*integer);
    if (!std::isfinite(value) || value < minimum || value > maximum)
    {
        return parameter_error("Retouch parameter is outside the permitted range", path,
                               "invalid_retouch_parameter_range");
    }
    return value;
}

[[nodiscard]] Result<std::int64_t> integer_value(const ParameterValue &parameter,
                                                 const std::string_view path,
                                                 const std::int64_t minimum,
                                                 const std::int64_t maximum)
{
    const auto *value = std::get_if<std::int64_t>(&parameter.value);
    if (value == nullptr)
    {
        return parameter_error("Retouch parameter must be an integer", path,
                               "invalid_retouch_parameter_type");
    }
    if (*value < minimum || *value > maximum)
    {
        return parameter_error("Retouch parameter is outside the permitted range", path,
                               "invalid_retouch_parameter_range");
    }
    return *value;
}

[[nodiscard]] Result<RetouchMode> parse_mode(const std::string_view value,
                                             const std::string_view path)
{
    if (value == "clone")
        return RetouchMode::kClone;
    if (value == "heal")
        return RetouchMode::kHeal;
    if (value == "blur")
        return RetouchMode::kBlur;
    if (value == "fill")
        return RetouchMode::kFill;
    return parameter_error("Retouch mode is unsupported", path, "unsupported_retouch_mode");
}

[[nodiscard]] Result<RetouchBlurType> parse_blur_type(const std::string_view value,
                                                      const std::string_view path)
{
    if (value == "gaussian")
        return RetouchBlurType::kGaussian;
    if (value == "bilateral")
        return RetouchBlurType::kBilateral;
    return parameter_error("Retouch blur type is unsupported", path,
                           "unsupported_retouch_blur_type");
}

[[nodiscard]] Result<RetouchFillMode> parse_fill_mode(const std::string_view value,
                                                      const std::string_view path)
{
    if (value == "erase")
        return RetouchFillMode::kErase;
    if (value == "color")
        return RetouchFillMode::kColor;
    return parameter_error("Retouch fill mode is unsupported", path,
                           "unsupported_retouch_fill_mode");
}

[[nodiscard]] Result<RetouchRegion> parse_region(const ParameterValue &parameter,
                                                 const std::size_t index,
                                                 const std::int64_t num_scales)
{
    const std::string prefix = "regions[" + std::to_string(index) + "]";
    const auto *object = std::get_if<ParameterValue::Object>(&parameter.value);
    if (object == nullptr)
    {
        return parameter_error("Retouch region must be an object", prefix,
                               "invalid_retouch_region_type");
    }
    for (const auto &[name, ignored] : *object)
    {
        (void)ignored;
        if (!known_name(name, kRegionNames))
        {
            return parameter_error("Retouch region parameter is unknown", prefix + "." + name,
                                   "unknown_retouch_region_parameter");
        }
    }
    const auto get = [&](const std::string_view name) -> Result<const ParameterValue *>
    {
        const auto found = object->find(std::string(name));
        if (found == object->end())
        {
            return parameter_error("Retouch region parameter is required",
                                   prefix + "." + std::string(name),
                                   "missing_retouch_region_parameter");
        }
        return &found->second;
    };
    auto mask = get("mask_id");
    auto mode = get("mode");
    auto scale = get("scale");
    auto opacity = get("opacity");
    auto source_x = get("source_x");
    auto source_y = get("source_y");
    auto blur_type = get("blur_type");
    auto blur_radius = get("blur_radius");
    auto fill_mode = get("fill_mode");
    auto fill_color = get("fill_color");
    auto fill_brightness = get("fill_brightness");
    if (!mask || !mode || !scale || !opacity || !source_x || !source_y || !blur_type ||
        !blur_radius || !fill_mode || !fill_color || !fill_brightness)
    {
        return !mask              ? mask.error() :
               !mode              ? mode.error() :
               !scale             ? scale.error() :
               !opacity           ? opacity.error() :
               !source_x          ? source_x.error() :
               !source_y          ? source_y.error() :
               !blur_type         ? blur_type.error() :
               !blur_radius       ? blur_radius.error() :
               !fill_mode         ? fill_mode.error() :
               !fill_color        ? fill_color.error() :
                                     fill_brightness.error();
    }

    auto parsed_mask = string_value(*mask.value(), prefix + ".mask_id");
    auto parsed_mode_text = string_value(*mode.value(), prefix + ".mode");
    auto parsed_scale = integer_value(*scale.value(), prefix + ".scale", 0, num_scales + 1);
    auto parsed_opacity = number_value(*opacity.value(), prefix + ".opacity", 0.0, 1.0);
    auto parsed_source_x = number_value(*source_x.value(), prefix + ".source_x", 0.0, 1.0);
    auto parsed_source_y = number_value(*source_y.value(), prefix + ".source_y", 0.0, 1.0);
    auto parsed_blur_text = string_value(*blur_type.value(), prefix + ".blur_type");
    auto parsed_radius = number_value(*blur_radius.value(), prefix + ".blur_radius",
                                      kRetouchBlurRadiusMin, kRetouchBlurRadiusMax);
    auto parsed_fill_text = string_value(*fill_mode.value(), prefix + ".fill_mode");
    auto parsed_brightness =
        number_value(*fill_brightness.value(), prefix + ".fill_brightness", -1.0, 1.0);
    if (!parsed_mask || !parsed_mode_text || !parsed_scale || !parsed_opacity ||
        !parsed_source_x || !parsed_source_y || !parsed_blur_text || !parsed_radius ||
        !parsed_fill_text || !parsed_brightness)
    {
        return !parsed_mask        ? parsed_mask.error() :
               !parsed_mode_text   ? parsed_mode_text.error() :
               !parsed_scale       ? parsed_scale.error() :
               !parsed_opacity     ? parsed_opacity.error() :
               !parsed_source_x    ? parsed_source_x.error() :
               !parsed_source_y    ? parsed_source_y.error() :
               !parsed_blur_text   ? parsed_blur_text.error() :
               !parsed_radius      ? parsed_radius.error() :
               !parsed_fill_text   ? parsed_fill_text.error() :
                                     parsed_brightness.error();
    }
    if (parsed_mask.value().empty())
    {
        return parameter_error("Retouch region mask ID must not be empty", prefix + ".mask_id",
                               "empty_retouch_mask_id");
    }
    auto parsed_mode = parse_mode(parsed_mode_text.value(), prefix + ".mode");
    auto parsed_blur = parse_blur_type(parsed_blur_text.value(), prefix + ".blur_type");
    auto parsed_fill = parse_fill_mode(parsed_fill_text.value(), prefix + ".fill_mode");
    if (!parsed_mode || !parsed_blur || !parsed_fill)
    {
        return !parsed_mode ? parsed_mode.error() : !parsed_blur ? parsed_blur.error() :
                                                                 parsed_fill.error();
    }
    const auto *colors = std::get_if<ParameterValue::Array>(&fill_color.value()->value);
    if (colors == nullptr || colors->size() != 3U)
    {
        return parameter_error("Retouch fill color must contain three numeric channels",
                               prefix + ".fill_color", "invalid_retouch_fill_color");
    }
    std::array<double, 3> parsed_colors{};
    for (std::size_t channel = 0; channel < parsed_colors.size(); ++channel)
    {
        auto value = number_value((*colors)[channel],
                                  prefix + ".fill_color[" + std::to_string(channel) + "]", 0.0,
                                  1.0);
        if (!value)
            return value.error();
        parsed_colors[channel] = value.value();
    }
    return RetouchRegion{std::move(parsed_mask).value(),
                         parsed_mode.value(),
                         parsed_scale.value(),
                         parsed_opacity.value(),
                         parsed_source_x.value(),
                         parsed_source_y.value(),
                         parsed_blur.value(),
                         parsed_radius.value(),
                         parsed_fill.value(),
                         parsed_colors,
                         parsed_brightness.value()};
}

[[nodiscard]] const Mask *find_mask(const std::vector<Mask> &masks,
                                    const std::string_view id) noexcept
{
    const auto found = std::find_if(masks.begin(), masks.end(),
                                    [id](const Mask &mask) { return mask.id == id; });
    return found == masks.end() ? nullptr : &*found;
}

} // namespace

bool RetouchParams::is_identity() const noexcept
{
    return regions.empty();
}

std::string_view retouch_mode_name(const RetouchMode mode) noexcept
{
    switch (mode)
    {
    case RetouchMode::kClone:
        return "clone";
    case RetouchMode::kHeal:
        return "heal";
    case RetouchMode::kBlur:
        return "blur";
    case RetouchMode::kFill:
        return "fill";
    }
    return "heal";
}

std::string_view retouch_blur_type_name(const RetouchBlurType type) noexcept
{
    return type == RetouchBlurType::kGaussian ? "gaussian" : "bilateral";
}

std::string_view retouch_fill_mode_name(const RetouchFillMode mode) noexcept
{
    return mode == RetouchFillMode::kErase ? "erase" : "color";
}

Result<RetouchParams>
retouch_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    for (const auto &[name, ignored] : parameters)
    {
        (void)ignored;
        if (!known_name(name, kOperationParameterNames) && name != "regions")
        {
            return parameter_error("Retouch parameter is unknown", name,
                                   "unknown_retouch_parameter");
        }
    }
    auto working = required(parameters, "working_space");
    auto algorithm = required(parameters, "algorithm");
    auto scales = required(parameters, "num_scales");
    auto merge = required(parameters, "merge_from_scale");
    auto iterations = required(parameters, "max_heal_iterations");
    auto regions = required(parameters, "regions");
    if (!working || !algorithm || !scales || !merge || !iterations || !regions)
    {
        return !working    ? working.error() :
               !algorithm  ? algorithm.error() :
               !scales     ? scales.error() :
               !merge      ? merge.error() :
               !iterations ? iterations.error() :
                             regions.error();
    }
    auto parsed_working = string_value(*working.value(), "working_space");
    auto parsed_algorithm = string_value(*algorithm.value(), "algorithm");
    auto parsed_scales = integer_value(*scales.value(), "num_scales", 0, kRetouchMaxScales);
    auto parsed_iterations = integer_value(*iterations.value(), "max_heal_iterations", 1,
                                           kRetouchMaxHealIterations);
    if (!parsed_working || !parsed_algorithm || !parsed_scales || !parsed_iterations)
    {
        return !parsed_working   ? parsed_working.error() :
               !parsed_algorithm ? parsed_algorithm.error() :
               !parsed_scales    ? parsed_scales.error() :
                                   parsed_iterations.error();
    }
    if (parsed_working.value() != kRetouchWorkingSpaceLinearRec709D50)
    {
        return parameter_error("Retouch working space is unsupported", "working_space",
                               "unsupported_retouch_working_space");
    }
    if (parsed_algorithm.value() != kRetouchAlgorithmOrderedWaveletV1)
    {
        return parameter_error("Retouch algorithm is unsupported", "algorithm",
                               "unsupported_retouch_algorithm");
    }
    auto parsed_merge =
        integer_value(*merge.value(), "merge_from_scale", 0, parsed_scales.value());
    if (!parsed_merge)
        return parsed_merge.error();
    const auto *region_array = std::get_if<ParameterValue::Array>(&regions.value()->value);
    if (region_array == nullptr || region_array->size() > kRetouchMaxRegions)
    {
        return parameter_error("Retouch regions must be a bounded array", "regions",
                               "invalid_retouch_regions");
    }
    RetouchParams result;
    result.num_scales = parsed_scales.value();
    result.merge_from_scale = parsed_merge.value();
    result.max_heal_iterations = parsed_iterations.value();
    result.regions.reserve(region_array->size());
    for (std::size_t index = 0; index < region_array->size(); ++index)
    {
        auto region = parse_region((*region_array)[index], index, result.num_scales);
        if (!region)
            return region.error();
        result.regions.push_back(std::move(region).value());
    }
    return result;
}

std::map<std::string, ParameterValue, std::less<>> retouch_to_parameters(const RetouchParams &params)
{
    ParameterValue::Array regions;
    regions.reserve(params.regions.size());
    for (const auto &region : params.regions)
    {
        ParameterValue::Array color;
        for (const double channel : region.fill_color)
            color.emplace_back(channel);
        regions.emplace_back(ParameterValue::Object{
            {"blur_radius", ParameterValue{region.blur_radius}},
            {"blur_type", ParameterValue{std::string(retouch_blur_type_name(region.blur_type))}},
            {"fill_brightness", ParameterValue{region.fill_brightness}},
            {"fill_color", ParameterValue{std::move(color)}},
            {"fill_mode", ParameterValue{std::string(retouch_fill_mode_name(region.fill_mode))}},
            {"mask_id", ParameterValue{region.mask_id}},
            {"mode", ParameterValue{std::string(retouch_mode_name(region.mode))}},
            {"opacity", ParameterValue{region.opacity}},
            {"scale", ParameterValue{region.scale}},
            {"source_x", ParameterValue{region.source_x}},
            {"source_y", ParameterValue{region.source_y}},
        });
    }
    return {{"algorithm", ParameterValue{std::string(kRetouchAlgorithmOrderedWaveletV1)}},
            {"max_heal_iterations", ParameterValue{params.max_heal_iterations}},
            {"merge_from_scale", ParameterValue{params.merge_from_scale}},
            {"num_scales", ParameterValue{params.num_scales}},
            {"regions", ParameterValue{std::move(regions)}},
            {"working_space", ParameterValue{std::string(kRetouchWorkingSpaceLinearRec709D50)}}};
}

Result<void> validate_retouch_operation(const OperationInstance &operation,
                                        const std::vector<Mask> &masks)
{
    if (operation.id != kRetouchOperationId)
    {
        return make_error(ErrorCode::kValidation, "Operation is not Retouch",
                          {{"operation_id", operation.id}});
    }
    if (operation.schema_version != kRetouchOperationSchemaVersion)
    {
        return make_error(ErrorCode::kUnsupported, "Retouch schema version is unsupported",
                          {{"operation_id", operation.id},
                           {"schema_version", std::to_string(operation.schema_version)},
                           {"reason", "unsupported_retouch_schema"}});
    }
    if (operation.mask_id.has_value())
    {
        return make_error(ErrorCode::kUnsupported,
                          "Retouch owns per-region masks and cannot use an operation mask",
                          {{"operation_id", operation.id},
                           {"reason", "unsupported_retouch_operation_mask"}});
    }
    auto parsed = retouch_from_parameters(operation.parameters);
    if (!parsed)
        return parsed.error();
    std::set<std::string, std::less<>> seen;
    for (const auto &region : parsed.value().regions)
    {
        if (!seen.emplace(region.mask_id).second)
        {
            return make_error(ErrorCode::kConflict,
                              "Retouch regions must not reuse one mutable mask",
                              {{"operation_id", operation.id},
                               {"mask_id", region.mask_id},
                               {"reason", "duplicate_retouch_mask"}});
        }
        const Mask *mask = find_mask(masks, region.mask_id);
        if (mask == nullptr)
        {
            return make_error(ErrorCode::kValidation, "Retouch region references a missing mask",
                              {{"operation_id", operation.id},
                               {"mask_id", region.mask_id},
                               {"reason", "missing_retouch_mask"}});
        }
        if (mask->kind != MaskKind::kCircle && mask->kind != MaskKind::kEllipse &&
            mask->kind != MaskKind::kPath && mask->kind != MaskKind::kBrush)
        {
            return make_error(ErrorCode::kUnsupported,
                              "Retouch region requires a drawable leaf mask",
                              {{"operation_id", operation.id},
                               {"mask_id", region.mask_id},
                               {"reason", "unsupported_retouch_mask_kind"}});
        }
    }
    return {};
}

} // namespace ravo
