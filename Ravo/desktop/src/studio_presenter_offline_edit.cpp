#include "ravo/desktop/studio_presenter.h"

#include "studio_qt.h"

#include <string>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QMetaObject>
#include <QVariantList>
#include <QVariantMap>

#include "ravo/services/catalog_service.h"
#include "ravo/services/offline_edit_proxy.h"

namespace ravo
{
namespace
{

[[nodiscard]] QVariantMap offline_manifest_map(const OfflineEditProxyManifest &manifest)
{
    return QVariantMap{
        {QStringLiteral("assetId"), qstring_from_utf8(manifest.asset_id)},
        {QStringLiteral("proxyPath"), qstring_from_utf8(manifest.proxy_path)},
        {QStringLiteral("maxEdge"), QVariant::fromValue(static_cast<quint32>(manifest.max_edge))},
        {QStringLiteral("width"), QVariant::fromValue(static_cast<quint32>(manifest.width))},
        {QStringLiteral("height"), QVariant::fromValue(static_cast<quint32>(manifest.height))},
        {QStringLiteral("profile"), qstring_from_utf8(manifest.profile)},
        {QStringLiteral("pixelProvenance"), qstring_from_utf8(manifest.pixel_provenance)},
        {QStringLiteral("recipeCacheKey"), qstring_from_utf8(manifest.recipe_cache_key)},
        {QStringLiteral("pinned"), manifest.pinned},
    };
}

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
        map.insert(QStringLiteral("pixelProvenance"),
                   qstring_from_utf8(status.manifest->pixel_provenance));
        map.insert(QStringLiteral("pinned"), status.manifest->pinned);
        map.insert(QStringLiteral("recipeCacheKey"),
                   qstring_from_utf8(status.manifest->recipe_cache_key));
    }
    return map;
}

} // namespace

QVariantMap StudioPresenter::offlineEditMediaStatus() const
{
    return offline_edit_media_status_;
}

