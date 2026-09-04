#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"
#include "ravo/domain/types.h"

namespace ravo
{

// ADR-0147/0149/0150: deterministic exact-duplicate, burst, near-duplicate
// fingerprint proposals, and keyboard cull review mutations (no auto-delete).
inline constexpr std::string_view kCullExactDuplicateContractVersion =
    "ravo.cull.exact-duplicate/v1";
inline constexpr std::int64_t kCullExactDuplicateSchemaVersion = 1;
inline constexpr std::string_view kCullBurstProposalContractVersion = "ravo.cull.burst-proposal/v1";
inline constexpr std::int64_t kCullBurstProposalSchemaVersion = 1;
inline constexpr std::int64_t kCullBurstDefaultWindowSeconds = 1;

enum class ExactDuplicateOutcome : std::uint8_t
{
    kSameFile = 0,
    kSameBytes = 1,
};

[[nodiscard]] inline std::string_view
exact_duplicate_outcome_name(const ExactDuplicateOutcome outcome) noexcept
{
    switch (outcome)
    {
    case ExactDuplicateOutcome::kSameFile:
        return "same_file";
    case ExactDuplicateOutcome::kSameBytes:
        return "same_bytes";
    }
    return "same_bytes";
}

struct ExactDuplicateMember
{
    std::string asset_id;
    std::string normalized_uri;
    int version_ordinal = kAssetVersionOrdinalPrimary;
    std::optional<std::string> source_asset_id;
    std::uint64_t size_bytes = 0;
};

struct ExactDuplicateGroup
{
    std::string schema{std::string(kCullExactDuplicateContractVersion)};
    std::int64_t schema_version = kCullExactDuplicateSchemaVersion;
    std::string sha256;
    ExactDuplicateOutcome outcome = ExactDuplicateOutcome::kSameBytes;
    std::vector<ExactDuplicateMember> members;
};

struct ExactDuplicateSkip
{
    std::string asset_id;
    std::string reason;
    std::optional<std::string> path;
};

struct ExactDuplicateReport
{
    std::string schema{std::string(kCullExactDuplicateContractVersion)};
    std::int64_t schema_version = kCullExactDuplicateSchemaVersion;
    std::vector<ExactDuplicateGroup> groups;
    std::vector<ExactDuplicateSkip> skipped;
    std::size_t assets_considered = 0;
};

struct ExactDuplicateRequest
{
    CancellationToken cancellation{};
};

struct BurstGroupMember
{
    std::string asset_id;
    std::optional<std::int64_t> captured_unix_s;
    std::optional<std::string> camera_make;
    std::optional<std::string> camera_model;
};

struct BurstGroupProposal
{
    std::string schema{std::string(kCullBurstProposalContractVersion)};
    std::int64_t schema_version = kCullBurstProposalSchemaVersion;
    std::string proposal_id;
    std::string camera_key;
    std::int64_t window_start_unix_s = 0;
    std::int64_t window_end_unix_s = 0;
    std::vector<BurstGroupMember> members;
};

struct BurstProposeRequest
{
    std::int64_t window_seconds = kCullBurstDefaultWindowSeconds;
    CancellationToken cancellation{};
};

struct BurstProposeReport
{
    std::string schema{std::string(kCullBurstProposalContractVersion)};
    std::int64_t schema_version = kCullBurstProposalSchemaVersion;
    std::int64_t window_seconds = kCullBurstDefaultWindowSeconds;
    std::vector<BurstGroupProposal> proposals;
    std::size_t assets_considered = 0;
    std::size_t assets_skipped_missing_capture = 0;
};

struct BurstAcceptRequest
{
    std::vector<std::string> asset_ids;
    std::optional<std::string> pick_asset_id;
    bool user_initiated = false;
    std::optional<std::int64_t> expected_catalog_revision;
    CancellationToken cancellation{};
};

inline constexpr std::string_view kCullNearDuplicateContractVersion = "ravo.cull.near-duplicate/v1";
inline constexpr std::int64_t kCullNearDuplicateSchemaVersion = 1;
inline constexpr int kCullNearDupDefaultMaxHamming = 5;
inline constexpr std::size_t kCullNearDupDefaultMaxGroups = 200;
// Hard upper bound for fingerprinted inputs. Above this, fail closed rather than
// run unbounded O(N^2) Hamming work on a professional catalog (PERF/COR-01).
inline constexpr std::size_t kCullNearDupDefaultMaxAssets = 4096;

struct NearDuplicateMember
{
    std::string asset_id;
    std::string normalized_uri;
    std::string fingerprint_hex;
    int version_ordinal = kAssetVersionOrdinalPrimary;
    std::optional<std::string> source_asset_id;
};

struct NearDuplicateGroup
{
    std::string schema{std::string(kCullNearDuplicateContractVersion)};
    std::int64_t schema_version = kCullNearDuplicateSchemaVersion;
    std::string fingerprint_hex;
    int max_hamming_in_group = 0;
    std::vector<NearDuplicateMember> members;
};

struct NearDuplicateSkip
{
    std::string asset_id;
    std::string reason;
    std::optional<std::string> path;
};

struct NearDuplicateReport
{
    std::string schema{std::string(kCullNearDuplicateContractVersion)};
    std::int64_t schema_version = kCullNearDuplicateSchemaVersion;
    int max_hamming = kCullNearDupDefaultMaxHamming;
    std::size_t max_groups = kCullNearDupDefaultMaxGroups;
    std::size_t max_assets = kCullNearDupDefaultMaxAssets;
    std::vector<NearDuplicateGroup> groups;
    std::vector<NearDuplicateSkip> skipped;
    std::size_t assets_considered = 0;
    std::size_t assets_fingerprinted = 0;
    // Heuristic only: never authoritative for delete/reject/stack.
    bool non_authoritative = true;
};

struct NearDuplicateRequest
{
    int max_hamming = kCullNearDupDefaultMaxHamming;
    std::size_t max_groups = kCullNearDupDefaultMaxGroups;
    std::size_t max_assets = kCullNearDupDefaultMaxAssets;
    CancellationToken cancellation{};
};

struct BurstAcceptResult
{
    LibraryStackMutation stack;
    bool catalog_mutated = true;
};

// ADR-0155: ordered Survey/1:1 compare pair inside a durable library stack
// (accepted burst or ordinary stack). No mutation; no proposal-id authority.
inline constexpr std::string_view kCullBurstCompareContractVersion = "ravo.cull.burst-compare/v1";
inline constexpr std::int64_t kCullBurstCompareSchemaVersion = 1;

enum class BurstCompareStep : std::uint8_t
{
    kCurrent = 0,
    kPrevious = 1,
    kNext = 2,
};

[[nodiscard]] inline std::string_view burst_compare_step_name(const BurstCompareStep step) noexcept
{
    switch (step)
    {
    case BurstCompareStep::kCurrent:
        return "current";
    case BurstCompareStep::kPrevious:
        return "previous";
    case BurstCompareStep::kNext:
        return "next";
    }
    return "current";
}

[[nodiscard]] inline std::optional<BurstCompareStep>
burst_compare_step_from_name(const std::string_view name) noexcept
{
    if (name == "current" || name.empty())
        return BurstCompareStep::kCurrent;
    if (name == "previous")
        return BurstCompareStep::kPrevious;
    if (name == "next")
        return BurstCompareStep::kNext;
    return std::nullopt;
}

struct BurstComparePair
{
    std::string schema{std::string(kCullBurstCompareContractVersion)};
    std::int64_t schema_version = kCullBurstCompareSchemaVersion;
    std::string stack_id;
    std::vector<std::string> member_ids;
    std::size_t focus_index = 0;
    std::string focus_asset_id;
    std::string compare_asset_id;
    BurstCompareStep step = BurstCompareStep::kCurrent;
};

struct BurstCompareRequest
{
    std::string asset_id;
    BurstCompareStep step = BurstCompareStep::kCurrent;
};

[[nodiscard]] Result<BurstComparePair>
resolve_burst_compare_pair(const LibraryStackRecord &stack, std::string_view focus_asset_id,
                           BurstCompareStep step = BurstCompareStep::kCurrent);

inline constexpr std::string_view kCullReviewContractVersion = "ravo.cull.review/v1";
inline constexpr std::int64_t kCullReviewSchemaVersion = 1;

enum class CullReviewFlagAction : std::uint8_t
{
    kUnchanged = 0,
    kPick = 1,
    kReject = 2,
    kUnflag = 3,
};

[[nodiscard]] inline std::string_view
cull_review_flag_action_name(const CullReviewFlagAction action) noexcept
{
    switch (action)
    {
    case CullReviewFlagAction::kUnchanged:
        return "unchanged";
    case CullReviewFlagAction::kPick:
        return "pick";
    case CullReviewFlagAction::kReject:
        return "reject";
    case CullReviewFlagAction::kUnflag:
        return "unflag";
    }
    return "unchanged";
}

struct CullReviewRequest
{
    std::string asset_id;
    CullReviewFlagAction flag_action = CullReviewFlagAction::kUnchanged;
    std::optional<int> rating;
    std::optional<ColorLabel> color_label;
    bool auto_advance = false;
    std::vector<std::string> selection_asset_ids;
    std::optional<LibraryQuery> query;
    std::optional<std::int64_t> expected_catalog_revision;
    CancellationToken cancellation{};
};

struct CullReviewResult
{
    std::string schema{std::string(kCullReviewContractVersion)};
    std::int64_t schema_version = kCullReviewSchemaVersion;
    AssetRecord asset;
    ReviewState previous_review{};
    ReviewState review{};
    std::int64_t revision = 0;
    std::optional<std::string> next_asset_id;
    bool catalog_mutated = true;
};

} // namespace ravo
