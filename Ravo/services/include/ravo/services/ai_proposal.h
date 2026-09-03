#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <span>

#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/develop.h"

namespace ravo
{

// Versioned reviewable AI proposal contract (ADR-0121 / AI-01). Proposals are
// not silent recipe writes: create/show/reject leave the catalog unchanged;
// apply goes through save_develop_with_history.

inline constexpr std::string_view kAiProposalContractVersion = "ravo.ai.proposal/v1";
inline constexpr std::string_view kAiStubProviderId = "ravo.local.stub";
inline constexpr std::string_view kAiStubModelId = "deterministic-global-v1";
inline constexpr std::string_view kAiStubSemanticMaskModelId = "deterministic-semantic-mask-v1";
inline constexpr std::string_view kAiStubModelVersion = "1.0.0";
inline constexpr std::string_view kAiStubWeightContentHash = "stub:no-weights";
inline constexpr std::size_t kAiProposalSessionLimit = 64;

enum class AiProposalKind : std::uint8_t
{
    kGlobal = 0,
    kSemanticMask = 1,
};

[[nodiscard]] constexpr std::string_view ai_proposal_kind_name(AiProposalKind kind) noexcept
{
    switch (kind)
    {
    case AiProposalKind::kGlobal:
        return "global";
    case AiProposalKind::kSemanticMask:
        return "semantic-mask";
    }
    return "global";
}

[[nodiscard]] Result<AiProposalKind> parse_ai_proposal_kind(std::string_view text);

enum class AiProposalStatus : std::uint8_t
{
    kPending = 0,
    kApplied = 1,
    kRejected = 2,
    kCancelled = 3,
};

[[nodiscard]] constexpr std::string_view ai_proposal_status_name(AiProposalStatus status) noexcept
{
    switch (status)
    {
    case AiProposalStatus::kPending:
        return "pending";
    case AiProposalStatus::kApplied:
        return "applied";
    case AiProposalStatus::kRejected:
        return "rejected";
    case AiProposalStatus::kCancelled:
        return "cancelled";
    }
    return "pending";
}

struct AiProposalFieldChange
{
    std::string field;
    double value = 0.0;
    std::optional<double> confidence;
};

struct AiProposalAlternative
{
    std::string label;
    std::vector<AiProposalFieldChange> fields;
    std::optional<double> confidence;
};

struct AiProposalProviderInfo
{
    std::string provider_id{std::string(kAiStubProviderId)};
    std::string model_id{std::string(kAiStubModelId)};
    std::string model_version{std::string(kAiStubModelVersion)};
    std::string weight_content_hash{std::string(kAiStubWeightContentHash)};
    std::map<std::string, std::string, std::less<>> parameters;
};

struct AiProposal
{
    std::string id;
    std::string contract_version{std::string(kAiProposalContractVersion)};
    AiProposalKind kind = AiProposalKind::kGlobal;
    std::optional<std::string> semantic_label;
    std::int64_t created_unix_ms = 0;
    std::string asset_id;
    std::int64_t observed_catalog_revision = 0;
    std::int64_t observed_recovery_generation = 0;
    AiProposalProviderInfo provider;
    std::vector<AiProposalFieldChange> fields;
    std::vector<DevelopChange> field_diff;
    std::vector<AiProposalAlternative> alternatives;
    AiProposalStatus status = AiProposalStatus::kPending;
    std::optional<std::int64_t> applied_history_id;
};

struct AiProposalCreateRequest
{
    std::string asset_id;
    // Explicit user initiation is mandatory (ADR-0121). False fails closed.
    bool user_initiated = false;
    AiProposalKind kind = AiProposalKind::kGlobal;
    // Required for semantic-mask kind: subject|sky|background|person|clothing|object.
    std::optional<std::string> semantic_label;
    std::optional<std::int64_t> expected_catalog_revision;
    std::string provider_id{std::string(kAiStubProviderId)};
    std::string model_id{std::string(kAiStubModelId)};
    CancellationToken cancellation;
};

struct AiProposalApplyResult
{
    AiProposal proposal;
    AssetRecord asset;
    std::int64_t revision = 0;
    std::optional<std::int64_t> history_id;
};

[[nodiscard]] std::span<const std::string_view> ai_proposal_allowed_fields() noexcept;
[[nodiscard]] std::span<const std::string_view> ai_semantic_mask_allowed_fields() noexcept;
[[nodiscard]] bool is_ai_proposal_allowed_field(std::string_view field) noexcept;
[[nodiscard]] bool is_ai_semantic_mask_allowed_field(std::string_view field) noexcept;
[[nodiscard]] Result<void>
validate_ai_proposal_fields(const std::vector<AiProposalFieldChange> &fields,
                            AiProposalKind kind = AiProposalKind::kGlobal);
[[nodiscard]] Result<DevelopParams>
apply_ai_proposal_fields(DevelopParams params, const std::vector<AiProposalFieldChange> &fields,
                         AiProposalKind kind = AiProposalKind::kGlobal);
[[nodiscard]] Result<std::vector<AiProposalFieldChange>>
build_stub_ai_proposal_fields(std::string_view asset_id);
[[nodiscard]] Result<std::vector<AiProposalFieldChange>>
build_stub_semantic_mask_proposal_fields(std::string_view asset_id,
                                         std::string_view semantic_label);
[[nodiscard]] Result<void> validate_ai_semantic_label(std::string_view label);
[[nodiscard]] Result<std::vector<AiProposalAlternative>>
build_stub_ai_proposal_alternatives(std::string_view asset_id);

} // namespace ravo
