#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo
{

inline constexpr std::string_view kAiSuggestionContractVersion = "ravo.ai.suggestion/v1";
inline constexpr std::string_view kAiStubSuggestionModelId = "deterministic-suggestion-v1";
inline constexpr std::size_t kAiSuggestionSessionLimit = 64;

enum class AiSuggestionKind : std::uint8_t
{
    kKeyword = 0,
    kCaption = 1,
    kFocus = 2,
    kDuplicate = 3,
};

[[nodiscard]] constexpr std::string_view ai_suggestion_kind_name(AiSuggestionKind kind) noexcept
{
    switch (kind)
    {
    case AiSuggestionKind::kKeyword:
        return "keyword";
    case AiSuggestionKind::kCaption:
        return "caption";
    case AiSuggestionKind::kFocus:
        return "focus";
    case AiSuggestionKind::kDuplicate:
        return "duplicate";
    }
    return "keyword";
}

[[nodiscard]] Result<AiSuggestionKind> parse_ai_suggestion_kind(std::string_view text);

enum class AiSuggestionStatus : std::uint8_t
{
    kPending = 0,
    kAccepted = 1,
    kRejected = 2,
    kCancelled = 3,
};

[[nodiscard]] constexpr std::string_view
ai_suggestion_status_name(AiSuggestionStatus status) noexcept
{
    switch (status)
    {
    case AiSuggestionStatus::kPending:
        return "pending";
    case AiSuggestionStatus::kAccepted:
        return "accepted";
    case AiSuggestionStatus::kRejected:
        return "rejected";
    case AiSuggestionStatus::kCancelled:
        return "cancelled";
    }
    return "pending";
}

struct AiSuggestionProviderInfo
{
    std::string provider_id{std::string("ravo.local.stub")};
    std::string model_id{std::string(kAiStubSuggestionModelId)};
    std::string model_version{"1.0.0"};
    std::string weight_content_hash{"stub:no-weights"};
};

struct AiSuggestion
{
    std::string id;
    std::string contract_version{std::string(kAiSuggestionContractVersion)};
    AiSuggestionKind kind = AiSuggestionKind::kKeyword;
    std::int64_t created_unix_ms = 0;
    std::string asset_id;
    std::int64_t observed_catalog_revision = 0;
    AiSuggestionProviderInfo provider;
    std::vector<std::string> suggested_keywords;
    std::optional<std::string> suggested_caption;
    std::optional<std::string> suggested_headline;
    std::optional<std::string> cue_text;
    std::vector<std::string> peer_asset_ids;
    std::optional<double> confidence;
    AiSuggestionStatus status = AiSuggestionStatus::kPending;
    bool catalog_mutated_on_accept = false;
};

struct AiSuggestionCreateRequest
{
    std::string asset_id;
    AiSuggestionKind kind = AiSuggestionKind::kKeyword;
    bool user_initiated = false;
    std::optional<std::int64_t> expected_catalog_revision;
    // Optional peer for duplicate stub (second imported asset). Empty => stub
    // synthesizes a self-peer cue that still never deletes.
    std::optional<std::string> peer_asset_id;
    CancellationToken cancellation{};
};

struct AiSuggestionAcceptResult
{
    AiSuggestion suggestion;
    bool catalog_mutated = false;
};

} // namespace ravo
