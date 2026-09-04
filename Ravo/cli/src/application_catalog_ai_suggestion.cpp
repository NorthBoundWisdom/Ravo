#include "application_internal.h"

#include <string>
#include <utility>
#include <vector>

#include "ravo/services/ai_suggestion.h"

namespace ravo::cli_internal
{

JsonValue ai_suggestion_to_json(const AiSuggestion &suggestion)
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
        {"provider_id", suggestion.provider.provider_id},
        {"model_id", suggestion.provider.model_id},
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

Result<JsonValue> run_catalog_ai_suggestion_command(CatalogService &service,
                                                    const std::string_view subcommand,
                                                    const CatalogCliArguments &flags)
{
    if (subcommand == "ai-suggest")
    {
        if (flags.asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog ai-suggest requires --asset-id");
        }
        if (!flags.user_initiated)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog ai-suggest requires --user-initiated",
                              {{"reason", "missing_user_initiated"}});
        }
        if (flags.suggestion_kind.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog ai-suggest requires --suggestion-kind");
        }
        auto kind = parse_ai_suggestion_kind(flags.suggestion_kind);
        if (!kind)
            return kind.error();
        AiSuggestionCreateRequest request;
        request.asset_id = std::string(flags.asset_id);
        request.kind = kind.value();
        request.user_initiated = true;
        request.expected_catalog_revision = flags.expected_revision;
        if (!flags.destination_assets.empty())
            request.peer_asset_id = std::string(flags.destination_assets.front());
        auto created = service.create_ai_suggestion(request);
        if (!created)
            return created.error();
        return ai_suggestion_to_json(created.value());
    }
    if (subcommand == "ai-suggestion")
    {
        if (flags.suggestion_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog ai-suggestion requires --suggestion-id");
        }
        auto suggestion = service.get_ai_suggestion(flags.suggestion_id);
        if (!suggestion)
            return suggestion.error();
        return ai_suggestion_to_json(suggestion.value());
    }
    if (subcommand == "ai-suggestions")
    {
        std::optional<std::string_view> asset;
        if (!flags.asset_id.empty())
            asset = flags.asset_id;
        auto listed = service.list_ai_suggestions(asset);
        if (!listed)
            return listed.error();
        JsonValue::Array rows;
        rows.reserve(listed.value().size());
        for (const auto &suggestion : listed.value())
            rows.push_back(ai_suggestion_to_json(suggestion));
        return JsonValue{JsonValue::Object{{"suggestions", std::move(rows)}}};
    }
    if (subcommand == "ai-suggestion-accept")
    {
        if (flags.suggestion_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog ai-suggestion-accept requires --suggestion-id");
        }
        auto accepted = service.accept_ai_suggestion(flags.suggestion_id, flags.expected_revision);
        if (!accepted)
            return accepted.error();
        return JsonValue{JsonValue::Object{
            {"suggestion", ai_suggestion_to_json(accepted.value().suggestion)},
            {"catalog_mutated", accepted.value().catalog_mutated},
        }};
    }
    if (subcommand == "ai-suggestion-reject")
    {
        if (flags.suggestion_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog ai-suggestion-reject requires --suggestion-id");
        }
        auto rejected = service.reject_ai_suggestion(flags.suggestion_id);
        if (!rejected)
            return rejected.error();
        return ai_suggestion_to_json(rejected.value());
    }
    if (subcommand == "ai-suggestion-cancel")
    {
        if (flags.suggestion_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog ai-suggestion-cancel requires --suggestion-id");
        }
        auto cancelled = service.cancel_ai_suggestion(flags.suggestion_id);
        if (!cancelled)
            return cancelled.error();
        return ai_suggestion_to_json(cancelled.value());
    }
    return make_error(ErrorCode::kInvalidArgument, "Unknown AI suggestion subcommand",
                      {{"subcommand", std::string(subcommand)}});
}

} // namespace ravo::cli_internal
