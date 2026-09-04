#include "ravo/desktop/studio_presenter.h"

#include "ravo/desktop/export_option_conversion.h"

#include <algorithm>
#include <climits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QMetaObject>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>

#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/filesystem_recovery_store.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/domain/types.h"
#include "ravo/services/cull_assistance.h"
#include "ravo/services/catalog_service.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"
#include "studio_file_manager.h"
#include "studio_qt.h"

namespace ravo
{

void StudioPresenter::publish_selection()
{
    assets_.setSelectedIds(selected_ids_);
    emit selectionChanged();
}

void StudioPresenter::activate_primary(const QString &asset_id, const bool reload_preview)
{
    const bool same = selected_asset_id_ == asset_id;
    selected_asset_id_ = asset_id;
    if (!reload_preview && same && !preview_url_.isEmpty())
    {
        publish_selection();
        return;
    }
    clear_displayed_preview();
    clear_inspect_roi();
    if (const auto asset = assets_.assetById(asset_id); asset && asset->width && asset->height)
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        fit_within_max_edge(*asset->width, *asset->height, kDefaultPreviewMaxEdge, width, height);
        preview_viewport_width_ = static_cast<int>(width);
        preview_viewport_height_ = static_cast<int>(height);
    }
    preview_loading_ = !asset_id.isEmpty();
    before_after_ = false;
    crop_tool_active_ = false;
    static_cast<void>(develop_preview_owner_.supersede("selection_changed"));
    static_cast<void>(perspective_analysis_owner_.supersede("selection_changed"));
    pending_preview_.reset();
    load_develop_for_selection();
    publish_selection();
    emit previewChanged();
    emit thumbnailsChanged();
    emit editChanged();
    requestPreviewForSelection();
    refreshOfflineEditMediaStatus();
}

std::vector<std::string> StudioPresenter::selected_asset_ids() const
{
    std::vector<std::string> ids;
    ids.reserve(selected_ids_.size());
    for (const auto &asset : assets_.records())
    {
        if (selected_ids_.contains(asset.id))
            ids.push_back(asset.id);
    }
    return ids;
}

void StudioPresenter::selectAsset(const QString &asset_id)
{
    if (selected_asset_id_ == asset_id && selected_ids_.size() == 1U && !preview_url_.isEmpty())
    {
        return;
    }
    selected_ids_.clear();
    if (!asset_id.isEmpty())
    {
        selected_ids_.insert(utf8_from_qstring(asset_id));
    }
    selection_anchor_id_ = asset_id;
    activate_primary(asset_id, true);
}

void StudioPresenter::selectAssetRange(const QString &asset_id)
{
    if (asset_id.isEmpty())
    {
        return;
    }
    const int clicked = assets_.indexOf(asset_id);
    if (clicked < 0)
    {
        return;
    }
    int anchor = assets_.indexOf(selection_anchor_id_);
    if (anchor < 0)
    {
        anchor = assets_.indexOf(selected_asset_id_);
    }
    if (anchor < 0)
    {
        selectAsset(asset_id);
        return;
    }
    const int begin = std::min(anchor, clicked);
    const int end = std::max(anchor, clicked);
    selected_ids_.clear();
    for (int row = begin; row <= end; ++row)
    {
        selected_ids_.insert(utf8_from_qstring(assets_.assetIdAt(row)));
    }
    activate_primary(asset_id, true);
}

void StudioPresenter::toggleAssetSelected(const QString &asset_id)
{
    if (asset_id.isEmpty())
    {
        return;
    }
    const auto id = utf8_from_qstring(asset_id);
    if (selected_ids_.contains(id))
    {
        selected_ids_.erase(id);
        selection_anchor_id_ = asset_id;
        if (selected_asset_id_ == asset_id)
        {
            const auto remaining = selected_asset_ids();
            activate_primary(remaining.empty() ? QString{} : qstring_from_utf8(remaining.back()),
                             true);
            return;
        }
        publish_selection();
        return;
    }
    selected_ids_.insert(id);
    selection_anchor_id_ = asset_id;
    activate_primary(asset_id, true);
}

void StudioPresenter::selectAllVisible()
{
    std::unordered_set<std::string> ids;
    QString first_id;
    for (int row = 0; row < assets_.rowCount(); ++row)
    {
        const auto id = assets_.assetIdAt(row);
        if (id.isEmpty())
            continue;
        if (first_id.isEmpty())
            first_id = id;
        ids.insert(utf8_from_qstring(id));
    }
    if (ids.empty())
        return;
    const bool keep_primary =
        !selected_asset_id_.isEmpty() && ids.contains(utf8_from_qstring(selected_asset_id_));
    if (ids == selected_ids_ && keep_primary)
        return;
    selected_ids_ = std::move(ids);
    const auto primary = keep_primary ? selected_asset_id_ : first_id;
    if (selection_anchor_id_.isEmpty() ||
        !selected_ids_.contains(utf8_from_qstring(selection_anchor_id_)))
        selection_anchor_id_ = first_id;
    activate_primary(primary, false);
}

void StudioPresenter::selectNext()
{
    const auto row = assets_.indexOf(selected_asset_id_);
    if (row < 0 || row + 1 >= assets_.rowCount())
    {
        return;
    }
    selectAsset(assets_.assetIdAt(row + 1));
}

void StudioPresenter::selectPrevious()
{
    const auto row = assets_.indexOf(selected_asset_id_);
    if (row <= 0)
    {
        return;
    }
    selectAsset(assets_.assetIdAt(row - 1));
}

void StudioPresenter::setBrowseMode(const QString &mode)
{
    QString normalized = QStringLiteral("grid");
    if (mode == QStringLiteral("loupe"))
    {
        normalized = QStringLiteral("loupe");
    }
    else if (mode == QStringLiteral("develop"))
    {
        normalized = QStringLiteral("develop");
    }
    else if (mode == QStringLiteral("survey"))
    {
        normalized = QStringLiteral("survey");
    }
    if (browse_mode_ == normalized)
    {
        return;
    }
    const bool comparison_changed = normalized != QLatin1String("develop") && clear_comparison();
    const QString previous = browse_mode_;
    if (previous == QLatin1String("develop") && normalized != QLatin1String("develop"))
    {
        break_history_coalescing();
    }
    browse_mode_ = normalized;
    emit browseModeChanged();
    if (previous == QLatin1String("develop") && normalized != QLatin1String("develop") &&
        crop_tool_active_)
    {
        setCropToolActive(false);
    }
    if (comparison_changed)
    {
        emit editChanged();
        emit previewChanged();
    }
    if (normalized == QLatin1String("survey"))
    {
        requestSurveyPreviews();
        return;
    }
    if (previous == QLatin1String("survey"))
    {
        pending_survey_ids_.clear();
        survey_preview_requests_.clear();
        survey_preview_in_flight_ = false;
        burst_compare_slot_ids_.clear();
        emit surveyChanged();
    }
    if (normalized != QLatin1String("grid") &&
        (previous == QLatin1String("grid") || previous == QLatin1String("survey") ||
         normalized == QLatin1String("develop")) &&
        !selected_asset_id_.isEmpty())
    {
        requestPreviewForSelection();
    }
}

