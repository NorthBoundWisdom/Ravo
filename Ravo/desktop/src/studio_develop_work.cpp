#include "ravo/desktop/studio_presenter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <numbers>
#include <set>
#include <string_view>
#include <utility>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>
#include <QMetaObject>
#include <QMutexLocker>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include "ravo/recipe/develop.h"
#include "ravo/recipe/develop_mask.h"
#include "ravo/recipe/recipe.h"
#include "ravo/recipe/style.h"
#include "ravo/adapters/crs_xmp.h"
#include "ravo/adapters/text_file.h"
#include "studio_debug_info.h"
#include "studio_qt.h"

namespace ravo
{

void StudioPresenter::load_develop_for_selection()
{
    break_history_coalescing();
    develop_ = {};
    saved_develop_ = {};
    develop_loaded_ = false;
    develop_load_error_.clear();
    white_balance_pick_active_ = false;
    mask_place_active_ = false;
    mask_parametric_assist_active_ = false;
    undo_stack_.clear();
    redo_stack_.clear();
    recipe_history_.clear();
    recipe_history_entries_.clear();
    active_history_id_ = 0;
    active_history_seq_ = 0;
    if (selected_asset_id_.isEmpty())
    {
        emit editChanged();
        return;
    }
    const auto asset_id = utf8_from_qstring(selected_asset_id_);
    executor_.post(
        [this, asset_id]()
        {
            Result<Recipe> loaded = make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<std::vector<RecipeHistoryEntry>> history =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                loaded = service_->load_recipe(asset_id);
                history = service_->list_recipe_history(asset_id);
            }
            QMetaObject::invokeMethod(
                this,
                [this, asset_id, loaded = std::move(loaded), history = std::move(history)]() mutable
                {
                    if (utf8_from_qstring(selected_asset_id_) != asset_id)
                    {
                        return;
                    }
                    if (history)
                    {
                        apply_recipe_history(history.value());
                    }
                    else
                    {
                        recipe_history_.clear();
                        recipe_history_entries_.clear();
                    }
                    if (!loaded)
                    {
                        develop_ = {};
                        saved_develop_ = {};
                        develop_load_error_ = qstring_from_utf8(loaded.error().message);
                        sync_active_history();
                        emit editChanged();
                        setError(qstring_from_utf8(loaded.error().message));
                        return;
                    }
                    auto params = develop_from_recipe(loaded.value());
                    if (!params)
                    {
                        develop_ = {};
                        saved_develop_ = {};
                        develop_load_error_ = qstring_from_utf8(params.error().message);
                        sync_active_history();
                        emit editChanged();
                        setError(qstring_from_utf8(params.error().message));
                        return;
                    }
                    develop_ = params.value();
                    selected_exposure_instance_index_ = 0;
                    selected_color_balance_rgb_instance_index_ = 0;
                    sync_selected_instance_edit_buffers(develop_);
                    saved_develop_ = develop_;
                    develop_loaded_ = true;
                    develop_load_error_.clear();
                    sync_curve_ui_from_develop();
                    sync_active_history();
                    emit editChanged();
                },
                Qt::QueuedConnection);
        },
        TaskPriority::kForeground);
}

void StudioPresenter::break_history_coalescing()
{
    history_coalesce_key_.reset();
    history_coalesce_id_.reset();
}

