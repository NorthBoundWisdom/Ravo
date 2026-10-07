#include "ravo/desktop/studio_develop_presenter.h"
#include <utility>
#include <QCoreApplication>
#include "studio_qt.h"
namespace ravo
{
StudioDevelopPresenter::StudioDevelopPresenter(Context context, Host host, QObject *parent)
    : QObject(parent)
    , host_(std::move(host))
    , selected_asset_id_(context.selected_asset_id_)
    , catalog_path_(context.catalog_path_)
    , browse_mode_(context.browse_mode_)
    , busy_(context.busy_)
    , catalog_operation_active_(context.catalog_operation_active_)
    , import_work_active_(context.import_work_active_)
    , observed_catalog_revision_(context.observed_catalog_revision_)
    , assets_(context.assets_)
    , inspect_(context.inspect_)
    , engine_(context.engine_)
    , executor_(context.executor_)
{
    connect(this, &StudioDevelopPresenter::editChanged, this,
            &StudioDevelopPresenter::sync_local_edit_scope);
}

const StudioDevelopPresenter::State &StudioDevelopPresenter::state() const noexcept
{
    return state_;
}

void StudioDevelopPresenter::shutdown()
{
    if (stopped_)
        return;
    stopped_ = true;
    state_.develop_preview_owner_.cancel("window_closed");
    state_.perspective_analysis_owner_.cancel("window_closed");
}

void StudioDevelopPresenter::selectionInvalidated()
{
    state_.before_after_ = false;
    state_.crop_tool_active_ = false;
    static_cast<void>(state_.develop_preview_owner_.supersede("selection_changed"));
    static_cast<void>(state_.perspective_analysis_owner_.supersede("selection_changed"));
    state_.pending_preview_.reset();
}

void StudioDevelopPresenter::acceptLoadedHistory(
    const Result<std::vector<RecipeHistoryEntry>> &history)
{
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
}

void StudioDevelopPresenter::acceptExternalRecipe(const Recipe &recipe, DevelopParams params)
{
    const bool same_recipe =
        params == state_.develop_ && params == state_.saved_develop_ && !state_.crop_tool_active_;
    state_.loaded_recipe_asset_ = recipe.asset;
    if (!same_recipe)
    {
        state_.undo_stack_.clear();
        state_.redo_stack_.clear();
        state_.before_after_ = false;
        state_.crop_tool_active_ = false;
        state_.crop_guide_ready_ = false;
        state_.develop_ = params;
        state_.saved_develop_ = state_.develop_;
        state_.develop_loaded_ = true;
        state_.develop_load_error_.clear();
        static_cast<void>(state_.develop_preview_owner_.supersede("catalog_revision"));
        state_.pending_preview_.reset();
        host_.request_selection_preview();
    }
    else
    {
        state_.saved_develop_ = params;
        state_.develop_loaded_ = true;
        state_.develop_load_error_.clear();
    }
    sync_active_history();
    emit editChanged();
}

void StudioDevelopPresenter::setCropGuideReady(bool ready)
{
    state_.crop_guide_ready_ = ready;
}

void StudioDevelopPresenter::refreshContextProjection()
{
    emit editChanged();
}

bool StudioDevelopPresenter::liveBatchBusy() const noexcept
{
    return !state_.develop_loaded_ || busy_ || state_.mask_gesture_before_ ||
           state_.local_creation_before_ || state_.develop_job_in_flight_ || state_.pending_save_ ||
           state_.pending_preview_;
}

bool StudioDevelopPresenter::clear_comparison()
{
    const bool changed = state_.comparison_active_ || state_.comparison_before_requested_ ||
                         !inspect_.comparisonBeforeUrl().isEmpty();
    state_.comparison_active_ = false;
    state_.comparison_before_requested_ = false;
    if (state_.pending_preview_.has_value() && state_.pending_preview_->comparison_before)
    {
        state_.pending_preview_.reset();
    }
    host_.clear_comparison_images();
    return changed;
}
void StudioDevelopPresenter::restoreHistory(const int history_id)
{
    if (selected_asset_id_.isEmpty())
    {
        return;
    }
    DevelopParams params;
    std::int64_t seq = 0;
    if (history_id == 0)
    {
        params = baseline_develop();
    }
    else
    {
        const RecipeHistoryEntry *found = nullptr;
        for (const auto &entry : state_.recipe_history_entries_)
        {
            if (entry.id == history_id)
            {
                found = &entry;
                break;
            }
        }
        if (found == nullptr)
        {
            emit errorOccurred(QCoreApplication::translate("DevelopHistoryPanel",
                                                           "Recipe history entry does not exist."));
            return;
        }
        params = develop_from_history_entry(*found);
        seq = found->seq;
    }
    state_.active_history_id_ = history_id;
    state_.active_history_seq_ = seq;
    if (!mutate_develop(std::move(params), StudioDevelopPresenter::DevelopEdit::Restore))
    {
        emit editChanged();
    }
}
} // namespace ravo