void StudioPresenter::openLoupe()
{
    if (selected_asset_id_.isEmpty())
    {
        return;
    }
    if (crop_tool_active_)
    {
        setCropToolActive(false);
        return;
    }
    setBrowseMode(QStringLiteral("loupe"));
}

void StudioPresenter::openDevelop()
{
    if (selected_asset_id_.isEmpty())
    {
        return;
    }
    setBrowseMode(QStringLiteral("develop"));
}

void StudioPresenter::openSurvey()
{
    if (selected_ids_.size() < static_cast<std::size_t>(kSurveySlotMinimum))
        return;
    burst_compare_slot_ids_.clear();
    setBrowseMode(QStringLiteral("survey"));
}

void StudioPresenter::apply_burst_compare_pair(const BurstComparePair &pair,
                                               const bool preserve_inspect_roi)
{
    const double roi_x = inspect_roi_x_;
    const double roi_y = inspect_roi_y_;
    const double roi_w = inspect_roi_width_;
    const double roi_h = inspect_roi_height_;
    const bool keep_roi = preserve_inspect_roi && roi_w > 0.0 && roi_h > 0.0;

    selected_ids_.clear();
    selected_ids_.insert(pair.focus_asset_id);
    selected_ids_.insert(pair.compare_asset_id);
    burst_compare_slot_ids_ = {pair.focus_asset_id, pair.compare_asset_id};
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
        requestInspectRoi(roi_x, roi_y, roi_w, roi_h);
    }
}

void StudioPresenter::openBurstCompare()
{
    if (catalog_path_.isEmpty() || selected_asset_id_.isEmpty() || service_ == nullptr)
    {
        return;
    }
    BurstCompareRequest request;
    request.asset_id = utf8_from_qstring(selected_asset_id_);
    request.step = BurstCompareStep::kCurrent;
    auto pair = service_->resolve_burst_compare_pair(request);
    if (!pair)
    {
        setError(qstring_from_utf8(pair.error().message));
        return;
    }
    apply_burst_compare_pair(pair.value(), false);
    setStatus(QCoreApplication::translate("StudioPresenter", "Burst compare (Survey pair)."));
}

void StudioPresenter::stepBurstComparePrevious()
{
    if (catalog_path_.isEmpty() || selected_asset_id_.isEmpty() || service_ == nullptr)
    {
        return;
    }
    BurstCompareRequest request;
    request.asset_id = utf8_from_qstring(selected_asset_id_);
    request.step = BurstCompareStep::kPrevious;
    auto pair = service_->resolve_burst_compare_pair(request);
    if (!pair)
    {
        setError(qstring_from_utf8(pair.error().message));
        return;
    }
    apply_burst_compare_pair(pair.value(), true);
}

void StudioPresenter::stepBurstCompareNext()
{
    if (catalog_path_.isEmpty() || selected_asset_id_.isEmpty() || service_ == nullptr)
    {
        return;
    }
    BurstCompareRequest request;
    request.asset_id = utf8_from_qstring(selected_asset_id_);
    request.step = BurstCompareStep::kNext;
    auto pair = service_->resolve_burst_compare_pair(request);
    if (!pair)
    {
        setError(qstring_from_utf8(pair.error().message));
        return;
    }
    apply_burst_compare_pair(pair.value(), true);
}

void StudioPresenter::selectSurveySlot(const QString &asset_id)
{
    if (asset_id.isEmpty() || !selected_ids_.contains(utf8_from_qstring(asset_id)))
        return;
    selected_asset_id_ = asset_id;
    publish_selection();
    emit surveyChanged();
}

void StudioPresenter::returnToGrid()
{
    setBrowseMode(QStringLiteral("grid"));
}