QVariantList StudioPresenter::offlineEditProxyList() const
{
    return offline_edit_proxy_list_;
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

void StudioPresenter::refreshOfflineEditProxyList()
{
    if (catalog_path_.isEmpty())
    {
        if (!offline_edit_proxy_list_.isEmpty())
        {
            offline_edit_proxy_list_.clear();
            emit offlineEditProxyListChanged();
        }
        return;
    }
    executor_.post(
        [this]()
        {
            Result<OfflineEditProxyListReport> listed =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                listed = service_->list_offline_edit_proxies();
            QMetaObject::invokeMethod(
                this,
                [this, listed = std::move(listed)]() mutable
                {
                    if (!listed)
                    {
                        setError(qstring_from_utf8(listed.error().message));
                        return;
                    }
                    QVariantList rows;
                    rows.reserve(static_cast<int>(listed.value().manifests.size() +
                                                  listed.value().corrupt.size()));
                    for (const auto &manifest : listed.value().manifests)
                    {
                        auto row = offline_manifest_map(manifest);
                        row.insert(QStringLiteral("corrupt"), false);
                        rows.push_back(row);
                    }
                    for (const auto &entry : listed.value().corrupt)
                    {
                        rows.push_back(QVariantMap{
                            {QStringLiteral("assetId"), qstring_from_utf8(entry.asset_id)},
                            {QStringLiteral("proxyPath"), qstring_from_utf8(entry.path)},
                            {QStringLiteral("reason"), qstring_from_utf8(entry.reason)},
                            {QStringLiteral("corrupt"), true},
                            {QStringLiteral("pinned"), false},
                        });
                    }
                    offline_edit_proxy_list_ = std::move(rows);
                    emit offlineEditProxyListChanged();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::createOfflineEditProxy(const unsigned int max_edge)
{
    if (catalog_path_.isEmpty() || selected_asset_id_.isEmpty())
        return;
    if (busy_ || catalog_operation_active_ || import_work_active_)
        return;
    const auto asset_id = utf8_from_qstring(selected_asset_id_);
    setError({});
    setStatus(QCoreApplication::translate("StudioPresenter", "Creating offline-edit proxy…"));
    executor_.post(
        [this, asset_id, max_edge]()
        {
            Result<OfflineEditProxyCreateResult> created =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                OfflineEditProxyCreateRequest request;
                request.asset_id = asset_id;
                request.user_initiated = true;
                if (max_edge > 0U)
                    request.max_edge = max_edge;
                created = service_->create_offline_edit_proxy(request);
            }
            QMetaObject::invokeMethod(
                this,
                [this, created = std::move(created)]() mutable
                {
                    if (!created)
                    {
                        setError(qstring_from_utf8(created.error().message));
                        return;
                    }
                    setStatus(QCoreApplication::translate("StudioPresenter",
                                                          "Offline-edit proxy created."));
                    refreshOfflineEditMediaStatus();
                    refreshOfflineEditProxyList();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::reconnectOfflineEditProxy(const bool clear_proxy)
{
    if (catalog_path_.isEmpty() || selected_asset_id_.isEmpty())
        return;
    if (busy_ || catalog_operation_active_ || import_work_active_)
        return;
    const auto asset_id = utf8_from_qstring(selected_asset_id_);
    setError({});
    setStatus(QCoreApplication::translate("StudioPresenter", "Reconnecting original…"));
    executor_.post(
        [this, asset_id, clear_proxy]()
        {
            Result<OfflineEditProxyReconnectResult> reconnected =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                OfflineEditProxyReconnectRequest request;
                request.asset_id = asset_id;
                request.user_initiated = true;
                request.clear_proxy = clear_proxy;
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
                    offline_edit_media_status_.insert(QStringLiteral("proxyCleared"),
                                                      reconnected.value().proxy_cleared);
                    emit offlineEditMediaStatusChanged();
                    setStatus(QCoreApplication::translate(
                        "StudioPresenter", "Original reconnected; offline state cleared."));
                    refreshOfflineEditProxyList();
                    reloadVisibleAssets();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::deleteOfflineEditProxy(const bool force)
{
    if (catalog_path_.isEmpty() || selected_asset_id_.isEmpty())
        return;
    if (busy_ || catalog_operation_active_ || import_work_active_)
        return;
    const auto asset_id = utf8_from_qstring(selected_asset_id_);
    setError({});
    setStatus(QCoreApplication::translate("StudioPresenter", "Deleting offline-edit proxy…"));
    executor_.post(
        [this, asset_id, force]()
        {
            Result<OfflineEditProxyDeleteResult> deleted =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                OfflineEditProxyDeleteRequest request;
                request.asset_id = asset_id;
                request.user_initiated = true;
                request.force = force;
                deleted = service_->delete_offline_edit_proxy(request);
            }
            QMetaObject::invokeMethod(
                this,
                [this, deleted = std::move(deleted)]() mutable
                {
                    if (!deleted)
                    {
                        setError(qstring_from_utf8(deleted.error().message));
                        return;
                    }
                    setStatus(QCoreApplication::translate("StudioPresenter",
                                                          "Offline-edit proxy deleted."));
                    refreshOfflineEditMediaStatus();
                    refreshOfflineEditProxyList();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::pinOfflineEditProxy(const bool pinned)
{
    if (catalog_path_.isEmpty() || selected_asset_id_.isEmpty())
        return;
    if (busy_ || catalog_operation_active_ || import_work_active_)
        return;
    const auto asset_id = utf8_from_qstring(selected_asset_id_);
    setError({});
    setStatus(pinned ?
                  QCoreApplication::translate("StudioPresenter", "Pinning offline-edit proxy…") :
                  QCoreApplication::translate("StudioPresenter", "Unpinning offline-edit proxy…"));
    executor_.post(
        [this, asset_id, pinned]()
        {
            Result<OfflineEditProxyPinResult> result =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                OfflineEditProxyPinRequest request;
                request.asset_id = asset_id;
                request.user_initiated = true;
                request.pinned = pinned;
                result = service_->pin_offline_edit_proxy(request);
            }
            QMetaObject::invokeMethod(
                this,
                [this, result = std::move(result)]() mutable
                {
                    if (!result)
                    {
                        setError(qstring_from_utf8(result.error().message));
                        return;
                    }
                    setStatus(QCoreApplication::translate("StudioPresenter",
                                                          "Offline-edit proxy pin updated."));
                    refreshOfflineEditMediaStatus();
                    refreshOfflineEditProxyList();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::evictOfflineEditProxies(const qulonglong max_total_bytes)
{
    if (catalog_path_.isEmpty())
        return;
    if (busy_ || catalog_operation_active_ || import_work_active_)
        return;
    setError({});
    setStatus(QCoreApplication::translate("StudioPresenter", "Evicting unpinned offline proxies…"));
    executor_.post(
        [this, max_total_bytes]()
        {
            Result<OfflineEditProxyEvictResult> evicted =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                OfflineEditProxyEvictRequest request;
                request.user_initiated = true;
                request.max_total_bytes = static_cast<std::uint64_t>(max_total_bytes);
                evicted = service_->evict_offline_edit_proxies(request);
            }
            QMetaObject::invokeMethod(
                this,
                [this, evicted = std::move(evicted)]() mutable
                {
                    if (!evicted)
                    {
                        setError(qstring_from_utf8(evicted.error().message));
                        return;
                    }
                    setStatus(QCoreApplication::translate(
                                  "StudioPresenter", "Evicted %1 unpinned offline-edit prox%2.")
                                  .arg(static_cast<qulonglong>(evicted.value().evicted))
                                  .arg(evicted.value().evicted == 1U ? QStringLiteral("y") :
                                                                       QStringLiteral("ies")));
                    refreshOfflineEditMediaStatus();
                    refreshOfflineEditProxyList();
                },
                Qt::QueuedConnection);
        });
}

} // namespace ravo