void StudioPresenter::commit_develop(DevelopParams params, const bool push_history,
                                     const bool refresh_preview,
                                     const RecipeHistoryWrite history_write,
                                     std::optional<std::string> history_coalesce_key)
{
    if (selected_asset_id_.isEmpty() || catalog_path_.isEmpty())
    {
        return;
    }
    const auto intent_started_at = std::chrono::steady_clock::now();
    clamp_develop(params);
    const auto previous = saved_develop_;
    const bool same_control = push_history && params != saved_develop_ &&
                              history_write == RecipeHistoryWrite::kAppendIfNew &&
                              history_coalesce_key && history_coalesce_key_ &&
                              *history_coalesce_key == *history_coalesce_key_;
    bool pushed_undo = false;
    if (push_history && params != saved_develop_ && !same_control)
    {
        undo_stack_.push_back(saved_develop_);
        pushed_undo = true;
        if (undo_stack_.size() > 40U)
        {
            undo_stack_.erase(undo_stack_.begin());
        }
        redo_stack_.clear();
    }
    if (history_write != RecipeHistoryWrite::kAppendIfNew || !history_coalesce_key)
    {
        break_history_coalescing();
    }
    else if (!same_control)
    {
        history_coalesce_key_ = history_coalesce_key;
        history_coalesce_id_.reset();
    }
    const auto coalesce_history_id = same_control ? history_coalesce_id_ : std::nullopt;
    std::optional<std::int64_t> discard_after;
    if (push_history && history_write == RecipeHistoryWrite::kAppendIfNew && !same_control &&
        !recipe_history_entries_.empty() &&
        active_history_seq_ < recipe_history_entries_.front().seq)
    {
        discard_after = active_history_seq_;
        const auto cursor_seq = *discard_after;
        recipe_history_entries_.erase(std::remove_if(recipe_history_entries_.begin(),
                                                     recipe_history_entries_.end(),
                                                     [cursor_seq](const RecipeHistoryEntry &entry)
                                                     { return entry.seq > cursor_seq; }),
                                      recipe_history_entries_.end());
        QVariantList kept;
        kept.reserve(recipe_history_.size());
        for (const auto &row : recipe_history_)
        {
            if (row.toMap().value(QStringLiteral("seq")).toLongLong() <= cursor_seq)
            {
                kept.push_back(row);
            }
        }
        recipe_history_ = std::move(kept);
    }
    develop_ = params;
    emit editChanged();
    if (refresh_preview)
    {
        refresh_inspect_roi();
    }
    const bool crop_guides = crop_tool_active_ && !before_after_;
    const bool overlay = mask_overlay_visible_ && !before_after_;
    const bool needs_first_preview =
        refresh_preview && !crop_guides && !overlay && !before_after_ &&
        (!displayed_develop_.has_value() || *displayed_develop_ != params);
    preview_loading_ = refresh_preview;
    emit previewChanged();
    const auto request_revision = develop_preview_owner_.supersede("develop_save_superseded");
    pending_save_ = PendingDevelopWork{
        .save = true,
        .interactive = crop_guides || overlay || needs_first_preview,
        .params = params,
        .previous = previous,
        .push_history = push_history,
        .pushed_undo = pushed_undo,
        .history_write = history_write,
        .discard_history_after_seq = discard_after,
        .history_coalesce_key = std::move(history_coalesce_key),
        .coalesce_history_id = coalesce_history_id,
        .asset_id = utf8_from_qstring(selected_asset_id_),
        .ignore_edits = before_after_,
        .ignore_crop = crop_guides,
        .ignore_straighten = false,
        .refresh_preview = refresh_preview,
        .settle_preview = needs_first_preview,
        .overlay_mask_id = current_overlay_mask_id(params),
        .request_revision = request_revision,
        .intent_started_at = intent_started_at,
    };
    pending_preview_.reset();
    kick_develop_work();
}

[[nodiscard]] std::optional<std::string>
StudioPresenter::current_overlay_mask_id(const DevelopParams &params) const
{
    if (!mask_overlay_visible_ || before_after_)
    {
        return std::nullopt;
    }
    if (mask_overlay_target_ == QLatin1String("graduatednd"))
        return params.graduated_mask_id;
    if (mask_overlay_target_ == QLatin1String("color_balance_rgb"))
        return params.color_balance_rgb_mask_id;
    if (mask_overlay_target_ == QLatin1String("exposure"))
        return params.exposure_mask_id;
    if (mask_overlay_target_ == QLatin1String("rgb_curve"))
        return params.rgb_curve_mask_id;
    if (mask_overlay_target_ == QLatin1String("tone_curve"))
        return params.tone_curve_mask_id;
    if (mask_overlay_target_ == QLatin1String("highlights"))
        return params.highlights_mask_id;
    if (mask_overlay_target_ == QLatin1String("shadows"))
        return params.shadows_mask_id;
    if (mask_overlay_target_ == QLatin1String("whites"))
        return params.whites_mask_id;
    if (mask_overlay_target_ == QLatin1String("blacks"))
        return params.blacks_mask_id;
    return params.color_harmonizer_mask_id;
}

