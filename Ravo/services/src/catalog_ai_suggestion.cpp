#include "ravo/services/ai_suggestion.h"
#include "ravo/services/catalog_service.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "catalog_internal.h"
#include "ravo/adapters/text_file.h"
#include "ravo/domain/types.h"
#include "ravo/foundation/error.h"
#include "ravo/foundation/json.h"

namespace ravo
{
namespace
{

[[nodiscard]] JsonValue suggestion_to_storage_json(const AiSuggestion &suggestion)
{
    JsonValue::Array keywords;
    keywords.reserve(suggestion.suggested_keywords.size());
    for (const auto &keyword : suggestion.suggested_keywords)
        keywords.emplace_back(keyword);
    JsonValue::Array peers;
    peers.reserve(suggestion.peer_asset_ids.size());
    for (const auto &peer : suggestion.peer_asset_ids)
        peers.emplace_back(peer);
    JsonValue::Object object{
        {"schema", suggestion.contract_version},
        {"id", suggestion.id},
        {"kind", std::string(ai_suggestion_kind_name(suggestion.kind))},
        {"created_unix_ms", JsonValue::number(std::to_string(suggestion.created_unix_ms))},
        {"asset_id", suggestion.asset_id},
        {"observed_catalog_revision",
         JsonValue::number(std::to_string(suggestion.observed_catalog_revision))},
        {"status", std::string(ai_suggestion_status_name(suggestion.status))},
        {"catalog_mutated_on_accept", suggestion.catalog_mutated_on_accept},
        {"provider", JsonValue{JsonValue::Object{
                         {"provider_id", suggestion.provider.provider_id},
                         {"model_id", suggestion.provider.model_id},
                         {"model_version", suggestion.provider.model_version},
                         {"weight_content_hash", suggestion.provider.weight_content_hash},
                     }}},
        {"suggested_keywords", std::move(keywords)},
        {"peer_asset_ids", std::move(peers)},
    };
    if (suggestion.suggested_caption)
        object.emplace("suggested_caption", *suggestion.suggested_caption);
    if (suggestion.suggested_headline)
        object.emplace("suggested_headline", *suggestion.suggested_headline);
    if (suggestion.cue_text)
        object.emplace("cue_text", *suggestion.cue_text);
    if (suggestion.confidence)
        object.emplace("confidence", JsonValue::number(std::to_string(*suggestion.confidence)));
    return JsonValue{std::move(object)};
}

[[nodiscard]] Result<AiSuggestion> suggestion_from_storage_json(const JsonValue &value)
{
    const auto *object = value.object_if();
    if (object == nullptr)
    {
        return make_error(ErrorCode::kValidation, "AI suggestion document is not an object",
                          {{"reason", "invalid_ai_suggestion"}});
    }
    const auto require_string = [&](const char *key) -> Result<std::string>
    {
        const auto found = object->find(key);
        if (found == object->end() || found->second.string_if() == nullptr)
        {
            return make_error(ErrorCode::kValidation, "AI suggestion field missing",
                              {{"field", key}, {"reason", "invalid_ai_suggestion"}});
        }
        return *found->second.string_if();
    };
    const auto require_i64 = [&](const char *key) -> Result<std::int64_t>
    {
        const auto found = object->find(key);
        if (found == object->end() || found->second.number_if() == nullptr)
        {
            return make_error(ErrorCode::kValidation, "AI suggestion field missing",
                              {{"field", key}, {"reason", "invalid_ai_suggestion"}});
        }
        return std::stoll(found->second.number_if()->text);
    };

    auto schema = require_string("schema");
    if (!schema)
        return schema.error();
    if (schema.value() != kAiSuggestionContractVersion)
    {
        return make_error(ErrorCode::kValidation, "AI suggestion schema unsupported",
                          {{"reason", "unsupported_ai_suggestion_schema"}});
    }
    auto id = require_string("id");
    if (!id)
        return id.error();
    auto kind_text = require_string("kind");
    if (!kind_text)
        return kind_text.error();
    auto kind = parse_ai_suggestion_kind(kind_text.value());
    if (!kind)
        return kind.error();
    auto created = require_i64("created_unix_ms");
    if (!created)
        return created.error();
    auto asset_id = require_string("asset_id");
    if (!asset_id)
        return asset_id.error();
    auto revision = require_i64("observed_catalog_revision");
    if (!revision)
        return revision.error();
    auto status_text = require_string("status");
    if (!status_text)
        return status_text.error();

    AiSuggestion suggestion;
    suggestion.id = std::move(id).value();
    suggestion.kind = kind.value();
    suggestion.created_unix_ms = created.value();
    suggestion.asset_id = std::move(asset_id).value();
    suggestion.observed_catalog_revision = revision.value();
    if (status_text.value() == "pending")
        suggestion.status = AiSuggestionStatus::kPending;
    else if (status_text.value() == "accepted")
        suggestion.status = AiSuggestionStatus::kAccepted;
    else if (status_text.value() == "rejected")
        suggestion.status = AiSuggestionStatus::kRejected;
    else if (status_text.value() == "cancelled")
        suggestion.status = AiSuggestionStatus::kCancelled;
    else
    {
        return make_error(
            ErrorCode::kValidation, "AI suggestion status unsupported",
            {{"status", status_text.value()}, {"reason", "unsupported_ai_suggestion_status"}});
    }

    const auto *provider = value.find("provider");
    if (provider != nullptr && provider->object_if() != nullptr)
    {
        const auto *pid = provider->find("provider_id");
        const auto *mid = provider->find("model_id");
        if (pid && pid->string_if())
            suggestion.provider.provider_id = *pid->string_if();
        if (mid && mid->string_if())
            suggestion.provider.model_id = *mid->string_if();
        const auto *mver = provider->find("model_version");
        const auto *whash = provider->find("weight_content_hash");
        if (mver && mver->string_if())
            suggestion.provider.model_version = *mver->string_if();
        if (whash && whash->string_if())
            suggestion.provider.weight_content_hash = *whash->string_if();
    }
    const auto *keywords = value.find("suggested_keywords");
    if (keywords && keywords->array_if())
    {
        for (const auto &entry : *keywords->array_if())
        {
            if (entry.string_if())
                suggestion.suggested_keywords.push_back(*entry.string_if());
        }
    }
    const auto *peers = value.find("peer_asset_ids");
    if (peers && peers->array_if())
    {
        for (const auto &entry : *peers->array_if())
        {
            if (entry.string_if())
                suggestion.peer_asset_ids.push_back(*entry.string_if());
        }
    }
    const auto *caption = value.find("suggested_caption");
    if (caption && caption->string_if())
        suggestion.suggested_caption = *caption->string_if();
    const auto *headline = value.find("suggested_headline");
    if (headline && headline->string_if())
        suggestion.suggested_headline = *headline->string_if();
    const auto *cue = value.find("cue_text");
    if (cue && cue->string_if())
        suggestion.cue_text = *cue->string_if();
    const auto *confidence = value.find("confidence");
    if (confidence && confidence->number_if())
        suggestion.confidence = std::stod(confidence->number_if()->text);
    const auto *mutated = value.find("catalog_mutated_on_accept");
    if (mutated && mutated->boolean_if())
        suggestion.catalog_mutated_on_accept = *mutated->boolean_if();
    return suggestion;
}

[[nodiscard]] Result<void> fill_stub_payload(AiSuggestion &suggestion,
                                             const AiSuggestionCreateRequest &request)
{
    suggestion.confidence = 0.55;
    switch (suggestion.kind)
    {
    case AiSuggestionKind::kKeyword:
        suggestion.suggested_keywords = {"stub-keyword", "stub-scene"};
        suggestion.catalog_mutated_on_accept = true;
        break;
    case AiSuggestionKind::kCaption:
        suggestion.suggested_caption =
            "Stub caption for " + suggestion.asset_id + " (non-authoritative)";
        suggestion.suggested_headline = "Stub headline";
        suggestion.catalog_mutated_on_accept = true;
        break;
    case AiSuggestionKind::kFocus:
        suggestion.cue_text = "Stub focus cue: check subject sharpness / exposure";
        suggestion.catalog_mutated_on_accept = false;
        break;
    case AiSuggestionKind::kDuplicate:
        suggestion.cue_text = "Stub duplicate cue: review peers; never auto-delete";
        suggestion.catalog_mutated_on_accept = false;
        if (request.peer_asset_id && !request.peer_asset_id->empty() &&
            *request.peer_asset_id != suggestion.asset_id)
        {
            suggestion.peer_asset_ids.push_back(*request.peer_asset_id);
        }
        else
        {
            suggestion.peer_asset_ids.push_back(suggestion.asset_id);
        }
        break;
    }
    return {};
}

} // namespace

Result<AiSuggestionKind> parse_ai_suggestion_kind(const std::string_view text)
{
    if (text == "keyword")
        return AiSuggestionKind::kKeyword;
    if (text == "caption")
        return AiSuggestionKind::kCaption;
    if (text == "focus")
        return AiSuggestionKind::kFocus;
    if (text == "duplicate")
        return AiSuggestionKind::kDuplicate;
    return make_error(ErrorCode::kInvalidArgument, "AI suggestion kind is unsupported",
                      {{"kind", std::string(text)}, {"reason", "unsupported_ai_suggestion_kind"}});
}

Result<std::filesystem::path> CatalogService::ai_suggestions_directory() const
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto snapshot = this->snapshot();
    if (!snapshot)
        return snapshot.error();
    return std::filesystem::path(snapshot.value().database_path).concat(".ai_suggestions");
}

