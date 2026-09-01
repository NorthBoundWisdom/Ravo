#include "ravo/recipe/develop_mask.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <utility>

namespace ravo
{
namespace
{

enum class MaskField
{
    kWhole,
    kKind,
    kOpacity,
    kInverted,
    kAnchorX,
    kAnchorY,
    kRotationDegrees,
    kTransition,
    kCenterX,
    kCenterY,
    kRadius,
    kFeather,
    kRadiusX,
    kRadiusY,
    kSource,
    kChannel,
    kThreshold0,
    kThreshold1,
    kThreshold2,
    kThreshold3,
    kChildIndex,
    kAddChild,
    kRemoveChild,
    kChildOperator,
    kChildOpacity,
    kChildInverted,
    kChildKind,
    kPointIndex,
    kPointX,
    kPointY,
    kPointRadius,
    kPointHardness,
    kPointDensity,
    kAddPoint,
    kRemovePoint,
    kPathFeather,
};

struct ParsedMaskField
{
    DevelopMaskTarget target;
    MaskField field;
};

[[nodiscard]] std::optional<ParsedMaskField> parse_mask_field(const std::string_view field) noexcept
{
    const auto parse_suffix = [](const DevelopMaskTarget target,
                                 const std::string_view suffix) -> std::optional<ParsedMaskField>
    {
        const auto parsed = [target](const MaskField value)
        { return std::optional<ParsedMaskField>{{target, value}}; };
        if (suffix.empty())
            return parsed(MaskField::kWhole);
        if (suffix == "Kind")
            return parsed(MaskField::kKind);
        if (suffix == "Opacity")
            return parsed(MaskField::kOpacity);
        if (suffix == "Inverted")
            return parsed(MaskField::kInverted);
        if (suffix == "AnchorX")
            return parsed(MaskField::kAnchorX);
        if (suffix == "AnchorY")
            return parsed(MaskField::kAnchorY);
        if (suffix == "RotationDegrees")
            return parsed(MaskField::kRotationDegrees);
        if (suffix == "Transition")
            return parsed(MaskField::kTransition);
        if (suffix == "CenterX")
            return parsed(MaskField::kCenterX);
        if (suffix == "CenterY")
            return parsed(MaskField::kCenterY);
        if (suffix == "Radius")
            return parsed(MaskField::kRadius);
        if (suffix == "Feather")
            return parsed(MaskField::kFeather);
        if (suffix == "RadiusX")
            return parsed(MaskField::kRadiusX);
        if (suffix == "RadiusY")
            return parsed(MaskField::kRadiusY);
        if (suffix == "Source")
            return parsed(MaskField::kSource);
        if (suffix == "Channel")
            return parsed(MaskField::kChannel);
        if (suffix == "Threshold0")
            return parsed(MaskField::kThreshold0);
        if (suffix == "Threshold1")
            return parsed(MaskField::kThreshold1);
        if (suffix == "Threshold2")
            return parsed(MaskField::kThreshold2);
        if (suffix == "Threshold3")
            return parsed(MaskField::kThreshold3);
        if (suffix == "ChildIndex")
            return parsed(MaskField::kChildIndex);
        if (suffix == "AddChild")
            return parsed(MaskField::kAddChild);
        if (suffix == "RemoveChild")
            return parsed(MaskField::kRemoveChild);
        if (suffix == "ChildOperator")
            return parsed(MaskField::kChildOperator);
        if (suffix == "ChildOpacity")
            return parsed(MaskField::kChildOpacity);
        if (suffix == "ChildInverted")
            return parsed(MaskField::kChildInverted);
        if (suffix == "ChildKind")
            return parsed(MaskField::kChildKind);
        if (suffix == "PointIndex")
            return parsed(MaskField::kPointIndex);
        if (suffix == "PointX")
            return parsed(MaskField::kPointX);
        if (suffix == "PointY")
            return parsed(MaskField::kPointY);
        if (suffix == "PointRadius")
            return parsed(MaskField::kPointRadius);
        if (suffix == "PointHardness")
            return parsed(MaskField::kPointHardness);
        if (suffix == "PointDensity")
            return parsed(MaskField::kPointDensity);
        if (suffix == "AddPoint")
            return parsed(MaskField::kAddPoint);
        if (suffix == "RemovePoint")
            return parsed(MaskField::kRemovePoint);
        if (suffix == "PathFeather")
            return parsed(MaskField::kPathFeather);
        return std::nullopt;
    };

    if (field.starts_with(kColorHarmonizerMaskFieldPrefix))
    {
        return parse_suffix(DevelopMaskTarget::kColorHarmonizer,
                            field.substr(kColorHarmonizerMaskFieldPrefix.size()));
    }
    if (field.starts_with(kGraduatedMaskFieldPrefix))
    {
        return parse_suffix(DevelopMaskTarget::kGraduatedNd,
                            field.substr(kGraduatedMaskFieldPrefix.size()));
    }
    if (field.starts_with(kColorBalanceRgbMaskFieldPrefix))
    {
        return parse_suffix(DevelopMaskTarget::kColorBalanceRgb,
                            field.substr(kColorBalanceRgbMaskFieldPrefix.size()));
    }
    if (field.starts_with(kExposureMaskFieldPrefix))
    {
        return parse_suffix(DevelopMaskTarget::kExposure,
                            field.substr(kExposureMaskFieldPrefix.size()));
    }
    if (field.starts_with(kRgbCurveMaskFieldPrefix))
    {
        return parse_suffix(DevelopMaskTarget::kRgbCurve,
                            field.substr(kRgbCurveMaskFieldPrefix.size()));
    }
    return std::nullopt;
}

[[nodiscard]] std::string_view studio_mask_id_prefix(const DevelopMaskTarget target) noexcept
{
    switch (target)
    {
    case DevelopMaskTarget::kColorHarmonizer:
        return "ravo.studio.mask.color_harmonizer.";
    case DevelopMaskTarget::kGraduatedNd:
        return "ravo.studio.mask.graduatednd.";
    case DevelopMaskTarget::kColorBalanceRgb:
        return "ravo.studio.mask.color_balance_rgb.";
    case DevelopMaskTarget::kExposure:
        return "ravo.studio.mask.exposure.";
    case DevelopMaskTarget::kRgbCurve:
        return "ravo.studio.mask.rgb_curve.";
    }
    return {};
}

[[nodiscard]] bool studio_owns_mask_id(const DevelopMaskTarget target,
                                       const std::string_view id) noexcept
{
    const auto prefix = studio_mask_id_prefix(target);
    if (!id.starts_with(prefix) || id.size() == prefix.size())
    {
        return false;
    }
    const auto suffix = id.substr(prefix.size());
    if (suffix.front() == '0')
    {
        return false;
    }
    return std::all_of(suffix.begin(), suffix.end(),
                       [](const char character) { return character >= '0' && character <= '9'; });
}

[[nodiscard]] std::optional<std::string> &mask_attachment(DevelopParams &params,
                                                          const DevelopMaskTarget target) noexcept
{
    switch (target)
    {
    case DevelopMaskTarget::kColorHarmonizer:
        return params.color_harmonizer_mask_id;
    case DevelopMaskTarget::kGraduatedNd:
        return params.graduated_mask_id;
    case DevelopMaskTarget::kColorBalanceRgb:
        return params.color_balance_rgb_mask_id;
    case DevelopMaskTarget::kExposure:
        return params.exposure_mask_id;
    case DevelopMaskTarget::kRgbCurve:
        return params.rgb_curve_mask_id;
    }
    return params.color_harmonizer_mask_id;
}

[[nodiscard]] const std::optional<std::string> &
mask_attachment(const DevelopParams &params, const DevelopMaskTarget target) noexcept
{
    switch (target)
    {
    case DevelopMaskTarget::kColorHarmonizer:
        return params.color_harmonizer_mask_id;
    case DevelopMaskTarget::kGraduatedNd:
        return params.graduated_mask_id;
    case DevelopMaskTarget::kColorBalanceRgb:
        return params.color_balance_rgb_mask_id;
    case DevelopMaskTarget::kExposure:
        return params.exposure_mask_id;
    case DevelopMaskTarget::kRgbCurve:
        return params.rgb_curve_mask_id;
    }
    return params.color_harmonizer_mask_id;
}

[[nodiscard]] Mask *find_mask(std::vector<Mask> &masks, const std::string_view id) noexcept
{
    const auto found =
        std::find_if(masks.begin(), masks.end(), [id](const Mask &mask) { return mask.id == id; });
    return found == masks.end() ? nullptr : &*found;
}

[[nodiscard]] const Mask *find_mask(const std::vector<Mask> &masks,
                                    const std::string_view id) noexcept
{
    const auto found =
        std::find_if(masks.begin(), masks.end(), [id](const Mask &mask) { return mask.id == id; });
    return found == masks.end() ? nullptr : &*found;
}

[[nodiscard]] bool develop_mask_attachments_resolve(const DevelopParams &params) noexcept
{
    for (const auto target : {DevelopMaskTarget::kColorHarmonizer, DevelopMaskTarget::kGraduatedNd,
                              DevelopMaskTarget::kColorBalanceRgb, DevelopMaskTarget::kExposure,
                              DevelopMaskTarget::kRgbCurve})
    {
        const auto &attachment = mask_attachment(params, target);
        if (attachment && find_mask(params.masks, *attachment) == nullptr)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool has_group_edge_to(const DevelopParams &params,
                                     const std::string_view id) noexcept
{
    for (const auto &mask : params.masks)
    {
        const auto *group = std::get_if<MaskGroup>(&mask.payload);
        if (group == nullptr)
        {
            continue;
        }
        if (std::any_of(group->children.begin(), group->children.end(),
                        [id](const MaskGroupChild &child) { return child.mask_id == id; }))
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool is_attached_by_other_operation(const DevelopParams &params,
                                                  const DevelopMaskTarget target,
                                                  const std::string_view id) noexcept
{
    for (const auto other :
         {DevelopMaskTarget::kColorHarmonizer, DevelopMaskTarget::kGraduatedNd,
          DevelopMaskTarget::kColorBalanceRgb, DevelopMaskTarget::kExposure,
          DevelopMaskTarget::kRgbCurve})
    {
        if (other == target)
            continue;
        const auto &other_attachment = mask_attachment(params, other);
        if (other_attachment.has_value() && *other_attachment == id)
            return true;
    }
    return false;
}

[[nodiscard]] TaskError mask_edit_error(const ErrorCode code, std::string message,
                                        const std::string_view reason,
                                        const DevelopMaskTarget target,
                                        const std::string_view field)
{
    return make_error(code, std::move(message),
                      {{"field", std::string(field)},
                       {"reason", std::string(reason)},
                       {"target", std::string(develop_mask_target_name(target))}});
}

[[nodiscard]] TaskError decorate_mask_error(TaskError error, const DevelopMaskTarget target,
                                            const std::string_view field)
{
    error.context.insert_or_assign("field", std::string(field));
    error.context.insert_or_assign("target", std::string(develop_mask_target_name(target)));
    if (!error.context.contains("reason"))
    {
        error.context.emplace("reason", "invalid_develop_mask_graph");
    }
    return error;
}

[[nodiscard]] Result<void> validate_develop_mask_state(const DevelopParams &params,
                                                       const DevelopMaskTarget target,
                                                       const std::string_view field)
{
    auto graph = validate_mask_graph(params.masks);
    if (!graph)
    {
        return decorate_mask_error(graph.error(), target, field);
    }
    if (!develop_mask_attachments_resolve(params))
    {
        return mask_edit_error(ErrorCode::kValidation,
                               "Develop mask attachment does not resolve to a canonical node",
                               "missing_develop_mask_attachment", target, field);
    }
    return {};
}

[[nodiscard]] std::int64_t mask_kind_index(const MaskKind kind) noexcept
{
    switch (kind)
    {
    case MaskKind::kAll:
        return 1;
    case MaskKind::kLinearGradient:
        return 2;
    case MaskKind::kCircle:
        return 3;
    case MaskKind::kEllipse:
        return 4;
    case MaskKind::kParametric:
        return 5;
    case MaskKind::kGroup:
        return 6;
    case MaskKind::kPath:
        return 7;
    case MaskKind::kBrush:
        return 8;
    }
    return -1;
}

[[nodiscard]] std::optional<MaskKind> mask_kind_from_index(const std::int64_t index) noexcept
{
    switch (index)
    {
    case 1:
        return MaskKind::kAll;
    case 2:
        return MaskKind::kLinearGradient;
    case 3:
        return MaskKind::kCircle;
    case 4:
        return MaskKind::kEllipse;
    case 5:
        return MaskKind::kParametric;
    case 6:
        return MaskKind::kGroup;
    case 7:
        return MaskKind::kPath;
    case 8:
        return MaskKind::kBrush;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] std::int64_t &child_index_slot(DevelopParams &params,
                                             const DevelopMaskTarget target) noexcept
{
    switch (target)
    {
    case DevelopMaskTarget::kColorHarmonizer:
        return params.color_harmonizer_mask_child_index;
    case DevelopMaskTarget::kGraduatedNd:
        return params.graduated_mask_child_index;
    case DevelopMaskTarget::kColorBalanceRgb:
        return params.color_balance_rgb_mask_child_index;
    case DevelopMaskTarget::kExposure:
        return params.exposure_mask_child_index;
    case DevelopMaskTarget::kRgbCurve:
        return params.rgb_curve_mask_child_index;
    }
    return params.color_harmonizer_mask_child_index;
}

[[nodiscard]] const std::int64_t &child_index_slot(const DevelopParams &params,
                                                   const DevelopMaskTarget target) noexcept
{
    switch (target)
    {
    case DevelopMaskTarget::kColorHarmonizer:
        return params.color_harmonizer_mask_child_index;
    case DevelopMaskTarget::kGraduatedNd:
        return params.graduated_mask_child_index;
    case DevelopMaskTarget::kColorBalanceRgb:
        return params.color_balance_rgb_mask_child_index;
    case DevelopMaskTarget::kExposure:
        return params.exposure_mask_child_index;
    case DevelopMaskTarget::kRgbCurve:
        return params.rgb_curve_mask_child_index;
    }
    return params.color_harmonizer_mask_child_index;
}

[[nodiscard]] std::int64_t &point_index_slot(DevelopParams &params,
                                             const DevelopMaskTarget target) noexcept
{
    switch (target)
    {
    case DevelopMaskTarget::kColorHarmonizer:
        return params.color_harmonizer_mask_point_index;
    case DevelopMaskTarget::kGraduatedNd:
        return params.graduated_mask_point_index;
    case DevelopMaskTarget::kColorBalanceRgb:
        return params.color_balance_rgb_mask_point_index;
    case DevelopMaskTarget::kExposure:
        return params.exposure_mask_point_index;
    case DevelopMaskTarget::kRgbCurve:
        return params.rgb_curve_mask_point_index;
    }
    return params.color_harmonizer_mask_point_index;
}

[[nodiscard]] const std::int64_t &point_index_slot(const DevelopParams &params,
                                                   const DevelopMaskTarget target) noexcept
{
    switch (target)
    {
    case DevelopMaskTarget::kColorHarmonizer:
        return params.color_harmonizer_mask_point_index;
    case DevelopMaskTarget::kGraduatedNd:
        return params.graduated_mask_point_index;
    case DevelopMaskTarget::kColorBalanceRgb:
        return params.color_balance_rgb_mask_point_index;
    case DevelopMaskTarget::kExposure:
        return params.exposure_mask_point_index;
    case DevelopMaskTarget::kRgbCurve:
        return params.rgb_curve_mask_point_index;
    }
    return params.color_harmonizer_mask_point_index;
}

void clamp_cursor(std::int64_t &index, const std::size_t count) noexcept
{
    if (count == 0U)
    {
        index = 0;
        return;
    }
    if (index < 0)
    {
        index = 0;
        return;
    }
    if (static_cast<std::size_t>(index) >= count)
    {
        index = static_cast<std::int64_t>(count - 1U);
    }
}

void smooth_path_handles(std::vector<PathMaskPoint> &points) noexcept
{
    const std::size_t count = points.size();
    if (count < kCanonicalMaskMinPathPoints)
    {
        return;
    }
    std::vector<PathMaskPoint> next = points;
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto &prev = points[(index + count - 1U) % count];
        const auto &curr = points[index];
        const auto &succ = points[(index + 1U) % count];
        const double dx = succ.x - prev.x;
        const double dy = succ.y - prev.y;
        const double length = std::hypot(dx, dy);
        const double incoming = std::hypot(curr.x - prev.x, curr.y - prev.y) / 3.0;
        const double outgoing = std::hypot(succ.x - curr.x, succ.y - curr.y) / 3.0;
        if (length <= 0.0)
        {
            next[index].ctrl1_x = curr.x;
            next[index].ctrl1_y = curr.y;
            next[index].ctrl2_x = curr.x;
            next[index].ctrl2_y = curr.y;
            continue;
        }
        const double ux = dx / length;
        const double uy = dy / length;
        next[index].ctrl1_x = std::clamp(curr.x - ux * incoming, kCanonicalMaskUnitMin, kCanonicalMaskUnitMax);
        next[index].ctrl1_y = std::clamp(curr.y - uy * incoming, kCanonicalMaskUnitMin, kCanonicalMaskUnitMax);
        next[index].ctrl2_x = std::clamp(curr.x + ux * outgoing, kCanonicalMaskUnitMin, kCanonicalMaskUnitMax);
        next[index].ctrl2_y = std::clamp(curr.y + uy * outgoing, kCanonicalMaskUnitMin, kCanonicalMaskUnitMax);
    }
    points = std::move(next);
}

void smooth_brush_handles(std::vector<BrushMaskPoint> &points) noexcept
{
    const std::size_t count = points.size();
    if (count < kCanonicalMaskMinBrushPoints)
    {
        return;
    }
    std::vector<BrushMaskPoint> next = points;
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto &prev = points[index == 0U ? 0U : index - 1U];
        const auto &curr = points[index];
        const auto &succ = points[index + 1U == count ? count - 1U : index + 1U];
        const double dx = succ.x - prev.x;
        const double dy = succ.y - prev.y;
        const double length = std::hypot(dx, dy);
        if (length <= 0.0)
        {
            next[index].ctrl1_x = curr.x;
            next[index].ctrl1_y = curr.y;
            next[index].ctrl2_x = curr.x;
            next[index].ctrl2_y = curr.y;
            continue;
        }
        const double ux = dx / length;
        const double uy = dy / length;
        const double incoming = std::hypot(curr.x - prev.x, curr.y - prev.y) / 3.0;
        const double outgoing = std::hypot(succ.x - curr.x, succ.y - curr.y) / 3.0;
        next[index].ctrl1_x = std::clamp(curr.x - ux * incoming, kCanonicalMaskUnitMin, kCanonicalMaskUnitMax);
        next[index].ctrl1_y = std::clamp(curr.y - uy * incoming, kCanonicalMaskUnitMin, kCanonicalMaskUnitMax);
        next[index].ctrl2_x = std::clamp(curr.x + ux * outgoing, kCanonicalMaskUnitMin, kCanonicalMaskUnitMax);
        next[index].ctrl2_y = std::clamp(curr.y + uy * outgoing, kCanonicalMaskUnitMin, kCanonicalMaskUnitMax);
    }
    points = std::move(next);
}

[[nodiscard]] PathMask default_path_mask()
{
    PathMask path;
    path.feather = 0.05;
    path.points = {{0.35, 0.30}, {0.65, 0.30}, {0.50, 0.70}};
    smooth_path_handles(path.points);
    return path;
}

[[nodiscard]] BrushMask default_brush_mask()
{
    BrushMask brush;
    brush.points = {{0.30, 0.50, 0.30, 0.50, 0.30, 0.50, 0.05, 0.5, 1.0},
                    {0.70, 0.50, 0.70, 0.50, 0.70, 0.50, 0.05, 0.5, 1.0}};
    smooth_brush_handles(brush.points);
    return brush;
}

[[nodiscard]] Mask make_kind_mask(std::string id, const MaskKind kind)
{
    Mask mask{std::move(id), kCanonicalMaskSchemaVersion, kind};
    if (kind == MaskKind::kPath)
    {
        mask.payload = default_path_mask();
    }
    else if (kind == MaskKind::kBrush)
    {
        mask.payload = default_brush_mask();
    }
    else if (kind == MaskKind::kGroup)
    {
        mask.payload = MaskGroup{};
    }
    return mask;
}

[[nodiscard]] std::size_t reference_count(const DevelopParams &params, const std::string_view id) noexcept
{
    std::size_t count = 0U;
    if (params.color_harmonizer_mask_id && *params.color_harmonizer_mask_id == id)
    {
        ++count;
    }
    if (params.graduated_mask_id && *params.graduated_mask_id == id)
    {
        ++count;
    }
    if (params.color_balance_rgb_mask_id && *params.color_balance_rgb_mask_id == id)
    {
        ++count;
    }
    if (params.exposure_mask_id && *params.exposure_mask_id == id)
    {
        ++count;
    }
    if (params.rgb_curve_mask_id && *params.rgb_curve_mask_id == id)
    {
        ++count;
    }
    for (const auto &mask : params.masks)
    {
        const auto *group = std::get_if<MaskGroup>(&mask.payload);
        if (group == nullptr)
        {
            continue;
        }
        count += static_cast<std::size_t>(
            std::count_if(group->children.begin(), group->children.end(),
                          [id](const MaskGroupChild &child) { return child.mask_id == id; }));
    }
    return count;
}

void erase_mask_id(DevelopParams &params, const std::string_view id)
{
    params.masks.erase(std::remove_if(params.masks.begin(), params.masks.end(),
                                      [id](const Mask &mask) { return mask.id == id; }),
                       params.masks.end());
}

void delete_unreferenced_studio_children(DevelopParams &params, const MaskGroup &group,
                                         const DevelopMaskTarget target)
{
    for (const auto &child : group.children)
    {
        if (studio_owns_mask_id(target, child.mask_id) && reference_count(params, child.mask_id) == 0U)
        {
            erase_mask_id(params, child.mask_id);
        }
    }
}

[[nodiscard]] Mask *selected_group_child(DevelopParams &params, const DevelopMaskTarget target)
{
    const auto &attachment = mask_attachment(params, target);
    if (!attachment)
    {
        return nullptr;
    }
    auto *group_mask = find_mask(params.masks, *attachment);
    if (group_mask == nullptr)
    {
        return nullptr;
    }
    auto *group = std::get_if<MaskGroup>(&group_mask->payload);
    if (group == nullptr || group->children.empty())
    {
        return nullptr;
    }
    clamp_cursor(child_index_slot(params, target), group->children.size());
    const auto index = static_cast<std::size_t>(child_index_slot(params, target));
    return find_mask(params.masks, group->children[index].mask_id);
}

[[nodiscard]] Mask *edited_leaf(DevelopParams &params, const DevelopMaskTarget target)
{
    const auto &attachment = mask_attachment(params, target);
    if (!attachment)
    {
        return nullptr;
    }
    auto *mask = find_mask(params.masks, *attachment);
    if (mask == nullptr)
    {
        return nullptr;
    }
    if (mask->kind == MaskKind::kGroup)
    {
        return selected_group_child(params, target);
    }
    return mask;
}

[[nodiscard]] Result<std::int64_t> exact_index(const double value, const std::int64_t minimum,
                                               const std::int64_t maximum,
                                               const DevelopMaskTarget target,
                                               const std::string_view field,
                                               const std::string_view reason)
{
    if (!std::isfinite(value) || value != std::trunc(value) ||
        value < static_cast<double>(minimum) || value > static_cast<double>(maximum))
    {
        return mask_edit_error(ErrorCode::kInvalidArgument,
                               "Develop mask selector must be a known exact integer", reason,
                               target, field);
    }
    return static_cast<std::int64_t>(value);
}

[[nodiscard]] Result<void> finite_range(const double value, const double minimum,
                                        const double maximum, const DevelopMaskTarget target,
                                        const std::string_view field)
{
    if (!std::isfinite(value) || value < minimum || value > maximum)
    {
        return mask_edit_error(ErrorCode::kInvalidArgument,
                               "Develop mask value is outside the canonical bounds",
                               "invalid_develop_mask_value", target, field);
    }
    return {};
}

[[nodiscard]] Result<void> require_editable(const DevelopParams &params,
                                            const DevelopMaskTarget target,
                                            const std::string_view field)
{
    const auto state = develop_mask_editor_state(params, target);
    if (!state.attached)
    {
        return mask_edit_error(ErrorCode::kNotFound,
                               "Develop mask field requires an attached canonical mask",
                               "missing_develop_mask_attachment", target, field);
    }
    if (state.editable)
    {
        return {};
    }
    const auto reason = develop_mask_attachment_status_name(state.status);
    return mask_edit_error(ErrorCode::kUnsupported,
                           "Attached Develop mask is read-only for Studio authoring", reason,
                           target, field);
}

void enable_target_operation(DevelopParams &params, const DevelopMaskTarget target) noexcept
{
    switch (target)
    {
    case DevelopMaskTarget::kColorHarmonizer:
        params.color_harmonizer_present = true;
        params.color_harmonizer_enabled = true;
        return;
    case DevelopMaskTarget::kGraduatedNd:
        params.graduated_present = true;
        params.graduated_enabled = true;
        return;
    case DevelopMaskTarget::kColorBalanceRgb:
        return;
    case DevelopMaskTarget::kExposure:
        return;
    case DevelopMaskTarget::kRgbCurve:
        return;
    }
}

[[nodiscard]] Result<std::string> next_studio_mask_id(const DevelopParams &params,
                                                      const DevelopMaskTarget target,
                                                      const std::string_view field)
{
    if (params.masks.size() >= kCanonicalMaskMaxNodes)
    {
        return mask_edit_error(ErrorCode::kValidation,
                               "Canonical mask graph has reached its node limit",
                               "develop_mask_node_limit", target, field);
    }
    const auto prefix = studio_mask_id_prefix(target);
    for (std::uint64_t ordinal = 1U; ordinal < std::numeric_limits<std::uint64_t>::max(); ++ordinal)
    {
        std::string candidate{prefix};
        candidate += std::to_string(ordinal);
        if (find_mask(params.masks, candidate) == nullptr)
        {
            return candidate;
        }
    }
    return mask_edit_error(ErrorCode::kConflict,
                           "Unable to allocate a collision-safe Studio mask ID",
                           "develop_mask_id_exhausted", target, field);
}

[[nodiscard]] Result<void> detach_mask(DevelopParams &params, const DevelopMaskTarget target,
                                       const std::string_view field)
{
    auto valid = validate_develop_mask_state(params, target, field);
    if (!valid)
    {
        return valid.error();
    }
    auto &attachment = mask_attachment(params, target);
    if (!attachment)
    {
        return {};
    }
    const auto state = develop_mask_editor_state(params, target);
    const std::string id = *attachment;
    const Mask *attached = find_mask(params.masks, id);
    MaskGroup owned_group;
    const bool owned_group_detach =
        attached != nullptr && attached->kind == MaskKind::kGroup &&
        state.status == DevelopMaskAttachmentStatus::kEditable;
    if (owned_group_detach)
    {
        owned_group = std::get<MaskGroup>(attached->payload);
    }
    attachment.reset();
    if (state.status == DevelopMaskAttachmentStatus::kEditable)
    {
        erase_mask_id(params, id);
        if (owned_group_detach)
        {
            delete_unreferenced_studio_children(params, owned_group, target);
        }
    }
    child_index_slot(params, target) = 0;
    point_index_slot(params, target) = 0;
    return validate_develop_mask_state(params, target, field);
}

[[nodiscard]] Result<void> type_mismatch(const Mask &, const DevelopMaskTarget target,
                                         const std::string_view field)
{
    return mask_edit_error(ErrorCode::kUnsupported,
                           "Develop mask field is unavailable for the attached mask kind",
                           "develop_mask_field_kind_mismatch", target, field);
}

[[nodiscard]] Result<void> apply_mask_value(Mask &mask, const MaskField field, const double value,
                                            const DevelopMaskTarget target,
                                            const std::string_view field_name)
{
    switch (field)
    {
    case MaskField::kOpacity:
    {
        auto valid =
            finite_range(value, kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, target, field_name);
        if (!valid)
            return valid.error();
        mask.common.opacity = value;
        return {};
    }
    case MaskField::kInverted:
    {
        auto parsed = exact_index(value, 0, 1, target, field_name, "invalid_develop_mask_boolean");
        if (!parsed)
            return parsed.error();
        mask.common.inverted = parsed.value() == 1;
        return {};
    }
    case MaskField::kAnchorX:
    case MaskField::kAnchorY:
    case MaskField::kTransition:
    {
        auto *gradient = std::get_if<LinearGradientMask>(&mask.payload);
        if (gradient == nullptr)
            return type_mismatch(mask, target, field_name);
        auto valid =
            finite_range(value, kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, target, field_name);
        if (!valid)
            return valid.error();
        if (field == MaskField::kAnchorX)
            gradient->anchor_x = value;
        else if (field == MaskField::kAnchorY)
            gradient->anchor_y = value;
        else
            gradient->transition = value;
        return {};
    }
    case MaskField::kRotationDegrees:
    {
        auto valid =
            finite_range(value, kCanonicalMaskAngleMin, kCanonicalMaskAngleMax, target, field_name);
        if (!valid)
            return valid.error();
        if (auto *gradient = std::get_if<LinearGradientMask>(&mask.payload); gradient != nullptr)
        {
            gradient->rotation_degrees = value;
            return {};
        }
        if (auto *ellipse = std::get_if<EllipseMask>(&mask.payload); ellipse != nullptr)
        {
            ellipse->rotation_degrees = value;
            return {};
        }
        return type_mismatch(mask, target, field_name);
    }
    case MaskField::kCenterX:
    case MaskField::kCenterY:
    case MaskField::kRadius:
    case MaskField::kFeather:
    {
        auto *circle = std::get_if<CircleMask>(&mask.payload);
        if (circle == nullptr)
            return type_mismatch(mask, target, field_name);
        const double minimum =
            field == MaskField::kRadius ? kCanonicalMaskPositiveMin : kCanonicalMaskUnitMin;
        auto valid = finite_range(value, minimum, kCanonicalMaskUnitMax, target, field_name);
        if (!valid)
            return valid.error();
        if (field == MaskField::kCenterX)
            circle->center_x = value;
        else if (field == MaskField::kCenterY)
            circle->center_y = value;
        else if (field == MaskField::kRadius)
            circle->radius = value;
        else
            circle->feather = value;
        return {};
    }
    case MaskField::kRadiusX:
    case MaskField::kRadiusY:
    {
        auto *ellipse = std::get_if<EllipseMask>(&mask.payload);
        if (ellipse == nullptr)
            return type_mismatch(mask, target, field_name);
        auto valid = finite_range(value, kCanonicalMaskPositiveMin, kCanonicalMaskUnitMax, target,
                                  field_name);
        if (!valid)
            return valid.error();
        if (field == MaskField::kRadiusX)
            ellipse->radius_x = value;
        else
            ellipse->radius_y = value;
        return {};
    }
    case MaskField::kSource:
    case MaskField::kChannel:
    case MaskField::kThreshold0:
    case MaskField::kThreshold1:
    case MaskField::kThreshold2:
    case MaskField::kThreshold3:
    {
        auto *parametric = std::get_if<ParametricMask>(&mask.payload);
        if (parametric == nullptr)
            return type_mismatch(mask, target, field_name);
        if (field == MaskField::kSource)
        {
            auto parsed =
                exact_index(value, 0, 1, target, field_name, "invalid_develop_mask_source");
            if (!parsed)
                return parsed.error();
            parametric->source = parsed.value() == 0 ? ParametricMaskSource::kInput :
                                                       ParametricMaskSource::kOperationOutput;
            return {};
        }
        if (field == MaskField::kChannel)
        {
            auto parsed =
                exact_index(value, 0, 3, target, field_name, "invalid_develop_mask_channel");
            if (!parsed)
                return parsed.error();
            parametric->channel = static_cast<ParametricMaskChannel>(parsed.value());
            return {};
        }
        auto valid =
            finite_range(value, kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, target, field_name);
        if (!valid)
            return valid.error();
        const auto index = field == MaskField::kThreshold0 ? 0U :
                           field == MaskField::kThreshold1 ? 1U :
                           field == MaskField::kThreshold2 ? 2U :
                                                             3U;
        parametric->thresholds[index] = value;
        return {};
    }
    case MaskField::kPathFeather:
    {
        auto *path = std::get_if<PathMask>(&mask.payload);
        if (path == nullptr)
            return type_mismatch(mask, target, field_name);
        auto valid =
            finite_range(value, kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, target, field_name);
        if (!valid)
            return valid.error();
        path->feather = value;
        return {};
    }
    case MaskField::kKind:
    case MaskField::kWhole:
    case MaskField::kChildIndex:
    case MaskField::kAddChild:
    case MaskField::kRemoveChild:
    case MaskField::kChildOperator:
    case MaskField::kChildOpacity:
    case MaskField::kChildInverted:
    case MaskField::kChildKind:
    case MaskField::kPointIndex:
    case MaskField::kPointX:
    case MaskField::kPointY:
    case MaskField::kPointRadius:
    case MaskField::kPointHardness:
    case MaskField::kPointDensity:
    case MaskField::kAddPoint:
    case MaskField::kRemovePoint:
        break;
    }
    return mask_edit_error(ErrorCode::kInvalidArgument, "Develop mask field is unsupported",
                           "unknown_develop_mask_field", target, field_name);
}

[[nodiscard]] Result<void> reset_mask_value(Mask &mask, const MaskField field,
                                            const DevelopMaskTarget target,
                                            const std::string_view field_name)
{
    if (field == MaskField::kKind)
    {
        Mask replacement{mask.id, kCanonicalMaskSchemaVersion, MaskKind::kAll};
        mask = std::move(replacement);
        return {};
    }
    Mask defaults{mask.id, kCanonicalMaskSchemaVersion, mask.kind};
    switch (field)
    {
    case MaskField::kOpacity:
        mask.common.opacity = defaults.common.opacity;
        return {};
    case MaskField::kInverted:
        mask.common.inverted = defaults.common.inverted;
        return {};
    case MaskField::kAnchorX:
    case MaskField::kAnchorY:
    case MaskField::kTransition:
    {
        auto *gradient = std::get_if<LinearGradientMask>(&mask.payload);
        const auto *identity = std::get_if<LinearGradientMask>(&defaults.payload);
        if (gradient == nullptr || identity == nullptr)
            return type_mismatch(mask, target, field_name);
        if (field == MaskField::kAnchorX)
            gradient->anchor_x = identity->anchor_x;
        else if (field == MaskField::kAnchorY)
            gradient->anchor_y = identity->anchor_y;
        else
            gradient->transition = identity->transition;
        return {};
    }
    case MaskField::kRotationDegrees:
    {
        if (auto *gradient = std::get_if<LinearGradientMask>(&mask.payload); gradient != nullptr)
        {
            gradient->rotation_degrees =
                std::get<LinearGradientMask>(defaults.payload).rotation_degrees;
            return {};
        }
        if (auto *ellipse = std::get_if<EllipseMask>(&mask.payload); ellipse != nullptr)
        {
            ellipse->rotation_degrees = std::get<EllipseMask>(defaults.payload).rotation_degrees;
            return {};
        }
        return type_mismatch(mask, target, field_name);
    }
    case MaskField::kCenterX:
    case MaskField::kCenterY:
    case MaskField::kRadius:
    case MaskField::kFeather:
    {
        auto *circle = std::get_if<CircleMask>(&mask.payload);
        const auto *identity = std::get_if<CircleMask>(&defaults.payload);
        if (circle == nullptr || identity == nullptr)
            return type_mismatch(mask, target, field_name);
        if (field == MaskField::kCenterX)
            circle->center_x = identity->center_x;
        else if (field == MaskField::kCenterY)
            circle->center_y = identity->center_y;
        else if (field == MaskField::kRadius)
            circle->radius = identity->radius;
        else
            circle->feather = identity->feather;
        return {};
    }
    case MaskField::kRadiusX:
    case MaskField::kRadiusY:
    {
        auto *ellipse = std::get_if<EllipseMask>(&mask.payload);
        const auto *identity = std::get_if<EllipseMask>(&defaults.payload);
        if (ellipse == nullptr || identity == nullptr)
            return type_mismatch(mask, target, field_name);
        if (field == MaskField::kRadiusX)
            ellipse->radius_x = identity->radius_x;
        else
            ellipse->radius_y = identity->radius_y;
        return {};
    }
    case MaskField::kSource:
    {
        auto *parametric = std::get_if<ParametricMask>(&mask.payload);
        const auto *identity = std::get_if<ParametricMask>(&defaults.payload);
        if (parametric == nullptr || identity == nullptr)
            return type_mismatch(mask, target, field_name);
        parametric->source = identity->source;
        return {};
    }
    case MaskField::kChannel:
    {
        auto *parametric = std::get_if<ParametricMask>(&mask.payload);
        const auto *identity = std::get_if<ParametricMask>(&defaults.payload);
        if (parametric == nullptr || identity == nullptr)
            return type_mismatch(mask, target, field_name);
        parametric->channel = identity->channel;
        return {};
    }
    case MaskField::kThreshold0:
    case MaskField::kThreshold1:
    case MaskField::kThreshold2:
    case MaskField::kThreshold3:
    {
        auto *parametric = std::get_if<ParametricMask>(&mask.payload);
        const auto *identity = std::get_if<ParametricMask>(&defaults.payload);
        if (parametric == nullptr || identity == nullptr)
            return type_mismatch(mask, target, field_name);
        // A single threshold cannot always return to its scalar default
        // without crossing an unchanged neighbour. Restore only the ramp as
        // one atomic value; source and channel remain independently owned.
        parametric->thresholds = identity->thresholds;
        return {};
    }
    case MaskField::kPathFeather:
    {
        auto *path = std::get_if<PathMask>(&mask.payload);
        if (path == nullptr)
            return type_mismatch(mask, target, field_name);
        path->feather = 0.0;
        return {};
    }
    case MaskField::kWhole:
    case MaskField::kKind:
    case MaskField::kChildIndex:
    case MaskField::kAddChild:
    case MaskField::kRemoveChild:
    case MaskField::kChildOperator:
    case MaskField::kChildOpacity:
    case MaskField::kChildInverted:
    case MaskField::kChildKind:
    case MaskField::kPointIndex:
    case MaskField::kPointX:
    case MaskField::kPointY:
    case MaskField::kPointRadius:
    case MaskField::kPointHardness:
    case MaskField::kPointDensity:
    case MaskField::kAddPoint:
    case MaskField::kRemovePoint:
        break;
    }
    return mask_edit_error(ErrorCode::kInvalidArgument, "Develop mask field is unsupported",
                           "unknown_develop_mask_field", target, field_name);
}

} // namespace

std::string_view develop_mask_target_name(const DevelopMaskTarget target) noexcept
{
    switch (target)
    {
    case DevelopMaskTarget::kColorHarmonizer:
        return "color_harmonizer";
    case DevelopMaskTarget::kGraduatedNd:
        return "graduatednd";
    case DevelopMaskTarget::kColorBalanceRgb:
        return "color_balance_rgb";
    case DevelopMaskTarget::kExposure:
        return "exposure";
    case DevelopMaskTarget::kRgbCurve:
        return "rgb_curve";
    }
    return "unknown";
}

std::string_view
develop_mask_attachment_status_name(const DevelopMaskAttachmentStatus status) noexcept
{
    switch (status)
    {
    case DevelopMaskAttachmentStatus::kNoMask:
        return "no_mask";
    case DevelopMaskAttachmentStatus::kEditable:
        return "editable";
    case DevelopMaskAttachmentStatus::kExternalReadOnly:
        return "external_read_only";
    case DevelopMaskAttachmentStatus::kSharedReadOnly:
        return "shared_read_only";
    case DevelopMaskAttachmentStatus::kGroupReadOnly:
        return "group_read_only";
    case DevelopMaskAttachmentStatus::kInvalid:
        return "invalid";
    }
    return "invalid";
}

bool is_develop_mask_field(const std::string_view field) noexcept
{
    return field.starts_with(kColorHarmonizerMaskFieldPrefix) ||
           field.starts_with(kGraduatedMaskFieldPrefix) ||
           field.starts_with(kColorBalanceRgbMaskFieldPrefix) ||
           field.starts_with(kExposureMaskFieldPrefix) ||
           field.starts_with(kRgbCurveMaskFieldPrefix);
}

DevelopMaskEditorState develop_mask_editor_state(const DevelopParams &params,
                                                 const DevelopMaskTarget target)
{
    DevelopMaskEditorState result;
    const auto &attachment = mask_attachment(params, target);
    if (!validate_mask_graph(params.masks) || !develop_mask_attachments_resolve(params))
    {
        result.attached = attachment.has_value();
        result.editable = false;
        result.status = DevelopMaskAttachmentStatus::kInvalid;
        return result;
    }
    if (!attachment)
    {
        return result;
    }

    result.attached = true;
    result.can_detach = true;
    const auto *mask = find_mask(params.masks, *attachment);
    if (mask == nullptr)
    {
        result.editable = false;
        result.status = DevelopMaskAttachmentStatus::kInvalid;
        return result;
    }

    result.kind_index = mask_kind_index(mask->kind);
    result.kind_name = std::string(mask_kind_name(mask->kind));
    result.opacity = mask->common.opacity;
    result.inverted = mask->common.inverted;
    const auto fill_leaf = [&](const Mask &leaf)
    {
        if (const auto *gradient = std::get_if<LinearGradientMask>(&leaf.payload);
            gradient != nullptr)
        {
            result.anchor_x = gradient->anchor_x;
            result.anchor_y = gradient->anchor_y;
            result.rotation_degrees = gradient->rotation_degrees;
            result.transition = gradient->transition;
        }
        else if (const auto *circle = std::get_if<CircleMask>(&leaf.payload); circle != nullptr)
        {
            result.center_x = circle->center_x;
            result.center_y = circle->center_y;
            result.radius = circle->radius;
            result.feather = circle->feather;
        }
        else if (const auto *ellipse = std::get_if<EllipseMask>(&leaf.payload); ellipse != nullptr)
        {
            result.center_x = ellipse->center_x;
            result.center_y = ellipse->center_y;
            result.radius_x = ellipse->radius_x;
            result.radius_y = ellipse->radius_y;
            result.rotation_degrees = ellipse->rotation_degrees;
            result.feather = ellipse->feather;
        }
        else if (const auto *parametric = std::get_if<ParametricMask>(&leaf.payload);
                 parametric != nullptr)
        {
            result.source_index = parametric->source == ParametricMaskSource::kInput ? 0 : 1;
            result.channel_index = static_cast<std::int64_t>(parametric->channel);
            result.threshold0 = parametric->thresholds[0];
            result.threshold1 = parametric->thresholds[1];
            result.threshold2 = parametric->thresholds[2];
            result.threshold3 = parametric->thresholds[3];
        }
        else if (const auto *path = std::get_if<PathMask>(&leaf.payload); path != nullptr)
        {
            result.path_feather = path->feather;
            result.point_count = static_cast<std::int64_t>(path->points.size());
            auto point_index = point_index_slot(params, target);
            clamp_cursor(point_index, path->points.size());
            result.point_index = point_index;
            if (!path->points.empty())
            {
                const auto &point = path->points[static_cast<std::size_t>(point_index)];
                result.point_x = point.x;
                result.point_y = point.y;
            }
        }
        else if (const auto *brush = std::get_if<BrushMask>(&leaf.payload); brush != nullptr)
        {
            result.point_count = static_cast<std::int64_t>(brush->points.size());
            auto point_index = point_index_slot(params, target);
            clamp_cursor(point_index, brush->points.size());
            result.point_index = point_index;
            if (!brush->points.empty())
            {
                const auto &point = brush->points[static_cast<std::size_t>(point_index)];
                result.point_x = point.x;
                result.point_y = point.y;
                result.point_radius = point.radius;
                result.point_hardness = point.hardness;
                result.point_density = point.density;
            }
        }
    };
    fill_leaf(*mask);
    if (const auto *group = std::get_if<MaskGroup>(&mask->payload); group != nullptr)
    {
        result.child_count = static_cast<std::int64_t>(group->children.size());
        auto child_index = child_index_slot(params, target);
        clamp_cursor(child_index, group->children.size());
        result.child_index = child_index;
        if (!group->children.empty())
        {
            const auto &edge = group->children[static_cast<std::size_t>(child_index)];
            result.child_operator_index = static_cast<std::int64_t>(edge.operation);
            result.child_opacity = edge.opacity;
            result.child_inverted = edge.inverted;
            const auto *child = find_mask(params.masks, edge.mask_id);
            if (child != nullptr)
            {
                result.child_kind_index = mask_kind_index(child->kind);
                result.child_kind_name = std::string(mask_kind_name(child->kind));
                fill_leaf(*child);
            }
        }
    }

    if (mask->kind == MaskKind::kGroup && !studio_owns_mask_id(target, mask->id))
    {
        result.editable = false;
        result.status = DevelopMaskAttachmentStatus::kGroupReadOnly;
    }
    else if (!studio_owns_mask_id(target, mask->id))
    {
        result.editable = false;
        result.status = DevelopMaskAttachmentStatus::kExternalReadOnly;
    }
    else if (is_attached_by_other_operation(params, target, mask->id) ||
             has_group_edge_to(params, mask->id))
    {
        result.editable = false;
        result.status = DevelopMaskAttachmentStatus::kSharedReadOnly;
    }
    else
    {
        result.status = DevelopMaskAttachmentStatus::kEditable;
    }
    return result;
}

[[nodiscard]] bool is_group_field(const MaskField field) noexcept
{
    switch (field)
    {
    case MaskField::kChildIndex:
    case MaskField::kAddChild:
    case MaskField::kRemoveChild:
    case MaskField::kChildOperator:
    case MaskField::kChildOpacity:
    case MaskField::kChildInverted:
    case MaskField::kChildKind:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool is_point_field(const MaskField field) noexcept
{
    switch (field)
    {
    case MaskField::kPointIndex:
    case MaskField::kPointX:
    case MaskField::kPointY:
    case MaskField::kPointRadius:
    case MaskField::kPointHardness:
    case MaskField::kPointDensity:
    case MaskField::kAddPoint:
    case MaskField::kRemovePoint:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] Result<void> apply_kind_change(DevelopParams &params, const DevelopMaskTarget target,
                                             const std::string_view field, const MaskKind kind)
{
    auto &attachment = mask_attachment(params, target);
    if (!attachment)
    {
        if (kind == MaskKind::kGroup)
        {
            auto child_id = next_studio_mask_id(params, target, field);
            if (!child_id)
                return child_id.error();
            params.masks.push_back(make_kind_mask(child_id.value(), MaskKind::kAll));
            auto group_id = next_studio_mask_id(params, target, field);
            if (!group_id)
                return group_id.error();
            Mask group = make_kind_mask(group_id.value(), MaskKind::kGroup);
            group.payload = MaskGroup{{{child_id.value(), MaskGroupOperator::kReplace, 1.0, false}}};
            params.masks.push_back(std::move(group));
            attachment = params.masks.back().id;
            child_index_slot(params, target) = 0;
        }
        else
        {
            auto id = next_studio_mask_id(params, target, field);
            if (!id)
                return id.error();
            params.masks.push_back(make_kind_mask(std::move(id).value(), kind));
            attachment = params.masks.back().id;
        }
        enable_target_operation(params, target);
        point_index_slot(params, target) = 0;
        return {};
    }
    auto editable = require_editable(params, target, field);
    if (!editable)
        return editable.error();
    auto *mask = find_mask(params.masks, *attachment);
    if (mask == nullptr)
    {
        return mask_edit_error(ErrorCode::kValidation,
                               "Develop mask attachment does not resolve to a canonical node",
                               "missing_develop_mask_attachment", target, field);
    }
    if (mask->kind == kind)
    {
        return {};
    }
    if (kind == MaskKind::kGroup)
    {
        if (mask->kind == MaskKind::kGroup)
        {
            return {};
        }
        const std::string leaf_id = mask->id;
        auto group_id = next_studio_mask_id(params, target, field);
        if (!group_id)
            return group_id.error();
        Mask group = make_kind_mask(group_id.value(), MaskKind::kGroup);
        group.payload = MaskGroup{{{leaf_id, MaskGroupOperator::kReplace, 1.0, false}}};
        params.masks.push_back(std::move(group));
        attachment = params.masks.back().id;
        child_index_slot(params, target) = 0;
        enable_target_operation(params, target);
        return {};
    }
    if (mask->kind == MaskKind::kGroup)
    {
        const auto group = std::get<MaskGroup>(mask->payload);
        Mask replacement = make_kind_mask(mask->id, kind);
        replacement.common = mask->common;
        *mask = std::move(replacement);
        delete_unreferenced_studio_children(params, group, target);
        child_index_slot(params, target) = 0;
        point_index_slot(params, target) = 0;
        enable_target_operation(params, target);
        return {};
    }
    Mask replacement = make_kind_mask(mask->id, kind);
    replacement.common = mask->common;
    *mask = std::move(replacement);
    point_index_slot(params, target) = 0;
    enable_target_operation(params, target);
    return {};
}

[[nodiscard]] Result<void> apply_group_field(DevelopParams &params, const DevelopMaskTarget target,
                                             const std::string_view field_name,
                                             const MaskField field, const double value)
{
    auto *root = find_mask(params.masks, *mask_attachment(params, target));
    if (root == nullptr)
    {
        return mask_edit_error(ErrorCode::kValidation,
                               "Develop mask attachment does not resolve to a canonical node",
                               "missing_develop_mask_attachment", target, field_name);
    }
    auto *group = std::get_if<MaskGroup>(&root->payload);
    if (group == nullptr)
    {
        return type_mismatch(*root, target, field_name);
    }
    if (field == MaskField::kChildIndex)
    {
        auto index = exact_index(value, 0, static_cast<std::int64_t>(group->children.size()) - 1,
                                 target, field_name, "invalid_develop_mask_child_index");
        if (!index)
            return index.error();
        child_index_slot(params, target) = index.value();
        return {};
    }
    if (field == MaskField::kAddChild)
    {
        auto kind_index = exact_index(value, 1, 8, target, field_name, "invalid_develop_mask_kind");
        if (!kind_index)
            return kind_index.error();
        const auto kind = mask_kind_from_index(kind_index.value());
        if (!kind || kind.value() == MaskKind::kGroup)
        {
            return mask_edit_error(ErrorCode::kInvalidArgument,
                                   "Group children must be Studio-owned leaves",
                                   "invalid_develop_mask_kind", target, field_name);
        }
        if (group->children.size() >= kCanonicalMaskMaxGroupChildren)
        {
            return mask_edit_error(ErrorCode::kValidation, "Mask group child count is at the limit",
                                   "invalid_mask_group_size", target, field_name);
        }
        auto id = next_studio_mask_id(params, target, field_name);
        if (!id)
            return id.error();
        const std::string child_id = id.value();
        params.masks.push_back(make_kind_mask(child_id, kind.value()));
        root = find_mask(params.masks, *mask_attachment(params, target));
        if (root == nullptr)
        {
            return mask_edit_error(ErrorCode::kValidation,
                                   "Develop mask attachment does not resolve to a canonical node",
                                   "missing_develop_mask_attachment", target, field_name);
        }
        group = std::get_if<MaskGroup>(&root->payload);
        if (group == nullptr)
        {
            return type_mismatch(*root, target, field_name);
        }
        group->children.push_back({child_id, MaskGroupOperator::kUnion, 1.0, false});
        child_index_slot(params, target) = static_cast<std::int64_t>(group->children.size() - 1U);
        return {};
    }
    if (group->children.empty())
    {
        return mask_edit_error(ErrorCode::kValidation, "Mask group has no children",
                               "invalid_mask_group_size", target, field_name);
    }
    clamp_cursor(child_index_slot(params, target), group->children.size());
    auto &edge = group->children[static_cast<std::size_t>(child_index_slot(params, target))];
    if (field == MaskField::kRemoveChild)
    {
        auto flag = exact_index(value, 1, 1, target, field_name, "invalid_develop_mask_boolean");
        if (!flag)
            return flag.error();
        if (group->children.size() <= 1U)
        {
            return mask_edit_error(ErrorCode::kValidation,
                                   "A group must keep at least one child",
                                   "invalid_mask_group_size", target, field_name);
        }
        const std::string child_id = edge.mask_id;
        group->children.erase(group->children.begin() +
                              static_cast<std::ptrdiff_t>(child_index_slot(params, target)));
        if (!group->children.empty())
        {
            group->children.front().operation = MaskGroupOperator::kReplace;
        }
        if (studio_owns_mask_id(target, child_id) && reference_count(params, child_id) == 0U)
        {
            erase_mask_id(params, child_id);
        }
        clamp_cursor(child_index_slot(params, target), group->children.size());
        return {};
    }
    if (field == MaskField::kChildOperator)
    {
        auto op = exact_index(value, 0, 4, target, field_name, "invalid_mask_group_operator");
        if (!op)
            return op.error();
        if (child_index_slot(params, target) == 0 &&
            static_cast<MaskGroupOperator>(op.value()) != MaskGroupOperator::kReplace)
        {
            return mask_edit_error(ErrorCode::kValidation,
                                   "The first group child must replace the accumulator",
                                   "invalid_mask_group_order", target, field_name);
        }
        if (child_index_slot(params, target) > 0 &&
            static_cast<MaskGroupOperator>(op.value()) == MaskGroupOperator::kReplace)
        {
            return mask_edit_error(ErrorCode::kValidation,
                                   "Later group children cannot replace the accumulator",
                                   "invalid_mask_group_order", target, field_name);
        }
        edge.operation = static_cast<MaskGroupOperator>(op.value());
        return {};
    }
    if (field == MaskField::kChildOpacity)
    {
        auto valid =
            finite_range(value, kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, target, field_name);
        if (!valid)
            return valid.error();
        edge.opacity = value;
        return {};
    }
    if (field == MaskField::kChildInverted)
    {
        auto parsed = exact_index(value, 0, 1, target, field_name, "invalid_develop_mask_boolean");
        if (!parsed)
            return parsed.error();
        edge.inverted = parsed.value() == 1;
        return {};
    }
    if (field == MaskField::kChildKind)
    {
        auto kind_index = exact_index(value, 1, 8, target, field_name, "invalid_develop_mask_kind");
        if (!kind_index)
            return kind_index.error();
        const auto kind = mask_kind_from_index(kind_index.value());
        if (!kind || kind.value() == MaskKind::kGroup)
        {
            return mask_edit_error(ErrorCode::kInvalidArgument,
                                   "Group children must be Studio-owned leaves",
                                   "invalid_develop_mask_kind", target, field_name);
        }
        auto *child = find_mask(params.masks, edge.mask_id);
        if (child == nullptr || !studio_owns_mask_id(target, child->id))
        {
            return mask_edit_error(ErrorCode::kUnsupported,
                                   "Attached Develop mask is read-only for Studio authoring",
                                   "external_read_only", target, field_name);
        }
        if (child->kind != kind.value())
        {
            Mask replacement = make_kind_mask(child->id, kind.value());
            replacement.common = child->common;
            *child = std::move(replacement);
        }
        return {};
    }
    return mask_edit_error(ErrorCode::kInvalidArgument, "Develop mask field is unsupported",
                           "unknown_develop_mask_field", target, field_name);
}

[[nodiscard]] Result<void> apply_point_field(DevelopParams &params, const DevelopMaskTarget target,
                                             const std::string_view field_name,
                                             const MaskField field, const double value)
{
    auto *leaf = edited_leaf(params, target);
    if (leaf == nullptr)
    {
        return mask_edit_error(ErrorCode::kValidation,
                               "Develop mask attachment does not resolve to a canonical node",
                               "missing_develop_mask_attachment", target, field_name);
    }
    auto *path = std::get_if<PathMask>(&leaf->payload);
    auto *brush = std::get_if<BrushMask>(&leaf->payload);
    if (path == nullptr && brush == nullptr)
    {
        return type_mismatch(*leaf, target, field_name);
    }
    const std::size_t count = path != nullptr ? path->points.size() : brush->points.size();
    if (field == MaskField::kPointIndex)
    {
        auto index = exact_index(value, 0, static_cast<std::int64_t>(count) - 1, target, field_name,
                                 "invalid_develop_mask_point_index");
        if (!index)
            return index.error();
        point_index_slot(params, target) = index.value();
        return {};
    }
    if (field == MaskField::kAddPoint)
    {
        auto flag = exact_index(value, 1, 1, target, field_name, "invalid_develop_mask_boolean");
        if (!flag)
            return flag.error();
        if (count >= kCanonicalMaskMaxPathPoints)
        {
            return mask_edit_error(ErrorCode::kValidation, "Mask point count is at the limit",
                                   path != nullptr ? "invalid_path_mask" : "invalid_brush_mask",
                                   target, field_name);
        }
        clamp_cursor(point_index_slot(params, target), count);
        const std::size_t at = static_cast<std::size_t>(point_index_slot(params, target));
        if (path != nullptr)
        {
            PathMaskPoint point = path->points[at];
            point.x = std::clamp(point.x + 0.05, kCanonicalMaskUnitMin, kCanonicalMaskUnitMax);
            point.y = std::clamp(point.y + 0.05, kCanonicalMaskUnitMin, kCanonicalMaskUnitMax);
            path->points.insert(path->points.begin() + static_cast<std::ptrdiff_t>(at + 1U), point);
            smooth_path_handles(path->points);
        }
        else
        {
            BrushMaskPoint point = brush->points[at];
            point.x = std::clamp(point.x + 0.05, kCanonicalMaskUnitMin, kCanonicalMaskUnitMax);
            point.y = std::clamp(point.y + 0.05, kCanonicalMaskUnitMin, kCanonicalMaskUnitMax);
            brush->points.insert(brush->points.begin() + static_cast<std::ptrdiff_t>(at + 1U),
                                 point);
            smooth_brush_handles(brush->points);
        }
        point_index_slot(params, target) = static_cast<std::int64_t>(at + 1U);
        return {};
    }
    if (field == MaskField::kRemovePoint)
    {
        auto flag = exact_index(value, 1, 1, target, field_name, "invalid_develop_mask_boolean");
        if (!flag)
            return flag.error();
        const std::size_t minimum =
            path != nullptr ? kCanonicalMaskMinPathPoints : kCanonicalMaskMinBrushPoints;
        if (count <= minimum)
        {
            return mask_edit_error(ErrorCode::kValidation, "Mask point count is at the minimum",
                                   path != nullptr ? "invalid_path_mask" : "invalid_brush_mask",
                                   target, field_name);
        }
        clamp_cursor(point_index_slot(params, target), count);
        const std::size_t at = static_cast<std::size_t>(point_index_slot(params, target));
        if (path != nullptr)
        {
            path->points.erase(path->points.begin() + static_cast<std::ptrdiff_t>(at));
            smooth_path_handles(path->points);
        }
        else
        {
            brush->points.erase(brush->points.begin() + static_cast<std::ptrdiff_t>(at));
            smooth_brush_handles(brush->points);
        }
        clamp_cursor(point_index_slot(params, target),
                     path != nullptr ? path->points.size() : brush->points.size());
        return {};
    }
    clamp_cursor(point_index_slot(params, target), count);
    const std::size_t at = static_cast<std::size_t>(point_index_slot(params, target));
    if (field == MaskField::kPointX || field == MaskField::kPointY)
    {
        auto valid =
            finite_range(value, kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, target, field_name);
        if (!valid)
            return valid.error();
        if (path != nullptr)
        {
            if (field == MaskField::kPointX)
                path->points[at].x = value;
            else
                path->points[at].y = value;
            smooth_path_handles(path->points);
        }
        else
        {
            if (field == MaskField::kPointX)
                brush->points[at].x = value;
            else
                brush->points[at].y = value;
            smooth_brush_handles(brush->points);
        }
        return {};
    }
    if (brush == nullptr)
    {
        return type_mismatch(*leaf, target, field_name);
    }
    if (field == MaskField::kPointRadius)
    {
        auto valid =
            finite_range(value, kCanonicalMaskPositiveMin, kCanonicalMaskUnitMax, target, field_name);
        if (!valid)
            return valid.error();
        brush->points[at].radius = value;
        return {};
    }
    auto valid = finite_range(value, kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, target, field_name);
    if (!valid)
        return valid.error();
    if (field == MaskField::kPointHardness)
        brush->points[at].hardness = value;
    else
        brush->points[at].density = value;
    return {};
}

Result<void> apply_develop_mask_field_strict(DevelopParams &params, const std::string_view field,
                                             const double value)
{
    const auto failed_target = field.starts_with(kGraduatedMaskFieldPrefix) ?
                                   DevelopMaskTarget::kGraduatedNd :
                               field.starts_with(kColorBalanceRgbMaskFieldPrefix) ?
                                   DevelopMaskTarget::kColorBalanceRgb :
                               field.starts_with(kExposureMaskFieldPrefix) ?
                                   DevelopMaskTarget::kExposure :
                               field.starts_with(kRgbCurveMaskFieldPrefix) ?
                                   DevelopMaskTarget::kRgbCurve :
                                   DevelopMaskTarget::kColorHarmonizer;
    try
    {
        const auto parsed = parse_mask_field(field);
        if (!parsed)
        {
            return mask_edit_error(ErrorCode::kInvalidArgument, "Develop mask field is unsupported",
                                   "unknown_develop_mask_field", failed_target, field);
        }

        DevelopParams candidate = params;
        auto valid = validate_develop_mask_state(candidate, parsed->target, field);
        if (!valid)
        {
            return valid.error();
        }
        if (parsed->field == MaskField::kWhole)
        {
            return mask_edit_error(ErrorCode::kInvalidArgument,
                                   "The whole Develop mask field accepts reset/detach only",
                                   "invalid_develop_mask_field", parsed->target, field);
        }
        if (parsed->field == MaskField::kKind)
        {
            auto index =
                exact_index(value, 0, 8, parsed->target, field, "invalid_develop_mask_kind");
            if (!index)
            {
                return index.error();
            }
            if (index.value() == 0)
            {
                auto detached = detach_mask(candidate, parsed->target, field);
                if (!detached)
                    return detached.error();
                params = std::move(candidate);
                return {};
            }
            const auto kind = mask_kind_from_index(index.value());
            if (!kind)
            {
                return mask_edit_error(ErrorCode::kInvalidArgument,
                                       "Develop mask kind is unsupported for Studio authoring",
                                       "invalid_develop_mask_kind", parsed->target, field);
            }
            auto changed = apply_kind_change(candidate, parsed->target, field, kind.value());
            if (!changed)
                return changed.error();
        }
        else
        {
            auto editable = require_editable(candidate, parsed->target, field);
            if (!editable)
                return editable.error();
            if (is_group_field(parsed->field))
            {
                auto applied =
                    apply_group_field(candidate, parsed->target, field, parsed->field, value);
                if (!applied)
                    return applied.error();
            }
            else if (is_point_field(parsed->field))
            {
                auto applied =
                    apply_point_field(candidate, parsed->target, field, parsed->field, value);
                if (!applied)
                    return applied.error();
            }
            else
            {
                auto *mask = (parsed->field == MaskField::kOpacity ||
                              parsed->field == MaskField::kInverted) ?
                                 find_mask(candidate.masks, *mask_attachment(candidate, parsed->target)) :
                                 edited_leaf(candidate, parsed->target);
                if (mask == nullptr)
                {
                    return mask_edit_error(
                        ErrorCode::kValidation,
                        "Develop mask attachment does not resolve to a canonical node",
                        "missing_develop_mask_attachment", parsed->target, field);
                }
                auto applied = apply_mask_value(*mask, parsed->field, value, parsed->target, field);
                if (!applied)
                    return applied.error();
            }
        }
        valid = validate_develop_mask_state(candidate, parsed->target, field);
        if (!valid)
        {
            return valid.error();
        }
        params = std::move(candidate);
        return {};
    }
    catch (const std::bad_alloc &)
    {
        return mask_edit_error(ErrorCode::kIo,
                               "Develop mask edit could not allocate its typed candidate",
                               "develop_mask_allocation_failed", failed_target, field);
    }
}

Result<void> reset_develop_mask_field(DevelopParams &params, const std::string_view field)
{
    const auto failed_target = field.starts_with(kGraduatedMaskFieldPrefix) ?
                                   DevelopMaskTarget::kGraduatedNd :
                               field.starts_with(kColorBalanceRgbMaskFieldPrefix) ?
                                   DevelopMaskTarget::kColorBalanceRgb :
                               field.starts_with(kExposureMaskFieldPrefix) ?
                                   DevelopMaskTarget::kExposure :
                               field.starts_with(kRgbCurveMaskFieldPrefix) ?
                                   DevelopMaskTarget::kRgbCurve :
                                   DevelopMaskTarget::kColorHarmonizer;
    try
    {
        const auto parsed = parse_mask_field(field);
        if (!parsed)
        {
            return mask_edit_error(ErrorCode::kInvalidArgument, "Develop mask field is unsupported",
                                   "unknown_develop_mask_field", failed_target, field);
        }

        DevelopParams candidate = params;
        if (parsed->field == MaskField::kWhole)
        {
            auto detached = detach_mask(candidate, parsed->target, field);
            if (!detached)
                return detached.error();
            params = std::move(candidate);
            return {};
        }
        auto valid = validate_develop_mask_state(candidate, parsed->target, field);
        if (!valid)
        {
            return valid.error();
        }
        auto editable = require_editable(candidate, parsed->target, field);
        if (!editable)
            return editable.error();
        if (parsed->field == MaskField::kChildIndex)
        {
            child_index_slot(candidate, parsed->target) = 0;
        }
        else if (parsed->field == MaskField::kPointIndex)
        {
            point_index_slot(candidate, parsed->target) = 0;
        }
        else if (parsed->field == MaskField::kKind)
        {
            auto changed = apply_kind_change(candidate, parsed->target, field, MaskKind::kAll);
            if (!changed)
                return changed.error();
        }
        else
        {
            auto *mask = (parsed->field == MaskField::kOpacity ||
                          parsed->field == MaskField::kInverted) ?
                             find_mask(candidate.masks, *mask_attachment(candidate, parsed->target)) :
                             edited_leaf(candidate, parsed->target);
            if (mask == nullptr)
            {
                return mask_edit_error(
                    ErrorCode::kValidation,
                    "Develop mask attachment does not resolve to a canonical node",
                    "missing_develop_mask_attachment", parsed->target, field);
            }
            auto reset = reset_mask_value(*mask, parsed->field, parsed->target, field);
            if (!reset)
                return reset.error();
        }
        valid = validate_develop_mask_state(candidate, parsed->target, field);
        if (!valid)
            return valid.error();
        params = std::move(candidate);
        return {};
    }
    catch (const std::bad_alloc &)
    {
        return mask_edit_error(ErrorCode::kIo,
                               "Develop mask reset could not allocate its typed candidate",
                               "develop_mask_allocation_failed", failed_target, field);
    }
}

} // namespace ravo
