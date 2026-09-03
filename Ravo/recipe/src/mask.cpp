#include "ravo/recipe/mask.h"
#include "ravo/recipe/develop_mask.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <functional>
#include <limits>
#include <locale>
#include <map>
#include <new>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace ravo
{
namespace
{

using JsonObject = JsonValue::Object;

[[nodiscard]] TaskError field_error(const std::string_view message, const std::string_view path)
{
    return make_error(ErrorCode::kValidation, std::string(message), {{"path", std::string(path)}});
}

[[nodiscard]] TaskError mask_error(const std::string_view message, const std::string_view reason,
                                   const std::string_view mask_id = {})
{
    std::map<std::string, std::string, std::less<>> context{{"reason", std::string(reason)}};
    if (!mask_id.empty())
    {
        context.emplace("mask_id", std::string(mask_id));
    }
    return make_error(ErrorCode::kValidation, std::string(message), std::move(context));
}

[[nodiscard]] Result<const JsonObject *> object_at(const JsonValue &value,
                                                   const std::string_view path)
{
    const auto *object = value.object_if();
    if (object == nullptr)
    {
        return field_error("Expected a JSON object", path);
    }
    return object;
}

[[nodiscard]] Result<const JsonValue *>
required_field(const JsonObject &object, const std::string_view name, const std::string_view path)
{
    const auto found = object.find(std::string(name));
    if (found == object.end())
    {
        return field_error("Required JSON field is missing",
                           std::string(path) + "." + std::string(name));
    }
    return &found->second;
}

[[nodiscard]] Result<void> reject_unknown_fields(const JsonObject &object,
                                                 const std::set<std::string, std::less<>> &allowed,
                                                 const std::string_view path)
{
    for (const auto &[name, ignored] : object)
    {
        static_cast<void>(ignored);
        if (!allowed.contains(name))
        {
            return field_error("Unknown JSON field", std::string(path) + "." + name);
        }
    }
    return {};
}

[[nodiscard]] Result<std::string> string_at(const JsonValue &value, const std::string_view path)
{
    const auto *string = value.string_if();
    if (string == nullptr)
    {
        return field_error("Expected a JSON string", path);
    }
    return *string;
}

[[nodiscard]] Result<bool> boolean_at(const JsonValue &value, const std::string_view path)
{
    const auto *boolean = value.boolean_if();
    if (boolean == nullptr)
    {
        return field_error("Expected a JSON boolean", path);
    }
    return *boolean;
}

[[nodiscard]] Result<std::int64_t> integer_at(const JsonValue &value, const std::string_view path)
{
    const auto *number = value.number_if();
    if (number == nullptr)
    {
        return field_error("Expected an integer JSON number", path);
    }
    std::int64_t result = 0;
    const auto [position, error] =
        std::from_chars(number->text.data(), number->text.data() + number->text.size(), result);
    if (error != std::errc{} || position != number->text.data() + number->text.size())
    {
        return field_error("Expected an integer JSON number", path);
    }
    return result;
}

[[nodiscard]] Result<double> number_at(const JsonValue &value, const std::string_view path)
{
    const auto *number = value.number_if();
    if (number == nullptr)
    {
        return field_error("Expected a numeric JSON value", path);
    }
    std::istringstream stream{std::string(number->text)};
    stream.imbue(std::locale::classic());
    stream >> std::noskipws;
    double result = 0.0;
    if (!(stream >> result) || stream.peek() != std::char_traits<char>::eof() ||
        !std::isfinite(result))
    {
        return field_error("Expected a finite numeric JSON value", path);
    }
    return result;
}

[[nodiscard]] Result<JsonValue> number_json(const double value)
{
    if (!std::isfinite(value))
    {
        return make_error(ErrorCode::kValidation, "Mask numeric values must be finite",
                          {{"reason", "non_finite_mask_value"}});
    }
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream.precision(std::numeric_limits<double>::max_digits10);
    stream << value;
    if (!stream)
    {
        return make_error(ErrorCode::kInternal, "Unable to serialize a mask numeric value");
    }
    return JsonValue::number(stream.str());
}

[[nodiscard]] JsonValue integer_json(const std::int64_t value)
{
    return JsonValue::number(std::to_string(value));
}

[[nodiscard]] bool finite_unit(const double value) noexcept
{
    return std::isfinite(value) && value >= kCanonicalMaskUnitMin && value <= kCanonicalMaskUnitMax;
}

[[nodiscard]] bool finite_angle(const double value) noexcept
{
    return std::isfinite(value) && value >= kCanonicalMaskAngleMin &&
           value <= kCanonicalMaskAngleMax;
}

[[nodiscard]] bool finite_positive_unit(const double value) noexcept
{
    return std::isfinite(value) && value >= kCanonicalMaskPositiveMin &&
           value <= kCanonicalMaskUnitMax;
}

[[nodiscard]] bool valid_path_point(const PathMaskPoint &point) noexcept
{
    return finite_unit(point.x) && finite_unit(point.y) && finite_unit(point.ctrl1_x) &&
           finite_unit(point.ctrl1_y) && finite_unit(point.ctrl2_x) && finite_unit(point.ctrl2_y);
}

[[nodiscard]] bool valid_brush_point(const BrushMaskPoint &point) noexcept
{
    return finite_unit(point.x) && finite_unit(point.y) && finite_unit(point.ctrl1_x) &&
           finite_unit(point.ctrl1_y) && finite_unit(point.ctrl2_x) && finite_unit(point.ctrl2_y) &&
           finite_positive_unit(point.radius) && finite_unit(point.hardness) &&
           finite_unit(point.density);
}

[[nodiscard]] Result<MaskKind> parse_kind(const std::string_view text, const std::string_view path)
{
    if (text == "all")
    {
        return MaskKind::kAll;
    }
    if (text == "linear_gradient")
    {
        return MaskKind::kLinearGradient;
    }
    if (text == "circle")
    {
        return MaskKind::kCircle;
    }
    if (text == "ellipse")
    {
        return MaskKind::kEllipse;
    }
    if (text == "parametric")
    {
        return MaskKind::kParametric;
    }
    if (text == "group")
    {
        return MaskKind::kGroup;
    }
    if (text == "path")
    {
        return MaskKind::kPath;
    }
    if (text == "brush")
    {
        return MaskKind::kBrush;
    }
    return make_error(ErrorCode::kUnsupported, "Unsupported canonical mask kind",
                      {{"kind", std::string(text)},
                       {"path", std::string(path)},
                       {"reason", "unsupported_mask_kind"}});
}

[[nodiscard]] Result<ParametricMaskSource> parse_parametric_source(const std::string_view text,
                                                                   const std::string_view path)
{
    if (text == "input")
    {
        return ParametricMaskSource::kInput;
    }
    if (text == "operation_output")
    {
        return ParametricMaskSource::kOperationOutput;
    }
    return make_error(ErrorCode::kUnsupported, "Unsupported parametric mask source",
                      {{"path", std::string(path)},
                       {"reason", "unsupported_mask_source"},
                       {"source", std::string(text)}});
}

[[nodiscard]] Result<ParametricMaskChannel> parse_parametric_channel(const std::string_view text,
                                                                     const std::string_view path)
{
    if (text == "luminance")
    {
        return ParametricMaskChannel::kLuminance;
    }
    if (text == "r")
    {
        return ParametricMaskChannel::kRed;
    }
    if (text == "g")
    {
        return ParametricMaskChannel::kGreen;
    }
    if (text == "b")
    {
        return ParametricMaskChannel::kBlue;
    }
    return make_error(ErrorCode::kUnsupported, "Unsupported parametric mask channel",
                      {{"path", std::string(path)},
                       {"reason", "unsupported_mask_channel"},
                       {"channel", std::string(text)}});
}

[[nodiscard]] Result<MaskGroupOperator> parse_group_operator(const std::string_view text,
                                                             const std::string_view path)
{
    if (text == "replace")
    {
        return MaskGroupOperator::kReplace;
    }
    if (text == "union")
    {
        return MaskGroupOperator::kUnion;
    }
    if (text == "intersection")
    {
        return MaskGroupOperator::kIntersection;
    }
    if (text == "difference")
    {
        return MaskGroupOperator::kDifference;
    }
    if (text == "exclusion")
    {
        return MaskGroupOperator::kExclusion;
    }
    return make_error(ErrorCode::kUnsupported, "Unsupported mask group operator",
                      {{"path", std::string(path)},
                       {"reason", "unsupported_mask_group_operator"},
                       {"operator", std::string(text)}});
}

[[nodiscard]] std::string_view parametric_source_name(const ParametricMaskSource source) noexcept
{
    switch (source)
    {
    case ParametricMaskSource::kInput:
        return "input";
    case ParametricMaskSource::kOperationOutput:
        return "operation_output";
    }
    return "input";
}

[[nodiscard]] std::string_view parametric_channel_name(const ParametricMaskChannel channel) noexcept
{
    switch (channel)
    {
    case ParametricMaskChannel::kLuminance:
        return "luminance";
    case ParametricMaskChannel::kRed:
        return "r";
    case ParametricMaskChannel::kGreen:
        return "g";
    case ParametricMaskChannel::kBlue:
        return "b";
    }
    return "luminance";
}

[[nodiscard]] std::string_view group_operator_name(const MaskGroupOperator operation) noexcept
{
    switch (operation)
    {
    case MaskGroupOperator::kReplace:
        return "replace";
    case MaskGroupOperator::kUnion:
        return "union";
    case MaskGroupOperator::kIntersection:
        return "intersection";
    case MaskGroupOperator::kDifference:
        return "difference";
    case MaskGroupOperator::kExclusion:
        return "exclusion";
    }
    return "replace";
}

[[nodiscard]] Result<MaskCommon> parse_common(const JsonObject &object, const std::string_view path)
{
    auto opacity = required_field(object, "opacity", path);
    auto inverted = required_field(object, "inverted", path);
    if (!opacity)
    {
        return opacity.error();
    }
    if (!inverted)
    {
        return inverted.error();
    }
    auto parsed_opacity = number_at(*opacity.value(), std::string(path) + ".opacity");
    auto parsed_inverted = boolean_at(*inverted.value(), std::string(path) + ".inverted");
    if (!parsed_opacity)
    {
        return parsed_opacity.error();
    }
    if (!parsed_inverted)
    {
        return parsed_inverted.error();
    }
    return MaskCommon{parsed_opacity.value(), parsed_inverted.value()};
}

[[nodiscard]] Result<MaskGroup> parse_group(const JsonObject &object, const std::string_view path)
{
    auto children = required_field(object, "children", path);
    if (!children)
    {
        return children.error();
    }
    const auto *array = children.value()->array_if();
    if (array == nullptr)
    {
        return field_error("Expected a JSON array", std::string(path) + ".children");
    }
    if (array->empty() || array->size() > kCanonicalMaskMaxGroupChildren)
    {
        return mask_error("Mask group child count is outside the canonical bounds",
                          "invalid_mask_group_size");
    }
    MaskGroup group;
    group.children.reserve(array->size());
    for (std::size_t index = 0; index < array->size(); ++index)
    {
        const std::string child_path =
            std::string(path) + ".children[" + std::to_string(index) + "]";
        auto child = object_at((*array)[index], child_path);
        if (!child)
        {
            return child.error();
        }
        auto fields = reject_unknown_fields(
            *child.value(), {"inverted", "mask_id", "opacity", "operator"}, child_path);
        if (!fields)
        {
            return fields.error();
        }
        auto mask_id = required_field(*child.value(), "mask_id", child_path);
        auto operation = required_field(*child.value(), "operator", child_path);
        auto opacity = required_field(*child.value(), "opacity", child_path);
        auto inverted = required_field(*child.value(), "inverted", child_path);
        if (!mask_id || !operation || !opacity || !inverted)
        {
            return !mask_id   ? mask_id.error() :
                   !operation ? operation.error() :
                   !opacity   ? opacity.error() :
                                inverted.error();
        }
        auto parsed_id = string_at(*mask_id.value(), child_path + ".mask_id");
        auto parsed_operation = string_at(*operation.value(), child_path + ".operator");
        auto parsed_opacity = number_at(*opacity.value(), child_path + ".opacity");
        auto parsed_inverted = boolean_at(*inverted.value(), child_path + ".inverted");
        if (!parsed_id || !parsed_operation || !parsed_opacity || !parsed_inverted)
        {
            return !parsed_id        ? parsed_id.error() :
                   !parsed_operation ? parsed_operation.error() :
                   !parsed_opacity   ? parsed_opacity.error() :
                                       parsed_inverted.error();
        }
        auto kind = parse_group_operator(parsed_operation.value(), child_path + ".operator");
        if (!kind)
        {
            return kind.error();
        }
        group.children.push_back({std::move(parsed_id).value(), kind.value(),
                                  parsed_opacity.value(), parsed_inverted.value()});
    }
    return group;
}

[[nodiscard]] Result<double> object_number(const JsonObject &object, const std::string_view key,
                                           const std::string_view path)
{
    auto field = required_field(object, key, path);
    if (!field)
    {
        return field.error();
    }
    return number_at(*field.value(), std::string(path) + "." + std::string(key));
}

[[nodiscard]] Result<PathMask> parse_path(const JsonObject &object, const std::string_view path)
{
    auto feather = object_number(object, "feather", path);
    auto points = required_field(object, "points", path);
    if (!feather || !points)
    {
        return !feather ? feather.error() : points.error();
    }
    const auto *array = points.value()->array_if();
    if (array == nullptr)
    {
        return field_error("Expected a JSON array", std::string(path) + ".points");
    }
    PathMask result;
    result.feather = feather.value();
    result.points.reserve(array->size());
    for (std::size_t index = 0; index < array->size(); ++index)
    {
        const std::string point_path = std::string(path) + ".points[" + std::to_string(index) + "]";
        auto point = object_at((*array)[index], point_path);
        if (!point)
        {
            return point.error();
        }
        auto fields = reject_unknown_fields(
            *point.value(), {"ctrl1_x", "ctrl1_y", "ctrl2_x", "ctrl2_y", "x", "y"}, point_path);
        if (!fields)
        {
            return fields.error();
        }
        auto x = object_number(*point.value(), "x", point_path);
        auto y = object_number(*point.value(), "y", point_path);
        auto ctrl1_x = object_number(*point.value(), "ctrl1_x", point_path);
        auto ctrl1_y = object_number(*point.value(), "ctrl1_y", point_path);
        auto ctrl2_x = object_number(*point.value(), "ctrl2_x", point_path);
        auto ctrl2_y = object_number(*point.value(), "ctrl2_y", point_path);
        if (!x || !y || !ctrl1_x || !ctrl1_y || !ctrl2_x || !ctrl2_y)
        {
            return !x       ? x.error() :
                   !y       ? y.error() :
                   !ctrl1_x ? ctrl1_x.error() :
                   !ctrl1_y ? ctrl1_y.error() :
                   !ctrl2_x ? ctrl2_x.error() :
                              ctrl2_y.error();
        }
        result.points.push_back({x.value(), y.value(), ctrl1_x.value(), ctrl1_y.value(),
                                 ctrl2_x.value(), ctrl2_y.value()});
    }
    return result;
}

[[nodiscard]] Result<BrushMask> parse_brush(const JsonObject &object, const std::string_view path)
{
    auto points = required_field(object, "points", path);
    if (!points)
    {
        return points.error();
    }
    const auto *array = points.value()->array_if();
    if (array == nullptr)
    {
        return field_error("Expected a JSON array", std::string(path) + ".points");
    }
    BrushMask result;
    result.points.reserve(array->size());
    for (std::size_t index = 0; index < array->size(); ++index)
    {
        const std::string point_path = std::string(path) + ".points[" + std::to_string(index) + "]";
        auto point = object_at((*array)[index], point_path);
        if (!point)
        {
            return point.error();
        }
        auto fields = reject_unknown_fields(
            *point.value(),
            {"ctrl1_x", "ctrl1_y", "ctrl2_x", "ctrl2_y", "density", "hardness", "radius", "x", "y"},
            point_path);
        if (!fields)
        {
            return fields.error();
        }
        auto x = object_number(*point.value(), "x", point_path);
        auto y = object_number(*point.value(), "y", point_path);
        auto ctrl1_x = object_number(*point.value(), "ctrl1_x", point_path);
        auto ctrl1_y = object_number(*point.value(), "ctrl1_y", point_path);
        auto ctrl2_x = object_number(*point.value(), "ctrl2_x", point_path);
        auto ctrl2_y = object_number(*point.value(), "ctrl2_y", point_path);
        auto radius = object_number(*point.value(), "radius", point_path);
        auto hardness = object_number(*point.value(), "hardness", point_path);
        auto density = object_number(*point.value(), "density", point_path);
        if (!x || !y || !ctrl1_x || !ctrl1_y || !ctrl2_x || !ctrl2_y || !radius || !hardness ||
            !density)
        {
            return !x        ? x.error() :
                   !y        ? y.error() :
                   !ctrl1_x  ? ctrl1_x.error() :
                   !ctrl1_y  ? ctrl1_y.error() :
                   !ctrl2_x  ? ctrl2_x.error() :
                   !ctrl2_y  ? ctrl2_y.error() :
                   !radius   ? radius.error() :
                   !hardness ? hardness.error() :
                               density.error();
        }
        result.points.push_back({x.value(), y.value(), ctrl1_x.value(), ctrl1_y.value(),
                                 ctrl2_x.value(), ctrl2_y.value(), radius.value(), hardness.value(),
                                 density.value()});
    }
    return result;
}

[[nodiscard]] Result<void> validate_mask_node(const Mask &mask)
{
    if (mask.id.empty() || mask.id.size() > 128U)
    {
        return mask_error("Mask ID is empty or exceeds the supported length", "invalid_mask_id",
                          mask.id);
    }
    if (mask.schema_version != 1 && mask.schema_version != kCanonicalMaskSchemaVersion)
    {
        return make_error(ErrorCode::kUnsupported, "Mask schema version is unsupported",
                          {{"mask_id", mask.id},
                           {"reason", "unsupported_mask_schema"},
                           {"schema_version", std::to_string(mask.schema_version)}});
    }
    if (mask.schema_version == 1 && mask.kind != MaskKind::kAll)
    {
        return make_error(ErrorCode::kUnsupported, "Mask schema v1 supports only all",
                          {{"mask_id", mask.id}, {"reason", "unsupported_mask_v1_kind"}});
    }
    if (!finite_unit(mask.common.opacity))
    {
        return mask_error("Mask opacity must be finite and within [0, 1]", "invalid_mask_opacity",
                          mask.id);
    }
    const auto wrong_payload = [&]
    {
        return mask_error("Mask kind does not match its typed payload",
                          "mask_kind_payload_mismatch", mask.id);
    };
    switch (mask.kind)
    {
    case MaskKind::kAll:
        if (!std::holds_alternative<AllMask>(mask.payload))
        {
            return wrong_payload();
        }
        if (mask.schema_version == 1 && mask.common != MaskCommon{})
        {
            return mask_error("Mask schema v1 has no opacity or inversion state",
                              "invalid_mask_v1_common", mask.id);
        }
        return {};
    case MaskKind::kLinearGradient:
    {
        const auto *gradient = std::get_if<LinearGradientMask>(&mask.payload);
        if (gradient == nullptr)
        {
            return wrong_payload();
        }
        if (!finite_unit(gradient->anchor_x) || !finite_unit(gradient->anchor_y) ||
            !finite_angle(gradient->rotation_degrees) || !finite_unit(gradient->transition))
        {
            return mask_error("Linear gradient parameters are outside the canonical bounds",
                              "invalid_linear_gradient", mask.id);
        }
        return {};
    }
    case MaskKind::kCircle:
    {
        const auto *circle = std::get_if<CircleMask>(&mask.payload);
        if (circle == nullptr)
        {
            return wrong_payload();
        }
        if (!finite_unit(circle->center_x) || !finite_unit(circle->center_y) ||
            !std::isfinite(circle->radius) || circle->radius < kCanonicalMaskPositiveMin ||
            circle->radius > kCanonicalMaskUnitMax || !finite_unit(circle->feather))
        {
            return mask_error("Circle parameters are outside the canonical bounds",
                              "invalid_circle", mask.id);
        }
        return {};
    }
    case MaskKind::kEllipse:
    {
        const auto *ellipse = std::get_if<EllipseMask>(&mask.payload);
        if (ellipse == nullptr)
        {
            return wrong_payload();
        }
        if (!finite_unit(ellipse->center_x) || !finite_unit(ellipse->center_y) ||
            !std::isfinite(ellipse->radius_x) || ellipse->radius_x < kCanonicalMaskPositiveMin ||
            ellipse->radius_x > kCanonicalMaskUnitMax || !std::isfinite(ellipse->radius_y) ||
            ellipse->radius_y < kCanonicalMaskPositiveMin ||
            ellipse->radius_y > kCanonicalMaskUnitMax || !finite_angle(ellipse->rotation_degrees) ||
            !finite_unit(ellipse->feather))
        {
            return mask_error("Ellipse parameters are outside the canonical bounds",
                              "invalid_ellipse", mask.id);
        }
        return {};
    }
    case MaskKind::kParametric:
    {
        const auto *parametric = std::get_if<ParametricMask>(&mask.payload);
        if (parametric == nullptr)
        {
            return wrong_payload();
        }
        if ((parametric->source != ParametricMaskSource::kInput &&
             parametric->source != ParametricMaskSource::kOperationOutput) ||
            (parametric->channel != ParametricMaskChannel::kLuminance &&
             parametric->channel != ParametricMaskChannel::kRed &&
             parametric->channel != ParametricMaskChannel::kGreen &&
             parametric->channel != ParametricMaskChannel::kBlue))
        {
            return mask_error("Parametric source or channel is invalid",
                              "invalid_parametric_selector", mask.id);
        }
        for (std::size_t index = 0; index < parametric->thresholds.size(); ++index)
        {
            if (!finite_unit(parametric->thresholds[index]) ||
                (index > 0U && parametric->thresholds[index - 1U] > parametric->thresholds[index]))
            {
                return mask_error("Parametric thresholds must be finite, normalized, and monotonic",
                                  "invalid_parametric_thresholds", mask.id);
            }
        }
        return {};
    }
    case MaskKind::kGroup:
    {
        const auto *group = std::get_if<MaskGroup>(&mask.payload);
        if (group == nullptr)
        {
            return wrong_payload();
        }
        if (group->children.empty() || group->children.size() > kCanonicalMaskMaxGroupChildren)
        {
            return mask_error("Mask group child count is outside the canonical bounds",
                              "invalid_mask_group_size", mask.id);
        }
        for (std::size_t index = 0; index < group->children.size(); ++index)
        {
            const auto &child = group->children[index];
            if (child.mask_id.empty() || child.mask_id.size() > 128U || !finite_unit(child.opacity))
            {
                return mask_error("Mask group child is invalid", "invalid_mask_group_child",
                                  mask.id);
            }
            if ((index == 0U && child.operation != MaskGroupOperator::kReplace) ||
                (index > 0U && child.operation == MaskGroupOperator::kReplace))
            {
                return mask_error("Mask group first-child composition is invalid",
                                  "invalid_mask_group_order", mask.id);
            }
            if (child.operation != MaskGroupOperator::kReplace &&
                child.operation != MaskGroupOperator::kUnion &&
                child.operation != MaskGroupOperator::kIntersection &&
                child.operation != MaskGroupOperator::kDifference &&
                child.operation != MaskGroupOperator::kExclusion)
            {
                return mask_error("Mask group operator is invalid", "invalid_mask_group_operator",
                                  mask.id);
            }
        }
        return {};
    }
    case MaskKind::kPath:
    {
        const auto *path = std::get_if<PathMask>(&mask.payload);
        if (path == nullptr)
        {
            return wrong_payload();
        }
        if (path->points.size() < kCanonicalMaskMinPathPoints ||
            path->points.size() > kCanonicalMaskMaxPathPoints || !finite_unit(path->feather))
        {
            return mask_error("Path mask point count or feather is outside the canonical bounds",
                              "invalid_path_mask", mask.id);
        }
        for (const auto &point : path->points)
        {
            if (!valid_path_point(point))
            {
                return mask_error("Path mask point is outside the canonical bounds",
                                  "invalid_path_mask_point", mask.id);
            }
        }
        return {};
    }
    case MaskKind::kBrush:
    {
        const auto *brush = std::get_if<BrushMask>(&mask.payload);
        if (brush == nullptr)
        {
            return wrong_payload();
        }
        if (brush->points.size() < kCanonicalMaskMinBrushPoints ||
            brush->points.size() > kCanonicalMaskMaxPathPoints)
        {
            return mask_error("Brush mask point count is outside the canonical bounds",
                              "invalid_brush_mask", mask.id);
        }
        for (const auto &point : brush->points)
        {
            if (!valid_brush_point(point))
            {
                return mask_error("Brush mask point is outside the canonical bounds",
                                  "invalid_brush_mask_point", mask.id);
            }
        }
        return {};
    }
    }
    return mask_error("Mask kind is invalid", "invalid_mask_kind", mask.id);
}

[[nodiscard]] Result<void> add_number(JsonObject &object, const std::string_view key,
                                      const double value)
{
    auto json = number_json(value);
    if (!json)
    {
        return json.error();
    }
    object.emplace(std::string(key), std::move(json).value());
    return {};
}

} // namespace

Mask::Mask(std::string value_id, const std::int64_t value_schema_version, const MaskKind value_kind)
    : id(std::move(value_id))
    , schema_version(value_schema_version)
    , kind(value_kind)
{
    switch (kind)
    {
    case MaskKind::kAll:
        payload = AllMask{};
        break;
    case MaskKind::kLinearGradient:
        payload = LinearGradientMask{};
        break;
    case MaskKind::kCircle:
        payload = CircleMask{};
        break;
    case MaskKind::kEllipse:
        payload = EllipseMask{};
        break;
    case MaskKind::kParametric:
        payload = ParametricMask{};
        break;
    case MaskKind::kGroup:
        payload = MaskGroup{};
        break;
    case MaskKind::kPath:
        payload = PathMask{};
        break;
    case MaskKind::kBrush:
        payload = BrushMask{};
        break;
    }
}

std::string_view mask_kind_name(const MaskKind kind) noexcept
{
    switch (kind)
    {
    case MaskKind::kAll:
        return "all";
    case MaskKind::kLinearGradient:
        return "linear_gradient";
    case MaskKind::kCircle:
        return "circle";
    case MaskKind::kEllipse:
        return "ellipse";
    case MaskKind::kParametric:
        return "parametric";
    case MaskKind::kGroup:
        return "group";
    case MaskKind::kPath:
        return "path";
    case MaskKind::kBrush:
        return "brush";
    }
    return "all";
}

Result<Mask> parse_canonical_mask(const JsonValue &value, const std::string_view path)
try
{
    auto object = object_at(value, path);
    if (!object)
    {
        return object.error();
    }
    auto id = required_field(*object.value(), "id", path);
    auto schema_version = required_field(*object.value(), "schema_version", path);
    auto kind = required_field(*object.value(), "kind", path);
    if (!id || !schema_version || !kind)
    {
        return !id ? id.error() : !schema_version ? schema_version.error() : kind.error();
    }
    auto parsed_id = string_at(*id.value(), std::string(path) + ".id");
    auto parsed_version =
        integer_at(*schema_version.value(), std::string(path) + ".schema_version");
    auto parsed_kind_text = string_at(*kind.value(), std::string(path) + ".kind");
    if (!parsed_id || !parsed_version || !parsed_kind_text)
    {
        return !parsed_id      ? parsed_id.error() :
               !parsed_version ? parsed_version.error() :
                                 parsed_kind_text.error();
    }
    auto parsed_kind = parse_kind(parsed_kind_text.value(), std::string(path) + ".kind");
    if (!parsed_kind)
    {
        return parsed_kind.error();
    }

    if (parsed_version.value() == 1)
    {
        auto fields =
            reject_unknown_fields(*object.value(), {"id", "kind", "schema_version"}, path);
        if (!fields)
        {
            return fields.error();
        }
        if (parsed_kind.value() != MaskKind::kAll)
        {
            return make_error(ErrorCode::kUnsupported, "Mask schema v1 supports only all",
                              {{"path", std::string(path)},
                               {"reason", "unsupported_mask_v1_kind"},
                               {"kind", parsed_kind_text.value()}});
        }
        // Upgrade on read.  V1 had no opacity/inversion fields, so their only
        // valid canonical interpretation is the identity common state.
        return Mask{std::move(parsed_id).value(), kCanonicalMaskSchemaVersion, MaskKind::kAll};
    }
    if (parsed_version.value() != kCanonicalMaskSchemaVersion)
    {
        return make_error(ErrorCode::kUnsupported, "Mask schema version is unsupported",
                          {{"path", std::string(path)},
                           {"reason", "unsupported_mask_schema"},
                           {"schema_version", std::to_string(parsed_version.value())}});
    }

    std::set<std::string, std::less<>> fields{"id", "inverted", "kind", "opacity",
                                              "schema_version"};
    switch (parsed_kind.value())
    {
    case MaskKind::kAll:
        break;
    case MaskKind::kLinearGradient:
        fields.insert("anchor_x");
        fields.insert("anchor_y");
        fields.insert("rotation_degrees");
        fields.insert("transition");
        break;
    case MaskKind::kCircle:
        fields.insert("center_x");
        fields.insert("center_y");
        fields.insert("radius");
        fields.insert("feather");
        break;
    case MaskKind::kEllipse:
        fields.insert("center_x");
        fields.insert("center_y");
        fields.insert("radius_x");
        fields.insert("radius_y");
        fields.insert("rotation_degrees");
        fields.insert("feather");
        break;
    case MaskKind::kParametric:
        fields.insert("source");
        fields.insert("channel");
        fields.insert("thresholds");
        break;
    case MaskKind::kGroup:
        fields.insert("children");
        break;
    case MaskKind::kPath:
        fields.insert("feather");
        fields.insert("points");
        break;
    case MaskKind::kBrush:
        fields.insert("points");
        break;
    }
    auto unknown = reject_unknown_fields(*object.value(), fields, path);
    if (!unknown)
    {
        return unknown.error();
    }
    auto common = parse_common(*object.value(), path);
    if (!common)
    {
        return common.error();
    }
    Mask result{std::move(parsed_id).value(), parsed_version.value(), parsed_kind.value()};
    result.common = std::move(common).value();

    const auto numeric = [&](const std::string_view key) -> Result<double>
    {
        auto field = required_field(*object.value(), key, path);
        if (!field)
        {
            return field.error();
        }
        return number_at(*field.value(), std::string(path) + "." + std::string(key));
    };
    switch (result.kind)
    {
    case MaskKind::kAll:
        break;
    case MaskKind::kLinearGradient:
    {
        auto x = numeric("anchor_x");
        auto y = numeric("anchor_y");
        auto rotation = numeric("rotation_degrees");
        auto transition = numeric("transition");
        if (!x || !y || !rotation || !transition)
        {
            return !x        ? x.error() :
                   !y        ? y.error() :
                   !rotation ? rotation.error() :
                               transition.error();
        }
        result.payload =
            LinearGradientMask{x.value(), y.value(), rotation.value(), transition.value()};
        break;
    }
    case MaskKind::kCircle:
    {
        auto x = numeric("center_x");
        auto y = numeric("center_y");
        auto radius = numeric("radius");
        auto feather = numeric("feather");
        if (!x || !y || !radius || !feather)
        {
            return !x ? x.error() : !y ? y.error() : !radius ? radius.error() : feather.error();
        }
        result.payload = CircleMask{x.value(), y.value(), radius.value(), feather.value()};
        break;
    }
    case MaskKind::kEllipse:
    {
        auto x = numeric("center_x");
        auto y = numeric("center_y");
        auto radius_x = numeric("radius_x");
        auto radius_y = numeric("radius_y");
        auto rotation = numeric("rotation_degrees");
        auto feather = numeric("feather");
        if (!x || !y || !radius_x || !radius_y || !rotation || !feather)
        {
            return !x        ? x.error() :
                   !y        ? y.error() :
                   !radius_x ? radius_x.error() :
                   !radius_y ? radius_y.error() :
                   !rotation ? rotation.error() :
                               feather.error();
        }
        result.payload = EllipseMask{x.value(),        y.value(),        radius_x.value(),
                                     radius_y.value(), rotation.value(), feather.value()};
        break;
    }
    case MaskKind::kParametric:
    {
        auto source = required_field(*object.value(), "source", path);
        auto channel = required_field(*object.value(), "channel", path);
        auto thresholds = required_field(*object.value(), "thresholds", path);
        if (!source || !channel || !thresholds)
        {
            return !source ? source.error() : !channel ? channel.error() : thresholds.error();
        }
        auto source_text = string_at(*source.value(), std::string(path) + ".source");
        auto channel_text = string_at(*channel.value(), std::string(path) + ".channel");
        const auto *array = thresholds.value()->array_if();
        if (!source_text || !channel_text || array == nullptr)
        {
            return !source_text ?
                       source_text.error() :
                   !channel_text ?
                       channel_text.error() :
                       field_error("Expected a JSON array", std::string(path) + ".thresholds");
        }
        if (array->size() != 4U)
        {
            return field_error("Parametric thresholds must contain exactly four values",
                               std::string(path) + ".thresholds");
        }
        auto parsed_source =
            parse_parametric_source(source_text.value(), std::string(path) + ".source");
        auto parsed_channel =
            parse_parametric_channel(channel_text.value(), std::string(path) + ".channel");
        if (!parsed_source || !parsed_channel)
        {
            return !parsed_source ? parsed_source.error() : parsed_channel.error();
        }
        ParametricMask parametric;
        parametric.source = parsed_source.value();
        parametric.channel = parsed_channel.value();
        for (std::size_t index = 0; index < parametric.thresholds.size(); ++index)
        {
            auto threshold = number_at((*array)[index], std::string(path) + ".thresholds[" +
                                                            std::to_string(index) + "]");
            if (!threshold)
            {
                return threshold.error();
            }
            parametric.thresholds[index] = threshold.value();
        }
        result.payload = std::move(parametric);
        break;
    }
    case MaskKind::kGroup:
    {
        auto group = parse_group(*object.value(), path);
        if (!group)
        {
            return group.error();
        }
        result.payload = std::move(group).value();
        break;
    }
    case MaskKind::kPath:
    {
        auto path_payload = parse_path(*object.value(), path);
        if (!path_payload)
        {
            return path_payload.error();
        }
        result.payload = std::move(path_payload).value();
        break;
    }
    case MaskKind::kBrush:
    {
        auto brush = parse_brush(*object.value(), path);
        if (!brush)
        {
            return brush.error();
        }
        result.payload = std::move(brush).value();
        break;
    }
    }
    auto valid = validate_mask_node(result);
    if (!valid)
    {
        return valid.error();
    }
    return result;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Canonical mask parsing allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<void> upgrade_mask_graph(std::vector<Mask> &masks)
try
{
    std::vector<Mask> upgraded = masks;
    for (auto &mask : upgraded)
    {
        if (mask.schema_version == 1)
        {
            if (mask.kind != MaskKind::kAll || !std::holds_alternative<AllMask>(mask.payload) ||
                mask.common != MaskCommon{})
            {
                return make_error(ErrorCode::kUnsupported, "Mask schema v1 supports only all",
                                  {{"mask_id", mask.id}, {"reason", "unsupported_mask_v1_state"}});
            }
            mask.schema_version = kCanonicalMaskSchemaVersion;
            mask.common = {};
        }
    }
    auto valid = validate_mask_graph(upgraded);
    if (!valid)
    {
        return valid.error();
    }
    masks = std::move(upgraded);
    return {};
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Canonical mask upgrade allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<void> validate_mask_graph(const std::vector<Mask> &masks)
try
{
    if (masks.size() > kCanonicalMaskMaxNodes)
    {
        return make_error(
            ErrorCode::kValidation, "Mask graph exceeds the canonical node limit",
            {{"reason", "mask_graph_too_large"}, {"count", std::to_string(masks.size())}});
    }
    std::map<std::string, std::size_t, std::less<>> indexes;
    for (std::size_t index = 0; index < masks.size(); ++index)
    {
        auto valid = validate_mask_node(masks[index]);
        if (!valid)
        {
            return valid.error();
        }
        if (!indexes.emplace(masks[index].id, index).second)
        {
            return make_error(ErrorCode::kConflict, "Mask graph contains duplicate IDs",
                              {{"reason", "duplicate_mask_id"}, {"mask_id", masks[index].id}});
        }
    }

    enum class VisitState : std::uint8_t
    {
        kUnseen,
        kVisiting,
        kDone,
    };
    std::vector<VisitState> state(masks.size(), VisitState::kUnseen);
    std::function<Result<void>(std::size_t)> visit_cycle;
    visit_cycle = [&](const std::size_t index) -> Result<void>
    {
        if (state[index] == VisitState::kVisiting)
        {
            return make_error(ErrorCode::kConflict, "Mask graph contains a cycle",
                              {{"reason", "mask_graph_cycle"}, {"mask_id", masks[index].id}});
        }
        if (state[index] == VisitState::kDone)
        {
            return {};
        }
        state[index] = VisitState::kVisiting;
        if (const auto *group = std::get_if<MaskGroup>(&masks[index].payload); group != nullptr)
        {
            for (const auto &child : group->children)
            {
                const auto found = indexes.find(child.mask_id);
                if (found == indexes.end())
                {
                    return make_error(ErrorCode::kValidation,
                                      "Mask group references a missing mask",
                                      {{"reason", "mask_graph_dangling_reference"},
                                       {"mask_id", masks[index].id},
                                       {"referenced_mask_id", child.mask_id}});
                }
                auto nested = visit_cycle(found->second);
                if (!nested)
                {
                    return nested.error();
                }
            }
        }
        state[index] = VisitState::kDone;
        return {};
    };
    for (std::size_t index = 0; index < masks.size(); ++index)
    {
        auto valid = visit_cycle(index);
        if (!valid)
        {
            return valid.error();
        }
    }

    // A completed cycle walk intentionally cannot answer longest-path depth:
    // a shared child may have been completed from a shallow root before a
    // later parent reaches it through a deeper path.  Compute a separate DAG
    // depth memo after cycle/dangling validation so graph ordering cannot
    // weaken the hard depth limit.
    std::vector<std::size_t> longest_depth(masks.size(), 0U);
    std::vector<std::size_t> expanded_nodes(masks.size(), 0U);
    std::function<std::size_t(std::size_t)> depth_from;
    depth_from = [&](const std::size_t index) -> std::size_t
    {
        if (longest_depth[index] != 0U)
        {
            return longest_depth[index];
        }
        std::size_t result = 1U;
        if (const auto *group = std::get_if<MaskGroup>(&masks[index].payload); group != nullptr)
        {
            for (const auto &child : group->children)
            {
                const std::size_t child_index = indexes.at(child.mask_id);
                result = std::max(result, 1U + depth_from(child_index));
            }
        }
        longest_depth[index] = result;
        return result;
    };
    std::function<std::size_t(std::size_t)> expansion_from;
    expansion_from = [&](const std::size_t index) -> std::size_t
    {
        if (expanded_nodes[index] != 0U)
        {
            return expanded_nodes[index];
        }
        std::size_t result = 1U;
        if (const auto *group = std::get_if<MaskGroup>(&masks[index].payload); group != nullptr)
        {
            for (const auto &child : group->children)
            {
                const std::size_t child_expansion = expansion_from(indexes.at(child.mask_id));
                if (child_expansion > kCanonicalMaskMaxExpandedNodes ||
                    result > kCanonicalMaskMaxExpandedNodes - child_expansion)
                {
                    result = kCanonicalMaskMaxExpandedNodes + 1U;
                    break;
                }
                result += child_expansion;
            }
        }
        expanded_nodes[index] = result;
        return result;
    };
    for (std::size_t index = 0; index < masks.size(); ++index)
    {
        const std::size_t depth = depth_from(index);
        if (depth > kCanonicalMaskMaxDepth)
        {
            return make_error(ErrorCode::kValidation,
                              "Mask graph exceeds the canonical depth limit",
                              {{"reason", "mask_graph_too_deep"},
                               {"mask_id", masks[index].id},
                               {"depth", std::to_string(depth)}});
        }
        const std::size_t expansion = expansion_from(index);
        if (expansion > kCanonicalMaskMaxExpandedNodes)
        {
            return make_error(ErrorCode::kValidation,
                              "Mask graph exceeds the canonical evaluation-work limit",
                              {{"reason", "mask_graph_expansion_too_large"},
                               {"mask_id", masks[index].id},
                               {"expanded_nodes", std::to_string(expansion)}});
        }
    }
    return {};
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Canonical mask validation allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<JsonValue> canonical_mask_to_json(const Mask &mask)
try
{
    auto valid = validate_mask_node(mask);
    if (!valid)
    {
        return valid.error();
    }
    JsonObject object{{"id", mask.id},
                      {"kind", std::string(mask_kind_name(mask.kind))},
                      {"schema_version", integer_json(kCanonicalMaskSchemaVersion)},
                      {"inverted", mask.common.inverted}};
    auto opacity = add_number(object, "opacity", mask.common.opacity);
    if (!opacity)
    {
        return opacity.error();
    }
    const auto add = [&](const std::string_view key, const double value) -> Result<void>
    { return add_number(object, key, value); };
    switch (mask.kind)
    {
    case MaskKind::kAll:
        break;
    case MaskKind::kLinearGradient:
    {
        const auto &gradient = std::get<LinearGradientMask>(mask.payload);
        for (const auto &[key, value] : std::array<std::pair<std::string_view, double>, 4>{
                 {{"anchor_x", gradient.anchor_x},
                  {"anchor_y", gradient.anchor_y},
                  {"rotation_degrees", gradient.rotation_degrees},
                  {"transition", gradient.transition}}})
        {
            auto field = add(key, value);
            if (!field)
            {
                return field.error();
            }
        }
        break;
    }
    case MaskKind::kCircle:
    {
        const auto &circle = std::get<CircleMask>(mask.payload);
        for (const auto &[key, value] :
             std::array<std::pair<std::string_view, double>, 4>{{{"center_x", circle.center_x},
                                                                 {"center_y", circle.center_y},
                                                                 {"radius", circle.radius},
                                                                 {"feather", circle.feather}}})
        {
            auto field = add(key, value);
            if (!field)
            {
                return field.error();
            }
        }
        break;
    }
    case MaskKind::kEllipse:
    {
        const auto &ellipse = std::get<EllipseMask>(mask.payload);
        for (const auto &[key, value] : std::array<std::pair<std::string_view, double>, 6>{
                 {{"center_x", ellipse.center_x},
                  {"center_y", ellipse.center_y},
                  {"radius_x", ellipse.radius_x},
                  {"radius_y", ellipse.radius_y},
                  {"rotation_degrees", ellipse.rotation_degrees},
                  {"feather", ellipse.feather}}})
        {
            auto field = add(key, value);
            if (!field)
            {
                return field.error();
            }
        }
        break;
    }
    case MaskKind::kParametric:
    {
        const auto &parametric = std::get<ParametricMask>(mask.payload);
        object.emplace("source", std::string(parametric_source_name(parametric.source)));
        object.emplace("channel", std::string(parametric_channel_name(parametric.channel)));
        JsonValue::Array thresholds;
        thresholds.reserve(parametric.thresholds.size());
        for (const double threshold : parametric.thresholds)
        {
            auto json = number_json(threshold);
            if (!json)
            {
                return json.error();
            }
            thresholds.emplace_back(std::move(json).value());
        }
        object.emplace("thresholds", std::move(thresholds));
        break;
    }
    case MaskKind::kGroup:
    {
        const auto &group = std::get<MaskGroup>(mask.payload);
        JsonValue::Array children;
        children.reserve(group.children.size());
        for (const auto &child : group.children)
        {
            auto child_opacity = number_json(child.opacity);
            if (!child_opacity)
            {
                return child_opacity.error();
            }
            children.emplace_back(
                JsonObject{{"inverted", child.inverted},
                           {"mask_id", child.mask_id},
                           {"opacity", std::move(child_opacity).value()},
                           {"operator", std::string(group_operator_name(child.operation))}});
        }
        object.emplace("children", std::move(children));
        break;
    }
    case MaskKind::kPath:
    {
        const auto &path_mask = std::get<PathMask>(mask.payload);
        auto feather = add("feather", path_mask.feather);
        if (!feather)
        {
            return feather.error();
        }
        JsonValue::Array points;
        points.reserve(path_mask.points.size());
        for (const auto &point : path_mask.points)
        {
            JsonObject point_object;
            for (const auto &[key, value] :
                 std::array<std::pair<std::string_view, double>, 6>{{{"x", point.x},
                                                                     {"y", point.y},
                                                                     {"ctrl1_x", point.ctrl1_x},
                                                                     {"ctrl1_y", point.ctrl1_y},
                                                                     {"ctrl2_x", point.ctrl2_x},
                                                                     {"ctrl2_y", point.ctrl2_y}}})
            {
                auto field = add_number(point_object, key, value);
                if (!field)
                {
                    return field.error();
                }
            }
            points.emplace_back(std::move(point_object));
        }
        object.emplace("points", std::move(points));
        break;
    }
    case MaskKind::kBrush:
    {
        const auto &brush = std::get<BrushMask>(mask.payload);
        JsonValue::Array points;
        points.reserve(brush.points.size());
        for (const auto &point : brush.points)
        {
            JsonObject point_object;
            for (const auto &[key, value] :
                 std::array<std::pair<std::string_view, double>, 9>{{{"x", point.x},
                                                                     {"y", point.y},
                                                                     {"ctrl1_x", point.ctrl1_x},
                                                                     {"ctrl1_y", point.ctrl1_y},
                                                                     {"ctrl2_x", point.ctrl2_x},
                                                                     {"ctrl2_y", point.ctrl2_y},
                                                                     {"radius", point.radius},
                                                                     {"hardness", point.hardness},
                                                                     {"density", point.density}}})
            {
                auto field = add_number(point_object, key, value);
                if (!field)
                {
                    return field.error();
                }
            }
            points.emplace_back(std::move(point_object));
        }
        object.emplace("points", std::move(points));
        break;
    }
    }
    return JsonValue{std::move(object)};
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Canonical mask serialization allocation failed",
                      {{"reason", "allocation_failed"}});
}

double normalized_display_mask_channel(const std::uint8_t red, const std::uint8_t green,
                                       const std::uint8_t blue,
                                       const std::int64_t channel_index) noexcept
{
    switch (channel_index)
    {
    case 1:
        return static_cast<double>(red) / 255.0;
    case 2:
        return static_cast<double>(green) / 255.0;
    case 3:
        return static_cast<double>(blue) / 255.0;
    default:
        // ADR-0061 / collect_rgb_histogram display luma weights.
        return std::clamp((0.2126 * static_cast<double>(red) + 0.7152 * static_cast<double>(green) +
                           0.0722 * static_cast<double>(blue)) /
                              255.0,
                          0.0, 1.0);
    }
}

Result<std::array<double, 4>>
parametric_thresholds_from_histogram_assist(const double sample,
                                            const std::array<std::uint32_t, 256> *bins) noexcept
{
    if (!std::isfinite(sample) || sample < 0.0 || sample > 1.0)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Parametric assist sample must be a finite unit value",
                          {{"reason", "invalid_parametric_assist_sample"}});
    }

    constexpr double kDefaultHalfWidth = 16.0 / 255.0;
    double lo = sample - 2.0 * kDefaultHalfWidth;
    double hi = sample + 2.0 * kDefaultHalfWidth;
    if (bins != nullptr)
    {
        const int center = static_cast<int>(std::clamp(std::lround(sample * 255.0), 0L, 255L));
        std::uint32_t peak = (*bins)[static_cast<std::size_t>(center)];
        const int window_lo = std::max(0, center - 8);
        const int window_hi = std::min(255, center + 8);
        for (int index = window_lo; index <= window_hi; ++index)
        {
            peak = std::max(peak, (*bins)[static_cast<std::size_t>(index)]);
        }
        const std::uint32_t floor = std::max<std::uint32_t>(1U, peak / 50U);
        int left = center;
        int right = center;
        while (left > 0 && (*bins)[static_cast<std::size_t>(left - 1)] >= floor)
        {
            --left;
        }
        while (right < 255 && (*bins)[static_cast<std::size_t>(right + 1)] >= floor)
        {
            ++right;
        }
        lo = static_cast<double>(left) / 255.0;
        hi = static_cast<double>(right) / 255.0;
    }

    lo = std::clamp(lo, 0.0, 1.0);
    hi = std::clamp(hi, 0.0, 1.0);
    if (hi < lo)
    {
        std::swap(lo, hi);
    }
    const double width = hi - lo;
    const double soft = std::max(1.0 / 255.0, width * 0.25);
    double t0 = lo;
    double t3 = hi;
    double t1 = std::min(t3, t0 + soft);
    double t2 = std::max(t0, t3 - soft);
    if (t1 > t2)
    {
        t1 = t0;
        t2 = t3;
    }
    if (!(t0 <= t1 && t1 <= t2 && t2 <= t3))
    {
        return make_error(ErrorCode::kValidation,
                          "Parametric assist produced non-monotonic thresholds",
                          {{"reason", "invalid_parametric_assist_thresholds"}});
    }
    return std::array<double, 4>{t0, t1, t2, t3};
}

} // namespace ravo
