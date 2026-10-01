#pragma once

#include <optional>
#include <string>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/engine/photo_merge.h"
#include "ravo/services/image_artifact_verification.h"

namespace ravo
{
struct PhotoMergeRequest
{
    std::vector<std::string> asset_ids;
    PhotoMergeOptions options;
    // Empty means an owned unique catalog support-tree TIFF. Explicit outputs
    // must end in .tif/.tiff; image and provenance both use no-replace publication.
    std::string output_path;
    std::uint32_t max_edge = 0;
    std::optional<std::int64_t> expected_catalog_revision;
    CancellationToken cancellation;
};
struct PhotoMergeResult
{
    std::string schema = "ravo.photo_merge";
    int schema_version = 1;
    PhotoMergeKind kind = PhotoMergeKind::kHdr;
    AssetRecord asset;
    std::string output_path;
    std::string provenance_path;
    VerifiedImageArtifact artifact;
    std::vector<PhotoMergeAlignment> alignments;
    std::uint64_t deghosted_pixels = 0;
    bool originals_unchanged = true;
    double origin_x = 0;
    double origin_y = 0;
    double exposure_normalization_ev = 0;
};
} // namespace ravo
