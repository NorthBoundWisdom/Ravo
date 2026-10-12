#include "ravo/desktop/studio_live_session_controller.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/services/asset_annotation.h"
#include "ravo/services/asset_annotation_batch.h"
#include "studio_operation_store.h"
#include "studio_qt.h"
#include <QMetaObject>
#include <algorithm>

namespace ravo
{
Result<JsonValue> StudioLiveSessionController::beginAnnotationOperation(
    std::string operation_id, std::string asset_id,
    std::variant<AssetAnnotationEdit, JsonValue> edit, const std::int64_t expected_revision)
{
    auto token = operations_->token(operation_id);
    if (!token)
        return token.error();
    annotation_binding_cancel_ = CancellationSource{};
    const auto cancellation =
        CancellationToken::combine(token.value(), annotation_binding_cancel_.token());
    const auto path = presenter_.catalog_path_;
    const auto history_head = presenter_.develop()->state().loaded_recipe_history_head_;
    annotation_active_ = true;
    annotation_session_revision_.reset();
    presenter_.setBusy(true);
    refresh();
    annotation_session_revision_ = session_revision_;
    const bool queued = presenter_.executor_.post(
        [this, operation_id, asset_id, edit = std::move(edit), cancellation, expected_revision,
         path, history_head]()
        {
            Result<AssetAnnotationResult> result =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            std::optional<AssetAnnotationBatchResult> batch;
            if (auto active = cancellation.check(); !active)
                result = active.error();
            else if (presenter_.service_)
            {
                if (const auto *items = std::get_if<JsonValue>(&edit))
                {
                    auto changed = apply_asset_annotation_batch(*presenter_.service_, *items,
                                                                expected_revision, cancellation);
                    if (!changed)
                        result = changed.error();
                    else
                    {
                        batch = std::move(changed).value();
                        AssetAnnotationResult summary;
                        summary.asset_id = asset_id;
                        summary.revision = batch->revision;
                        for (const auto &item : batch->items)
                            if (item.asset_id == asset_id && item.result)
                                summary.asset = item.result->asset;
                        result = std::move(summary);
                    }
                }
                else
                    result = apply_asset_annotation(*presenter_.service_, asset_id,
                                                    std::get<AssetAnnotationEdit>(edit),
                                                    expected_revision, cancellation);
            }
            QMetaObject::invokeMethod(
                this,
                [this, operation_id, path, history_head, result = std::move(result),
                 batch = std::move(batch)]() mutable
                {
                    if (!presenter_alive_)
                        return;
                    refresh();
                    const bool binding_current =
                        annotation_session_revision_ &&
                        *annotation_session_revision_ == session_revision_ &&
                        presenter_.catalog_path_ == path;
                    annotation_active_ = false;
                    annotation_session_revision_.reset();
                    presenter_.setBusy(false);
                    if (!result)
                    {
                        const auto committed = result.error().context.find("catalog_committed");
                        const auto mutation = result.error().context.find("mutation_applied");
                        if ((committed != result.error().context.end() &&
                             committed->second == "true") ||
                            (mutation != result.error().context.end() &&
                             mutation->second == "unknown"))
                            operations_->unresolved(operation_id, result.error());
                        else
                            operations_->finish(operation_id, false, 0, 0,
                                                std::string(error_code_name(result.error().code)),
                                                result.error().message);
                    }
                    else
                    {
                        const auto &value = result.value();
                        if (batch)
                        {
                            operations_->progress(operation_id,
                                                  asset_annotation_batch_result_json(*batch));
                            if (batch->unknown)
                                operations_->unresolved(
                                    operation_id,
                                    make_error(ErrorCode::kIo,
                                               "Annotation batch has an unknown outcome",
                                               {{"mutation_applied", "unknown"},
                                                {"catalog_committed",
                                                 batch->committed_count ? "true" : "unknown"},
                                                {"confirmed_catalog_revision",
                                                 std::to_string(batch->revision)}}));
                            else if (batch->committed_count != 0)
                                operations_->finish(operation_id, true, value.revision,
                                                    history_head);
                            else
                            {
                                const auto failed = std::find_if(
                                    batch->items.begin(), batch->items.end(),
                                    [](const auto &item) { return item.error.has_value(); });
                                Q_ASSERT(failed != batch->items.end());
                                operations_->finish(
                                    operation_id, false, 0, history_head,
                                    std::string(error_code_name(failed->error->code)),
                                    failed->error->message);
                            }
                        }
                        else
                        {
                            operations_->finish(operation_id, true, value.revision, history_head);
                            operations_->progress(operation_id,
                                                  asset_annotation_result_json(value));
                        }
                        if (binding_current)
                        {
                            if (value.asset)
                            {
                                presenter_.observed_catalog_revision_ =
                                    std::max(presenter_.observed_catalog_revision_, value.revision);
                                presenter_.assets_.updateAsset(*value.asset);
                                emit presenter_.selectionChanged();
                            }
                            presenter_.reloadVisibleAssets();
                        }
                    }
                    refresh();
                },
                Qt::QueuedConnection);
        },
        TaskPriority::kForeground);
    if (!queued)
    {
        annotation_active_ = false;
        annotation_session_revision_.reset();
        presenter_.setBusy(false);
        return make_error(ErrorCode::kIo, "Catalog executor is stopped");
    }
    return operations_->get(operation_id);
}
} // namespace ravo
