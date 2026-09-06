#include "ravo/recipe/local_adjustment.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include "ravo/recipe/develop_mask.h"

namespace ravo
{
namespace
{
Result<Mask *> selected_leaf(DevelopParams &local)
{
    if (!local.local_mask_id)
        return make_error(ErrorCode::kValidation, "Local mask is missing");
    auto root = std::find_if(local.masks.begin(), local.masks.end(),
                             [&](const auto &mask) { return mask.id == *local.local_mask_id; });
    if (root == local.masks.end())
        return make_error(ErrorCode::kValidation, "Local mask is missing");
    if (auto *group = std::get_if<MaskGroup>(&root->payload))
    {
        if (local.local_mask_child_index < 0 ||
            static_cast<std::size_t>(local.local_mask_child_index) >= group->children.size())
            return make_error(ErrorCode::kValidation, "Local mask component is missing");
        const auto &id =
            group->children[static_cast<std::size_t>(local.local_mask_child_index)].mask_id;
        root = std::find_if(local.masks.begin(), local.masks.end(),
                            [&](const auto &mask) { return mask.id == id; });
        if (root == local.masks.end())
            return make_error(ErrorCode::kValidation, "Local mask component is missing");
    }
    return &*root;
}
} // namespace

Result<void> add_local_mask_component(DevelopParams &local, const std::int64_t kind,
                                      const std::int64_t combine)
{
    const auto state = develop_mask_editor_state(local, DevelopMaskTarget::kLocal);
    if (!state.editable || !state.attached)
        return make_error(ErrorCode::kConflict, "Mask is not editable",
                          {{"reason", "mask_not_editable"}});
    DevelopParams next = local;
    auto root = std::find_if(next.masks.begin(), next.masks.end(),
                             [&](const auto &mask) { return mask.id == *next.local_mask_id; });
    if (root->kind != MaskKind::kGroup)
    {
        const auto common = root->common;
        auto child =
            clone_develop_mask_subgraph(next, next.local_mask_id, "ravo.studio.mask.local.");
        if (!child)
            return child.error();
        root = std::find_if(next.masks.begin(), next.masks.end(),
                            [&](const auto &mask) { return mask.id == *next.local_mask_id; });
        auto leaf = std::find_if(next.masks.begin(), next.masks.end(),
                                 [&](const auto &mask) { return mask.id == *child.value(); });
        leaf->common = {};
        root->kind = MaskKind::kGroup;
        root->payload = MaskGroup{{MaskGroupChild{*child.value(), MaskGroupOperator::kReplace}}};
        root->common = common;
    }
    auto added =
        apply_develop_mask_field_strict(next, "localMaskAddChild", static_cast<double>(kind));
    if (!added)
        return added.error();
    auto combined = apply_develop_mask_field_strict(next, "localMaskChildOperator",
                                                    static_cast<double>(combine));
    if (!combined)
        return combined.error();
    local = std::move(next);
    return {};
}

Result<void> author_local_mask_gesture(DevelopParams &local,
                                       const std::vector<LocalMaskPoint> &points,
                                       const std::string_view handle, const double source_aspect)
{
    if (points.empty() || points.size() > kCanonicalMaskMaxBrushPoints ||
        !std::isfinite(source_aspect) || source_aspect <= 0)
        return make_error(ErrorCode::kValidation, "Invalid mask gesture",
                          {{"reason", "invalid_mask_gesture"}});
    for (const auto &point : points)
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < 0 || point.x > 1 ||
            point.y < 0 || point.y > 1)
            return make_error(ErrorCode::kValidation, "Invalid mask coordinate",
                              {{"reason", "invalid_mask_coordinate"}});
    if (!develop_mask_editor_state(local, DevelopMaskTarget::kLocal).editable)
        return make_error(ErrorCode::kConflict, "Mask is not editable",
                          {{"reason", "mask_not_editable"}});
    DevelopParams next = local;
    auto selected = selected_leaf(next);
    if (!selected)
        return selected.error();
    Mask &mask = *selected.value();
    const bool supported_handle =
        handle == "draw" ||
        ((mask.kind == MaskKind::kCircle || mask.kind == MaskKind::kEllipse ||
          mask.kind == MaskKind::kLinearGradient) &&
         handle == "center") ||
        ((mask.kind == MaskKind::kCircle || mask.kind == MaskKind::kEllipse) &&
         handle == "radiusX") ||
        (mask.kind == MaskKind::kEllipse && (handle == "radiusY" || handle == "rotation")) ||
        (mask.kind == MaskKind::kLinearGradient && (handle == "rotation" || handle == "start"));
    if (!supported_handle)
        return make_error(ErrorCode::kUnsupported, "Unsupported mask handle",
                          {{"reason", "unsupported_mask_handle"}});
    const auto a = points.front();
    const auto b = points.back();
    const double sx = std::max(1.0, source_aspect);
    const double sy = std::max(1.0, 1.0 / source_aspect);
    const double dx = (b.x - a.x) * sx, dy = (b.y - a.y) * sy;
    const auto edit_radial = [&](auto &shape, const bool ellipse)
    {
        if (handle == "center")
        {
            shape.center_x = std::clamp(shape.center_x + b.x - a.x, 0.0, 1.0);
            shape.center_y = std::clamp(shape.center_y + b.y - a.y, 0.0, 1.0);
        }
        else if (handle == "draw")
        {
            shape.center_x = (a.x + b.x) * 0.5;
            shape.center_y = (a.y + b.y) * 0.5;
        }
        (void)ellipse;
    };
    if (auto *ellipse = std::get_if<EllipseMask>(&mask.payload))
    {
        edit_radial(*ellipse, true);
        if (handle == "draw")
        {
            ellipse->radius_x = std::clamp(std::abs(dx) * 0.5, 0.000001, 1.0);
            ellipse->radius_y = std::clamp(std::abs(dy) * 0.5, 0.000001, 1.0);
        }
        else if (handle == "radiusX" || handle == "radiusY")
        {
            const double radius = std::clamp(
                std::hypot((b.x - ellipse->center_x) * sx, (b.y - ellipse->center_y) * sy),
                0.000001, 1.0);
            (handle == "radiusX" ? ellipse->radius_x : ellipse->radius_y) = radius;
        }
        else if (handle == "rotation")
            ellipse->rotation_degrees =
                std::atan2((b.y - ellipse->center_y) * sy, (b.x - ellipse->center_x) * sx) * 180.0 /
                std::numbers::pi;
    }
    else if (auto *circle = std::get_if<CircleMask>(&mask.payload))
    {
        edit_radial(*circle, false);
        if (handle == "draw")
            circle->radius = std::clamp(std::hypot(dx, dy) * 0.5, 0.000001, 1.0);
        else if (handle == "radiusX")
            circle->radius =
                std::clamp(std::hypot((b.x - circle->center_x) * sx, (b.y - circle->center_y) * sy),
                           0.000001, 1.0);
    }
    else if (auto *gradient = std::get_if<LinearGradientMask>(&mask.payload))
    {
        if (handle == "center")
        {
            gradient->anchor_x = std::clamp(gradient->anchor_x + b.x - a.x, 0.0, 1.0);
            gradient->anchor_y = std::clamp(gradient->anchor_y + b.y - a.y, 0.0, 1.0);
        }
        else
        {
            if (handle == "draw")
            {
                gradient->anchor_x = (a.x + b.x) * 0.5;
                gradient->anchor_y = (a.y + b.y) * 0.5;
            }
            const double direction = handle == "start" ? -1.0 : 1.0;
            gradient->rotation_degrees = std::atan2(direction * (b.x - gradient->anchor_x) * sx,
                                                    direction * (b.y - gradient->anchor_y) * sy) *
                                         180.0 / std::numbers::pi;
            gradient->transition = std::clamp(
                std::hypot((b.x - gradient->anchor_x) * sx, (b.y - gradient->anchor_y) * sy) /
                    std::hypot(sx, sy),
                0.000001, 1.0);
        }
    }
    else if (auto *brush = std::get_if<BrushMask>(&mask.payload))
    {
        const auto settings = brush->points.empty() ? BrushMaskPoint{} : brush->points.front();
        brush->points.clear();
        for (const auto &point : points)
            brush->points.push_back({point.x, point.y, point.x, point.y, point.x, point.y,
                                     settings.radius, settings.hardness, settings.density});
        if (brush->points.size() == 1)
            brush->points.push_back(brush->points.front());
    }
    else if (auto *path = std::get_if<PathMask>(&mask.payload))
    {
        if (points.size() < kCanonicalMaskMinPathPoints ||
            points.size() > kCanonicalMaskMaxPathPoints)
            return make_error(ErrorCode::kValidation, "Path requires 3 through 32 points",
                              {{"reason", "invalid_path_point_count"}});
        path->points.clear();
        for (const auto &point : points)
            path->points.push_back({point.x, point.y, point.x, point.y, point.x, point.y});
    }
    else
        return make_error(ErrorCode::kUnsupported, "This mask cannot be drawn",
                          {{"reason", "mask_gesture_unsupported_kind"}});
    auto valid = validate_mask_graph(next.masks);
    if (!valid)
        return valid.error();
    local = std::move(next);
    return {};
}
} // namespace ravo
