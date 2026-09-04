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

// ADR-0147/0149: deterministic exact-duplicate, burst, and near-duplicate
// fingerprint proposals (no auto-delete).
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
    std::vector<NearDuplicateGroup> groups;
    std::vector<NearDuplicateSkip> skipped;
    std::size_t assets_considered = 0;
    std::size_t assets_fingerprinted = 0;
};

struct NearDuplicateRequest
{
    int max_hamming = kCullNearDupDefaultMaxHamming;
    std::size_t max_groups = kCullNearDupDefaultMaxGroups;
    CancellationToken cancellation{};
};

struct BurstAcceptResult
{
    LibraryStackMutation stack;
    bool catalog_mutated = true;
};

} // namespace ravo
