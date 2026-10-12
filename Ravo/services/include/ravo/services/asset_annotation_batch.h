#pragma once

#include "ravo/services/asset_annotation.h"

namespace ravo
{
inline constexpr std::size_t kAnnotationBatchMaximumItems = 32;
inline constexpr std::size_t kAnnotationBatchMaximumBytes = 65536;
struct AssetAnnotationBatchItem
{
    std::string asset_id;
    AssetAnnotationEdit edit;
};
enum class AssetAnnotationBatchStatus
{
    kNotStarted,
    kCommitted,
    kFailed,
    kCancelled,
    kUnknown
};
struct AssetAnnotationBatchItemResult
{
    std::string asset_id;
    AssetAnnotationBatchStatus status = AssetAnnotationBatchStatus::kNotStarted;
    std::optional<AssetAnnotationResult> result;
    std::optional<TaskError> error;
};
struct AssetAnnotationBatchResult
{
    std::int64_t initial_revision = 0;
    std::int64_t revision = 0;
    std::size_t committed_count = 0;
    bool unknown = false;
    std::vector<AssetAnnotationBatchItemResult> items;
};
// Borrowed inputs survive only the synchronous call. Parsed edits and results
// own their values. Execution uses the existing service/repository owner thread.
[[nodiscard]] Result<std::vector<AssetAnnotationBatchItem>>
parse_asset_annotation_batch(const JsonValue &items);
[[nodiscard]] Result<AssetAnnotationBatchResult>
apply_asset_annotation_batch(CatalogService &service, const JsonValue &items,
                             std::int64_t expected_revision,
                             const CancellationToken &cancellation = {});
[[nodiscard]] JsonValue
asset_annotation_batch_result_json(const AssetAnnotationBatchResult &result);
[[nodiscard]] Result<JsonValue> read_asset_annotations(CatalogService &service,
                                                       const std::vector<std::string> &asset_ids,
                                                       std::int64_t expected_revision,
                                                       const CancellationToken &cancellation = {});
} // namespace ravo
