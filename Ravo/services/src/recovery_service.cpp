#include "ravo/services/recovery_service.h"

#include "ravo/services/catalog_service.h"

namespace ravo
{

RecoveryService::RecoveryService(CatalogService &catalog) noexcept
    : catalog_(&catalog)
{
}

Result<AssetRecoveryState> RecoveryService::recovery_state(const std::string_view asset_id) const
{
    return catalog_->recovery_state(asset_id);
}

Result<std::vector<AssetRecoveryState>> RecoveryService::pending_recovery() const
{
    return catalog_->pending_recovery();
}

Result<RecoverySyncResult>
RecoveryService::sync_recovery(const std::optional<std::string_view> asset_id,
                               const CancellationToken &cancellation)
{
    return catalog_->sync_recovery(asset_id, cancellation);
}

Result<CatalogBackupArtifact> RecoveryService::create_backup(const std::string_view destination,
                                                             const CancellationToken &cancellation)
{
    return catalog_->create_backup(destination, cancellation);
}

Result<CatalogBackupVerification>
RecoveryService::verify_backup(const std::string_view backup_directory,
                               const CancellationToken &cancellation) const
{
    return catalog_->verify_backup(backup_directory, cancellation);
}

Result<CatalogBackupPolicy> RecoveryService::backup_policy() const
{
    return catalog_->backup_policy();
}

Result<CatalogBackupPolicy> RecoveryService::set_backup_policy(CatalogBackupPolicy policy,
                                                               const std::int64_t now_unix_ms)
{
    return catalog_->set_backup_policy(std::move(policy), now_unix_ms);
}

Result<CatalogBackupScheduleResult>
RecoveryService::run_scheduled_backup(const std::int64_t now_unix_ms,
                                      const CancellationToken &cancellation, const bool force)
{
    return catalog_->run_scheduled_backup(now_unix_ms, cancellation, force);
}

} // namespace ravo
