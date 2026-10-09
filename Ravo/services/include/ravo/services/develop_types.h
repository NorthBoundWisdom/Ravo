#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/recipe/develop.h"

namespace ravo
{

struct RecipeSaveOptions
{
    RecipeHistoryWrite history_write = RecipeHistoryWrite::kAppendIfNew;
    std::optional<std::int64_t> discard_history_after_seq;
    std::optional<std::int64_t> coalesce_history_id;
    // Studio's serial Develop owner may defer filesystem publication until the
    // new preview has been queued for the UI. The same worker then drains the
    // durable generation; close, reopen, explicit sync, and backup remain
    // recovery paths if that publication fails.
    bool defer_recovery_publication = false;
    // When set, recipe/develop saves reject stale catalog revisions so instance
    // mutations cannot land against a superseded catalog head (COR-01).
    std::optional<std::int64_t> expected_revision;
    // Interactive edits compare the observed photo, not unrelated catalog insertions.
    std::optional<DevelopParams> expected_base{};
    std::optional<AssetDescriptor> expected_source{};
    std::optional<std::int64_t> expected_history_head{};
};

struct RecipeSaveResult
{
    AssetRecord asset;
    std::int64_t revision = 0;
    std::optional<std::int64_t> history_id;
    std::int64_t history_head = 0;
};

struct DevelopApplyRequest
{
    DevelopParams source;
    std::vector<std::string> fields;
    std::vector<std::string> asset_ids;
    std::optional<std::int64_t> expected_revision;
    CancellationToken cancellation;
};

enum class DevelopApplyItemStatus : std::uint8_t
{
    kApplied = 0,
    kFailed = 1,
    kSkipped = 2,
};

struct DevelopApplyItemResult
{
    std::string asset_id;
    DevelopApplyItemStatus status = DevelopApplyItemStatus::kFailed;
    std::optional<std::int64_t> history_id;
    std::optional<TaskError> error;
};

struct DevelopApplyResult
{
    std::size_t applied = 0;
    std::size_t failed = 0;
    std::size_t skipped = 0;
    std::int64_t revision = 0;
    std::vector<DevelopApplyItemResult> items;
};

using DevelopApplyProgressCallback =
    std::function<void(std::size_t completed, std::size_t total, const DevelopApplyItemResult *)>;

[[nodiscard]] inline std::string_view
develop_apply_item_status_name(const DevelopApplyItemStatus status) noexcept
{
    switch (status)
    {
    case DevelopApplyItemStatus::kApplied:
        return "applied";
    case DevelopApplyItemStatus::kFailed:
        return "failed";
    case DevelopApplyItemStatus::kSkipped:
        return "skipped";
    }
    return "failed";
}

} // namespace ravo