void StudioPresenter::preview_develop(DevelopParams params)
{
    if (selected_asset_id_.isEmpty() || catalog_path_.isEmpty())
    {
        return;
    }
    const auto intent_started_at = std::chrono::steady_clock::now();
    clamp_develop(params);
    if (params == develop_)
    {
        return;
    }
    develop_ = params;
    refresh_inspect_roi();
    const bool crop_guides = crop_tool_active_ && !before_after_;
    std::optional<std::uint64_t> request_revision;
    const bool finish_active_frame =
        develop_job_in_flight_ && develop_interactive_job_in_flight_ && !pending_save_.has_value();
    if (!finish_active_frame)
    {
        request_revision = develop_preview_owner_.supersede("interactive_preview_superseded");
    }
    pending_preview_ = PendingDevelopWork{
        .interactive = true,
        .params = params,
        .pushed_undo = false,
        .history_write = RecipeHistoryWrite::kUnchanged,
        .discard_history_after_seq = {},
        .history_coalesce_key = {},
        .coalesce_history_id = {},
        .asset_id = utf8_from_qstring(selected_asset_id_),
        .ignore_edits = before_after_,
        .ignore_crop = crop_guides,
        .ignore_straighten = false,
        .overlay_mask_id = current_overlay_mask_id(params),
        .request_revision = request_revision,
        .intent_started_at = intent_started_at,
    };
    kick_develop_work();
    // Start the pixel job before notifying the broad inspector property set. QML may reevaluate
    // many edit bindings synchronously, while the owner-managed worker can render in parallel.
    emit editChanged();
}

bool StudioPresenter::mutate_develop(DevelopParams next, const DevelopEdit edit,
                                     const bool refresh_preview,
                                     std::optional<std::string> history_coalesce_key)
{
    clamp_develop(next);
    sync_selected_instance_edit_buffers(next);
    switch (edit)
    {
    case DevelopEdit::Overlay:
        if (next == develop_)
        {
            return false;
        }
        develop_ = std::move(next);
        emit editChanged();
        return true;
    case DevelopEdit::Preview:
        preview_develop(std::move(next));
        return true;
    case DevelopEdit::Commit:
        if (next == saved_develop_ && next == develop_)
        {
            return false;
        }
        if (next == saved_develop_)
        {
            develop_ = std::move(next);
            emit editChanged();
            if (refresh_preview)
            {
                enqueue_preview();
            }
            return true;
        }
        commit_develop(std::move(next), true, refresh_preview, RecipeHistoryWrite::kAppendIfNew,
                       std::move(history_coalesce_key));
        return true;
    case DevelopEdit::Restore:
        if (next == develop_ && next == saved_develop_)
        {
            return false;
        }
        commit_develop(std::move(next), true, refresh_preview, RecipeHistoryWrite::kUnchanged);
        return true;
    case DevelopEdit::Revert:
        commit_develop(std::move(next), false, refresh_preview, RecipeHistoryWrite::kUnchanged);
        return true;
    }
    return false;
}

