#include "ravo/desktop/studio_presenter.h"

#include <algorithm>
#include <utility>

#include <QCoreApplication>
#include <QMetaObject>

#include "studio_qt.h"

namespace ravo
{

bool StudioPresenter::burstCompareActive() const
{
    return browse_mode_ == QLatin1String("survey") &&
           burst_compare_generation_ == library_query_generation_ &&
           burst_compare_slot_ids_.size() == 2U && selected_ids_.size() == 2U &&
           selected_ids_.contains(burst_compare_slot_ids_[0]) &&
           selected_ids_.contains(burst_compare_slot_ids_[1]) &&
           std::find(burst_compare_slot_ids_.begin(), burst_compare_slot_ids_.end(),
                     utf8_from_qstring(selected_asset_id_)) != burst_compare_slot_ids_.end();
}

bool StudioPresenter::canOpenBurstCompare() const
{
    if (catalog_path_.isEmpty() || selected_asset_id_.isEmpty() || busy_ ||
        import_workspace_->importWorkActive() || burst_compare_request_in_flight_)
        return false;
    const auto asset = assets_.assetById(selected_asset_id_);
    if (asset)
        return asset->stack_id && !asset->stack_id->empty() && asset->stack_count >= 2;
    // Collapsed stacks may hide the focus row. Only the service-resolved pair
    // from this listing generation can establish membership for such a row.
    return burstCompareActive();
}

bool StudioPresenter::burstComparePending() const noexcept
{
    return burst_compare_request_in_flight_;
}

void StudioPresenter::apply_burst_compare_pair(const BurstComparePair &pair,
                                               const bool preserve_inspect_roi)
{
    const double roi_x = inspect_.inspectRoiX();
    const double roi_y = inspect_.inspectRoiY();
    const double roi_w = inspect_.inspectRoiWidth();
    const double roi_h = inspect_.inspectRoiHeight();
    const bool keep_roi = preserve_inspect_roi && roi_w > 0.0 && roi_h > 0.0;

    selected_ids_.clear();
    selected_ids_.insert(pair.focus_asset_id);
    selected_ids_.insert(pair.compare_asset_id);
    burst_compare_slot_ids_ = {pair.focus_asset_id, pair.compare_asset_id};
    burst_compare_generation_ = library_query_generation_;
    selection_anchor_id_ = qstring_from_utf8(pair.focus_asset_id);
    activate_primary(qstring_from_utf8(pair.focus_asset_id), true);
    if (browse_mode_ != QLatin1String("survey"))
    {
        setBrowseMode(QStringLiteral("survey"));
    }
    else
    {
        requestSurveyPreviews();
    }
    if (keep_roi)
    {
        inspect_.requestInspectRoi(roi_x, roi_y, roi_w, roi_h);
    }
}

void StudioPresenter::openBurstCompare()
{
    request_burst_compare(BurstCompareStep::kCurrent, false);
}

void StudioPresenter::stepBurstComparePrevious()
{
    request_burst_compare(BurstCompareStep::kPrevious, true);
}

void StudioPresenter::stepBurstCompareNext()
{
    request_burst_compare(BurstCompareStep::kNext, true);
}

void StudioPresenter::request_burst_compare(const BurstCompareStep step,
                                            const bool preserve_inspect_roi)
{
    if (!canOpenBurstCompare() || (step != BurstCompareStep::kCurrent && !burstCompareActive()))
        return;
    const auto catalog = catalog_path_;
    const auto generation = library_query_generation_;
    const auto selected = selected_asset_id_;
    const auto selection = selected_ids_;
    const auto mode = browse_mode_;
    const auto context_revision = burst_compare_context_revision_;
    const auto cancellation = shutdown_.token();
    burst_compare_request_in_flight_ = true;
    emit surveyChanged();
    const bool queued = executor_.post(
        [this, step, preserve_inspect_roi, catalog, generation, selected, selection, mode,
         context_revision, cancellation]
        {
            Result<BurstComparePair> pair = make_error(ErrorCode::kIo, "Catalog session is closed");
            if (auto active = cancellation.check(); !active)
                pair = active.error();
            else if (service_)
            {
                BurstCompareRequest request;
                request.asset_id = utf8_from_qstring(selected);
                request.step = step;
                pair = service_->cull().resolve_burst_compare_pair(request);
            }
            QMetaObject::invokeMethod(
                this,
                [this, pair = std::move(pair), step, preserve_inspect_roi, catalog, generation,
                 selected, selection, mode, context_revision, cancellation]() mutable
                {
                    burst_compare_request_in_flight_ = false;
                    emit surveyChanged();
                    if (cancellation.is_cancellation_requested() || catalog_path_ != catalog ||
                        burst_compare_context_revision_ != context_revision ||
                        library_query_generation_ != generation || selected_asset_id_ != selected ||
                        selected_ids_ != selection || browse_mode_ != mode)
                        return;
                    if (!pair)
                    {
                        setError(qstring_from_utf8(pair.error().message));
                        return;
                    }
                    apply_burst_compare_pair(pair.value(), preserve_inspect_roi);
                    setError({});
                    if (step == BurstCompareStep::kCurrent)
                        setStatus(QCoreApplication::translate("StudioPresenter",
                                                              "Burst compare (Survey pair)."));
                },
                Qt::QueuedConnection);
        });
    if (!queued)
    {
        burst_compare_request_in_flight_ = false;
        emit surveyChanged();
        setError(
            QCoreApplication::translate("StudioPresenter", "Burst compare worker is unavailable."));
    }
}

} // namespace ravo
