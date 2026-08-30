#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "ravo/domain/recovery_store.h"

namespace ravo
{

class FilesystemRecoveryStore final : public RecoveryStore
{
public:
    [[nodiscard]] static std::string default_root_for_catalog(std::string_view database_path);
    [[nodiscard]] static Result<std::unique_ptr<FilesystemRecoveryStore>>
    create(std::string_view root);
    // Opens an existing sidecar directory without creating or changing it.
    [[nodiscard]] static Result<std::unique_ptr<FilesystemRecoveryStore>>
    open_existing(std::string_view root);
    [[nodiscard]] static Result<std::unique_ptr<FilesystemRecoveryStore>>
    create_for_catalog(std::string_view database_path);

    [[nodiscard]] const std::string &root() const noexcept override;
    [[nodiscard]] Result<RecoveryArtifact> publish(const AssetRecoverySnapshot &snapshot,
                                                   const CancellationToken &cancellation) override;
    [[nodiscard]] Result<RecoveryArtifact>
    verify(std::string_view asset_id, std::int64_t generation,
           const CancellationToken &cancellation) const override;
    [[nodiscard]] Result<RecoveryArtifact>
    verify_artifact(std::string_view path, std::string_view asset_id, std::int64_t generation,
                    const CancellationToken &cancellation) const override;
    [[nodiscard]] Result<void> remove_older(std::string_view asset_id,
                                            std::int64_t keep_generation) override;
    [[nodiscard]] Result<void> remove_asset(std::string_view asset_id) override;

private:
    explicit FilesystemRecoveryStore(std::string root);

    std::string root_;
};

} // namespace ravo
