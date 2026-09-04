#include "ravo/desktop/studio_presenter.h"

#include "studio_qt.h"

#include <string>
#include <utility>

#include <QCoreApplication>
#include <QMetaObject>
#include <QVariantMap>

#include "ravo/services/catalog_service.h"
#include "ravo/services/offline_edit_proxy.h"

namespace ravo
{
namespace
{

[[nodiscard]] QVariantMap offline_status_map(const OfflineEditProxyStatus &status)
{
    QVariantMap map{
        {QStringLiteral("schema"), qstring_from_utf8(status.schema)},
        {QStringLiteral("assetId"), qstring_from_utf8(status.asset_id)},
        {QStringLiteral("mediaState"),
         qstring_from_utf8(offline_edit_media_state_name(status.media_state))},
        {QStringLiteral("proxyPresent"), status.proxy_present},
        {QStringLiteral("proxyVerified"), status.proxy_verified},
        {QStringLiteral("usableForDevelop"), status.usable_for_develop},
        {QStringLiteral("usableForExport"), status.usable_for_export},
        {QStringLiteral("reason"), qstring_from_utf8(status.reason)},
    };
    if (status.manifest)
    {
        map.insert(QStringLiteral("proxyPath"), qstring_from_utf8(status.manifest->proxy_path));
        map.insert(QStringLiteral("maxEdge"),
                   QVariant::fromValue(static_cast<quint32>(status.manifest->max_edge)));
    }
    return map;
}

} // namespace

QVariantMap StudioPresenter::offlineEditMediaStatus() const
{
    return offline_edit_media_status_;
}

void StudioPresenter::refreshOfflineEditMediaStatus()
{
    if (catalog_path_.isEmpty() || selected_asset_id_.isEmpty())
    {
        if (!offline_edit_media_status_.isEmpty())
        {
            offline_edit_media_status_.clear();
            emit offlineEditMediaStatusChanged();
        }
        return;
    }
    const auto asset_id = utf8_from_qstring(selected_asset_id_);
    executor_.post(
        [this, asset_id]()
        {
            Result<OfflineEditProxyStatus> status =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                status = service_->offline_edit_media_status(asset_id);
            QMetaObject::invokeMethod(
                this,
                [this, status = std::move(status)]() mutable
                {
                    if (!status)
                    {
                        setError(qstring_from_utf8(status.error().message));
                        return;
                    }
                    offline_edit_media_status_ = offline_status_map(status.value());
                    emit offlineEditMediaStatusChanged();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::reconnectOfflineEditProxy()
{
    if (catalog_path_.isEmpty() || selected_asset_id_.isEmpty())
        return;
    if (busy_ || catalog_operation_active_ || import_work_active_)
        return;
    const auto asset_id = utf8_from_qstring(selected_asset_id_);
    setError({});
    setStatus(QCoreApplication::translate("StudioPresenter", "Reconnecting original…"));
    executor_.post(
        [this, asset_id]()
        {
            Result<OfflineEditProxyReconnectResult> reconnected =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                OfflineEditProxyReconnectRequest request;
                request.asset_id = asset_id;
                request.user_initiated = true;
                reconnected = service_->reconnect_offline_edit_proxy(request);
            }
            QMetaObject::invokeMethod(
                this,
                [this, reconnected = std::move(reconnected)]() mutable
                {
                    if (!reconnected)
                    {
                        setError(qstring_from_utf8(reconnected.error().message));
                        return;
                    }
                    observed_catalog_revision_ = -1;
                    offline_edit_media_status_ = offline_status_map(reconnected.value().status);
                    offline_edit_media_status_.insert(QStringLiteral("sourceHashMatched"),
                                                      reconnected.value().source_hash_matched);
                    offline_edit_media_status_.insert(QStringLiteral("offlineStatesCleared"),
                                                      reconnected.value().offline_states_cleared);
                    emit offlineEditMediaStatusChanged();
                    setStatus(QCoreApplication::translate(
                        "StudioPresenter", "Original reconnected; offline state cleared."));
                    reloadVisibleAssets();
                },
                Qt::QueuedConnection);
        });
}

} // namespace ravo