void StudioPresenter::enqueue_preview()
{
    if (selected_asset_id_.isEmpty())
    {
        preview_loading_ = false;
        emit previewChanged();
        return;
    }
    preview_loading_ = true;
    emit previewChanged();
    const auto request_revision = develop_preview_owner_.supersede("preview_superseded");
    const bool crop_guides = crop_tool_active_ && !before_after_;
    const bool progressive_develop = browse_mode_ == QLatin1String("develop") &&
                                     !mask_overlay_visible_ && !crop_guides && !before_after_;
    refresh_inspect_roi();
    pending_preview_ = PendingDevelopWork{
        .interactive = mask_overlay_visible_ || crop_guides || progressive_develop,
        .params = develop_,
        .pushed_undo = false,
        .history_write = RecipeHistoryWrite::kUnchanged,
        .discard_history_after_seq = {},
        .history_coalesce_key = {},
        .coalesce_history_id = {},
        .asset_id = utf8_from_qstring(selected_asset_id_),
        .ignore_edits = before_after_,
        .ignore_crop = crop_guides,
        .ignore_straighten = false,
        .settle_preview = progressive_develop,
        .overlay_mask_id = current_overlay_mask_id(develop_),
        .request_revision = request_revision,
        .intent_started_at = std::chrono::steady_clock::now(),
    };
    kick_develop_work();
}

void StudioPresenter::request_comparison_before()
{
    if (!comparison_active_ || selected_asset_id_.isEmpty())
    {
        return;
    }
    comparison_before_requested_ = true;
    preview_loading_ = true;
    emit previewChanged();
    kick_develop_work();
}

