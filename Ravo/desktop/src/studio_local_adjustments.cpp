#include "ravo/desktop/studio_presenter.h"

#include <algorithm>
#include <QCoreApplication>
#include <QUuid>

#include "ravo/recipe/develop_mask.h"
#include "ravo/recipe/local_adjustment.h"
#include "studio_develop_internal.h"
#include "studio_qt.h"

namespace ravo
{
QString StudioPresenter::activeLocalId() const
{
    return active_local_id_;
}
bool StudioPresenter::localEditing() const noexcept
{
    return !active_local_id_.isEmpty();
}
bool StudioPresenter::localDonePending() const noexcept
{
    return local_done_pending_;
}

const DevelopParams &StudioPresenter::edit_develop() const noexcept
{
    return localEditing() ? local_projection_ : develop_;
}

QVariantList StudioPresenter::localAdjustments() const
{
    QVariantList result;
    for (const auto &local : develop_.local_adjustments)
    {
        const auto &operation = local.operation;
        result.push_back(QVariantMap{
            {QStringLiteral("id"), qstring_from_utf8(operation.instance_id)},
            {QStringLiteral("name"),
             qstring_from_utf8(operation.name.value_or(operation.instance_id))},
            {QStringLiteral("enabled"), operation.enabled && !operation.bypass},
            {QStringLiteral("selected"),
             operation.instance_id == utf8_from_qstring(active_local_id_)},
            {QStringLiteral("maskId"), qstring_from_utf8(operation.mask_id.value_or(""))}});
    }
    return result;
}

QVariantMap StudioPresenter::editLocalMask() const
{
    return studio_develop_internal::develop_mask_editor_map(
        develop_mask_editor_state(local_projection_, DevelopMaskTarget::kLocal),
        DevelopMaskTarget::kLocal);
}

void StudioPresenter::clear_local_edit_scope()
{
    const bool changed = localEditing();
    active_local_id_.clear();
    local_projection_ = {};
    local_done_pending_ = false;
    mask_drawing_active_ = false;
    mask_gesture_token_.clear();
    mask_gesture_before_.reset();
    mask_gesture_mapping_.reset();
    mask_gesture_points_.clear();
    local_creation_before_.reset();
    if (mask_overlay_target_ == QLatin1String("local"))
    {
        mask_overlay_visible_ = false;
        mask_overlay_target_.clear();
        mask_place_active_ = false;
        mask_parametric_assist_active_ = false;
    }
    if (changed)
        emit editingScopeChanged();
}

void StudioPresenter::sync_local_edit_scope()
{
    if (!localEditing())
        return;
    const auto id = utf8_from_qstring(active_local_id_);
    const bool exists =
        std::any_of(develop_.local_adjustments.begin(), develop_.local_adjustments.end(),
                    [&](const auto &local) { return local.operation.instance_id == id; });
    if (!exists)
    {
        clear_local_edit_scope();
        return;
    }
    auto local = local_adjustment_develop(develop_, id);
    if (!local)
    {
        setError(qstring_from_utf8(local.error().message));
        clear_local_edit_scope();
        return;
    }
    // These cursors describe the selected component, not another persisted mask.
    local.value().local_mask_child_index = local_projection_.local_mask_child_index;
    local.value().local_mask_point_index = local_projection_.local_mask_point_index;
    local.value().color_checker_patch = local_projection_.color_checker_patch;
    local_projection_ = std::move(local).value();
    if (local_done_pending_ && !pending_save_ && !develop_job_in_flight_ &&
        develop_ == saved_develop_)
    {
        clear_local_edit_scope();
        enqueue_preview();
        emit editChanged();
    }
}

bool StudioPresenter::mutate_scoped_develop(DevelopParams next, const DevelopEdit edit,
                                            const bool refresh_preview,
                                            std::optional<std::string> history_coalesce_key)
{
    if (!localEditing())
        return mutate_develop(std::move(next), edit, refresh_preview,
                              std::move(history_coalesce_key));
    if (mask_gesture_before_ && !mask_gesture_updating_)
    {
        setError(QStringLiteral("Finish the active mask gesture before adjusting parameters."));
        return false;
    }
    DevelopParams complete = develop_;
    auto applied =
        set_local_adjustment_develop(complete, utf8_from_qstring(active_local_id_), next);
    if (!applied)
    {
        setError(qstring_from_utf8(applied.error().message));
        return false;
    }
    const bool cursor_changed =
        local_projection_.local_mask_child_index != next.local_mask_child_index ||
        local_projection_.local_mask_point_index != next.local_mask_point_index ||
        local_projection_.color_checker_patch != next.color_checker_patch;
    local_projection_ = std::move(next);
    if (history_coalesce_key)
        *history_coalesce_key = utf8_from_qstring(active_local_id_) + ":" + *history_coalesce_key;
    const auto effective_edit =
        local_creation_before_ && !mask_gesture_updating_ && edit == DevelopEdit::Commit ?
            DevelopEdit::Preview :
            edit;
    const bool changed = mutate_develop(std::move(complete), effective_edit, refresh_preview,
                                        std::move(history_coalesce_key));
    if (!changed && cursor_changed)
    {
        emit editChanged();
        emit previewChanged();
    }
    return changed || cursor_changed;
}

Result<bool> StudioPresenter::applyLocalAdjustmentCommand(const QString &action,
                                                          const QVariantMap &arguments)
{
    const auto failure = [](const std::string_view reason) -> Result<bool>
    {
        return make_error(ErrorCode::kConflict, "Local adjustment command was rejected",
                          {{"reason", std::string(reason)}});
    };
    if (!develop_loaded_ || selected_asset_id_.isEmpty() || busy_ || import_work_active_)
        return failure("command_unavailable");
    QStringList allowed;
    if (action == QLatin1String("create"))
        allowed = {QStringLiteral("kind")};
    else if (action == QLatin1String("done"))
        allowed = {};
    else if (action == QLatin1String("draw"))
        allowed = {QStringLiteral("enabled")};
    else if (action == QLatin1String("rename"))
        allowed = {QStringLiteral("id"), QStringLiteral("name")};
    else if (action == QLatin1String("enable"))
        allowed = {QStringLiteral("id"), QStringLiteral("enabled")};
    else if (action == QLatin1String("set"))
        allowed = {QStringLiteral("id"), QStringLiteral("fields")};
    else if (action == QLatin1String("component"))
        allowed = {QStringLiteral("id"), QStringLiteral("kind"), QStringLiteral("combine")};
    else if (action.startsWith(QLatin1String("gesture_")))
        allowed = {QStringLiteral("id"), QStringLiteral("asset"), QStringLiteral("x"),
                   QStringLiteral("y"),
                   action == QLatin1String("gesture_begin") ? QStringLiteral("handle") :
                                                              QStringLiteral("token")};
    else
        allowed = {QStringLiteral("id")};
    for (auto it = arguments.constBegin(); it != arguments.constEnd(); ++it)
    {
        if (!allowed.contains(it.key()))
            return failure("unknown_local_adjustment_argument");
        const auto type = it.value().metaType().id();
        if (it.key() == QLatin1String("fields"))
        {
            if (type != QMetaType::QVariantMap)
                return failure("invalid_local_adjustment_fields");
        }
        else if (it.key() == QLatin1String("enabled"))
        {
            if (type != QMetaType::Bool)
                return failure("invalid_local_adjustment_enabled");
        }
        else if (it.key() == QLatin1String("kind") || it.key() == QLatin1String("combine") ||
                 it.key() == QLatin1String("x") || it.key() == QLatin1String("y"))
        {
            if (type != QMetaType::Double && type != QMetaType::Int &&
                type != QMetaType::LongLong && type != QMetaType::UInt &&
                type != QMetaType::ULongLong)
                return failure("invalid_local_adjustment_number");
        }
        else if (type != QMetaType::QString)
            return failure("invalid_local_adjustment_text");
    }
    if (arguments.size() > 5)
        return failure("invalid_local_adjustment_arguments");
    if (action.startsWith(QLatin1String("gesture_")))
        return applyMaskGesture(action, arguments);
    const QString id = arguments.value(QStringLiteral("id")).toString();
    if (action == QLatin1String("done"))
    {
        if (!localEditing())
            return false;
        if (local_creation_before_)
        {
            auto previous = *local_creation_before_;
            clear_local_edit_scope();
            mutate_develop(std::move(previous), DevelopEdit::Overlay);
            enqueue_preview();
            return true;
        }
        if (mask_gesture_before_)
            return failure("mask_gesture_active");
        local_done_pending_ = true;
        if (develop_ != saved_develop_ && !pending_save_ && !develop_job_in_flight_)
            mutate_develop(develop_, DevelopEdit::Commit);
        sync_local_edit_scope();
        emit editChanged();
        emit previewChanged();
        return true;
    }
    if (action == QLatin1String("select"))
    {
        if (mask_gesture_before_)
            return failure("mask_gesture_active");
        if (local_creation_before_ && id != active_local_id_ && !active_local_id_.isEmpty())
            return failure("mask_creation_active");
        auto local = local_adjustment_develop(develop_, utf8_from_qstring(id));
        if (!local)
            return local.error();
        break_history_coalescing();
        active_local_id_ = id;
        local_projection_ = std::move(local).value();
        local_done_pending_ = false;
        crop_tool_active_ = false;
        white_balance_pick_active_ = false;
        mask_place_active_ = false;
        mask_parametric_assist_active_ = false;
        mask_overlay_target_ = QStringLiteral("local");
        mask_overlay_visible_ = !local_creation_before_.has_value();
        mask_drawing_active_ = true;
        emit editingScopeChanged();
        emit editChanged();
        emit previewChanged();
        enqueue_preview();
        return true;
    }

    DevelopParams next = develop_;
    if (action == QLatin1String("set"))
    {
        auto local = local_adjustment_develop(next, utf8_from_qstring(id));
        if (!local)
            return local.error();
        if (id == active_local_id_)
            local = local_projection_;
        const auto fields = arguments.value(QStringLiteral("fields")).toMap();
        if (fields.isEmpty() || fields.size() > 256)
            return failure("invalid_local_adjustment_fields");
        for (auto it = fields.constBegin(); it != fields.constEnd(); ++it)
        {
            const auto type = it.value().metaType().id();
            if (type != QMetaType::Double && type != QMetaType::Int && type != QMetaType::LongLong)
                return failure("invalid_local_adjustment_value");
            bool ok = false;
            const double value = it.value().toDouble(&ok);
            if (!ok)
                return failure("invalid_local_adjustment_value");
            auto applied =
                apply_develop_field_strict(local.value(), utf8_from_qstring(it.key()), value);
            if (!applied)
                return applied.error();
        }
        auto applied = set_local_adjustment_develop(next, utf8_from_qstring(id), local.value());
        if (!applied)
            return applied.error();
        if (local_creation_before_ || mask_gesture_before_)
            return failure("mask_gesture_active");
        if (id == active_local_id_)
            return mutate_scoped_develop(std::move(local).value(), DevelopEdit::Commit);
        return mutate_develop(std::move(next), DevelopEdit::Commit);
    }
    if (action == QLatin1String("draw"))
    {
        if (!localEditing())
            return failure("local_adjustment_not_selected");
        mask_drawing_active_ = arguments.value(QStringLiteral("enabled")).toBool();
        emit editChanged();
        return true;
    }
    if (action == QLatin1String("component"))
    {
        if (id != active_local_id_)
            return failure("local_adjustment_not_selected");
        auto local = local_projection_;
        auto added =
            add_local_mask_component(local, arguments.value(QStringLiteral("kind")).toLongLong(),
                                     arguments.value(QStringLiteral("combine")).toLongLong());
        if (!added)
            return added.error();
        if (arguments.value(QStringLiteral("kind")).toLongLong() == 5)
            return mutate_scoped_develop(std::move(local), DevelopEdit::Commit);
        if (!local_creation_before_)
            local_creation_before_ = develop_;
        mask_drawing_active_ = true;
        return mutate_scoped_develop(std::move(local), DevelopEdit::Overlay);
    }
    QString select_after;
    if (action == QLatin1String("create"))
    {
        if (local_creation_before_ || mask_gesture_before_)
            return failure("mask_creation_active");
        bool ok = false;
        const auto kind = arguments.value(QStringLiteral("kind")).toLongLong(&ok);
        if (!ok || static_cast<double>(kind) != arguments.value(QStringLiteral("kind")).toDouble())
            return failure("invalid_mask_kind");
        const QString fresh =
            QStringLiteral("local-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto created = create_local_adjustment(next, utf8_from_qstring(fresh), kind);
        if (!created)
            return created.error();
        next.local_adjustments.back().operation.name =
            utf8_from_qstring(QCoreApplication::translate("DevelopPanel", "Mask %1")
                                  .arg(next.local_adjustments.size()));
        select_after = fresh;
        if (kind != 5)
            local_creation_before_ = develop_;
    }
    else if (action == QLatin1String("duplicate"))
    {
        const QString fresh =
            QStringLiteral("local-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto copied =
            duplicate_local_adjustment(next, utf8_from_qstring(id), utf8_from_qstring(fresh));
        if (!copied)
            return copied.error();
        select_after = fresh;
    }
    else if (action == QLatin1String("delete"))
    {
        auto removed = delete_local_adjustment(next, utf8_from_qstring(id));
        if (!removed)
            return removed.error();
    }
    else
    {
        auto found = std::find_if(next.local_adjustments.begin(), next.local_adjustments.end(),
                                  [&](const auto &local)
                                  { return local.operation.instance_id == utf8_from_qstring(id); });
        if (found == next.local_adjustments.end())
            return failure("local_adjustment_not_found");
        if (action == QLatin1String("rename"))
        {
            const auto name = arguments.value(QStringLiteral("name")).toString().trimmed();
            if (name.isEmpty() || name.toUtf8().size() > 200)
                return failure("invalid_local_adjustment_name");
            found->operation.name = utf8_from_qstring(name);
        }
        else if (action == QLatin1String("enable"))
        {
            if (arguments.value(QStringLiteral("enabled")).metaType().id() != QMetaType::Bool)
                return failure("invalid_local_adjustment_enabled");
            found->operation.enabled = arguments.value(QStringLiteral("enabled")).toBool();
            found->operation.bypass = false;
        }
        else if (action == QLatin1String("invert"))
        {
            auto local = local_adjustment_develop(next, utf8_from_qstring(id));
            if (!local)
                return local.error();
            const auto state = develop_mask_editor_state(local.value(), DevelopMaskTarget::kLocal);
            auto inverted = apply_develop_mask_field_strict(local.value(), "localMaskInverted",
                                                            state.inverted ? 0 : 1);
            if (!inverted)
                return inverted.error();
            auto updated = set_local_adjustment_develop(next, utf8_from_qstring(id), local.value());
            if (!updated)
                return updated.error();
        }
        else
            return failure("unknown_local_adjustment_action");
    }
    break_history_coalescing();
    const bool changed = mutate_develop(
        std::move(next), local_creation_before_ ? DevelopEdit::Overlay : DevelopEdit::Commit);
    if (!select_after.isEmpty())
    {
        // The newly staged creation is now the sole draft; the following select
        // publishes its scope once, without accepting a user switch away from it.
        if (local_creation_before_)
            active_local_id_.clear();
        return applyLocalAdjustmentCommand(QStringLiteral("select"),
                                           {{QStringLiteral("id"), select_after}});
    }
    return changed;
}
} // namespace ravo
