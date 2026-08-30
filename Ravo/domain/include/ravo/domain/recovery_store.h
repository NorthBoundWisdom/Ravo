#pragma once

#include <string>
#include <string_view>

#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo
{

// Filesystem recovery mirrors are derived from committed catalog snapshots.
// Implementations own serialization, checksums, atomic publication, validation,
// and cleanup; they never mutate a source image or catalog database.
class RecoveryStore
{
public:
    virtual ~RecoveryStore() = default;

    [[nodiscard]] virtual const std::string &root() const noexcept = 0;
    [[nodiscard]] virtual Result<RecoveryArtifact>
    publish(const AssetRecoverySnapshot &snapshot, const CancellationToken &cancellation) = 0;
    [[nodiscard]] virtual Result<RecoveryArtifact>
    verify(std::string_view asset_id, std::int64_t generation,
           const CancellationToken &cancellation) const = 0;
    [[nodiscard]] virtual Result<RecoveryArtifact>
    verify_artifact(std::string_view path, std::string_view asset_id, std::int64_t generation,
                    const CancellationToken &cancellation) const = 0;
    [[nodiscard]] virtual Result<void> remove_older(std::string_view asset_id,
                                                    std::int64_t keep_generation) = 0;
    [[nodiscard]] virtual Result<void> remove_asset(std::string_view asset_id) = 0;
};

} // namespace ravo
