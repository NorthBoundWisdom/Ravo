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
// ADR-0149 / CULL-01: rebuildable aHash cache + dismissals under {catalog}.cull/.
inline constexpr std::string_view kCullFingerprintCacheContractVersion =
    "ravo.cull.fingerprint-cache/v1";
inline constexpr std::string_view kCullNearDupFingerprintAlgorithm = "ahash_v1";
// Single accepted RAW/raster orientation+colour decode owner for aHash
// fingerprints (same path as import candidate thumbnails).
inline constexpr std::string_view kCullFingerprintDecodePath =
    "catalog.decode_import_candidate_thumbnail";
inline constexpr std::string_view kCullGroupKindExactByte = "exact_byte";
inline constexpr std::string_view kCullGroupKindSameFile = "same_file";
inline constexpr std::string_view kCullGroupKindHeuristicAHash = "heuristic_ahash";
inline constexpr std::string_view kCullGroupKindBurstProposal = "burst_proposal";
// Default throttle: skip a full recompute pass when the cache is complete and
// younger than this window (identity mismatches still recompute).
inline constexpr std::int64_t kCullFingerprintDefaultThrottleMs = 250;
inline constexpr std::size_t kCullFingerprintIncrementalPersistEvery = 32;

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
    // Distinguishes exact-byte / same-file authority from heuristic aHash groups.
    std::string group_kind{std::string(kCullGroupKindExactByte)};
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
    // Explicitly heuristic; never delete/reject/stack authority (ADR-0149).
    std::string group_kind{std::string(kCullGroupKindHeuristicAHash)};
    bool non_authoritative = true;
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
    std::size_t assets_from_cache = 0;
    std::size_t assets_computed = 0;
    std::size_t cache_entries = 0;
    std::size_t dismissed_groups_omitted = 0;
    bool cache_used = false;
    bool throttled = false;
    // Heuristic only: never authoritative for delete/reject/stack.
    bool non_authoritative = true;
    // Declares the one accepted image-resource decode path for fingerprints.
    std::string fingerprint_decode_path{std::string(kCullFingerprintDecodePath)};
};

struct NearDuplicateRequest
{
    int max_hamming = kCullNearDupDefaultMaxHamming;
    std::size_t max_groups = kCullNearDupDefaultMaxGroups;
    std::size_t max_assets = kCullNearDupDefaultMaxAssets;
    // 0 disables throttle; otherwise reuses a complete fresh cache without decode.
    std::int64_t throttle_ms = kCullFingerprintDefaultThrottleMs;
    bool persist_cache = true;
    bool omit_dismissed = true;
    CancellationToken cancellation{};
};

enum class CullSuggestionKind : std::uint8_t
{
    kExactDuplicate = 0,
    kNearDuplicate = 1,
    kBurst = 2,
};

[[nodiscard]] inline std::string_view
cull_suggestion_kind_name(const CullSuggestionKind kind) noexcept
{
    switch (kind)
    {
    case CullSuggestionKind::kExactDuplicate:
        return "exact_duplicate";
    case CullSuggestionKind::kNearDuplicate:
        return "near_duplicate";
    case CullSuggestionKind::kBurst:
        return "burst";
    }
    return "near_duplicate";
}

[[nodiscard]] inline std::optional<CullSuggestionKind>
parse_cull_suggestion_kind(const std::string_view name) noexcept
{
    if (name == "exact_duplicate")
        return CullSuggestionKind::kExactDuplicate;
    if (name == "near_duplicate")
        return CullSuggestionKind::kNearDuplicate;
    if (name == "burst")
        return CullSuggestionKind::kBurst;
    return std::nullopt;
}

struct CullSuggestionDismissRequest
{
    CullSuggestionKind kind = CullSuggestionKind::kNearDuplicate;
    // Stable key: exact sha256, near-dup sorted fingerprint pair, or burst proposal_id.
    std::string key;
    CancellationToken cancellation{};
};

struct CullSuggestionDismissResult
{
    std::string schema{std::string(kCullFingerprintCacheContractVersion)};
    CullSuggestionKind kind = CullSuggestionKind::kNearDuplicate;
    std::string key;
    bool dismissed = true;
    std::int64_t dismissed_unix_ms = 0;
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
