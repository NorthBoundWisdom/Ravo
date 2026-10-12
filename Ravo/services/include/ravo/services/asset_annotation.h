#pragma once

#include "ravo/domain/types.h"

namespace ravo
{
class CatalogService;
enum class AssetAnnotationKind
{
    kReview,
    kMetadata,
    kKeywords
};
struct AssetAnnotationEdit
{
    AssetAnnotationKind kind = AssetAnnotationKind::kReview;
    ReviewPatch review;
    WritableMetadataPatch metadata;
    std::vector<std::string> keywords;
};
struct AssetAnnotationResult
{
    std::string asset_id;
    std::int64_t revision = 0;
    std::optional<AssetRecord> asset;
    std::vector<TaskError> warnings;
};
// Version-one closed payload. All values are owned; no JSON views survive parsing.
[[nodiscard]] Result<AssetAnnotationEdit> parse_asset_annotation_edit(const JsonValue &payload);
[[nodiscard]] JsonValue asset_annotation_fields_schema(AssetAnnotationKind kind);
[[nodiscard]] JsonValue asset_annotation_capabilities_json();
[[nodiscard]] JsonValue asset_annotation_state_json(const AssetRecord &asset);
[[nodiscard]] JsonValue asset_annotation_result_json(const AssetAnnotationResult &result);
// Synchronous service-owner use case; acquires no task/thread and borrows service
// only until its existing repository/recovery capabilities return.
[[nodiscard]] Result<AssetAnnotationResult>
apply_asset_annotation(CatalogService &service, std::string_view asset_id,
                       const AssetAnnotationEdit &edit, std::int64_t expected_revision,
                       const CancellationToken &cancellation);
} // namespace ravo
