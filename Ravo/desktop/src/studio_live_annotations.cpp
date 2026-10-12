#include "ravo/desktop/studio_live_session_controller.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/services/asset_annotation.h"
#include "studio_operation_store.h"
#include "studio_qt.h"
#include <QMetaObject>
#include <algorithm>

namespace ravo
{
Result<JsonValue> StudioLiveSessionController::beginAnnotationOperation(
    std::string operation_id, std::string asset_id, AssetAnnotationEdit edit,
    const std::int64_t expected_revision)
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
            if (auto active = cancellation.check(); !active)
                result = active.error();
            else if (presenter_.service_)
                result = apply_asset_annotation(*presenter_.service_, asset_id, edit,
                                                expected_revision, cancellation);
            QMetaObject::invokeMethod(
                this,
                [this, operation_id, path, history_head, result = std::move(result)]() mutable
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
                        operations_->finish(operation_id, true, value.revision, history_head);
                        operations_->progress(operation_id, asset_annotation_result_json(value));
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