void StudioPresenter::kick_develop_work()
{
    if (develop_job_in_flight_)
    {
        return;
    }
    PendingDevelopWork job;
    bool starting_comparison_before = false;
    if (pending_save_.has_value())
    {
        job = *pending_save_;
        pending_save_.reset();
    }
    else if (pending_preview_.has_value())
    {
        job = *pending_preview_;
        pending_preview_.reset();
    }
    else if (comparison_active_ && comparison_before_requested_)
    {
        comparison_before_requested_ = false;
        starting_comparison_before = true;
        job = PendingDevelopWork{
            .interactive = false,
            .params = develop_,
            .pushed_undo = false,
            .history_write = RecipeHistoryWrite::kUnchanged,
            .discard_history_after_seq = {},
            .history_coalesce_key = {},
            .coalesce_history_id = {},
            .asset_id = utf8_from_qstring(selected_asset_id_),
            .ignore_edits = true,
            .refresh_preview = true,
            .comparison_before = true,
            .overlay_mask_id = {},
            .request_revision = {},
            .intent_started_at = std::chrono::steady_clock::now(),
        };
    }
    else
    {
        kickThumbnailDemand();
        return;
    }
    if (starting_comparison_before)
    {
        job.request_revision = develop_preview_owner_.supersede("comparison_before_requested");
        preview_loading_ = true;
        emit previewChanged();
    }
    static_cast<void>(thumbnail_work_.cancel("foreground_preview_requested"));
    develop_job_in_flight_ = true;
    develop_interactive_job_in_flight_ = job.interactive && !job.save && !job.comparison_before;
    const auto revision = job.request_revision ?
                              *job.request_revision :
                              develop_preview_owner_.supersede("queued_preview_started");
    const auto cancellation = develop_preview_owner_.begin();
    executor_.post(
        [this, job, revision, cancellation]()
        {
            Result<RecipeSaveResult> saved =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<PreviewResult> preview = make_error(ErrorCode::kIo, "Catalog session is closed");
            bool save_ok = !job.save;
            if (service_ != nullptr)
            {
                if (job.save)
                {
                    saved = service_->save_develop_with_history(
                        job.asset_id, job.params,
                        RecipeSaveOptions{
                            .history_write = job.history_write,
                            .discard_history_after_seq = job.discard_history_after_seq,
                            .coalesce_history_id = job.coalesce_history_id,
                            .defer_recovery_publication = true,
                        });
                    save_ok = static_cast<bool>(saved);
                }
                if (save_ok && job.refresh_preview)
                {
                    PreviewRequest request;
                    request.asset_id = job.asset_id;
                    request.max_edge =
                        job.interactive ? kInteractivePreviewMaxEdge : kDefaultPreviewMaxEdge;
                    request.request_revision = revision;
                    request.ignore_edits = job.ignore_edits;
                    request.ignore_crop = job.ignore_crop;
                    request.ignore_straighten = job.ignore_straighten;
                    request.persist_preview_record =
                        job.comparison_before ? false : !job.interactive;
                    request.cancellation = cancellation;
                    // CPU RGB is required for live identity, comparison Image,
                    // scopes, and headless presenter tests. Metal still
                    // publishes an IOSurface for QML when the recipe stays GPU.
                    request.need_cpu_pixels = true;
                    if (job.overlay_mask_id)
                    {
                        request.overlay_mask_id = job.overlay_mask_id;
                        request.persist_preview_record = false;
                    }
                    preview = service_->request_preview(
                        request, job.interactive && !job.comparison_before ?
                                     std::optional<DevelopParams>{job.params} :
                                     std::optional<DevelopParams>{});
                }
            }
            const bool recovery_due = job.save && save_ok;
            QMetaObject::invokeMethod(
                this,
                [this, job, revision, saved = std::move(saved),
                 preview = std::move(preview)]() mutable
                {
                    develop_job_in_flight_ = false;
                    develop_interactive_job_in_flight_ = false;
                    const bool selected_matches =
                        utf8_from_qstring(selected_asset_id_) == job.asset_id;
                    if (job.save)
                    {
                        if (!saved)
                        {
                            if (selected_matches && !pending_save_.has_value() &&
                                develop_ == job.params)
                            {
                                develop_ = job.previous;
                                saved_develop_ = job.previous;
                                if (job.pushed_undo && !undo_stack_.empty())
                                {
                                    undo_stack_.pop_back();
                                }
                                if (job.history_coalesce_key &&
                                    history_coalesce_key_ == job.history_coalesce_key &&
                                    !job.coalesce_history_id)
                                {
                                    break_history_coalescing();
                                }
                                active_history_id_ = 0;
                                if (job.discard_history_after_seq)
                                {
                                    reload_recipe_history();
                                }
                                else
                                {
                                    sync_active_history();
                                }
                                preview_loading_ = false;
                                emit editChanged();
                                emit previewChanged();
                            }
                            setError(qstring_from_utf8(saved.error().message));
                            kick_develop_work();
                            return;
                        }
                        if (selected_matches)
                        {
                            if (job.coalesce_history_id && saved.value().history_id &&
                                *saved.value().history_id != *job.coalesce_history_id)
                            {
                                undo_stack_.push_back(job.previous);
                                if (undo_stack_.size() > 40U)
                                {
                                    undo_stack_.erase(undo_stack_.begin());
                                }
                                redo_stack_.clear();
                            }
                            saved_develop_ = job.params;
                            observed_catalog_revision_ =
                                std::max(observed_catalog_revision_, saved.value().revision);
                            assets_.updateAsset(saved.value().asset);
                            if (job.history_coalesce_key &&
                                history_coalesce_key_ == job.history_coalesce_key)
                            {
                                history_coalesce_id_ = saved.value().history_id;
                            }
                            if (pending_save_)
                            {
                                pending_save_->previous = job.params;
                                if (job.history_coalesce_key &&
                                    pending_save_->history_coalesce_key ==
                                        job.history_coalesce_key &&
                                    saved.value().history_id)
                                {
                                    pending_save_->coalesce_history_id = saved.value().history_id;
                                }
                            }
                            emit selectionChanged();
                            emit editChanged();
                            if (job.history_write == RecipeHistoryWrite::kAppendIfNew)
                            {
                                active_history_id_ = 0;
                                active_history_seq_ = 0;
                                reload_recipe_history();
                            }
                            else
                            {
                                sync_active_history();
                            }
                        }
                    }
                    if (!develop_preview_owner_.accepts(revision, job.asset_id,
                                                        utf8_from_qstring(selected_asset_id_)))
                    {
                        if (job.comparison_before && comparison_active_ &&
                            comparison_before_url_.isEmpty())
                        {
                            comparison_before_requested_ = true;
                        }
                        kick_develop_work();
                        return;
                    }
                    if (job.save && !job.refresh_preview)
                    {
                        preview_loading_ = false;
                        emit previewChanged();
                        kick_develop_work();
                        return;
                    }
                    preview_loading_ = false;
                    if (!preview)
                    {
                        if (preview.error().code == ErrorCode::kCancelled)
                        {
                            if (job.comparison_before && comparison_active_ &&
                                comparison_before_url_.isEmpty())
                            {
                                comparison_before_requested_ = true;
                            }
                            kick_develop_work();
                            return;
                        }
                        if (preview.error().code == ErrorCode::kNotFound)
                        {
                            assets_.markOriginalMissing(job.asset_id);
                            emit selectionChanged();
                        }
                        else
                        {
                            setError(qstring_from_utf8(preview.error().message));
                        }
                        if (job.comparison_before && clear_comparison())
                        {
                            emit editChanged();
                        }
                        emit previewChanged();
                        kick_develop_work();
                        return;
                    }
                    if (preview.value().original_missing)
                    {
                        assets_.markOriginalMissing(job.asset_id);
                        emit selectionChanged();
                    }
                    if (job.ignore_crop && crop_tool_active_)
                    {
                        crop_guide_ready_ = true;
                    }
                    if (job.comparison_before)
                    {
                        if (comparison_active_)
                        {
                            show_comparison_before_result(preview.value(), revision);
                            if (comparison_before_url_.isEmpty() && clear_comparison())
                            {
                                emit editChanged();
                            }
                        }
                        emit previewChanged();
                        kick_develop_work();
                        return;
                    }
                    show_preview_result(preview.value(), revision, job.interactive);
                    displayed_develop_ = job.ignore_edits ?
                                             std::optional<DevelopParams>{} :
                                             std::optional<DevelopParams>{job.params};
                    if (job.interactive &&
                        job.intent_started_at != std::chrono::steady_clock::time_point{})
                    {
                        const auto intent_to_image_us =
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - job.intent_started_at)
                                .count();
                        emit interactivePreviewPublished(
                            static_cast<qulonglong>(revision),
                            static_cast<qlonglong>(intent_to_image_us));
                    }
                    emit previewChanged();
                    if (job.settle_preview)
                    {
                        pending_preview_ = PendingDevelopWork{
                            .save = false,
                            .interactive = false,
                            .params = job.params,
                            .previous = {},
                            .push_history = false,
                            .pushed_undo = false,
                            .history_write = RecipeHistoryWrite::kUnchanged,
                            .discard_history_after_seq = {},
                            .history_coalesce_key = {},
                            .coalesce_history_id = {},
                            .asset_id = job.asset_id,
                            .ignore_edits = job.ignore_edits,
                            .ignore_crop = job.ignore_crop,
                            .ignore_straighten = job.ignore_straighten,
                            .refresh_preview = true,
                            .settle_preview = false,
                            .overlay_mask_id = {},
                            .request_revision = {},
                            .intent_started_at = job.intent_started_at,
                        };
                    }
                    if (comparison_active_ && comparison_before_url_.isEmpty())
                    {
                        comparison_before_requested_ = true;
                    }
                    kick_develop_work();
                },
                Qt::QueuedConnection);
            if (recovery_due && service_ != nullptr)
            {
                auto synchronized = service_->sync_recovery(std::string_view{job.asset_id});
                if (!synchronized)
                {
                    const auto failure = qstring_from_utf8(synchronized.error().message);
                    QMetaObject::invokeMethod(
                        this,
                        [this, failure]
                        {
                            setError(QCoreApplication::translate(
                                         "StudioPresenter",
                                         "Edit was saved, but recovery synchronization failed: ") +
                                     failure);
                        },
                        Qt::QueuedConnection);
                }
            }
        },
        TaskPriority::kForeground);
}

} // namespace ravo