Result<void> CatalogService::persist_ai_suggestion(const AiSuggestion &suggestion)
{
    auto directory = ai_suggestions_directory();
    if (!directory)
        return directory.error();
    std::error_code ec;
    std::filesystem::create_directories(directory.value(), ec);
    if (ec)
    {
        return make_error(ErrorCode::kIo, "Failed to create AI suggestion directory",
                          {{"path", directory.value().string()}, {"reason", ec.message()}});
    }
    const auto path = directory.value() / (suggestion.id + ".json");
    return write_utf8_text_file_replace_atomically(
        path.string(), serialize_json(suggestion_to_storage_json(suggestion)));
}

Result<void> CatalogService::ensure_ai_suggestions_loaded() const
{
    if (ai_suggestions_loaded_)
        return {};
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto directory = ai_suggestions_directory();
    if (!directory)
        return directory.error();
    std::error_code ec;
    if (!std::filesystem::exists(directory.value(), ec))
    {
        ai_suggestions_loaded_ = true;
        return {};
    }
    for (const auto &entry : std::filesystem::directory_iterator(directory.value(), ec))
    {
        if (ec)
            break;
        if (!entry.is_regular_file() || entry.path().extension() != ".json")
            continue;
        auto text = read_utf8_text_file(entry.path().string());
        if (!text)
            return text.error();
        auto parsed = parse_json(text.value());
        if (!parsed)
            return parsed.error();
        auto suggestion = suggestion_from_storage_json(parsed.value());
        if (!suggestion)
            return suggestion.error();
        ai_suggestions_.insert_or_assign(suggestion.value().id, suggestion.value());
    }
    if (ec)
    {
        return make_error(ErrorCode::kIo, "Failed to read AI suggestion directory",
                          {{"path", directory.value().string()}, {"reason", ec.message()}});
    }
    ai_suggestions_loaded_ = true;
    return {};
}