void StudioPresenter::createAssetVersion()
{
    if (catalog_path_.isEmpty() || selected_asset_id_.isEmpty())
        return;
    const auto source_id = utf8_from_qstring(selected_asset_id_);
    const auto revision = observed_catalog_revision_;
    executor_.post(
        [this, source_id, revision]()
        {
            Result<AssetVersionMutation> created =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                created = service_->create_asset_version(source_id, revision);
            QMetaObject::invokeMethod(
                this,
                [this, created = std::move(created)]() mutable
                {
                    if (!created)
                    {
                        setError(qstring_from_utf8(created.error().message));
                        return;
                    }
                    observed_catalog_revision_ = created.value().revision;
                    const auto version_id = qstring_from_utf8(created.value().version.id);
                    selected_ids_.clear();
                    selected_ids_.insert(created.value().version.id);
                    selected_asset_id_ = version_id;
                    selection_anchor_id_ = version_id;
                    setStatus(
                        QCoreApplication::translate("StudioPresenter", "Virtual copy created."));
                    reloadVisibleAssets();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::stackSelection()
{
    if (catalog_path_.isEmpty())
        return;
    const auto ids = selected_asset_ids();
    if (ids.size() < 2)
        return;
    const auto pick =
        selected_asset_id_.isEmpty() ? ids.front() : utf8_from_qstring(selected_asset_id_);
    const auto revision = observed_catalog_revision_;
    executor_.post(
        [this, ids, pick, revision]()
        {
            Result<LibraryStackMutation> stacked =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                stacked = service_->stack_assets(ids, pick, revision);
            QMetaObject::invokeMethod(
                this,
                [this, stacked = std::move(stacked)]() mutable
                {
                    if (!stacked)
                    {
                        setError(qstring_from_utf8(stacked.error().message));
                        return;
                    }
                    observed_catalog_revision_ = stacked.value().revision;
                    setStatus(QCoreApplication::translate("StudioPresenter", "Photos stacked."));
                    reloadVisibleAssets();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::unstackSelection()
{
    if (catalog_path_.isEmpty() || selected_asset_id_.isEmpty())
        return;
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset || !asset->stack_id)
        return;
    const auto stack_id = *asset->stack_id;
    const auto revision = observed_catalog_revision_;
    executor_.post(
        [this, stack_id, revision]()
        {
            Result<std::int64_t> unstacked =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                unstacked = service_->unstack_assets(stack_id, revision);
            QMetaObject::invokeMethod(
                this,
                [this, unstacked = std::move(unstacked)]() mutable
                {
                    if (!unstacked)
                    {
                        setError(qstring_from_utf8(unstacked.error().message));
                        return;
                    }
                    observed_catalog_revision_ = unstacked.value();
                    setStatus(QCoreApplication::translate("StudioPresenter", "Stack dissolved."));
                    reloadVisibleAssets();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::setSelectedStackPick()
{
    if (catalog_path_.isEmpty() || selected_asset_id_.isEmpty())
        return;
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset || !asset->stack_id)
        return;
    const auto stack_id = *asset->stack_id;
    const auto pick = utf8_from_qstring(selected_asset_id_);
    const auto revision = observed_catalog_revision_;
    executor_.post(
        [this, stack_id, pick, revision]()
        {
            Result<LibraryStackMutation> mutated =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                mutated = service_->set_stack_pick(stack_id, pick, revision);
            QMetaObject::invokeMethod(
                this,
                [this, mutated = std::move(mutated)]() mutable
                {
                    if (!mutated)
                    {
                        setError(qstring_from_utf8(mutated.error().message));
                        return;
                    }
                    observed_catalog_revision_ = mutated.value().revision;
                    setStatus(
                        QCoreApplication::translate("StudioPresenter", "Stack pick updated."));
                    reloadVisibleAssets();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::setCollapseStacks(const bool collapse)
{
    if (collapse_stacks_ == collapse)
        return;
    collapse_stacks_ = collapse;
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::setZoomMode(const QString &mode)
{
    QString normalized = QStringLiteral("fit");
    double factor = zoom_factor_;
    if (mode == QStringLiteral("fill"))
    {
        normalized = QStringLiteral("fill");
    }
    else if (mode == QStringLiteral("actual") || mode == QStringLiteral("100"))
    {
        normalized = QStringLiteral("actual");
        factor = 1.0;
    }
    else if (mode == QStringLiteral("custom"))
    {
        normalized = QStringLiteral("custom");
    }
    if (zoom_mode_ == normalized && zoom_factor_ == factor)
    {
        return;
    }
    zoom_mode_ = normalized;
    zoom_factor_ = factor;
    if (zoom_mode_ != QStringLiteral("actual"))
    {
        last_non_actual_zoom_mode_ = zoom_mode_;
        last_non_actual_zoom_factor_ = zoom_factor_;
        clear_inspect_roi();
    }
    emit zoomChanged();
}

void StudioPresenter::setZoomFactor(const double factor)
{
    const double clamped = std::clamp(factor, 0.1, 8.0);
    if (zoom_mode_ == QStringLiteral("custom") && zoom_factor_ == clamped)
    {
        return;
    }
    zoom_mode_ = QStringLiteral("custom");
    zoom_factor_ = clamped;
    last_non_actual_zoom_mode_ = zoom_mode_;
    last_non_actual_zoom_factor_ = zoom_factor_;
    emit zoomChanged();
}

void StudioPresenter::adjustZoom(const int wheel_delta)
{
    const double step = wheel_delta > 0 ? 1.1 : 1.0 / 1.1;
    const double current = zoom_mode_ == QStringLiteral("actual") ? 1.0 : zoom_factor_;
    setZoomFactor(current * step);
}

void StudioPresenter::toggleActualSize()
{
    if (zoom_mode_ == QStringLiteral("actual"))
    {
        if (last_non_actual_zoom_mode_ == QStringLiteral("custom"))
        {
            setZoomFactor(last_non_actual_zoom_factor_);
            return;
        }
        setZoomMode(last_non_actual_zoom_mode_);
        return;
    }
    setZoomMode(QStringLiteral("actual"));
}

void StudioPresenter::setThumbnailSize(const int size)
{
    const int clamped = std::clamp(size, 96, 320);
    if (thumbnail_size_ == clamped)
    {
        return;
    }
    thumbnail_size_ = clamped;
    emit thumbnailSizeChanged();
}

void StudioPresenter::mutate_selected_review(
    const std::function<Result<AssetRecord>(CatalogService &, std::string_view)> &action)
{
    if (selected_ids_.empty() || catalog_path_.isEmpty())
    {
        return;
    }
    const auto ids = selected_asset_ids();
    executor_.post(
        [this, action, ids]()
        {
            std::vector<AssetRecord> updated;
            TaskError error = make_error(ErrorCode::kIo, "Catalog session is closed");
            bool ok = false;
            if (service_ != nullptr)
            {
                ok = true;
                for (const auto &asset_id : ids)
                {
                    auto result = action(*service_, asset_id);
                    if (!result)
                    {
                        error = result.error();
                        ok = false;
                        break;
                    }
                    updated.push_back(std::move(result).value());
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, ok, error = std::move(error), updated = std::move(updated)]() mutable
                {
                    if (!ok)
                    {
                        setError(qstring_from_utf8(error.message));
                        return;
                    }
                    for (const auto &asset : updated)
                    {
                        assets_.updateAsset(asset);
                    }
                    emit selectionChanged();
                    if (filtersActive())
                    {
                        reloadVisibleAssets();
                    }
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::apply_cull_review_request(const CullReviewFlagAction flag_action,
                                                const std::optional<int> rating,
                                                const std::optional<ColorLabel> color_label,
                                                const bool auto_advance)
{
    if (selected_asset_id_.isEmpty() || catalog_path_.isEmpty())
    {
        return;
    }
    CullReviewRequest request;
    request.asset_id = utf8_from_qstring(selected_asset_id_);
    request.flag_action = flag_action;
    request.rating = rating;
    request.color_label = color_label;
    request.auto_advance = auto_advance && selected_ids_.size() <= 1U;
    request.expected_catalog_revision = observed_catalog_revision_;
    request.selection_asset_ids.reserve(static_cast<std::size_t>(std::max(0, assets_.rowCount())));
    for (int row = 0; row < assets_.rowCount(); ++row)
    {
        if (!assets_.rowLoaded(row))
        {
            continue;
        }
        const auto id = assets_.assetIdAt(row);
        if (!id.isEmpty())
        {
            request.selection_asset_ids.push_back(utf8_from_qstring(id));
        }
    }

    // Multi-select without auto-advance: apply the same mutation to each selected asset.
    if (selected_ids_.size() > 1U)
    {
        const auto ids = selected_asset_ids();
        executor_.post(
            [this, flag_action, rating, color_label, ids]()
            {
                std::vector<AssetRecord> updated;
                TaskError error = make_error(ErrorCode::kIo, "Catalog session is closed");
                bool ok = false;
                std::int64_t revision = 0;
                if (service_ != nullptr)
                {
                    ok = true;
                    for (const auto &asset_id : ids)
                    {
                        CullReviewRequest per;
                        per.asset_id = asset_id;
                        per.flag_action = flag_action;
                        per.rating = rating;
                        per.color_label = color_label;
                        per.auto_advance = false;
                        auto applied = service_->apply_cull_review(per);
                        if (!applied)
                        {
                            error = applied.error();
                            ok = false;
                            break;
                        }
                        revision = applied.value().revision;
                        updated.push_back(std::move(applied).value().asset);
                    }
                }
                QMetaObject::invokeMethod(
                    this,
                    [this, ok, error = std::move(error), updated = std::move(updated),
                     revision]() mutable
                    {
                        if (!ok)
                        {
                            setError(qstring_from_utf8(error.message));
                            return;
                        }
                        observed_catalog_revision_ = revision;
                        for (const auto &asset : updated)
                        {
                            assets_.updateAsset(asset);
                        }
                        emit selectionChanged();
                        if (filtersActive())
                        {
                            reloadVisibleAssets();
                        }
                    },
                    Qt::QueuedConnection);
            });
        return;
    }

    executor_.post(
        [this, request = std::move(request)]() mutable
        {
            Result<CullReviewResult> applied =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                applied = service_->apply_cull_review(request);
            }
            QMetaObject::invokeMethod(
                this,
                [this, applied = std::move(applied)]() mutable
                {
                    if (!applied)
                    {
                        setError(qstring_from_utf8(applied.error().message));
                        return;
                    }
                    auto result = std::move(applied).value();
                    observed_catalog_revision_ = result.revision;
                    assets_.updateAsset(result.asset);
                    emit selectionChanged();
                    if (result.next_asset_id)
                    {
                        selectAsset(qstring_from_utf8(*result.next_asset_id));
                    }
                    else if (filtersActive())
                    {
                        reloadVisibleAssets();
                    }
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::setRating(const int rating)
{
    apply_cull_review_request(CullReviewFlagAction::kUnchanged, rating, std::nullopt, true);
}

void StudioPresenter::setColorLabel(const QString &label)
{
    auto parsed = parse_color_label(utf8_from_qstring(label));
    if (!parsed)
    {
        setError(qstring_from_utf8(parsed.error().message));
        return;
    }
    apply_cull_review_request(CullReviewFlagAction::kUnchanged, std::nullopt, parsed.value(), true);
}

void StudioPresenter::toggleRejected()
{
    const auto flag =
        selectedRejected() ? CullReviewFlagAction::kUnflag : CullReviewFlagAction::kReject;
    apply_cull_review_request(flag, std::nullopt, std::nullopt, true);
}

void StudioPresenter::togglePicked()
{
    const auto flag =
        selectedPicked() ? CullReviewFlagAction::kUnflag : CullReviewFlagAction::kPick;
    apply_cull_review_request(flag, std::nullopt, std::nullopt, true);
}

void StudioPresenter::applyCullReview(const QString &flag_action, const QVariant &rating,
                                      const QString &color_label, const bool auto_advance)
{
    CullReviewFlagAction flag = CullReviewFlagAction::kUnchanged;
    const auto flag_text = flag_action.trimmed().toLower();
    if (flag_text == QLatin1String("pick"))
    {
        flag = CullReviewFlagAction::kPick;
    }
    else if (flag_text == QLatin1String("reject"))
    {
        flag = CullReviewFlagAction::kReject;
    }
    else if (flag_text == QLatin1String("unflag"))
    {
        flag = CullReviewFlagAction::kUnflag;
    }
    else if (!flag_text.isEmpty() && flag_text != QLatin1String("unchanged"))
    {
        setError(QCoreApplication::translate(
            "StudioPresenter", "Cull review flag must be pick, reject, unflag, or unchanged."));
        return;
    }

    std::optional<int> rating_value;
    if (rating.isValid() && !rating.isNull())
    {
        bool ok = false;
        const int value = rating.toInt(&ok);
        if (!ok || value < 0 || value > 5)
        {
            setError(QCoreApplication::translate("StudioPresenter",
                                                 "Rating must be an integer between 0 and 5."));
            return;
        }
        rating_value = value;
    }

    std::optional<ColorLabel> color_value;
    if (!color_label.trimmed().isEmpty())
    {
        auto parsed = parse_color_label(utf8_from_qstring(color_label));
        if (!parsed)
        {
            setError(qstring_from_utf8(parsed.error().message));
            return;
        }
        color_value = parsed.value();
    }

    apply_cull_review_request(flag, rating_value, color_value, auto_advance);
}

void StudioPresenter::setAssetTags(const QString &text)
{
    auto parsed = parse_tag_list(utf8_from_qstring(text));
    if (!parsed)
    {
        setError(qstring_from_utf8(parsed.error().message));
        return;
    }
    if (selected_ids_.empty() || catalog_path_.isEmpty())
    {
        return;
    }
    const auto tags = parsed.value();
    const auto ids = selected_asset_ids();
    executor_.post(
        [this, tags, ids]()
        {
            TaskError error = make_error(ErrorCode::kIo, "Catalog session is closed");
            std::vector<AssetRecord> updated;
            bool ok = false;
            if (service_ != nullptr)
            {
                std::optional<std::int64_t> revision;
                auto snapshot = service_->snapshot();
                if (snapshot)
                    revision = snapshot.value().revision;
                auto mutated = service_->set_tags_selection(ids, tags, revision);
                if (!mutated)
                {
                    error = mutated.error();
                }
                else
                {
                    ok = true;
                    updated = std::move(mutated).value().assets;
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, ok, error = std::move(error), updated = std::move(updated)]() mutable
                {
                    if (!ok)
                    {
                        setError(qstring_from_utf8(error.message));
                        return;
                    }
                    for (const auto &asset : updated)
                    {
                        assets_.updateAsset(asset);
                    }
                    emit selectionChanged();
                    if (filtersActive())
                    {
                        reloadVisibleAssets();
                    }
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::setMetadataField(const QString &name, const QString &value)
{
    const auto field = utf8_from_qstring(name);
    const auto text = utf8_from_qstring(value);
    const std::optional<std::string> field_value =
        text.empty() ? std::optional<std::string>{} : std::optional<std::string>{text};
    auto patch = writable_metadata_patch_for_field(field, field_value);
    if (!patch)
    {
        setError(qstring_from_utf8(patch.error().message));
        return;
    }
    if (selected_ids_.empty() || catalog_path_.isEmpty())
    {
        return;
    }
    const auto ids = selected_asset_ids();
    const auto metadata_patch = patch.value();
    executor_.post(
        [this, ids, metadata_patch]()
        {
            TaskError error = make_error(ErrorCode::kIo, "Catalog session is closed");
            std::vector<AssetRecord> updated;
            bool ok = false;
            if (service_ != nullptr)
            {
                std::optional<std::int64_t> revision;
                auto snapshot = service_->snapshot();
                if (snapshot)
                    revision = snapshot.value().revision;
                auto mutated =
                    service_->set_writable_metadata_selection(ids, metadata_patch, revision);
                if (!mutated)
                {
                    error = mutated.error();
                }
                else
                {
                    ok = true;
                    updated = std::move(mutated).value().assets;
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, ok, error = std::move(error), updated = std::move(updated)]() mutable
                {
                    if (!ok)
                    {
                        setError(qstring_from_utf8(error.message));
                        return;
                    }
                    for (const auto &asset : updated)
                    {
                        assets_.updateAsset(asset);
                    }
                    emit selectionChanged();
                    if (filtersActive())
                    {
                        reloadVisibleAssets();
                    }
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::refreshSelectedMetadata()
{
    if (selected_asset_id_.isEmpty() || catalog_path_.isEmpty())
        return;
    const std::string asset_id = utf8_from_qstring(selected_asset_id_);
    executor_.post(
        [this, asset_id]()
        {
            Result<AssetRecord> refreshed = make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                refreshed = service_->refresh_capture_metadata(asset_id, shutdown_.token());
            QMetaObject::invokeMethod(
                this,
                [this, refreshed = std::move(refreshed)]() mutable
                {
                    if (!refreshed)
                    {
                        if (refreshed.error().code != ErrorCode::kCancelled)
                            setError(qstring_from_utf8(refreshed.error().message));
                        return;
                    }
                    assets_.updateAsset(refreshed.value());
                    emit selectionChanged();
                    setStatus(QCoreApplication::translate("StudioPresenter",
                                                          "Capture metadata refreshed."));
                    reloadVisibleAssets();
                },
                Qt::QueuedConnection);
        });
}

[[nodiscard]] QString next_snapshot_label(const std::vector<RecipeHistoryEntry> &entries)
{
    const QString format = QCoreApplication::translate("DevelopHistoryPanel", "Snapshot %1");
    QString pattern = QRegularExpression::escape(format);
    pattern.replace(QLatin1String("%1"), QStringLiteral("(\\d+)"));
    const QRegularExpression re(QStringLiteral("^") + pattern + QStringLiteral("$"));
    int next = 1;
    for (const auto &entry : entries)
    {
        if (entry.kind != kRecipeHistoryKindSnapshot || !entry.label)
        {
            continue;
        }
        const auto match = re.match(qstring_from_utf8(*entry.label));
        if (match.hasMatch())
        {
            next = std::max(next, match.captured(1).toInt() + 1);
        }
    }
    return format.arg(next);
}

void StudioPresenter::createSnapshot(const QString &label)
{
    QString trimmed = label.trimmed();
    const QString generic = QCoreApplication::translate("DevelopHistoryPanel", "Snapshot");
    if (trimmed.isEmpty() || trimmed.compare(generic, Qt::CaseInsensitive) == 0)
    {
        trimmed = next_snapshot_label(recipe_history_entries_);
    }
    const auto text = utf8_from_qstring(trimmed);
    mutate_selected_review([text](CatalogService &service, const std::string_view asset_id)
                           { return service.create_recipe_snapshot(asset_id, text); });
    load_develop_for_selection();
}

void StudioPresenter::renameSnapshot(const int history_id, const QString &label)
{
    if (selected_asset_id_.isEmpty() || catalog_path_.isEmpty())
    {
        return;
    }
    const auto asset_id = utf8_from_qstring(selected_asset_id_);
    const auto text = utf8_from_qstring(label);
    executor_.post(
        [this, asset_id, history_id, text]()
        {
            Result<AssetRecord> renamed = make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                renamed = service_->rename_recipe_snapshot(asset_id, history_id, text);
            }
            QMetaObject::invokeMethod(
                this,
                [this, asset_id, renamed = std::move(renamed)]() mutable
                {
                    if (utf8_from_qstring(selected_asset_id_) != asset_id)
                    {
                        return;
                    }
                    if (!renamed)
                    {
                        setError(qstring_from_utf8(renamed.error().message));
                        return;
                    }
                    reload_recipe_history();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::restoreHistory(const int history_id)
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
        for (const auto &entry : recipe_history_entries_)
        {
            if (entry.id == history_id)
            {
                found = &entry;
                break;
            }
        }
        if (found == nullptr)
        {
            setError(QCoreApplication::translate("DevelopHistoryPanel",
                                                 "Recipe history entry does not exist."));
            return;
        }
        params = develop_from_history_entry(*found);
        seq = found->seq;
    }
    active_history_id_ = history_id;
    active_history_seq_ = seq;
    if (!mutate_develop(std::move(params), StudioPresenter::DevelopEdit::Restore))
    {
        emit editChanged();
    }
}

void StudioPresenter::setTagFilter(const QString &tag)
{
    auto parsed = tag.trimmed().isEmpty() ? Result<std::string>{std::string{}} :
                                            normalize_tag_name(utf8_from_qstring(tag));
    if (!parsed)
    {
        setError(qstring_from_utf8(parsed.error().message));
        return;
    }
    if (query_.tag == parsed.value())
    {
        return;
    }
    query_.tag = parsed.value();
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::setRatingFilter(const QString &mode, const int value)
{
    RatingFilterMode next_mode = RatingFilterMode::kAny;
    if (mode == QStringLiteral("min"))
    {
        next_mode = RatingFilterMode::kMinimum;
    }
    else if (mode == QStringLiteral("exact"))
    {
        next_mode = RatingFilterMode::kExact;
    }
    if (query_.rating_mode == next_mode && query_.rating_value == value)
    {
        return;
    }
    query_.rating_mode = next_mode;
    query_.rating_value = value;
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::toggleColorFilter(const QString &label)
{
    auto parsed = parse_color_label(utf8_from_qstring(label));
    if (!parsed)
    {
        setError(qstring_from_utf8(parsed.error().message));
        return;
    }
    auto &labels = query_.color_labels;
    const auto found = std::find(labels.begin(), labels.end(), parsed.value());
    if (found == labels.end())
    {
        labels.push_back(parsed.value());
    }
    else
    {
        labels.erase(found);
    }
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::setRejectFilter(const QString &mode)
{
    RejectFilter next = RejectFilter::kInclude;
    if (mode == QStringLiteral("exclude"))
    {
        next = RejectFilter::kExclude;
    }
    else if (mode == QStringLiteral("only"))
    {
        next = RejectFilter::kOnly;
    }
    if (query_.reject_filter == next)
    {
        return;
    }
    query_.reject_filter = next;
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::setPickFilter(const QString &mode)
{
    PickFilter next = PickFilter::kInclude;
    if (mode == QStringLiteral("exclude"))
    {
        next = PickFilter::kExclude;
    }
    else if (mode == QStringLiteral("only"))
    {
        next = PickFilter::kOnly;
    }
    if (query_.pick_filter == next)
    {
        return;
    }
    query_.pick_filter = next;
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::setCullFlagFilter(const QString &mode)
{
    CullFlagFilter next = CullFlagFilter::kAny;
    if (mode == QStringLiteral("picked"))
    {
        next = CullFlagFilter::kPicked;
    }
    else if (mode == QStringLiteral("rejected"))
    {
        next = CullFlagFilter::kRejected;
    }
    else if (mode == QStringLiteral("unreviewed"))
    {
        next = CullFlagFilter::kUnreviewed;
    }
    else if (mode != QStringLiteral("any") && !mode.isEmpty())
    {
        setError(QCoreApplication::translate(
            "StudioPresenter", "Cull flag filter must be any, picked, rejected, or unreviewed."));
        return;
    }
    if (query_.cull_flag_filter == next)
    {
        return;
    }
    query_.cull_flag_filter = next;
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::setCullSuggestionFilter(const QString &mode)
{
    const QString normalized = mode.trimmed().isEmpty() ? QStringLiteral("none") : mode.trimmed();
    if (normalized != QStringLiteral("none") && normalized != QStringLiteral("exact_duplicate") &&
        normalized != QStringLiteral("near_duplicate") && normalized != QStringLiteral("burst"))
    {
        setError(QCoreApplication::translate(
            "StudioPresenter",
            "Cull suggestion filter must be none, exact_duplicate, near_duplicate, or burst."));
        return;
    }
    cull_suggestion_filter_ = normalized;
    cull_suggestion_asset_ids_.clear();
    if (!service_ || normalized == QStringLiteral("none"))
    {
        emit filterChanged();
        reloadVisibleAssets();
        return;
    }
    if (normalized == QStringLiteral("exact_duplicate"))
    {
        auto report = service_->find_exact_duplicate_groups({});
        if (!report)
        {
            setError(qstring_from_utf8(report.error().message));
            cull_suggestion_filter_ = QStringLiteral("none");
            emit filterChanged();
            return;
        }
        for (const auto &group : report.value().groups)
        {
            for (const auto &member : group.members)
            {
                cull_suggestion_asset_ids_.insert(member.asset_id);
            }
        }
    }
    else if (normalized == QStringLiteral("near_duplicate"))
    {
        auto report = service_->find_near_duplicate_groups({});
        if (!report)
        {
            setError(qstring_from_utf8(report.error().message));
            cull_suggestion_filter_ = QStringLiteral("none");
            emit filterChanged();
            return;
        }
        for (const auto &group : report.value().groups)
        {
            for (const auto &member : group.members)
            {
                cull_suggestion_asset_ids_.insert(member.asset_id);
            }
        }
    }
    else if (normalized == QStringLiteral("burst"))
    {
        auto report = service_->propose_burst_groups({});
        if (!report)
        {
            setError(qstring_from_utf8(report.error().message));
            cull_suggestion_filter_ = QStringLiteral("none");
            emit filterChanged();
            return;
        }
        for (const auto &proposal : report.value().proposals)
        {
            for (const auto &member : proposal.members)
            {
                cull_suggestion_asset_ids_.insert(member.asset_id);
            }
        }
    }
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::setFilterText(const QString &text)
{
    LibraryQuery next = query_;
    next.text = utf8_from_qstring(text.trimmed());
    auto valid = validate_library_query(next);
    if (!valid)
    {
        setError(qstring_from_utf8(valid.error().message));
        return;
    }
    if (next == query_)
        return;
    query_ = std::move(next);
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::setMediaFilter(const QString &mode)
{
    LibraryQuery next = query_;
    next.media_types.clear();
    if (mode == QLatin1String("raw"))
        next.media_types.emplace_back(kMediaTypeRaw);
    else if (mode == QLatin1String("jpeg"))
        next.media_types.emplace_back(kMediaTypeJpeg);
    else if (mode == QLatin1String("png"))
        next.media_types.emplace_back(kMediaTypePng);
    else if (mode == QLatin1String("tiff"))
        next.media_types.emplace_back(kMediaTypeTiff);
    else if (mode != QLatin1String("any"))
    {
        setError(QCoreApplication::translate("StudioPresenter", "Unknown media filter mode."));
        return;
    }
    if (next == query_)
        return;
    query_ = std::move(next);
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::setEditFilter(const QString &mode)
{
    EditFilter next = EditFilter::kAny;
    if (mode == QLatin1String("edited"))
        next = EditFilter::kEdited;
    else if (mode == QLatin1String("unedited"))
        next = EditFilter::kUnedited;
    else if (mode != QLatin1String("any"))
    {
        setError(QCoreApplication::translate("StudioPresenter", "Unknown edit filter mode."));
        return;
    }
    if (query_.edit_filter == next)
        return;
    query_.edit_filter = next;
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::setCameraFacetFilter(const QString &make, const QString &model)
{
    LibraryQuery next = query_;
    const auto make_utf8 = utf8_from_qstring(make.trimmed());
    const auto model_utf8 = utf8_from_qstring(model.trimmed());
    if (make_utf8.empty() && model_utf8.empty())
    {
        next.camera_make_equals.reset();
        next.camera_model_equals.reset();
    }
    else
    {
        next.camera_make_equals = make_utf8;
        next.camera_model_equals = model_utf8;
    }
    auto valid = validate_library_query(next);
    if (!valid)
    {
        setError(qstring_from_utf8(valid.error().message));
        return;
    }
    if (next == query_)
        return;
    query_ = std::move(next);
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::setLensFacetFilter(const QString &focal_mm)
{
    LibraryQuery next = query_;
    const auto trimmed = focal_mm.trimmed();
    if (trimmed.isEmpty())
    {
        next.focal_length_mm_equals.reset();
    }
    else
    {
        bool ok = false;
        const double value = trimmed.toDouble(&ok);
        if (!ok)
        {
            setError(QCoreApplication::translate(
                "StudioPresenter", "Lens facet must be a focal length in millimeters."));
            return;
        }
        next.focal_length_mm_equals = value;
    }
    auto valid = validate_library_query(next);
    if (!valid)
    {
        setError(qstring_from_utf8(valid.error().message));
        return;
    }
    if (next == query_)
        return;
    query_ = std::move(next);
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::setLensNameFacetFilter(const QString &make, const QString &model)
{
    LibraryQuery next = query_;
    const auto make_utf8 = utf8_from_qstring(make.trimmed());
    const auto model_utf8 = utf8_from_qstring(model.trimmed());
    if (make_utf8.empty() && model_utf8.empty())
    {
        next.lens_make_equals.reset();
        next.lens_model_equals.reset();
    }
    else
    {
        next.lens_make_equals = make_utf8;
        next.lens_model_equals = model_utf8;
    }
    auto valid = validate_library_query(next);
    if (!valid)
    {
        setError(qstring_from_utf8(valid.error().message));
        return;
    }
    if (next == query_)
        return;
    query_ = std::move(next);
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::setCaptureDateFacetFilter(const QString &local_date)
{
    LibraryQuery next = query_;
    const auto utf8 = utf8_from_qstring(local_date.trimmed());
    if (utf8.empty())
        next.captured_local_date.reset();
    else
        next.captured_local_date = utf8;
    auto valid = validate_library_query(next);
    if (!valid)
    {
        setError(qstring_from_utf8(valid.error().message));
        return;
    }
    if (next == query_)
        return;
    query_ = std::move(next);
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::setLocationFacetFilter(const QString &country, const QString &province_state,
                                             const QString &city, const QString &sublocation)
{
    LibraryQuery next = query_;
    const auto assign = [](std::optional<std::string> &field, const QString &text)
    {
        const auto utf8 = utf8_from_qstring(text.trimmed());
        if (utf8.empty())
            field.reset();
        else
            field = utf8;
    };
    assign(next.country_equals, country);
    assign(next.province_state_equals, province_state);
    assign(next.city_equals, city);
    assign(next.sublocation_equals, sublocation);
    auto valid = validate_library_query(next);
    if (!valid)
    {
        setError(qstring_from_utf8(valid.error().message));
        return;
    }
    if (next == query_)
        return;
    query_ = std::move(next);
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::setSort(const QString &field, const QString &direction)
{
    AssetSortField next_field = AssetSortField::kImportTime;
    if (field == QStringLiteral("name"))
    {
        next_field = AssetSortField::kDisplayName;
    }
    else if (field == QStringLiteral("rating"))
    {
        next_field = AssetSortField::kRating;
    }
    else if (field == QStringLiteral("captured"))
    {
        next_field = AssetSortField::kCaptureTime;
    }
    else if (field == QStringLiteral("size"))
    {
        next_field = AssetSortField::kFileSize;
    }
    const auto next_direction =
        direction == QStringLiteral("asc") ? SortDirection::kAscending : SortDirection::kDescending;
    if (query_.sort_field == next_field && query_.sort_direction == next_direction)
    {
        return;
    }
    query_.sort_field = next_field;
    query_.sort_direction = next_direction;
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::clearFilters()
{
    if (!filtersActive())
    {
        return;
    }
    query_.rating_mode = RatingFilterMode::kAny;
    query_.rating_value = 0;
    query_.color_labels.clear();
    query_.reject_filter = RejectFilter::kInclude;
    query_.tag.clear();
    query_.text.clear();
    query_.media_types.clear();
    query_.edit_filter = EditFilter::kAny;
    query_.camera.clear();
    query_.camera_make_equals.reset();
    query_.camera_model_equals.reset();
    query_.lens_make_equals.reset();
    query_.lens_model_equals.reset();
    query_.focal_length_mm_equals.reset();
    query_.captured_local_date.reset();
    query_.country_equals.reset();
    query_.province_state_equals.reset();
    query_.city_equals.reset();
    query_.sublocation_equals.reset();
    query_.iso = {};
    query_.aperture = {};
    query_.focal_length_mm = {};
    query_.shutter_s = {};
    query_.aspect_ratio = {};
    query_.imported_after_unix_ms.reset();
    query_.imported_before_unix_ms.reset();
    query_.captured_after_unix_s.reset();
    query_.captured_before_unix_s.reset();
    if (last_import_selected_)
    {
        query_.imported_after_unix_ms = last_import_after_unix_ms_;
        query_.imported_before_unix_ms = last_import_before_unix_ms_;
    }
    emit filterChanged();
    reloadVisibleAssets();
}

QString StudioPresenter::folderLocalPath(const QString &folder_uri) const
{
    const auto path = local_file_path_from_asset_uri(folder_uri);
    return path ? path.value() : QString{};
}

void StudioPresenter::revealFolderInFileManager(const QString &folder_uri)
{
    if (folder_uri.trimmed().isEmpty())
    {
        setError(QCoreApplication::translate("StudioPresenter", "Select a folder first."));
        return;
    }
    const auto path = local_file_path_from_asset_uri(folder_uri);
    if (!path)
    {
        setError(QCoreApplication::translate("StudioPresenter",
                                             "The selected folder has no local path."));
        return;
    }
    const auto launch = file_manager_open_directory_launch(path.value());
    if (!launch)
    {
        setError(QCoreApplication::translate(
            "StudioPresenter", "The folder is missing and cannot be shown in the file manager."));
        return;
    }
    if (!start_file_manager_reveal(launch.value()))
    {
        setError(QCoreApplication::translate("StudioPresenter",
                                             "The file manager could not be opened."));
        return;
    }
    setError({});
    setStatus(QCoreApplication::translate("StudioPresenter", "Showing the folder."));
}

void StudioPresenter::removeFolderFromCatalog(const QString &folder_uri)
{
    const auto uri = folder_uri.trimmed();
    if (uri.isEmpty() || busy_ || catalog_operation_active_ || catalog_path_.isEmpty())
        return;
    catalog_operation_ = CancellationSource{};
    const auto cancellation = catalog_operation_.token();
    const auto folder = utf8_from_qstring(uri);
    setBusy(true);
    setError({});
    setCatalogOperation(QCoreApplication::translate("StudioPresenter", "Removing folder…"), 0, 0,
                        true);
    executor_.post(
        [this, cancellation, folder]
        {
            Result<FolderRemoveResult> removed =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<std::vector<AssetRecord>> listed =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<std::vector<FolderRecord>> folders = std::vector<FolderRecord>{};
            Result<std::vector<LibrarySetRecord>> sets = std::vector<LibrarySetRecord>{};
            if (service_ != nullptr)
            {
                removed = service_->remove_folder_from_catalog(folder, cancellation);
                if (removed)
                {
                    listed = service_->list_assets(current_query());
                    folders = service_->list_folders();
                    sets = service_->list_library_sets();
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, folder, removed = std::move(removed), listed = std::move(listed),
                 folders = std::move(folders), sets = std::move(sets)]() mutable
                {
                    setBusy(false);
                    setCatalogOperation({}, 0, 0, false);
                    if (!removed)
                    {
                        setError(qstring_from_utf8(removed.error().message));
                        setStatus(QCoreApplication::translate("StudioPresenter",
                                                              "Folder removal failed."));
                        return;
                    }
                    if (!listed)
                    {
                        setError(qstring_from_utf8(listed.error().message));
                        return;
                    }
                    if (!folders)
                    {
                        setError(qstring_from_utf8(folders.error().message));
                        return;
                    }
                    if (!sets)
                    {
                        setError(qstring_from_utf8(sets.error().message));
                        return;
                    }
                    if (query_.folder_uri == folder)
                        query_.folder_uri.clear();
                    applyFolders(std::move(folders).value());
                    applyLibrarySets(std::move(sets).value());
                    const auto total = listed.value().size();
                    applyAssets(std::move(listed).value(), false, {}, {}, total, false);
                    if (assets_.rowCount() == 0)
                    {
                        selected_asset_id_.clear();
                        selection_anchor_id_.clear();
                        selected_ids_.clear();
                        assets_.setSelectedIds({});
                        clear_displayed_preview();
                        preview_loading_ = false;
                        emit selectionChanged();
                        emit previewChanged();
                    }
                    else if (selected_asset_id_.isEmpty() ||
                             assets_.indexOf(selected_asset_id_) < 0)
                    {
                        selectAsset(assets_.assetIdAt(0));
                    }
                    setStatus(
                        QCoreApplication::translate(
                            "StudioPresenter",
                            "Removed %1 photos from catalog. Original files were not deleted.")
                            .arg(removed.value().asset_count));
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::remove_selected_from_catalog()
{
    if (selected_ids_.empty() || catalog_path_.isEmpty())
    {
        return;
    }
    const auto ids = selected_asset_ids();
    const int keep_index = std::max(0, selectedIndex());
    const auto count = ids.size();
    executor_.post(
        [this, ids, keep_index, count]()
        {
            Result<void> removed = make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<std::vector<AssetRecord>> listed =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<std::vector<FolderRecord>> folders = std::vector<FolderRecord>{};
            Result<std::vector<LibrarySetRecord>> sets = std::vector<LibrarySetRecord>{};
            if (service_ != nullptr)
            {
                removed = Result<void>{};
                for (const auto &asset_id : ids)
                {
                    removed = service_->remove_from_catalog(asset_id);
                    if (!removed)
                    {
                        break;
                    }
                }
                if (removed)
                {
                    listed = service_->list_assets(current_query());
                    folders = service_->list_folders();
                    sets = service_->list_library_sets();
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, removed = std::move(removed), listed = std::move(listed),
                 folders = std::move(folders), sets = std::move(sets), keep_index, count]() mutable
                {
                    if (!removed)
                    {
                        setError(qstring_from_utf8(removed.error().message));
                        return;
                    }
                    if (!listed)
                    {
                        setError(qstring_from_utf8(listed.error().message));
                        return;
                    }
                    if (!folders)
                    {
                        setError(qstring_from_utf8(folders.error().message));
                        return;
                    }
                    if (!sets)
                    {
                        setError(qstring_from_utf8(sets.error().message));
                        return;
                    }
                    applyFolders(std::move(folders).value());
                    applyLibrarySets(std::move(sets).value());
                    const auto total = listed.value().size();
                    applyAssets(std::move(listed).value(), false, {}, {}, total, false);
                    if (assets_.rowCount() == 0)
                    {
                        selected_asset_id_.clear();
                        selection_anchor_id_.clear();
                        selected_ids_.clear();
                        assets_.setSelectedIds({});
                        clear_displayed_preview();
                        preview_loading_ = false;
                        emit selectionChanged();
                        emit previewChanged();
                    }
                    else
                    {
                        const int row = std::min(keep_index, assets_.rowCount() - 1);
                        selectAsset(assets_.assetIdAt(row));
                    }
                    setStatus(
                        count == 1 ?
                            QCoreApplication::translate(
                                "StudioPresenter",
                                "Removed from catalog. Original file was not deleted.") :
                            QCoreApplication::translate(
                                "StudioPresenter",
                                "Removed %1 photos from catalog. Original files were not deleted.")
                                .arg(count));
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::remove_selected_from_disk()
{
    if (!canDeleteFromDisk() || catalog_path_.isEmpty())
    {
        return;
    }
    const auto ids = selected_asset_ids();
    const int keep_index = std::max(0, selectedIndex());
    const auto count = ids.size();
    executor_.post(
        [this, ids, keep_index, count]()
        {
            Result<void> removed = make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<std::vector<AssetRecord>> listed =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<std::vector<FolderRecord>> folders = std::vector<FolderRecord>{};
            Result<std::vector<LibrarySetRecord>> sets = std::vector<LibrarySetRecord>{};
            if (service_ != nullptr)
            {
                removed = Result<void>{};
                for (const auto &asset_id : ids)
                {
                    removed = service_->remove_original_and_catalog(asset_id);
                    if (!removed)
                    {
                        break;
                    }
                }
                if (removed)
                {
                    listed = service_->list_assets(current_query());
                    folders = service_->list_folders();
                    sets = service_->list_library_sets();
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, removed = std::move(removed), listed = std::move(listed),
                 folders = std::move(folders), sets = std::move(sets), keep_index, count]() mutable
                {
                    if (!removed)
                    {
                        setError(qstring_from_utf8(removed.error().message));
                        return;
                    }
                    if (!listed)
                    {
                        setError(qstring_from_utf8(listed.error().message));
                        return;
                    }
                    if (!folders)
                    {
                        setError(qstring_from_utf8(folders.error().message));
                        return;
                    }
                    if (!sets)
                    {
                        setError(qstring_from_utf8(sets.error().message));
                        return;
                    }
                    applyFolders(std::move(folders).value());
                    applyLibrarySets(std::move(sets).value());
                    const auto total = listed.value().size();
                    applyAssets(std::move(listed).value(), false, {}, {}, total, false);
                    if (assets_.rowCount() == 0)
                    {
                        selected_asset_id_.clear();
                        selection_anchor_id_.clear();
                        selected_ids_.clear();
                        assets_.setSelectedIds({});
                        clear_displayed_preview();
                        preview_loading_ = false;
                        emit selectionChanged();
                        emit previewChanged();
                    }
                    else
                    {
                        const int row = std::min(keep_index, assets_.rowCount() - 1);
                        selectAsset(assets_.assetIdAt(row));
                    }
                    setStatus(
                        count == 1 ?
                            QCoreApplication::translate(
                                "StudioPresenter", "Deleted original file and catalog record.") :
                            QCoreApplication::translate(
                                "StudioPresenter", "Deleted %1 original files and catalog records.")
                                .arg(count));
                },
                Qt::QueuedConnection);
        });
}

} // namespace ravo
