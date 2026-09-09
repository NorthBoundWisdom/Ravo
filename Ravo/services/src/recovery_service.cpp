#include "ravo/services/recovery_service.h"

#include "ravo/services/catalog_service.h"

#include <utility>

namespace ravo
{

RecoveryService::RecoveryService(CatalogService &catalog) noexcept
    : catalog_(&catalog)
{
}

Result<AssetRecoveryState> RecoveryService::recovery_state(const std::string_view asset_id) const
{
    if (catalog_->repository_ == nullptr || catalog_->recovery_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    return catalog_->repository_->recovery_state(asset_id);
}

Result<std::vector<AssetRecoveryState>> RecoveryService::pending_recovery() const
{
    if (catalog_->repository_ == nullptr || catalog_->recovery_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    return catalog_->repository_->list_pending_recovery();
}

Result<RecoverySyncResult>
RecoveryService::sync_recovery(const std::optional<std::string_view> asset_id,
                               const CancellationToken &cancellation)
{
    if (catalog_->repository_ == nullptr || catalog_->recovery_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    RecoverySyncResult result;
    result.root = catalog_->recovery_->root();
    if (asset_id)
    {
        auto state = catalog_->repository_->recovery_state(*asset_id);
        if (!state)
        {
            return state.error();
        }
        result.pending_before = state.value().pending() ? 1U : 0U;
        auto artifact = catalog_->synchronize_recovery_asset(*asset_id, cancellation);
        if (!artifact)
        {
            return artifact.error();
        }
        result.artifacts.push_back(std::move(artifact).value());
    }
    else
    {
        auto pending = catalog_->repository_->list_pending_recovery();
        if (!pending)
        {
            return pending.error();
        }
        result.pending_before = pending.value().size();
        result.artifacts.reserve(pending.value().size());
        for (const auto &state : pending.value())
        {
            auto active = cancellation.check();
            if (!active)
            {
                auto error = active.error();
                error.context.insert_or_assign("completed_count",
                                               std::to_string(result.artifacts.size()));
                return error;
            }
            auto artifact = catalog_->synchronize_recovery_asset(state.asset_id, cancellation);
            if (!artifact)
            {
                auto error = artifact.error();
                error.context.insert_or_assign("asset_id", state.asset_id);
                error.context.insert_or_assign("completed_count",
                                               std::to_string(result.artifacts.size()));
                return error;
            }
            result.artifacts.push_back(std::move(artifact).value());
        }
    }
    auto remaining = catalog_->repository_->list_pending_recovery();
    if (!remaining)
    {
        return remaining.error();
    }
    result.pending_after = remaining.value().size();
    return result;
}

} // namespace ravo