Result<AiSuggestion> CatalogService::create_ai_suggestion(const AiSuggestionCreateRequest &request)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto loaded = ensure_ai_suggestions_loaded();
    if (!loaded)
        return loaded.error();
    if (!request.user_initiated)
    {
        return make_error(ErrorCode::kValidation, "AI suggestion requires explicit user initiation",
                          {{"reason", "ai_suggestion_not_user_initiated"}});
    }
    if (request.asset_id.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "AI suggestion requires an asset id",
                          {{"reason", "missing_asset_id"}});
    }
    auto cancelled = request.cancellation.check();
    if (!cancelled)
        return cancelled.error();

    auto snapshot = this->snapshot();
    if (!snapshot)
        return snapshot.error();
    if (request.expected_catalog_revision &&
        *request.expected_catalog_revision != snapshot.value().revision)
    {
        return make_error(ErrorCode::kConflict, "Catalog revision does not match",
                          {{"reason", "stale_catalog_revision"}});
    }
    auto asset = repository_->find_asset_by_id(request.asset_id);
    if (!asset)
        return asset.error();
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Source asset was not found",
                          {{"asset_id", request.asset_id}, {"reason", "asset_not_found"}});
    }
    if (request.peer_asset_id)
    {
        auto peer = repository_->find_asset_by_id(*request.peer_asset_id);
        if (!peer)
            return peer.error();
        if (!peer.value())
        {
            return make_error(
                ErrorCode::kNotFound, "Peer asset was not found",
                {{"asset_id", *request.peer_asset_id}, {"reason", "peer_asset_not_found"}});
        }
    }

    std::size_t pending = 0;
    for (const auto &[id, existing] : ai_suggestions_)
    {
        (void)id;
        if (existing.status == AiSuggestionStatus::kPending)
            ++pending;
    }
    if (pending >= kAiSuggestionSessionLimit)
    {
        return make_error(ErrorCode::kValidation, "AI suggestion session limit reached",
                          {{"reason", "ai_suggestion_session_limit"}});
    }

    AiSuggestion suggestion;
    suggestion.id = generate_ai_suggestion_id();
    suggestion.kind = request.kind;
    suggestion.created_unix_ms = now_unix_ms();
    suggestion.asset_id = request.asset_id;
    suggestion.observed_catalog_revision = snapshot.value().revision;
    suggestion.provider.model_id = std::string(kAiStubSuggestionModelId);
    auto filled = fill_stub_payload(suggestion, request);
    if (!filled)
        return filled.error();
    auto persisted = persist_ai_suggestion(suggestion);
    if (!persisted)
        return persisted.error();
    ai_suggestions_.insert_or_assign(suggestion.id, suggestion);
    return suggestion;
}

