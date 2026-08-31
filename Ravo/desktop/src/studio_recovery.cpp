#include "ravo/desktop/studio_presenter.h"

#include <algorithm>
#include <climits>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QFileInfo>
#include <QMetaObject>

#include "ravo/adapters/filesystem_recovery_store.h"
#include "ravo/adapters/sqlite_catalog.h"

#include "studio_qt.h"

namespace ravo
{
namespace
{

[[nodiscard]] std::filesystem::path path_from_utf8(const std::string_view value)
{
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

[[nodiscard]] QString restore_stage_text(const CatalogRestoreStage stage)
{
    switch (stage)
    {
    case CatalogRestoreStage::kVerifySource:
        return QCoreApplication::translate("StudioPresenter", "Verifying backup…");
    case CatalogRestoreStage::kStageDatabase:
        return QCoreApplication::translate("StudioPresenter", "Staging catalog…");
    case CatalogRestoreStage::kStageSidecars:
        return QCoreApplication::translate("StudioPresenter", "Staging recovery files…");
    case CatalogRestoreStage::kVerifyStaging:
        return QCoreApplication::translate("StudioPresenter", "Verifying staged restore…");
    case CatalogRestoreStage::kPublishSupport:
        return QCoreApplication::translate("StudioPresenter", "Publishing catalog support…");
    case CatalogRestoreStage::kPublishCatalog:
        return QCoreApplication::translate("StudioPresenter", "Publishing restored catalog…");
    case CatalogRestoreStage::kOpenCatalog:
        return QCoreApplication::translate("StudioPresenter", "Opening restored catalog…");
    case CatalogRestoreStage::kComplete:
        return QCoreApplication::translate("StudioPresenter", "Restore complete.");
    }
    return {};
}

[[nodiscard]] std::int64_t current_unix_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

QVariantMap StudioPresenter::backupScheduleStatus() const
{
    if (!backup_policy_)
        return {{QStringLiteral("loaded"), false}};
    return {
        {QStringLiteral("loaded"), true},
        {QStringLiteral("enabled"), backup_policy_->enabled},
        {QStringLiteral("destination"), qstring_from_utf8(backup_policy_->destination_directory)},
        {QStringLiteral("intervalMinutes"),
         QVariant::fromValue<qint64>(static_cast<qint64>(backup_policy_->interval_minutes))},
        {QStringLiteral("retentionCount"), backup_policy_->retention_count},
        {QStringLiteral("lastSuccessUnixMs"),
         QVariant::fromValue<qint64>(
             static_cast<qint64>(backup_policy_->last_success_unix_ms.value_or(0)))},
        {QStringLiteral("nextRunUnixMs"), QVariant::fromValue<qint64>(static_cast<qint64>(
                                              backup_policy_->next_run_unix_ms.value_or(0)))},
        {QStringLiteral("lastBackupBytes"),
         QVariant::fromValue<qulonglong>(backup_policy_->last_backup_bytes)},
        {QStringLiteral("lastError"),
         backup_policy_->last_error ? qstring_from_utf8(*backup_policy_->last_error) : QString{}},
    };
}

void StudioPresenter::refreshRecoveryStatus()
{
    if (busy_ || catalog_operation_active_ || catalog_path_.isEmpty())
        return;
    catalog_operation_ = CancellationSource{};
    const auto cancellation = catalog_operation_.token();
    setBusy(true);
    setError({});
    setCatalogOperation(QCoreApplication::translate("StudioPresenter", "Reading recovery state…"),
                        0, 0, true);
    executor_.post(
        [this, cancellation]
        {
            Result<std::vector<AssetRecoveryState>> pending =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                pending = service_->pending_recovery();
            QMetaObject::invokeMethod(
                this,
                [this, pending = std::move(pending)]() mutable
                {
                    setBusy(false);
                    setCatalogOperation({}, 0, 0, false);
                    if (!pending)
                    {
                        setError(qstring_from_utf8(pending.error().message));
                        setStatus(QCoreApplication::translate("StudioPresenter",
                                                              "Recovery status failed."));
                        return;
                    }
                    recovery_pending_count_ = static_cast<int>(pending.value().size());
                    emit libraryWorkChanged();
                    setStatus(recovery_pending_count_ == 0 ?
                                  QCoreApplication::translate("StudioPresenter",
                                                              "Recovery is synchronized.") :
                                  QCoreApplication::translate("StudioPresenter",
                                                              "%1 recovery items are pending.")
                                      .arg(recovery_pending_count_));
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::synchronizeRecovery()
{
    if (busy_ || catalog_operation_active_ || catalog_path_.isEmpty())
        return;
    catalog_operation_ = CancellationSource{};
    const auto cancellation = catalog_operation_.token();
    setBusy(true);
    setError({});
    setCatalogOperation(QCoreApplication::translate("StudioPresenter", "Synchronizing recovery…"),
                        0, 0, true);
    executor_.post(
        [this, cancellation]
        {
            Result<RecoverySyncResult> synchronized =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                synchronized = service_->sync_recovery(std::nullopt, cancellation);
            QMetaObject::invokeMethod(
                this,
                [this, synchronized = std::move(synchronized)]() mutable
                {
                    setBusy(false);
                    setCatalogOperation({}, 0, 0, false);
                    if (!synchronized)
                    {
                        setError(qstring_from_utf8(synchronized.error().message));
                        setStatus(QCoreApplication::translate("StudioPresenter",
                                                              "Recovery synchronization failed."));
                        return;
                    }
                    recovery_pending_count_ = static_cast<int>(synchronized.value().pending_after);
                    emit libraryWorkChanged();
                    setStatus(QCoreApplication::translate("StudioPresenter",
                                                          "Synchronized %1 recovery items.")
                                  .arg(synchronized.value().artifacts.size()));
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::createBackupAtPath(const QString &path)
{
    if (busy_ || catalog_operation_active_ || catalog_path_.isEmpty() || path.trimmed().isEmpty())
        return;
    catalog_operation_ = CancellationSource{};
    const auto cancellation = catalog_operation_.token();
    const auto destination = utf8_from_qstring(path);
    setBusy(true);
    setError({});
    setCatalogOperation(QCoreApplication::translate("StudioPresenter", "Creating backup…"), 0, 0,
                        true);
    executor_.post(
        [this, cancellation, destination]
        {
            Result<CatalogBackupArtifact> backup =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                backup = service_->create_backup(destination, cancellation);
            QMetaObject::invokeMethod(
                this,
                [this, backup = std::move(backup)]() mutable
                {
                    setBusy(false);
                    setCatalogOperation({}, 0, 0, false);
                    if (!backup)
                    {
                        setError(qstring_from_utf8(backup.error().message));
                        setStatus(QCoreApplication::translate("StudioPresenter", "Backup failed."));
                        return;
                    }
                    recovery_pending_count_ = 0;
                    emit libraryWorkChanged();
                    setStatus(QCoreApplication::translate("StudioPresenter",
                                                          "Created verified backup at %1")
                                  .arg(qstring_from_utf8(backup.value().path)));
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::verifyBackupAtPath(const QString &path)
{
    if (busy_ || catalog_operation_active_ || path.trimmed().isEmpty())
        return;
    catalog_operation_ = CancellationSource{};
    const auto cancellation = catalog_operation_.token();
    const auto backup_path = utf8_from_qstring(path);
    setBusy(true);
    setError({});
    setCatalogOperation(QCoreApplication::translate("StudioPresenter", "Verifying backup…"), 0, 0,
                        true);
    executor_.post(
        [this, cancellation, backup_path]
        {
            Result<CatalogBackupVerification> verified =
                make_error(ErrorCode::kIo, "Unable to open backup verifier");
            const auto sidecars = path_from_utf8(backup_path) / "sidecars";
            auto recovery = FilesystemRecoveryStore::open_existing(sidecars.string());
            if (recovery)
            {
                const SqliteCatalogBackupVerifier database;
                verified =
                    verify_catalog_backup(database, *recovery.value(), backup_path, cancellation);
            }
            else
            {
                verified = recovery.error();
            }
            QMetaObject::invokeMethod(
                this,
                [this, verified = std::move(verified)]() mutable
                {
                    setBusy(false);
                    setCatalogOperation({}, 0, 0, false);
                    if (!verified)
                    {
                        setError(qstring_from_utf8(verified.error().message));
                        setStatus(QCoreApplication::translate("StudioPresenter",
                                                              "Backup verification failed."));
                        return;
                    }
                    setStatus(
                        QCoreApplication::translate("StudioPresenter", "Backup verified: %1 photos")
                            .arg(verified.value().artifact.sidecar_count));
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::restoreBackupToPath(const QString &backup_path, const QString &catalog_path)
{
    if (busy_ || catalog_operation_active_ || backup_path.trimmed().isEmpty() ||
        catalog_path.trimmed().isEmpty())
        return;
    catalog_operation_ = CancellationSource{};
    const auto cancellation = catalog_operation_.token();
    CatalogRestoreRequest request;
    request.backup_directory = utf8_from_qstring(backup_path);
    request.destination_catalog = utf8_from_qstring(catalog_path);
    request.cancellation = cancellation;
    setBusy(true);
    setError({});
    setCatalogOperation(restore_stage_text(CatalogRestoreStage::kVerifySource), 0, 0, true);
    executor_.post(
        [this, request = std::move(request)]() mutable
        {
            Result<CatalogRestoreResult> restored =
                make_error(ErrorCode::kIo, "Unable to open backup verifier");
            const auto sidecars = path_from_utf8(request.backup_directory) / "sidecars";
            auto recovery = FilesystemRecoveryStore::open_existing(sidecars.string());
            if (recovery)
            {
                const SqliteCatalogBackupVerifier database;
                restored = restore_catalog_backup(
                    database, database, *recovery.value(), request,
                    [this](const CatalogRestoreProgress &progress)
                    {
                        const auto stage = restore_stage_text(progress.stage);
                        const int completed = static_cast<int>(progress.completed);
                        const int total = static_cast<int>(progress.total);
                        QMetaObject::invokeMethod(
                            this, [this, stage, completed, total]
                            { setCatalogOperation(stage, completed, total, true); },
                            Qt::QueuedConnection);
                    });
            }
            else
            {
                restored = recovery.error();
            }
            QMetaObject::invokeMethod(
                this,
                [this, restored = std::move(restored)]() mutable
                {
                    setBusy(false);
                    setCatalogOperation({}, 0, 0, false);
                    if (!restored)
                    {
                        setError(qstring_from_utf8(restored.error().message));
                        setStatus(
                            QCoreApplication::translate("StudioPresenter", "Restore failed."));
                        return;
                    }
                    setStatus(
                        QCoreApplication::translate("StudioPresenter", "Restored catalog to %1")
                            .arg(qstring_from_utf8(restored.value().catalog.database_path)));
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::rebuildSelectedPreviews()
{
    auto asset_ids = selected_asset_ids();
    if (asset_ids.empty())
        return;
    const auto total = asset_ids.size();
    startPreviewRebuild(std::move(asset_ids), total);
}

void StudioPresenter::startPreviewRebuild(std::vector<std::string> asset_ids,
                                          const std::size_t expected_total)
{
    if (busy_ || catalog_operation_active_ || catalog_path_.isEmpty())
        return;
    catalog_operation_ = CancellationSource{};
    const auto cancellation = catalog_operation_.token();
    setBusy(true);
    setError({});
    setCatalogOperation(
        QCoreApplication::translate("StudioPresenter", "Rebuilding previews…"), 0,
        static_cast<int>(std::min<std::size_t>(expected_total, static_cast<std::size_t>(INT_MAX))),
        true);
    executor_.post(
        [this, asset_ids = std::move(asset_ids), cancellation]
        {
            Result<PreviewRebuildResult> rebuilt =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                rebuilt = service_->rebuild_previews(
                    asset_ids, cancellation,
                    [this](const std::size_t completed, const std::size_t total,
                           const PreviewRebuildItemResult *)
                    {
                        QMetaObject::invokeMethod(
                            this,
                            [this, completed = static_cast<int>(completed),
                             total = static_cast<int>(total)]
                            {
                                setCatalogOperation(QCoreApplication::translate(
                                                        "StudioPresenter", "Rebuilding previews…"),
                                                    completed, total, true);
                            },
                            Qt::QueuedConnection);
                    });
            }
            QMetaObject::invokeMethod(
                this,
                [this, rebuilt = std::move(rebuilt)]() mutable
                {
                    setBusy(false);
                    setCatalogOperation({}, 0, 0, false);
                    if (!rebuilt)
                    {
                        setError(qstring_from_utf8(rebuilt.error().message));
                        setStatus(QCoreApplication::translate("StudioPresenter",
                                                              "Preview rebuild stopped."));
                        return;
                    }
                    QString first_error;
                    for (const auto &item : rebuilt.value().items)
                    {
                        if (item.browse_cache_path)
                            assets_.setThumbnail(
                                item.asset_id,
                                QUrl::fromLocalFile(qstring_from_utf8(*item.browse_cache_path)),
                                QStringLiteral("ready"));
                        else if (item.error)
                            assets_.setThumbnail(item.asset_id, {}, QStringLiteral("failed"));
                        if (first_error.isEmpty() && item.error)
                            first_error = qstring_from_utf8(item.error->message);
                    }
                    emit thumbnailsChanged();
                    setError(first_error);
                    setStatus(
                        QCoreApplication::translate("StudioPresenter", "Rebuilt %1 of %2 previews.")
                            .arg(rebuilt.value().succeeded)
                            .arg(rebuilt.value().total));
                    requestPreviewForSelection();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::rebuildAllPreviews()
{
    if (busy_ || catalog_operation_active_ || catalog_path_.isEmpty())
        return;
    if (library_total_ == 0U)
        return;
    startPreviewRebuild({}, library_total_);
}

void StudioPresenter::configureBackupSchedule(const QString &directory, const int interval_minutes,
                                              const int retention_count, const bool enabled)
{
    if (busy_ || catalog_operation_active_ || catalog_path_.isEmpty())
        return;
    catalog_operation_ = CancellationSource{};
    setBusy(true);
    setError({});
    setCatalogOperation(QCoreApplication::translate("StudioPresenter", "Saving backup schedule…"),
                        0, 0, true);
    CatalogBackupPolicy requested;
    requested.enabled = enabled;
    requested.destination_directory = utf8_from_qstring(directory);
    requested.interval_minutes = interval_minutes;
    requested.retention_count = retention_count;
    executor_.post(
        [this, requested = std::move(requested)]() mutable
        {
            Result<CatalogBackupPolicy> saved =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                saved = service_->set_backup_policy(std::move(requested), current_unix_ms());
            QMetaObject::invokeMethod(
                this,
                [this, saved = std::move(saved)]() mutable
                {
                    setBusy(false);
                    setCatalogOperation({}, 0, 0, false);
                    if (!saved)
                    {
                        setError(qstring_from_utf8(saved.error().message));
                        setStatus(QCoreApplication::translate("StudioPresenter",
                                                              "Backup schedule failed."));
                        return;
                    }
                    backup_policy_ = saved.value();
                    emit libraryWorkChanged();
                    setStatus(backup_policy_->enabled ?
                                  QCoreApplication::translate("StudioPresenter",
                                                              "Scheduled backups enabled.") :
                                  QCoreApplication::translate("StudioPresenter",
                                                              "Scheduled backups disabled."));
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::runScheduledBackupNow()
{
    startScheduledBackup(true);
}

void StudioPresenter::disableBackupSchedule()
{
    const auto current = backup_policy_.value_or(CatalogBackupPolicy{});
    configureBackupSchedule(qstring_from_utf8(current.destination_directory),
                            static_cast<int>(current.interval_minutes), current.retention_count,
                            false);
}

void StudioPresenter::relinkFolder(const QString &folder_id, const QString &replacement_directory)
{
    if (busy_ || catalog_operation_active_ || catalog_path_.isEmpty() ||
        folder_id.trimmed().isEmpty() || replacement_directory.trimmed().isEmpty())
        return;
    catalog_operation_ = CancellationSource{};
    const auto cancellation = catalog_operation_.token();
    const auto id = utf8_from_qstring(folder_id);
    const auto replacement = utf8_from_qstring(replacement_directory);
    setBusy(true);
    setError({});
    setCatalogOperation(QCoreApplication::translate("StudioPresenter", "Relinking folder…"), 0, 0,
                        true);
    executor_.post(
        [this, cancellation, id, replacement]
        {
            Result<FolderRelinkResult> relinked =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<std::vector<FolderRecord>> folders = std::vector<FolderRecord>{};
            if (service_ != nullptr)
            {
                relinked = service_->relink_folder(id, replacement, cancellation);
                if (relinked)
                    folders = service_->list_folders();
            }
            QMetaObject::invokeMethod(
                this,
                [this, relinked = std::move(relinked), folders = std::move(folders)]() mutable
                {
                    setBusy(false);
                    setCatalogOperation({}, 0, 0, false);
                    if (!relinked)
                    {
                        setError(qstring_from_utf8(relinked.error().message));
                        setStatus(QCoreApplication::translate("StudioPresenter",
                                                              "Folder relink failed."));
                        return;
                    }
                    if (!folders)
                    {
                        setError(qstring_from_utf8(folders.error().message));
                        setStatus(QCoreApplication::translate(
                            "StudioPresenter", "Folder relinked; folder refresh failed."));
                        return;
                    }
                    if (query_.folder_uri == relinked.value().previous_uri)
                        query_.folder_uri = relinked.value().replacement_uri;
                    recovery_pending_count_ = static_cast<int>(std::min<std::size_t>(
                        relinked.value().recovery_pending, static_cast<std::size_t>(INT_MAX)));
                    applyFolders(std::move(folders).value());
                    emit libraryWorkChanged();
                    setStatus(
                        QCoreApplication::translate("StudioPresenter", "Relinked %1 photos to %2")
                            .arg(relinked.value().asset_count)
                            .arg(qstring_from_utf8(relinked.value().replacement_uri)));
                    reloadVisibleAssets();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::checkScheduledBackup()
{
    startScheduledBackup(false);
}

void StudioPresenter::startScheduledBackup(const bool force)
{
    if (busy_ || import_work_active_ || catalog_operation_active_ || catalog_path_.isEmpty())
        return;
    catalog_operation_ = CancellationSource{};
    const auto cancellation = catalog_operation_.token();
    setCatalogOperation(
        force ? QCoreApplication::translate("StudioPresenter", "Running scheduled backup…") :
                QCoreApplication::translate("StudioPresenter", "Checking backup schedule…"),
        0, 0, true);
    executor_.post(
        [this, cancellation, force]
        {
            Result<CatalogBackupScheduleResult> scheduled =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                scheduled = service_->run_scheduled_backup(current_unix_ms(), cancellation, force);
            QMetaObject::invokeMethod(
                this,
                [this, scheduled = std::move(scheduled), force]() mutable
                {
                    setCatalogOperation({}, 0, 0, false);
                    if (!scheduled)
                    {
                        if (scheduled.error().code != ErrorCode::kCancelled)
                        {
                            setError(qstring_from_utf8(scheduled.error().message));
                            setStatus(QCoreApplication::translate("StudioPresenter",
                                                                  "Scheduled backup failed."));
                        }
                        return;
                    }
                    backup_policy_ = scheduled.value().policy;
                    emit libraryWorkChanged();
                    if (scheduled.value().ran && scheduled.value().backup)
                        setStatus(QCoreApplication::translate("StudioPresenter",
                                                              "Scheduled backup created at %1")
                                      .arg(qstring_from_utf8(scheduled.value().backup->path)));
                    else if (force && !scheduled.value().policy.enabled)
                        setStatus(QCoreApplication::translate("StudioPresenter",
                                                              "Scheduled backups are disabled."));
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::cancelCatalogOperation()
{
    if (!catalog_operation_active_ && !import_work_active_)
        return;
    if (catalog_operation_active_)
        static_cast<void>(catalog_operation_.cancel("user_cancelled"));
    if (import_work_active_)
        static_cast<void>(import_operation_.cancel("user_cancelled"));
    setStatus(QCoreApplication::translate("StudioPresenter", "Cancelling catalog operation…"));
}

} // namespace ravo
