#include "ravo/desktop/studio_develop_presenter.h"

#include <cmath>
#include <algorithm>
#include <numbers>
#include <QUuid>
#include "ravo/recipe/develop_mask.h"
#include "studio_qt.h"

namespace ravo
{
bool StudioDevelopPresenter::maskDrawingActive() const noexcept
{
    if (!localEditing() || !state_.mask_drawing_active_)
        return false;
    const auto state =
        develop_mask_editor_state(state_.local_projection_, DevelopMaskTarget::kLocal);
    const auto kind = state.kind_name == "group" ? state.child_kind_name : state.kind_name;
    return kind == "brush" || kind == "path" || kind == "circle" || kind == "ellipse" ||
           kind == "linear_gradient";
}

QVariantList StudioDevelopPresenter::localMaskGeometry() const
{
    QVariantList strokes;
    if (!localEditing() || (state_.local_creation_before_ && state_.mask_gesture_points_.empty()))
        return strokes;
    double width = 0, height = 0;
    if (!working_source_size(width, height))
        return strokes;
    const auto mapping = prepare_mask_geometry(state_.develop_, static_cast<std::uint32_t>(width),
                                               static_cast<std::uint32_t>(height));
    if (!mapping)
        return strokes;
    const auto &local = state_.local_projection_;
    if (!local.local_mask_id)
        return strokes;
    auto node = std::find_if(local.masks.begin(), local.masks.end(),
                             [&](const auto &mask) { return mask.id == *local.local_mask_id; });
    if (node == local.masks.end())
        return strokes;
    if (const auto *group = std::get_if<MaskGroup>(&node->payload))
    {
        if (local.local_mask_child_index < 0 ||
            static_cast<std::size_t>(local.local_mask_child_index) >= group->children.size())
            return strokes;
        const auto id =
            group->children[static_cast<std::size_t>(local.local_mask_child_index)].mask_id;
        node = std::find_if(local.masks.begin(), local.masks.end(),
                            [&](const auto &mask) { return mask.id == id; });
        if (node == local.masks.end())
            return strokes;
    }
    // Geometry guides are presentation paths, not an alternative alpha evaluator.
    // Project every sample through the Engine mapping, including crop and rotation.
    const auto append = [&](const std::vector<MaskPoint> &points, bool closed, bool dashed)
    {
        QVariantList projected;
        for (const auto &point : points)
        {
            const auto mapped = map_mask_point(mapping.value(), point, false);
            if (!mapped)
                return;
            projected.push_back(QVariantMap{{"x", mapped.value().x}, {"y", mapped.value().y}});
        }
        if (projected.size() >= 2)
            strokes.push_back(
                QVariantMap{{"points", projected}, {"closed", closed}, {"dashed", dashed}});
    };
    const double sx = std::max(1.0, width / height), sy = std::max(1.0, height / width);
    const auto ellipse =
        [&](double cx, double cy, double rx, double ry, double degrees, bool dashed)
    {
        std::vector<MaskPoint> points;
        const double angle = degrees * std::numbers::pi / 180.0;
        for (int i = 0; i < 128; ++i)
        {
            const double t = 2 * std::numbers::pi * i / 128;
            const double x = rx * std::cos(t), y = ry * std::sin(t);
            points.push_back({cx + (x * std::cos(angle) - y * std::sin(angle)) / sx,
                              cy + (x * std::sin(angle) + y * std::cos(angle)) / sy});
        }
        append(points, true, dashed);
    };
    if (const auto *gradient = std::get_if<LinearGradientMask>(&node->payload))
    {
        const double angle = gradient->rotation_degrees * std::numbers::pi / 180.0;
        const double reach = gradient->transition * std::hypot(sx, sy);
        for (const double offset : {-reach, 0.0, reach})
        {
            const double cx = gradient->anchor_x + offset * std::sin(angle) / sx;
            const double cy = gradient->anchor_y + offset * std::cos(angle) / sy;
            const double length = 2 * std::hypot(sx, sy);
            append({{cx - length * std::cos(angle) / sx, cy + length * std::sin(angle) / sy},
                    {cx + length * std::cos(angle) / sx, cy - length * std::sin(angle) / sy}},
                   false, offset != 0);
        }
    }
    else if (const auto *circle = std::get_if<CircleMask>(&node->payload))
    {
        ellipse(circle->center_x, circle->center_y, circle->radius, circle->radius, 0, false);
        if (circle->feather > 0)
            ellipse(circle->center_x, circle->center_y, circle->radius + circle->feather,
                    circle->radius + circle->feather, 0, true);
    }
    else if (const auto *radial = std::get_if<EllipseMask>(&node->payload))
    {
        ellipse(radial->center_x, radial->center_y, radial->radius_x, radial->radius_y,
                radial->rotation_degrees, false);
        if (radial->feather > 0)
            ellipse(radial->center_x, radial->center_y, radial->radius_x + radial->feather,
                    radial->radius_y + radial->feather, radial->rotation_degrees, true);
    }
    const auto path = [&](const auto &points, bool closed)
    {
        if (points.size() < 2)
            return;
        std::vector<MaskPoint> samples;
        const auto segments = closed ? points.size() : points.size() - 1;
        for (std::size_t i = 0; i < segments; ++i)
        {
            const auto &a = points[i];
            const auto &b = points[(i + 1) % points.size()];
            for (int step = 0; step <= 16; ++step)
            {
                const double t = step / 16.0, u = 1 - t;
                samples.push_back({u * u * u * a.x + 3 * u * u * t * a.ctrl2_x +
                                       3 * u * t * t * b.ctrl1_x + t * t * t * b.x,
                                   u * u * u * a.y + 3 * u * u * t * a.ctrl2_y +
                                       3 * u * t * t * b.ctrl1_y + t * t * t * b.y});
            }
        }
        append(samples, closed, false);
    };
    if (const auto *outline = std::get_if<PathMask>(&node->payload))
        path(outline->points, true);
    else if (const auto *brush = std::get_if<BrushMask>(&node->payload))
    {
        path(brush->points, false);
        if (!brush->points.empty())
        {
            const auto index = std::min(
                static_cast<std::size_t>(std::max(std::int64_t{0}, local.local_mask_point_index)),
                brush->points.size() - 1);
            const auto &point = brush->points[index];
            ellipse(point.x, point.y, point.radius, point.radius, 0, false);
            if (point.hardness > 0 && point.hardness < 1)
                ellipse(point.x, point.y, point.radius * point.hardness,
                        point.radius * point.hardness, 0, true);
        }
    }
    return strokes;
}

QVariantList StudioDevelopPresenter::localMaskHandles() const
{
    QVariantList handles;
    if (!localEditing() || (state_.local_creation_before_ && state_.mask_gesture_points_.empty()))
        return handles;
    double width = 0, height = 0;
    if (!working_source_size(width, height))
        return handles;
    auto mapping = prepare_mask_geometry(state_.develop_, static_cast<std::uint32_t>(width),
                                         static_cast<std::uint32_t>(height));
    if (!mapping)
        return handles;
    const auto state =
        develop_mask_editor_state(state_.local_projection_, DevelopMaskTarget::kLocal);
    const auto kind = state.kind_name == "group" ? state.child_kind_name : state.kind_name;
    const auto add = [&](const QString &id, const double x, const double y)
    {
        auto point = map_mask_point(mapping.value(), {x, y}, false);
        if (point)
            handles.push_back(QVariantMap{{QStringLiteral("id"), id},
                                          {QStringLiteral("x"), point.value().x},
                                          {QStringLiteral("y"), point.value().y}});
    };
    const double sx = std::max(1.0, width / height), sy = std::max(1.0, height / width);
    const double angle = state.rotation_degrees * std::numbers::pi / 180.0;
    if (kind == "ellipse" || kind == "circle")
    {
        const double rx = kind == "circle" ? state.radius : state.radius_x;
        add(QStringLiteral("center"), state.center_x, state.center_y);
        add(QStringLiteral("radiusX"), state.center_x + rx * std::cos(angle) / sx,
            state.center_y + rx * std::sin(angle) / sy);
        if (kind == "ellipse")
        {
            add(QStringLiteral("radiusY"), state.center_x - state.radius_y * std::sin(angle) / sx,
                state.center_y + state.radius_y * std::cos(angle) / sy);
            add(QStringLiteral("rotation"), state.center_x + (rx + 0.04) * std::cos(angle) / sx,
                state.center_y + (rx + 0.04) * std::sin(angle) / sy);
        }
    }
    else if (kind == "linear_gradient")
    {
        add(QStringLiteral("center"), state.anchor_x, state.anchor_y);
        const double reach = state.transition * std::hypot(sx, sy);
        add(QStringLiteral("rotation"), state.anchor_x + reach * std::sin(angle) / sx,
            state.anchor_y + reach * std::cos(angle) / sy);
        add(QStringLiteral("start"), state.anchor_x - reach * std::sin(angle) / sx,
            state.anchor_y - reach * std::cos(angle) / sy);
    }
    return handles;
}

Result<bool> StudioDevelopPresenter::applyMaskGesture(const QString &action,
                                                      const QVariantMap &arguments)
{
    const auto rejected = [](const std::string_view reason) -> Result<bool>
    {
        return make_error(ErrorCode::kConflict, "Mask gesture was rejected",
                          {{"reason", std::string(reason)}});
    };
    if (!localEditing() ||
        arguments.value(QStringLiteral("id")).toString() != state_.active_local_id_ ||
        arguments.value(QStringLiteral("asset")).toString() != selected_asset_id_)
        return rejected("stale_mask_gesture_target");
    const bool begin = action == QLatin1String("gesture_begin");
    if (!begin &&
        (state_.mask_gesture_token_.isEmpty() ||
         arguments.value(QStringLiteral("token")).toString() != state_.mask_gesture_token_ ||
         state_.mask_gesture_scope_ != state_.active_local_id_ ||
         state_.mask_gesture_asset_ != selected_asset_id_))
        return rejected("stale_mask_gesture");
    if (action == QLatin1String("gesture_cancel"))
    {
        auto previous = *state_.mask_gesture_before_;
        state_.mask_gesture_before_.reset();
        state_.mask_gesture_token_.clear();
        state_.mask_gesture_points_.clear();
        if (state_.local_creation_before_)
            state_.mask_overlay_visible_ = false;
        mutate_develop(std::move(previous), DevelopEdit::Overlay);
        enqueue_preview();
        return true;
    }
    bool x_ok = false, y_ok = false;
    const double x = arguments.value(QStringLiteral("x")).toDouble(&x_ok);
    const double y = arguments.value(QStringLiteral("y")).toDouble(&y_ok);
    if (!x_ok || !y_ok || !std::isfinite(x) || !std::isfinite(y) || x < 0 || x > 1 || y < 0 ||
        y > 1)
        return rejected("invalid_mask_coordinate");
    if (begin)
    {
        if (state_.mask_gesture_before_)
            return rejected("mask_gesture_active");
        double width = 0, height = 0;
        if (!working_source_size(width, height))
            return rejected("mask_geometry_unavailable");
        auto mapping = prepare_mask_geometry(state_.develop_, static_cast<std::uint32_t>(width),
                                             static_cast<std::uint32_t>(height));
        if (!mapping)
            return mapping.error();
        auto point = map_mask_point(mapping.value(), {x, y}, true);
        if (!point)
            return point.error();
        const auto handle =
            arguments.value(QStringLiteral("handle"), QStringLiteral("draw")).toString();
        auto preflight = state_.local_projection_;
        const LocalMaskPoint p{std::clamp(point.value().x, 0.0, 1.0),
                               std::clamp(point.value().y, 0.0, 1.0)};
        auto valid = author_local_mask_gesture(preflight, {p, p, p}, utf8_from_qstring(handle),
                                               width / height);
        if (!valid)
            return valid.error();
        state_.mask_gesture_mapping_ = mapping.value();
        state_.mask_gesture_before_ = state_.develop_;
        state_.mask_gesture_local_ = state_.local_projection_;
        state_.mask_gesture_handle_ = handle;
        state_.mask_gesture_scope_ = state_.active_local_id_;
        state_.mask_gesture_asset_ = selected_asset_id_;
        state_.mask_gesture_token_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        state_.mask_gesture_points_.clear();
        const auto state =
            develop_mask_editor_state(state_.mask_gesture_local_, DevelopMaskTarget::kLocal);
        const auto kind = state.kind_name == "group" ? state.child_kind_name : state.kind_name;
        if (kind == "brush" && state_.mask_gesture_handle_ == QLatin1String("draw") &&
            !state_.local_creation_before_)
        {
            auto added = add_local_mask_component(state_.mask_gesture_local_, 8, 1);
            if (!added)
            {
                state_.mask_gesture_before_.reset();
                state_.mask_gesture_token_.clear();
                return added.error();
            }
        }
    }
    else if (action != QLatin1String("gesture_update") && action != QLatin1String("gesture_end"))
        return rejected("unknown_mask_gesture_action");
    auto mapped = map_mask_point(*state_.mask_gesture_mapping_, {x, y}, true);
    if (!mapped)
        return mapped.error();
    if (state_.mask_gesture_points_.size() >= kCanonicalMaskMaxBrushPoints)
        return rejected("mask_gesture_point_limit");
    state_.mask_gesture_points_.push_back(
        {std::clamp(mapped.value().x, 0.0, 1.0), std::clamp(mapped.value().y, 0.0, 1.0)});
    auto next = state_.mask_gesture_local_;
    double width = 0, height = 0;
    if (!working_source_size(width, height))
        return rejected("mask_geometry_unavailable");
    auto authored =
        author_local_mask_gesture(next, state_.mask_gesture_points_,
                                  utf8_from_qstring(state_.mask_gesture_handle_), width / height);
    const bool end = action == QLatin1String("gesture_end");
    if (!authored)
    {
        const auto state = develop_mask_editor_state(next, DevelopMaskTarget::kLocal);
        if (!end && (state.kind_name == "path" || state.child_kind_name == "path") &&
            state_.mask_gesture_points_.size() < 3)
            return true;
        return authored.error();
    }
    if (state_.local_creation_before_)
        state_.mask_overlay_visible_ = true;
    state_.mask_gesture_updating_ = true;
    const bool changed = mutate_scoped_develop(
        std::move(next), end ? DevelopEdit::Commit : DevelopEdit::Preview, true, std::nullopt);
    state_.mask_gesture_updating_ = false;
    if (end)
    {
        state_.mask_gesture_before_.reset();
        state_.mask_gesture_token_.clear();
        state_.local_creation_before_.reset();
        state_.mask_gesture_points_.clear();
    }
    return changed;
}
} // namespace ravo
