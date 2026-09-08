#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo
{

class CatalogService;

class RecoveryService
{
public:
    explicit RecoveryService(CatalogService &catalog) noexcept;

    RecoveryService(const RecoveryService &) = delete;
    RecoveryService &operator=(const RecoveryService &) = delete;
    RecoveryService(RecoveryService &&) noexcept = default;
    RecoveryService &operator=(RecoveryService &&) noexcept = default;

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
    [[nodiscard]] Result<CatalogBackupPolicy> set_backup_policy(CatalogBackupPolicy policy,
                                                                std::int64_t now_unix_ms);
    [[nodiscard]] Result<CatalogBackupScheduleResult>
    run_scheduled_backup(std::int64_t now_unix_ms, const CancellationToken &cancellation = {},
                         bool force = false);

private:
    CatalogService *catalog_ = nullptr;
};

} // namespace ravo
