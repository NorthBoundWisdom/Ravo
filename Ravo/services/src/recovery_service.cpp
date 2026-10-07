#include "ravo/services/recovery_service.h"

#include "ravo/domain/catalog_repository.h"
#include "ravo/domain/recovery_store.h"

#include <utility>

namespace ravo
{

RecoveryService::RecoveryService(
    const std::unique_ptr<CatalogRepository> &repository,
    const std::unique_ptr<RecoveryStore> &recovery,
    const std::shared_ptr<std::mutex> &publication_mutex,
    const std::function<Result<void>(std::string_view, std::string_view)>
        &backup_checkpoint) noexcept
    : repository_(repository)
    , recovery_(recovery)
    , recovery_publication_mutex_(publication_mutex)
    , testing_backup_checkpoint_(backup_checkpoint)
{
}

Result<AssetRecoveryState> RecoveryService::recovery_state(const std::string_view asset_id) const
{
    if (repository_ == nullptr || recovery_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    return repository_->recovery_state(asset_id);
}

Result<std::vector<AssetRecoveryState>> RecoveryService::pending_recovery() const
{
    if (repository_ == nullptr || recovery_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    return repository_->list_pending_recovery();
}

Result<RecoverySyncResult>
RecoveryService::sync_recovery(const std::optional<std::string_view> asset_id,
                               const CancellationToken &cancellation)
{
    if (repository_ == nullptr || recovery_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    RecoverySyncResult result;
    result.root = recovery_->root();
    if (asset_id)
    {
        auto state = repository_->recovery_state(*asset_id);
        if (!state)
        {
            return state.error();
        }
        result.pending_before = state.value().pending() ? 1U : 0U;
        auto artifact = synchronize_recovery_asset(*asset_id, cancellation);
        if (!artifact)
        {
            return artifact.error();
        }
        result.artifacts.push_back(std::move(artifact).value());
    }
    else
    {
        auto pending = repository_->list_pending_recovery();
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
            auto artifact = synchronize_recovery_asset(state.asset_id, cancellation);
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
    auto remaining = repository_->list_pending_recovery();
    if (!remaining)
    {
        return remaining.error();
    }
    result.pending_after = remaining.value().size();
    return result;
}

Result<RecoveryArtifact>
RecoveryService::synchronize_recovery_asset(const std::string_view asset_id,
                                            const CancellationToken &cancellation)
{
    const std::lock_guard publication_lock(*recovery_publication_mutex_);
    if (repository_ == nullptr || recovery_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    auto state = repository_->recovery_state(asset_id);
    if (!state)
    {
        return state.error();
    }
    if (!state.value().pending())
    {
        return recovery_->verify(asset_id, state.value().generation, cancellation);
    }
    auto snapshot = repository_->load_recovery_snapshot(asset_id);
    if (!snapshot)
    {
        return snapshot.error();
    }
    auto artifact = recovery_->publish(snapshot.value(), cancellation);
    if (!artifact)
    {
        return artifact.error();
    }
    auto acknowledged =
        repository_->acknowledge_recovery(asset_id, snapshot.value().state.generation);
    if (!acknowledged)
    {
        auto error = acknowledged.error();
        error.context.insert_or_assign("sidecar_published", "true");
        error.context.insert_or_assign("sidecar_path", artifact.value().path);
        return error;
    }
    auto cleaned = recovery_->remove_older(asset_id, snapshot.value().state.generation);
    if (!cleaned)
    {
        auto error = cleaned.error();
        error.context.insert_or_assign("recovery_acknowledged", "true");
        error.context.insert_or_assign("sidecar_published", "true");
        error.context.insert_or_assign("sidecar_path", artifact.value().path);
        return error;
    }
    return artifact;
}

Result<void> RecoveryService::synchronize_committed_change(const std::string_view asset_id,
                                                           const CancellationToken &cancellation)
{
    if (repository_ == nullptr || recovery_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto synchronized = synchronize_recovery_asset(asset_id, cancellation);
    if (!synchronized)
    {
        auto error = synchronized.error();
        error.context.insert_or_assign("asset_id", std::string(asset_id));
        error.context.insert_or_assign("catalog_committed", "true");
        auto state = repository_->recovery_state(asset_id);
        if (state)
        {
            error.context.insert_or_assign("recovery_generation",
                                           std::to_string(state.value().generation));
            error.context.insert_or_assign("recovery_synchronized_generation",
                                           std::to_string(state.value().synchronized_generation));
            error.context.insert_or_assign("recovery_pending",
                                           state.value().pending() ? "true" : "false");
        }
        else
        {
            error.context.insert_or_assign("recovery_pending", "unknown");
            error.context.insert_or_assign("recovery_state_error", state.error().message);
        }
        return error;
    }
    return {};
}

} // namespace ravo
