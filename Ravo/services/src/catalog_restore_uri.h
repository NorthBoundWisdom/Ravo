#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo
{

class RecoveryStore;

// Rewrite absolute paths/URIs that still name a source `{catalog}.ravo/` prefix
// onto destination_support_root (ADR-0136 residual). Fail-closed when a value
// contains `.ravo/` but the suffix is outside known support trees
// (derived/, external-editor/, sidecars/, dng-conversion/, smart-previews/).
[[nodiscard]] Result<std::string>
catalog_restore_rewrite_support_rooted_value(std::string_view value,
                                             std::string_view destination_support_root);

[[nodiscard]] Result<std::size_t>
catalog_restore_rewrite_support_json_tree(const std::filesystem::path &support_root,
                                          std::string_view destination_support_root,
                                          const CancellationToken &cancellation);

[[nodiscard]] Result<std::size_t> catalog_restore_rewrite_recovery_sidecars(
    const std::filesystem::path &sidecar_root, std::string_view destination_support_root,
    std::vector<RecoveryArtifact> &sidecars, const RecoveryStore &recovery_verifier,
    const CancellationToken &cancellation);

[[nodiscard]] Result<std::size_t>
catalog_restore_rewrite_catalog_uris(std::string_view catalog_path,
                                     std::string_view destination_support_root,
                                     const CancellationToken &cancellation);

} // namespace ravo
