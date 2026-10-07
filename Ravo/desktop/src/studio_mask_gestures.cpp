#include "ravo/desktop/studio_develop_presenter.h"

#include <cmath>
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

QVariantList StudioDevelopPresenter::localMaskHandles() const
{
    QVariantList handles;
    if (!localEditing() || !state_.mask_overlay_visible_)
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
