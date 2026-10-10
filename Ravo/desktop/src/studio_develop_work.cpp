#include "ravo/desktop/studio_develop_presenter.h"
#include "ravo/services/preview_service.h"
#include "ravo/services/recovery_service.h"

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
#include <QThread>
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
#include "ravo/recipe/local_adjustment.h"
#include "ravo/recipe/develop_mask.h"
#include "ravo/recipe/recipe.h"
#include "ravo/recipe/style.h"
#include "ravo/adapters/crs_xmp.h"
#include "ravo/adapters/text_file.h"
#include "studio_debug_info.h"
#include "studio_qt.h"
#include "studio_preview_handoff.h"

namespace ravo
{

void StudioDevelopPresenter::load_develop_for_selection()
{
    Q_ASSERT(QThread::currentThread() == thread());
    const auto load_generation = ++recipe_load_generation_;
    const auto session_generation = catalog_session_generation_;
    const auto catalog = catalog_path_;
    break_history_coalescing();
    state_.develop_ = {};
    clear_local_edit_scope();
    state_.saved_develop_ = {};
    state_.loaded_recipe_asset_.reset();
    state_.loaded_recipe_history_head_ = 0;
    state_.develop_loaded_ = false;
    state_.develop_preview_deferred_ = false;
    state_.develop_load_error_.clear();
    state_.white_balance_pick_active_ = false;
    state_.mask_place_active_ = false;
    state_.mask_parametric_assist_active_ = false;
    state_.undo_stack_.clear();
    state_.redo_stack_.clear();
    state_.recipe_history_.clear();
    state_.recipe_history_entries_.clear();
    state_.active_history_id_ = 0;
    state_.active_history_seq_ = 0;
    if (selected_asset_id_.isEmpty() ||
        host_.selected_media_type().startsWith(QLatin1String("video/")))
    {
        emit editChanged();
        return;
    }
    const auto asset_id = utf8_from_qstring(selected_asset_id_);
    const bool queued = executor_.post(
        [this, asset_id, catalog, load_generation, session_generation]()
        {
            Q_ASSERT(executor_.is_worker_thread());
            Result<Recipe> loaded = make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<std::vector<RecipeHistoryEntry>> history =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (host_.worker_develop_service() != nullptr)
            {
                loaded = host_.worker_develop_service()->load_recipe(asset_id);
                history = host_.worker_develop_service()->list_recipe_history(asset_id);
            }
            QMetaObject::invokeMethod(
                this,
                [this, asset_id, catalog, load_generation, session_generation,
                 loaded = std::move(loaded), history = std::move(history)]() mutable
                {
                    Q_ASSERT(QThread::currentThread() == thread());
                    if (stopped_ || catalog != catalog_path_ ||
                        session_generation != catalog_session_generation_ ||
                        load_generation != recipe_load_generation_ ||
                        utf8_from_qstring(selected_asset_id_) != asset_id)
                    {
                        return;
                    }
                    if (history)
                    {
                        apply_recipe_history(history.value());
                        state_.loaded_recipe_history_head_ =
                            history.value().empty() ? 0 : history.value().front().id;
                    }
                    else
                    {
                        state_.recipe_history_.clear();
                        state_.recipe_history_entries_.clear();
                    }
                    if (!loaded || !history)
                    {
                        state_.develop_ = {};
                        state_.saved_develop_ = {};
                        const auto &error = !loaded ? loaded.error() : history.error();
                        state_.develop_load_error_ = qstring_from_utf8(error.message);
                        sync_active_history();
                        state_.develop_preview_deferred_ = false;
                        if (browse_mode_ == QLatin1String("develop"))
                        {
                            host_.preview_loading(false);
                            emit previewChanged();
                        }
                        emit editChanged();
                        emit errorOccurred(state_.develop_load_error_);
                        return;
                    }
                    auto params = develop_from_recipe(loaded.value());
                    if (params)
                    {
                        auto promoted = promote_legacy_local_adjustments(params.value());
                        if (!promoted)
                            params = promoted.error();
                    }
                    if (!params)
                    {
                        state_.develop_ = {};
                        state_.saved_develop_ = {};
                        state_.develop_load_error_ = qstring_from_utf8(params.error().message);
                        sync_active_history();
                        state_.develop_preview_deferred_ = false;
                        if (browse_mode_ == QLatin1String("develop"))
                        {
                            host_.preview_loading(false);
                            emit previewChanged();
                        }
                        emit editChanged();
                        emit errorOccurred(qstring_from_utf8(params.error().message));
                        return;
                    }
                    state_.develop_ = params.value();
                    state_.selected_exposure_instance_index_ = 0;
                    state_.selected_color_balance_rgb_instance_index_ = 0;
                    sync_selected_instance_edit_buffers(state_.develop_);
                    state_.saved_develop_ = state_.develop_;
                    state_.loaded_recipe_asset_ = loaded.value().asset;
                    state_.develop_loaded_ = true;
                    state_.develop_load_error_.clear();
                    sync_curve_ui_from_develop();
                    sync_active_history();
                    const bool preview_deferred = state_.develop_preview_deferred_;
                    state_.develop_preview_deferred_ = false;
                    if (preview_deferred && browse_mode_ == QLatin1String("develop"))
                    {
                        host_.request_selection_preview();
                    }
                    emit editChanged();
                },
                Qt::QueuedConnection);
        },
        TaskPriority::kForeground);
    if (!queued)
    {
        state_.develop_load_error_ = QStringLiteral("Recipe load executor is stopped");
        state_.develop_preview_deferred_ = false;
        host_.preview_loading(false);
        emit errorOccurred(state_.develop_load_error_);
        emit previewChanged();
        emit editChanged();
    }
}

void StudioDevelopPresenter::break_history_coalescing()
{
    state_.history_coalesce_key_.reset();
    state_.history_coalesce_id_.reset();
}

void StudioDevelopPresenter::commit_develop(DevelopParams params, const bool push_history,
                                            const bool refresh_preview,
                                            const RecipeHistoryWrite history_write,
                                            std::optional<std::string> history_coalesce_key)
{
    if (!state_.develop_load_error_.isEmpty())
    {
        emit errorOccurred(state_.develop_load_error_);
        return;
    }
    if (selected_asset_id_.isEmpty() || catalog_path_.isEmpty())
    {
        return;
    }
    const auto intent_started_at = std::chrono::steady_clock::now();
    clamp_develop(params);
    const auto previous = state_.saved_develop_;
    const auto history_head_before = state_.loaded_recipe_history_head_;
    const bool same_control = push_history && params != state_.saved_develop_ &&
                              history_write == RecipeHistoryWrite::kAppendIfNew &&
                              history_coalesce_key && state_.history_coalesce_key_ &&
                              *history_coalesce_key == *state_.history_coalesce_key_;
    bool pushed_undo = false;
    if (push_history && params != state_.saved_develop_ && !same_control)
    {
        state_.undo_stack_.push_back(state_.saved_develop_);
        pushed_undo = true;
        if (state_.undo_stack_.size() > 40U)
        {
            state_.undo_stack_.erase(state_.undo_stack_.begin());
        }
        state_.redo_stack_.clear();
    }
    if (history_write != RecipeHistoryWrite::kAppendIfNew || !history_coalesce_key)
    {
        break_history_coalescing();
    }
    else if (!same_control)
    {
        state_.history_coalesce_key_ = history_coalesce_key;
        state_.history_coalesce_id_.reset();
    }
    const auto coalesce_history_id = same_control ? state_.history_coalesce_id_ : std::nullopt;
    std::optional<std::int64_t> discard_after;
    if (push_history && history_write == RecipeHistoryWrite::kAppendIfNew && !same_control &&
        !state_.recipe_history_entries_.empty() &&
        state_.active_history_seq_ < state_.recipe_history_entries_.front().seq)
    {
        discard_after = state_.active_history_seq_;
        const auto cursor_seq = *discard_after;
        state_.recipe_history_entries_.erase(
            std::remove_if(
                state_.recipe_history_entries_.begin(), state_.recipe_history_entries_.end(),
                [cursor_seq](const RecipeHistoryEntry &entry) { return entry.seq > cursor_seq; }),
            state_.recipe_history_entries_.end());
        QVariantList kept;
        kept.reserve(state_.recipe_history_.size());
        for (const auto &row : state_.recipe_history_)
        {
            if (row.toMap().value(QStringLiteral("seq")).toLongLong() <= cursor_seq)
            {
                kept.push_back(row);
            }
        }
        state_.recipe_history_ = std::move(kept);
    }
    state_.develop_ = params;
    emit editChanged();
    if (refresh_preview)
    {
        host_.refresh_roi();
    }
    const bool crop_guides = state_.crop_tool_active_ && !state_.before_after_;
    const bool overlay = maskOverlayActive() && !state_.before_after_;
    const bool needs_first_preview =
        refresh_preview && !crop_guides && !overlay && !state_.before_after_ &&
        (!inspect_.displayedDevelop().has_value() || *inspect_.displayedDevelop() != params);
    host_.preview_loading(refresh_preview);
    emit previewChanged();
    const auto request_revision =
        state_.develop_preview_owner_.supersede("develop_save_superseded");
    state_.pending_save_ = PendingDevelopWork{
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
        .ignore_edits = state_.before_after_,
        .ignore_crop = crop_guides,
        .ignore_straighten = false,
        .refresh_preview = refresh_preview,
        .settle_preview = needs_first_preview,
        .overlay_mask_id = current_overlay_mask_id(params),
        .request_revision = request_revision,
        .intent_started_at = intent_started_at,
        .expected_source = state_.loaded_recipe_asset_,
        .expected_history_head =
            discard_after ? std::optional<std::int64_t>{history_head_before} : std::nullopt,
    };
    state_.pending_preview_.reset();
    kick_develop_work();
}

[[nodiscard]] std::optional<std::string>
StudioDevelopPresenter::current_overlay_mask_id(const DevelopParams &params) const
{
    if (!maskOverlayActive() || state_.before_after_)
    {
        return std::nullopt;
    }
    if (state_.mask_overlay_target_ == QLatin1String("local"))
    {
        const auto id = utf8_from_qstring(state_.active_local_id_);
        for (const auto &local : params.local_adjustments)
            if (local.operation.instance_id == id)
                return local.operation.mask_id;
        return std::nullopt;
    }
    if (state_.mask_overlay_target_ == QLatin1String("graduatednd"))
        return params.graduated_mask_id;
    if (state_.mask_overlay_target_ == QLatin1String("color_balance_rgb"))
        return params.color_balance_rgb_mask_id;
    if (state_.mask_overlay_target_ == QLatin1String("exposure"))
        return params.exposure_mask_id;
    if (state_.mask_overlay_target_ == QLatin1String("rgb_curve"))
        return params.rgb_curve_mask_id;
    if (state_.mask_overlay_target_ == QLatin1String("tone_curve"))
        return params.tone_curve_mask_id;
    if (state_.mask_overlay_target_ == QLatin1String("highlights"))
        return params.highlights_mask_id;
    if (state_.mask_overlay_target_ == QLatin1String("shadows"))
        return params.shadows_mask_id;
    if (state_.mask_overlay_target_ == QLatin1String("whites"))
        return params.whites_mask_id;
    if (state_.mask_overlay_target_ == QLatin1String("blacks"))
        return params.blacks_mask_id;
    return params.color_harmonizer_mask_id;
}

void StudioDevelopPresenter::preview_develop(DevelopParams params)
{
    if (selected_asset_id_.isEmpty() || catalog_path_.isEmpty())
    {
        return;
    }
    const auto intent_started_at = std::chrono::steady_clock::now();
    clamp_develop(params);
    if (params == state_.develop_)
    {
        return;
    }
    state_.develop_ = params;
    host_.refresh_roi();
    const bool crop_guides = state_.crop_tool_active_ && !state_.before_after_;
    std::optional<std::uint64_t> request_revision;
    const bool finish_active_frame = state_.develop_job_in_flight_ &&
                                     state_.develop_interactive_job_in_flight_ &&
                                     !state_.pending_save_.has_value();
    if (!finish_active_frame)
    {
        request_revision =
            state_.develop_preview_owner_.supersede("interactive_preview_superseded");
    }
    state_.pending_preview_ = PendingDevelopWork{
        .interactive = true,
        .params = params,
        .pushed_undo = false,
        .history_write = RecipeHistoryWrite::kUnchanged,
        .discard_history_after_seq = {},
        .history_coalesce_key = {},
        .coalesce_history_id = {},
        .asset_id = utf8_from_qstring(selected_asset_id_),
        .ignore_edits = state_.before_after_,
        .ignore_crop = crop_guides,
        .ignore_straighten = false,
        .overlay_mask_id = current_overlay_mask_id(params),
        .request_revision = request_revision,
        .intent_started_at = intent_started_at,
        .expected_source = {},
        .expected_history_head = {},
    };
    kick_develop_work();
    // Start the pixel job before notifying the broad inspector property set. QML may reevaluate
    // many edit bindings synchronously, while the owner-managed worker can render in parallel.
    emit editChanged();
}

bool StudioDevelopPresenter::mutate_develop(DevelopParams next, const DevelopEdit edit,
                                            const bool refresh_preview,
                                            std::optional<std::string> history_coalesce_key)
{
    if (!state_.develop_load_error_.isEmpty())
    {
        emit errorOccurred(state_.develop_load_error_);
        return false;
    }
    if (!state_.mask_gesture_updating_ &&
        (state_.mask_gesture_before_ ||
         (state_.local_creation_before_ && edit == DevelopEdit::Commit)))
    {
        emit errorOccurred(QCoreApplication::translate(
            "DevelopPanel", "Finish mask editing before using global tools."));
        return false;
    }
    clamp_develop(next);
    sync_selected_instance_edit_buffers(next);
    switch (edit)
    {
    case DevelopEdit::Overlay:
        if (next == state_.develop_)
        {
            return false;
        }
        state_.develop_ = std::move(next);
        emit editChanged();
        return true;
    case DevelopEdit::Preview:
        preview_develop(std::move(next));
        return true;
    case DevelopEdit::Commit:
        if (next == state_.saved_develop_ && next == state_.develop_)
        {
            return false;
        }
        if (next == state_.saved_develop_)
        {
            state_.develop_ = std::move(next);
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
        if (next == state_.develop_ && next == state_.saved_develop_)
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

void StudioDevelopPresenter::enqueue_preview()
{
    if (selected_asset_id_.isEmpty())
    {
        host_.preview_loading(false);
        emit previewChanged();
        return;
    }
    // Selection loads the recipe on the serial catalog executor and publishes
    // the resulting DevelopParams back on the UI thread. Do not capture the
    // temporary identity value for a Develop render while that publication is
    // still queued: it warms the RAW/GPU working generation for the wrong
    // recipe and makes the first real slider intent pay the complete rebuild.
    if (browse_mode_ == QLatin1String("develop") && !state_.develop_loaded_)
    {
        state_.develop_preview_deferred_ = true;
        host_.preview_loading(true);
        emit previewChanged();
        return;
    }
    host_.preview_loading(true);
    emit previewChanged();
    const auto request_revision = state_.develop_preview_owner_.supersede("preview_superseded");
    const bool crop_guides = state_.crop_tool_active_ && !state_.before_after_;
    const bool progressive_develop = browse_mode_ == QLatin1String("develop") &&
                                     !maskOverlayActive() && !crop_guides && !state_.before_after_;
    host_.refresh_roi();
    state_.pending_preview_ = PendingDevelopWork{
        .interactive = maskOverlayActive() || crop_guides || progressive_develop,
        .params = state_.develop_,
        .pushed_undo = false,
        .history_write = RecipeHistoryWrite::kUnchanged,
        .discard_history_after_seq = {},
        .history_coalesce_key = {},
        .coalesce_history_id = {},
        .asset_id = utf8_from_qstring(selected_asset_id_),
        .ignore_edits = state_.before_after_,
        .ignore_crop = crop_guides,
        .ignore_straighten = false,
        .settle_preview = progressive_develop,
        .prefer_cached_settled_preview = progressive_develop,
        .overlay_mask_id = current_overlay_mask_id(state_.develop_),
        .request_revision = request_revision,
        .intent_started_at = std::chrono::steady_clock::now(),
        .expected_source = {},
        .expected_history_head = {},
    };
    kick_develop_work();
}

void StudioDevelopPresenter::request_comparison_before()
{
    if (!state_.comparison_active_ || selected_asset_id_.isEmpty())
    {
        return;
    }
    state_.comparison_before_requested_ = true;
    host_.preview_loading(true);
    emit previewChanged();
    kick_develop_work();
}

void StudioDevelopPresenter::kick_develop_work()
{
    if (state_.develop_job_in_flight_)
    {
        return;
    }
    PendingDevelopWork job;
    bool starting_comparison_before = false;
    if (state_.pending_save_.has_value())
    {
        job = *state_.pending_save_;
        state_.pending_save_.reset();
    }
    else if (state_.pending_preview_.has_value())
    {
        job = *state_.pending_preview_;
        state_.pending_preview_.reset();
    }
    else if (state_.comparison_active_ && state_.comparison_before_requested_)
    {
        state_.comparison_before_requested_ = false;
        starting_comparison_before = true;
        job = PendingDevelopWork{
            .interactive = false,
            .params = state_.develop_,
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
            .expected_source = {},
            .expected_history_head = {},
        };
    }
    else
    {
        host_.kick_thumbnails();
        return;
    }
    if (starting_comparison_before)
    {
        job.request_revision =
            state_.develop_preview_owner_.supersede("comparison_before_requested");
        host_.preview_loading(true);
        emit previewChanged();
    }
    host_.cancel_thumbnails("foreground_preview_requested");
    state_.develop_job_in_flight_ = true;
    state_.develop_interactive_job_in_flight_ =
        job.interactive && !job.save && !job.comparison_before;
    const auto revision = job.request_revision ?
                              *job.request_revision :
                              state_.develop_preview_owner_.supersede("queued_preview_started");
    const auto cancellation = state_.develop_preview_owner_.begin();
    const auto catalog = catalog_path_;
    const auto session_generation = catalog_session_generation_;
    const auto load_generation = recipe_load_generation_;
    executor_.post(
        [this, job, revision, cancellation, catalog, session_generation, load_generation]()
        {
            Result<RecipeSaveResult> saved =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<PreviewResult> preview = make_error(ErrorCode::kIo, "Catalog session is closed");
            bool save_ok = !job.save;
            if (host_.worker_develop_service() != nullptr)
            {
                if (job.save)
                {
                    saved = host_.worker_develop_service()->save_develop_with_history(
                        job.asset_id, job.params,
                        RecipeSaveOptions{
                            .history_write = job.history_write,
                            .discard_history_after_seq = job.discard_history_after_seq,
                            .coalesce_history_id = job.coalesce_history_id,
                            .defer_recovery_publication = true,
                            .expected_revision = {},
                            .expected_base = job.previous,
                            .expected_source = job.expected_source,
                            .expected_history_head = job.expected_history_head,
                        });
                    save_ok = static_cast<bool>(saved);
                }
                if (save_ok && job.refresh_preview)
                {
                    PreviewRequest request;
                    request.asset_id = job.asset_id;
                    // Live and settled frames share the prepared display-size
                    // working image; a slider must not replace it with a smaller frame.
                    request.max_edge = kDefaultPreviewMaxEdge;
                    request.request_revision = revision;
                    request.ignore_edits = job.ignore_edits;
                    request.ignore_crop = job.ignore_crop;
                    request.crop_workspace = job.ignore_crop;
                    request.ignore_straighten = job.ignore_straighten;
                    request.persist_preview_record =
                        job.comparison_before ? false : !job.interactive;
                    request.cancellation = cancellation;
                    request.prefer_cached_settled_preview = job.prefer_cached_settled_preview;
                    // Pure interactive frames publish the Engine-owned native
                    // display surface. Snapshot its display RGB8 on this executor
                    // before another render, then queue owned bytes for UI/scopes.
                    request.need_cpu_pixels = !job.interactive || job.comparison_before ||
                                              job.overlay_mask_id.has_value();
                    if (job.overlay_mask_id)
                    {
                        request.overlay_mask_id = job.overlay_mask_id;
                        request.persist_preview_record = false;
                    }
                    preview = host_.worker_preview_service()->request_preview(
                        request, job.interactive && !job.comparison_before ?
                                     std::optional<DevelopParams>{job.params} :
                                     std::optional<DevelopParams>{});
                    if (preview)
                    {
                        auto owned = own_preview_pixels_for_handoff(preview.value(), cancellation);
                        if (!owned)
                            preview = owned.error();
                    }
                }
            }
            const bool recovery_due = job.save && save_ok;
            QMetaObject::invokeMethod(
                this,
                [this, job, revision, catalog, session_generation, load_generation,
                 saved = std::move(saved), preview = std::move(preview)]() mutable
                {
                    state_.develop_job_in_flight_ = false;
                    state_.develop_interactive_job_in_flight_ = false;
                    const bool selected_matches =
                        session_generation == catalog_session_generation_ &&
                        load_generation == recipe_load_generation_ &&
                        utf8_from_qstring(selected_asset_id_) == job.asset_id;
                    if (job.save)
                    {
                        if (!saved)
                        {
                            if (selected_matches)
                                state_.local_done_pending_ = false;
                            if (selected_matches && !state_.pending_save_.has_value() &&
                                state_.develop_ == job.params)
                            {
                                state_.develop_ = job.previous;
                                state_.saved_develop_ = job.previous;
                                if (job.pushed_undo && !state_.undo_stack_.empty())
                                {
                                    state_.undo_stack_.pop_back();
                                }
                                if (job.history_coalesce_key &&
                                    state_.history_coalesce_key_ == job.history_coalesce_key &&
                                    !job.coalesce_history_id)
                                {
                                    break_history_coalescing();
                                }
                                state_.active_history_id_ = 0;
                                if (job.discard_history_after_seq)
                                {
                                    reload_recipe_history();
                                }
                                else
                                {
                                    sync_active_history();
                                }
                                host_.preview_loading(false);
                                emit editChanged();
                                emit previewChanged();
                            }
                            emit errorOccurred(qstring_from_utf8(saved.error().message));
                            kick_develop_work();
                            return;
                        }
                        if (catalog_path_ == catalog &&
                            session_generation == catalog_session_generation_)
                            host_.publish_saved_asset(saved.value().asset);
                        if (selected_matches)
                        {
                            if (job.coalesce_history_id && saved.value().history_id &&
                                *saved.value().history_id != *job.coalesce_history_id)
                            {
                                state_.undo_stack_.push_back(job.previous);
                                if (state_.undo_stack_.size() > 40U)
                                {
                                    state_.undo_stack_.erase(state_.undo_stack_.begin());
                                }
                                state_.redo_stack_.clear();
                            }
                            state_.saved_develop_ = job.params;
                            state_.loaded_recipe_history_head_ = saved.value().history_head;
                            host_.observed_revision(
                                std::max(observed_catalog_revision_, saved.value().revision));
                            if (job.history_coalesce_key &&
                                state_.history_coalesce_key_ == job.history_coalesce_key)
                            {
                                state_.history_coalesce_id_ = saved.value().history_id;
                            }
                            if (state_.pending_save_)
                            {
                                state_.pending_save_->previous = job.params;
                                if (state_.pending_save_->expected_history_head)
                                    state_.pending_save_->expected_history_head =
                                        saved.value().history_head;
                                if (job.history_coalesce_key &&
                                    state_.pending_save_->history_coalesce_key ==
                                        job.history_coalesce_key &&
                                    saved.value().history_id)
                                {
                                    state_.pending_save_->coalesce_history_id =
                                        saved.value().history_id;
                                }
                            }
                            emit selectionChanged();
                            emit editChanged();
                            if (job.history_write == RecipeHistoryWrite::kAppendIfNew)
                            {
                                state_.active_history_id_ = 0;
                                state_.active_history_seq_ = 0;
                                reload_recipe_history();
                            }
                            else
                            {
                                sync_active_history();
                            }
                        }
                    }
                    if (session_generation != catalog_session_generation_ ||
                        !state_.develop_preview_owner_.accepts(
                            revision, job.asset_id, utf8_from_qstring(selected_asset_id_)))
                    {
                        if (job.comparison_before && state_.comparison_active_ &&
                            inspect_.comparisonBeforeUrl().isEmpty())
                        {
                            state_.comparison_before_requested_ = true;
                        }
                        kick_develop_work();
                        return;
                    }
                    if (job.save && !job.refresh_preview)
                    {
                        host_.preview_loading(false);
                        emit previewChanged();
                        kick_develop_work();
                        return;
                    }
                    // A progressive live frame is visible now, but a queued
                    // settled frame still owns the completion state. Keep the
                    // lifecycle busy until that exact request publishes.
                    const bool settle_preview =
                        job.settle_preview && !(job.prefer_cached_settled_preview && preview &&
                                                !preview.value().cache_path.empty());
                    host_.preview_loading(settle_preview);
                    if (!preview)
                    {
                        if (preview.error().code == ErrorCode::kCancelled)
                        {
                            if (job.comparison_before && state_.comparison_active_ &&
                                inspect_.comparisonBeforeUrl().isEmpty())
                            {
                                state_.comparison_before_requested_ = true;
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
                            emit errorOccurred(qstring_from_utf8(preview.error().message));
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
                    if (job.ignore_crop && state_.crop_tool_active_)
                    {
                        state_.crop_guide_ready_ = true;
                    }
                    if (job.comparison_before)
                    {
                        if (state_.comparison_active_)
                        {
                            host_.publish_before(preview.value(), revision);
                            if (inspect_.comparisonBeforeUrl().isEmpty() && clear_comparison())
                            {
                                emit editChanged();
                            }
                        }
                        emit previewChanged();
                        kick_develop_work();
                        return;
                    }
                    if (!host_.publish_preview(preview.value(), revision, job.interactive))
                    {
                        host_.preview_loading(false);
                        emit previewChanged();
                        kick_develop_work();
                        return;
                    }
                    inspect_.observeDisplayedDevelop(job.ignore_edits ?
                                                         std::optional<DevelopParams>{} :
                                                         std::optional<DevelopParams>{job.params});
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
                    if (settle_preview)
                    {
                        state_.pending_preview_ = PendingDevelopWork{
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
                            .expected_source = {},
                            .expected_history_head = {},
                        };
                    }
                    if (state_.comparison_active_ && inspect_.comparisonBeforeUrl().isEmpty())
                    {
                        state_.comparison_before_requested_ = true;
                    }
                    kick_develop_work();
                },
                Qt::QueuedConnection);
            if (recovery_due && host_.worker_develop_service() != nullptr)
            {
                auto synchronized =
                    host_.worker_recovery_service()->sync_recovery(std::string_view{job.asset_id});
                if (!synchronized)
                {
                    const auto failure = qstring_from_utf8(synchronized.error().message);
                    QMetaObject::invokeMethod(
                        this,
                        [this, failure]
                        {
                            emit errorOccurred(
                                QCoreApplication::translate(
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
