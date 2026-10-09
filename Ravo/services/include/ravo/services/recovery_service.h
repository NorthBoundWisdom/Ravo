#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo
{

class CatalogService;
class CatalogRepository;
class RecoveryStore;

class RecoveryService
{
public:
    RecoveryService(const RecoveryService &) = delete;
    RecoveryService &operator=(const RecoveryService &) = delete;
    RecoveryService(RecoveryService &&) = delete;
    RecoveryService &operator=(RecoveryService &&) = delete;

    [[nodiscard]] Result<AssetRecoveryState> recovery_state(std::string_view asset_id) const;
    [[nodiscard]] Result<std::vector<AssetRecoveryState>> pending_recovery() const;
    [[nodiscard]] Result<RecoverySyncResult>
    sync_recovery(std::optional<std::string_view> asset_id,
                  const CancellationToken &cancellation = {});
    [[nodiscard]] Result<CatalogBackupArtifact>
    create_backup(std::string_view destination, const CancellationToken &cancellation = {});
    [[nodiscard]] Result<CatalogBackupVerification>
    verify_backup(std::string_view backup_directory,
                  const CancellationToken &cancellation = {}) const;
    [[nodiscard]] Result<CatalogBackupPolicy> backup_policy() const;
    // Replaces configuration, including disabled policies; service-owned backup
    // history is preserved. A toggle-only client passes the observed configuration.
    [[nodiscard]] Result<CatalogBackupPolicy> set_backup_policy(CatalogBackupPolicy policy,
                                                                std::int64_t now_unix_ms);
    [[nodiscard]] Result<CatalogBackupScheduleResult>
    run_scheduled_backup(std::int64_t now_unix_ms, const CancellationToken &cancellation = {},
                         bool force = false);

    // Called only after the repository commit; failures retain committed-state context.
    [[nodiscard]] Result<void>
    synchronize_committed_change(std::string_view asset_id,
                                 const CancellationToken &cancellation = {});

private:
    [[nodiscard]] Result<RecoveryArtifact>
    synchronize_recovery_asset(std::string_view asset_id, const CancellationToken &cancellation);
    friend class CatalogService;
    // Borrowed owner slots stay valid until this capability is destroyed. The
    // composition owner is immovable; reset slots make post-close calls fail.
    RecoveryService(const std::unique_ptr<CatalogRepository> &repository,
                    const std::unique_ptr<RecoveryStore> &recovery,
                    const std::shared_ptr<std::mutex> &publication_mutex,
                    const std::function<Result<void>(std::string_view, std::string_view)>
                        &backup_checkpoint) noexcept;

    const std::unique_ptr<CatalogRepository> &repository_;
    const std::unique_ptr<RecoveryStore> &recovery_;
    const std::shared_ptr<std::mutex> &recovery_publication_mutex_;
    const std::function<Result<void>(std::string_view, std::string_view)>
        &testing_backup_checkpoint_;
};

} // namespace ravo