Result<AiSuggestion> CatalogService::get_ai_suggestion(const std::string_view suggestion_id) const
{
    auto loaded = ensure_ai_suggestions_loaded();
    if (!loaded)
        return loaded.error();
    if (suggestion_id.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "AI suggestion id is required",
                          {{"reason", "missing_suggestion_id"}});
    }
    const auto found = ai_suggestions_.find(std::string(suggestion_id));
    if (found == ai_suggestions_.end())
    {
        return make_error(ErrorCode::kNotFound, "AI suggestion was not found",
                          {{"suggestion_id", std::string(suggestion_id)}});
    }
    return found->second;
}

Result<std::vector<AiSuggestion>>
CatalogService::list_ai_suggestions(const std::optional<std::string_view> asset_id) const
{
    auto loaded = ensure_ai_suggestions_loaded();
    if (!loaded)
        return loaded.error();
    std::vector<AiSuggestion> listed;
    listed.reserve(ai_suggestions_.size());
    for (const auto &[id, suggestion] : ai_suggestions_)
    {
        (void)id;
        if (asset_id && suggestion.asset_id != *asset_id)
            continue;
        listed.push_back(suggestion);
    }
    std::sort(listed.begin(), listed.end(),
              [](const AiSuggestion &lhs, const AiSuggestion &rhs)
              {
                  if (lhs.created_unix_ms != rhs.created_unix_ms)
                      return lhs.created_unix_ms < rhs.created_unix_ms;
                  return lhs.id < rhs.id;
              });
    return listed;
}

Result<AiSuggestionAcceptResult>
CatalogService::accept_ai_suggestion(const std::string_view suggestion_id,
                                     const std::optional<std::int64_t> expected_catalog_revision)
{
    auto loaded = ensure_ai_suggestions_loaded();
    if (!loaded)
        return loaded.error();
    auto found = ai_suggestions_.find(std::string(suggestion_id));
    if (found == ai_suggestions_.end())
    {
        return make_error(ErrorCode::kNotFound, "AI suggestion was not found",
                          {{"suggestion_id", std::string(suggestion_id)}});
    }
    if (found->second.status != AiSuggestionStatus::kPending)
    {
        return make_error(
            ErrorCode::kConflict, "AI suggestion is not pending",
            {{"suggestion_id", std::string(suggestion_id)},
             {"status", std::string(ai_suggestion_status_name(found->second.status))}});
    }

    auto snapshot = this->snapshot();
    if (!snapshot)
        return snapshot.error();
    if (expected_catalog_revision && *expected_catalog_revision != snapshot.value().revision)
    {
        return make_error(ErrorCode::kConflict, "Catalog revision does not match",
                          {{"reason", "stale_catalog_revision"}});
    }

    AiSuggestionAcceptResult result;
    result.catalog_mutated = false;
    AiSuggestion &suggestion = found->second;

    if (suggestion.kind == AiSuggestionKind::kKeyword)
    {
        auto asset = repository_->find_asset_by_id(suggestion.asset_id);
        if (!asset)
            return asset.error();
        if (!asset.value())
        {
            return make_error(ErrorCode::kNotFound, "Source asset was not found",
                              {{"asset_id", suggestion.asset_id}});
        }
        std::vector<std::string> tags = asset.value()->tags;
        for (const auto &keyword : suggestion.suggested_keywords)
        {
            if (std::find(tags.begin(), tags.end(), keyword) == tags.end())
                tags.push_back(keyword);
        }
        auto updated = set_tags(suggestion.asset_id, tags);
        if (!updated)
            return updated.error();
        result.catalog_mutated = true;
    }
    else if (suggestion.kind == AiSuggestionKind::kCaption)
    {
        if (!suggestion.suggested_caption)
        {
            return make_error(ErrorCode::kValidation, "Caption suggestion is empty",
                              {{"reason", "missing_suggested_caption"}});
        }
        auto asset = repository_->find_asset_by_id(suggestion.asset_id);
        if (!asset)
            return asset.error();
        if (!asset.value())
        {
            return make_error(ErrorCode::kNotFound, "Source asset was not found",
                              {{"asset_id", suggestion.asset_id}});
        }
        WritableMetadata metadata = asset.value()->metadata;
        metadata.description = *suggestion.suggested_caption;
        if (suggestion.suggested_headline)
            metadata.headline = *suggestion.suggested_headline;
        auto updated = set_writable_metadata(suggestion.asset_id, metadata);
        if (!updated)
            return updated.error();
        result.catalog_mutated = true;
    }
    else if (suggestion.kind == AiSuggestionKind::kFocus ||
             suggestion.kind == AiSuggestionKind::kDuplicate)
    {
        // Acknowledgement only — never reject/delete/publish peers.
        result.catalog_mutated = false;
    }

    suggestion.status = AiSuggestionStatus::kAccepted;
    suggestion.catalog_mutated_on_accept = result.catalog_mutated;
    auto persisted = persist_ai_suggestion(suggestion);
    if (!persisted)
        return persisted.error();
    result.suggestion = suggestion;
    return result;
}

Result<AiSuggestion> CatalogService::reject_ai_suggestion(const std::string_view suggestion_id)
{
    auto loaded = ensure_ai_suggestions_loaded();
    if (!loaded)
        return loaded.error();
    auto found = ai_suggestions_.find(std::string(suggestion_id));
    if (found == ai_suggestions_.end())
    {
        return make_error(ErrorCode::kNotFound, "AI suggestion was not found",
                          {{"suggestion_id", std::string(suggestion_id)}});
    }
    if (found->second.status != AiSuggestionStatus::kPending)
    {
        return make_error(ErrorCode::kConflict, "AI suggestion is not pending",
                          {{"suggestion_id", std::string(suggestion_id)},
                           {"reason", "ai_suggestion_not_pending"}});
    }
    found->second.status = AiSuggestionStatus::kRejected;
    auto persisted = persist_ai_suggestion(found->second);
    if (!persisted)
        return persisted.error();
    return found->second;
}

Result<AiSuggestion> CatalogService::cancel_ai_suggestion(const std::string_view suggestion_id)
{
    auto loaded = ensure_ai_suggestions_loaded();
    if (!loaded)
        return loaded.error();
    auto found = ai_suggestions_.find(std::string(suggestion_id));
    if (found == ai_suggestions_.end())
    {
        return make_error(ErrorCode::kNotFound, "AI suggestion was not found",
                          {{"suggestion_id", std::string(suggestion_id)}});
    }
    if (found->second.status != AiSuggestionStatus::kPending)
    {
        return make_error(ErrorCode::kConflict, "AI suggestion is not pending",
                          {{"suggestion_id", std::string(suggestion_id)},
                           {"reason", "ai_suggestion_not_pending"}});
    }
    found->second.status = AiSuggestionStatus::kCancelled;
    auto persisted = persist_ai_suggestion(found->second);
    if (!persisted)
        return persisted.error();
    return found->second;
}

} // namespace ravo
