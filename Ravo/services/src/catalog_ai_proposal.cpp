#include "ravo/services/ai_proposal.h"
#include "ravo/services/catalog_service.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ravo/adapters/text_file.h"
#include "ravo/foundation/json.h"
#include <filesystem>
#include "catalog_internal.h"
#include "ravo/domain/types.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/develop.h"

namespace ravo
{
namespace
{

constexpr auto kAllowedFields = std::to_array<std::string_view>({
    "exposure",
    "contrast",
    "highlights",
    "shadows",
    "whites",
    "blacks",
    "vibrance",
    "saturation",
    "straighten",
    "cropX",
    "cropY",
    "cropWidth",
    "cropHeight",
    "toneEqBlacks",
    "toneEqShadows",
    "toneEqMidtones",
    "toneEqHighlights",
    "toneEqWhites",
    "colorBalanceContrast",
    "colorBalanceVibrance",
    "colorBalanceSaturationGlobal",
    "whiteBalanceMode",
    "whiteBalanceRed",
    "whiteBalanceGreen",
    "whiteBalanceBlue",
    "whiteBalanceFourth",
});

[[nodiscard]] Result<void> require_stub_provider(const std::string_view provider_id,
                                                 const std::string_view model_id)
{
    if (provider_id.empty() || model_id.empty())
    {
        return make_error(ErrorCode::kValidation, "AI provider or model is missing",
                          {{"provider_id", std::string(provider_id)},
                           {"model_id", std::string(model_id)},
                           {"reason", "missing_ai_provider_or_model"}});
    }
    if (provider_id != kAiStubProviderId)
    {
        return make_error(ErrorCode::kUnsupported, "AI provider is not packaged",
                          {{"provider_id", std::string(provider_id)},
                           {"reason", "ai_provider_not_packaged"},
                           {"available_provider", std::string(kAiStubProviderId)}});
    }
    if (model_id != kAiStubModelId)
    {
        return make_error(ErrorCode::kUnsupported, "AI model is not packaged",
                          {{"provider_id", std::string(provider_id)},
                           {"model_id", std::string(model_id)},
                           {"reason", "ai_model_not_packaged"},
                           {"available_model", std::string(kAiStubModelId)}});
    }
    return {};
}

[[nodiscard]] double stub_unit(const std::string_view asset_id, const std::uint32_t salt) noexcept
{
    std::uint32_t hash = 2166136261U ^ salt;
    for (const char ch : asset_id)
    {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 16777619U;
    }
    return static_cast<double>(hash % 1000U) / 1000.0;
}

[[nodiscard]] Result<JsonValue> ai_proposal_to_storage_json(const AiProposal &proposal)
{
    JsonValue::Array fields;
    for (const auto &change : proposal.fields)
    {
        JsonValue::Object object{{"field", change.field},
                                 {"value", JsonValue::number(std::to_string(change.value))}};
        if (change.confidence)
            object.emplace("confidence", JsonValue::number(std::to_string(*change.confidence)));
        fields.push_back(std::move(object));
    }
    JsonValue::Array diffs;
    for (const auto &diff : proposal.field_diff)
        diffs.push_back(JsonValue::Object{{"field", diff.field}, {"value", diff.value}});
    JsonValue::Array alternatives;
    for (const auto &alternative : proposal.alternatives)
    {
        JsonValue::Array alt_fields;
        for (const auto &change : alternative.fields)
        {
            JsonValue::Object object{{"field", change.field},
                                     {"value", JsonValue::number(std::to_string(change.value))}};
            if (change.confidence)
                object.emplace("confidence", JsonValue::number(std::to_string(*change.confidence)));
            alt_fields.push_back(std::move(object));
        }
        JsonValue::Object object{{"label", alternative.label}, {"fields", std::move(alt_fields)}};
        if (alternative.confidence)
            object.emplace("confidence",
                           JsonValue::number(std::to_string(*alternative.confidence)));
        alternatives.push_back(std::move(object));
    }
    JsonValue::Object parameters;
    for (const auto &[key, value] : proposal.provider.parameters)
        parameters.emplace(key, value);
    JsonValue::Object provider{{"model_id", proposal.provider.model_id},
                               {"model_version", proposal.provider.model_version},
                               {"parameters", std::move(parameters)},
                               {"provider_id", proposal.provider.provider_id},
                               {"weight_content_hash", proposal.provider.weight_content_hash}};
    JsonValue::Object object{
        {"alternatives", std::move(alternatives)},
        {"asset_id", proposal.asset_id},
        {"contract_version", proposal.contract_version},
        {"created_unix_ms", JsonValue::number(std::to_string(proposal.created_unix_ms))},
        {"field_diff", std::move(diffs)},
        {"fields", std::move(fields)},
        {"id", proposal.id},
        {"observed_catalog_revision",
         JsonValue::number(std::to_string(proposal.observed_catalog_revision))},
        {"observed_recovery_generation",
         JsonValue::number(std::to_string(proposal.observed_recovery_generation))},
        {"provider", std::move(provider)},
        {"status", std::string(ai_proposal_status_name(proposal.status))},
    };
    if (proposal.applied_history_id)
        object.emplace("applied_history_id",
                       JsonValue::number(std::to_string(*proposal.applied_history_id)));
    else
        object.emplace("applied_history_id", nullptr);
    return JsonValue{std::move(object)};
}

[[nodiscard]] Result<double> require_ai_json_number(const JsonValue::Object &object,
                                                    const std::string_view key)
{
    const auto it = object.find(std::string(key));
    if (it == object.end() || !it->second.number_if())
        return make_error(ErrorCode::kValidation, "AI proposal JSON number field is missing",
                          {{"field", std::string(key)}});
    return std::stod(it->second.number_if()->text);
}

[[nodiscard]] Result<std::string> require_ai_json_string(const JsonValue::Object &object,
                                                         const std::string_view key)
{
    const auto it = object.find(std::string(key));
    if (it == object.end() || !it->second.string_if())
        return make_error(ErrorCode::kValidation, "AI proposal JSON string field is missing",
                          {{"field", std::string(key)}});
    return std::string(*it->second.string_if());
}

[[nodiscard]] Result<AiProposalFieldChange> parse_ai_field_change(const JsonValue &value)
{
    const auto *object = value.object_if();
    if (!object)
        return make_error(ErrorCode::kValidation, "AI proposal field change must be an object");
    auto field = require_ai_json_string(*object, "field");
    if (!field)
        return field.error();
    auto number = require_ai_json_number(*object, "value");
    if (!number)
        return number.error();
    AiProposalFieldChange change;
    change.field = std::move(field).value();
    change.value = number.value();
    if (const auto it = object->find("confidence"); it != object->end() && it->second.number_if())
        change.confidence = std::stod(it->second.number_if()->text);
    return change;
}

[[nodiscard]] Result<AiProposal> ai_proposal_from_storage_json(const JsonValue &value)
{
    const auto *object = value.object_if();
    if (!object)
        return make_error(ErrorCode::kValidation, "AI proposal document must be an object");
    AiProposal proposal;
    auto id = require_ai_json_string(*object, "id");
    if (!id)
        return id.error();
    proposal.id = std::move(id).value();
    auto contract = require_ai_json_string(*object, "contract_version");
    if (!contract)
        return contract.error();
    proposal.contract_version = std::move(contract).value();
    auto created = require_ai_json_number(*object, "created_unix_ms");
    if (!created)
        return created.error();
    proposal.created_unix_ms = static_cast<std::int64_t>(created.value());
    auto asset = require_ai_json_string(*object, "asset_id");
    if (!asset)
        return asset.error();
    proposal.asset_id = std::move(asset).value();
    auto revision = require_ai_json_number(*object, "observed_catalog_revision");
    if (!revision)
        return revision.error();
    proposal.observed_catalog_revision = static_cast<std::int64_t>(revision.value());
    auto generation = require_ai_json_number(*object, "observed_recovery_generation");
    if (!generation)
        return generation.error();
    proposal.observed_recovery_generation = static_cast<std::int64_t>(generation.value());
    auto status = require_ai_json_string(*object, "status");
    if (!status)
        return status.error();
    if (status.value() == "pending")
        proposal.status = AiProposalStatus::kPending;
    else if (status.value() == "applied")
        proposal.status = AiProposalStatus::kApplied;
    else if (status.value() == "rejected")
        proposal.status = AiProposalStatus::kRejected;
    else if (status.value() == "cancelled")
        proposal.status = AiProposalStatus::kCancelled;
    else
        return make_error(ErrorCode::kValidation, "AI proposal status is unknown",
                          {{"status", status.value()}});
    const auto provider_it = object->find("provider");
    if (provider_it == object->end() || !provider_it->second.object_if())
        return make_error(ErrorCode::kValidation, "AI proposal provider is missing");
    const auto &provider = *provider_it->second.object_if();
    auto provider_id = require_ai_json_string(provider, "provider_id");
    auto model_id = require_ai_json_string(provider, "model_id");
    auto model_version = require_ai_json_string(provider, "model_version");
    auto weight = require_ai_json_string(provider, "weight_content_hash");
    if (!provider_id || !model_id || !model_version || !weight)
        return make_error(ErrorCode::kValidation, "AI proposal provider fields are incomplete");
    proposal.provider.provider_id = std::move(provider_id).value();
    proposal.provider.model_id = std::move(model_id).value();
    proposal.provider.model_version = std::move(model_version).value();
    proposal.provider.weight_content_hash = std::move(weight).value();
    if (const auto params_it = provider.find("parameters");
        params_it != provider.end() && params_it->second.object_if())
    {
        for (const auto &[key, param] : *params_it->second.object_if())
        {
            if (param.string_if())
                proposal.provider.parameters.emplace(key, *param.string_if());
        }
    }
    const auto fields_it = object->find("fields");
    if (fields_it == object->end() || !fields_it->second.array_if())
        return make_error(ErrorCode::kValidation, "AI proposal fields are missing");
    for (const auto &entry : *fields_it->second.array_if())
    {
        auto change = parse_ai_field_change(entry);
        if (!change)
            return change.error();
        proposal.fields.push_back(std::move(change).value());
    }
    if (const auto diff_it = object->find("field_diff");
        diff_it != object->end() && diff_it->second.array_if())
    {
        for (const auto &entry : *diff_it->second.array_if())
        {
            const auto *diff = entry.object_if();
            if (!diff)
                continue;
            DevelopChange change;
            if (const auto field = diff->find("field");
                field != diff->end() && field->second.string_if())
                change.field = *field->second.string_if();
            if (const auto val = diff->find("value"); val != diff->end() && val->second.string_if())
                change.value = *val->second.string_if();
            proposal.field_diff.push_back(std::move(change));
        }
    }
    if (const auto alt_it = object->find("alternatives");
        alt_it != object->end() && alt_it->second.array_if())
    {
        for (const auto &entry : *alt_it->second.array_if())
        {
            const auto *alt = entry.object_if();
            if (!alt)
                continue;
            AiProposalAlternative alternative;
            if (const auto label = alt->find("label");
                label != alt->end() && label->second.string_if())
                alternative.label = *label->second.string_if();
            if (const auto fields = alt->find("fields");
                fields != alt->end() && fields->second.array_if())
            {
                for (const auto &field_entry : *fields->second.array_if())
                {
                    auto change = parse_ai_field_change(field_entry);
                    if (!change)
                        return change.error();
                    alternative.fields.push_back(std::move(change).value());
                }
            }
            if (const auto confidence = alt->find("confidence");
                confidence != alt->end() && confidence->second.number_if())
                alternative.confidence = std::stod(confidence->second.number_if()->text);
            proposal.alternatives.push_back(std::move(alternative));
        }
    }
    if (const auto history = object->find("applied_history_id");
        history != object->end() && history->second.number_if())
        proposal.applied_history_id =
            static_cast<std::int64_t>(std::stoll(history->second.number_if()->text));
    return proposal;
}

} // namespace

std::span<const std::string_view> ai_proposal_allowed_fields() noexcept
{
    return kAllowedFields;
}

bool is_ai_proposal_allowed_field(const std::string_view field) noexcept
{
    return std::find(kAllowedFields.begin(), kAllowedFields.end(), field) != kAllowedFields.end();
}

[[nodiscard]] int ai_field_apply_rank(const std::string_view field) noexcept
{
    if (field == "cropWidth")
        return 0;
    if (field == "cropHeight")
        return 1;
    if (field == "cropX")
        return 2;
    if (field == "cropY")
        return 3;
    return 4;
}

[[nodiscard]] std::vector<AiProposalFieldChange>
order_ai_proposal_fields(std::vector<AiProposalFieldChange> fields)
{
    std::stable_sort(fields.begin(), fields.end(),
                     [](const AiProposalFieldChange &lhs, const AiProposalFieldChange &rhs)
                     { return ai_field_apply_rank(lhs.field) < ai_field_apply_rank(rhs.field); });
    return fields;
}

Result<void> validate_ai_proposal_fields(const std::vector<AiProposalFieldChange> &fields)
{
    if (fields.empty())
    {
        return make_error(ErrorCode::kValidation, "AI proposal fields are empty",
                          {{"reason", "empty_ai_proposal_fields"}});
    }
    if (fields.size() > kAllowedFields.size())
    {
        return make_error(ErrorCode::kValidation, "AI proposal field count exceeds allowlist",
                          {{"reason", "ai_proposal_fields_too_large"},
                           {"field_count", std::to_string(fields.size())}});
    }
    std::set<std::string, std::less<>> seen;
    for (const auto &change : fields)
    {
        if (change.field.empty() || !seen.emplace(change.field).second)
        {
            return make_error(ErrorCode::kValidation, "AI proposal field is empty or duplicated",
                              {{"field", change.field}, {"reason", "duplicate_or_empty_ai_field"}});
        }
        if (!is_ai_proposal_allowed_field(change.field))
        {
            return make_error(ErrorCode::kValidation, "AI proposal field is not allowed",
                              {{"field", change.field}, {"reason", "unknown_ai_proposal_field"}});
        }
    }
    DevelopParams probe;
    for (const auto &change : order_ai_proposal_fields(fields))
    {
        auto applied = apply_develop_field_strict(probe, change.field, change.value);
        if (!applied)
        {
            auto error = applied.error();
            error.context.insert_or_assign("reason", "invalid_ai_proposal_field_bounds");
            error.context.insert_or_assign("field", change.field);
            return error;
        }
    }
    return {};
}

Result<DevelopParams> apply_ai_proposal_fields(DevelopParams params,
                                               const std::vector<AiProposalFieldChange> &fields)
{
    auto valid = validate_ai_proposal_fields(fields);
    if (!valid)
        return valid.error();
    for (const auto &change : order_ai_proposal_fields(fields))
    {
        auto applied = apply_develop_field_strict(params, change.field, change.value);
        if (!applied)
        {
            auto error = applied.error();
            error.context.insert_or_assign("reason", "invalid_ai_proposal_field_bounds");
            error.context.insert_or_assign("field", change.field);
            return error;
        }
    }
    return params;
}

Result<std::vector<AiProposalFieldChange>>
build_stub_ai_proposal_fields(const std::string_view asset_id)
{
    if (asset_id.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "AI proposal requires an asset id",
                          {{"reason", "missing_asset_id"}});
    }
    const double unit = stub_unit(asset_id, 1U);
    const double tone = stub_unit(asset_id, 2U);
    std::vector<AiProposalFieldChange> fields;
    fields.push_back({"exposure", 0.20 + unit * 0.30, 0.92});
    fields.push_back({"contrast", 0.08 + tone * 0.12, 0.88});
    fields.push_back({"highlights", -0.05 - unit * 0.10, 0.80});
    fields.push_back({"shadows", 0.08 + tone * 0.10, 0.80});
    fields.push_back({"saturation", 0.04 + unit * 0.06, 0.75});
    fields.push_back({"vibrance", 0.03 + tone * 0.05, 0.75});
    fields.push_back({"toneEqMidtones", 0.03 + unit * 0.07, 0.70});
    fields.push_back({"colorBalanceContrast", 0.05 + tone * 0.08, 0.70});
    fields.push_back({"whiteBalanceMode", 3.0, 0.65});
    fields.push_back({"whiteBalanceRed", 1.02 + unit * 0.04, 0.65});
    fields.push_back({"whiteBalanceGreen", 1.0, 0.65});
    fields.push_back({"whiteBalanceBlue", 0.96 - tone * 0.03, 0.65});
    fields.push_back({"cropWidth", 0.96, 0.60});
    fields.push_back({"cropHeight", 0.96, 0.60});
    fields.push_back({"cropX", 0.02, 0.60});
    fields.push_back({"cropY", 0.02, 0.60});
    fields.push_back({"straighten", (unit - 0.5) * 1.2, 0.55});
    auto valid = validate_ai_proposal_fields(fields);
    if (!valid)
        return valid.error();
    return fields;
}

Result<std::vector<AiProposalAlternative>>
build_stub_ai_proposal_alternatives(const std::string_view asset_id)
{
    auto primary = build_stub_ai_proposal_fields(asset_id);
    if (!primary)
        return primary.error();
    std::vector<AiProposalFieldChange> punchier = primary.value();
    for (auto &change : punchier)
    {
        if (change.field == "exposure")
            change.value = std::min(1.5, change.value + 0.25);
        else if (change.field == "contrast")
            change.value = std::min(1.0, change.value + 0.10);
        else if (change.field == "saturation")
            change.value = std::min(1.0, change.value + 0.08);
    }
    auto valid = validate_ai_proposal_fields(punchier);
    if (!valid)
        return valid.error();
    AiProposalAlternative alternative;
    alternative.label = "punchier";
    alternative.fields = std::move(punchier);
    alternative.confidence = 0.58;
    return std::vector<AiProposalAlternative>{std::move(alternative)};
}

Result<std::filesystem::path> CatalogService::ai_proposals_directory() const
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto snapshot = this->snapshot();
    if (!snapshot)
        return snapshot.error();
    return std::filesystem::path(snapshot.value().database_path).concat(".ai_proposals");
}

Result<void> CatalogService::persist_ai_proposal(const AiProposal &proposal)
{
    auto directory = ai_proposals_directory();
    if (!directory)
        return directory.error();
    std::error_code ec;
    std::filesystem::create_directories(directory.value(), ec);
    if (ec)
    {
        return make_error(ErrorCode::kIo, "Failed to create AI proposal directory",
                          {{"path", directory.value().string()}, {"reason", ec.message()}});
    }
    auto json = ai_proposal_to_storage_json(proposal);
    if (!json)
        return json.error();
    const auto path = directory.value() / (proposal.id + ".json");
    return write_utf8_text_file_replace_atomically(path.string(), serialize_json(json.value()));
}

Result<void> CatalogService::ensure_ai_proposals_loaded() const
{
    if (ai_proposals_loaded_)
        return {};
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto directory = ai_proposals_directory();
    if (!directory)
        return directory.error();
    std::error_code ec;
    if (!std::filesystem::exists(directory.value(), ec))
    {
        ai_proposals_loaded_ = true;
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
        auto proposal = ai_proposal_from_storage_json(parsed.value());
        if (!proposal)
            return proposal.error();
        ai_proposals_.insert_or_assign(proposal.value().id, proposal.value());
    }
    if (ec)
    {
        return make_error(ErrorCode::kIo, "Failed to read AI proposal directory",
                          {{"path", directory.value().string()}, {"reason", ec.message()}});
    }
    ai_proposals_loaded_ = true;
    return {};
}

Result<AiProposal> CatalogService::create_ai_proposal(const AiProposalCreateRequest &request)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto loaded = ensure_ai_proposals_loaded();
    if (!loaded)
        return loaded.error();
    auto cancelled = request.cancellation.check();
    if (!cancelled)
        return cancelled.error();
    if (!request.user_initiated)
    {
        return make_error(ErrorCode::kValidation, "AI proposal requires explicit user initiation",
                          {{"reason", "ai_proposal_not_user_initiated"}});
    }
    if (request.asset_id.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "AI proposal requires an asset id",
                          {{"reason", "missing_asset_id"}});
    }
    auto provider_ok = require_stub_provider(request.provider_id, request.model_id);
    if (!provider_ok)
        return provider_ok.error();
    std::size_t pending_count = 0;
    for (const auto &[id, existing] : ai_proposals_)
    {
        (void)id;
        if (existing.status == AiProposalStatus::kPending)
            ++pending_count;
    }
    if (pending_count >= kAiProposalSessionLimit)
    {
        return make_error(ErrorCode::kValidation, "AI proposal session limit reached",
                          {{"reason", "ai_proposal_session_limit"},
                           {"limit", std::to_string(kAiProposalSessionLimit)}});
    }

    auto asset = repository_->find_asset_by_id(request.asset_id);
    if (!asset)
        return asset.error();
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", request.asset_id}});
    }
    auto snapshot = this->snapshot();
    if (!snapshot)
        return snapshot.error();
    if (request.expected_catalog_revision &&
        *request.expected_catalog_revision != snapshot.value().revision)
    {
        return make_error(
            ErrorCode::kConflict, "Catalog revision is stale",
            {{"reason", "stale_catalog_revision"},
             {"expected_revision", std::to_string(*request.expected_catalog_revision)},
             {"revision", std::to_string(snapshot.value().revision)}});
    }
    auto recovery = recovery_state(request.asset_id);
    if (!recovery)
        return recovery.error();
    auto recipe = load_recipe(request.asset_id);
    if (!recipe)
        return recipe.error();
    auto current = develop_from_recipe(recipe.value());
    if (!current)
        return current.error();

    cancelled = request.cancellation.check();
    if (!cancelled)
        return cancelled.error();

    auto fields = build_stub_ai_proposal_fields(request.asset_id);
    if (!fields)
        return fields.error();
    auto alternatives = build_stub_ai_proposal_alternatives(request.asset_id);
    if (!alternatives)
        return alternatives.error();
    auto proposed = apply_ai_proposal_fields(current.value(), fields.value());
    if (!proposed)
        return proposed.error();

    AiProposal proposal;
    proposal.id = generate_ai_proposal_id();
    proposal.contract_version = std::string(kAiProposalContractVersion);
    proposal.created_unix_ms = now_unix_ms();
    proposal.asset_id = request.asset_id;
    proposal.observed_catalog_revision = snapshot.value().revision;
    proposal.observed_recovery_generation = recovery.value().generation;
    proposal.provider.provider_id = std::string(kAiStubProviderId);
    proposal.provider.model_id = std::string(kAiStubModelId);
    proposal.provider.model_version = std::string(kAiStubModelVersion);
    proposal.provider.weight_content_hash = std::string(kAiStubWeightContentHash);
    proposal.provider.parameters = {
        {"kind", "deterministic_stub"}, {"network", "never"}, {"training", "never"}};
    proposal.fields = std::move(fields).value();
    proposal.field_diff = develop_change_summary(current.value(), proposed.value());
    proposal.alternatives = std::move(alternatives).value();
    proposal.status = AiProposalStatus::kPending;
    ai_proposals_.insert_or_assign(proposal.id, proposal);
    auto persisted = persist_ai_proposal(proposal);
    if (!persisted)
        return persisted.error();
    return proposal;
}

Result<AiProposal> CatalogService::get_ai_proposal(const std::string_view proposal_id) const
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto loaded = ensure_ai_proposals_loaded();
    if (!loaded)
        return loaded.error();
    if (proposal_id.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "AI proposal id is required",
                          {{"reason", "missing_proposal_id"}});
    }
    const auto found = ai_proposals_.find(std::string(proposal_id));
    if (found == ai_proposals_.end())
    {
        return make_error(ErrorCode::kNotFound, "AI proposal does not exist",
                          {{"proposal_id", std::string(proposal_id)}});
    }
    return found->second;
}

Result<std::vector<AiProposal>>
CatalogService::list_ai_proposals(const std::optional<std::string_view> asset_id) const
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto loaded = ensure_ai_proposals_loaded();
    if (!loaded)
        return loaded.error();
    std::vector<AiProposal> listed;
    listed.reserve(ai_proposals_.size());
    for (const auto &[id, proposal] : ai_proposals_)
    {
        (void)id;
        if (asset_id && proposal.asset_id != *asset_id)
            continue;
        listed.push_back(proposal);
    }
    std::sort(listed.begin(), listed.end(),
              [](const AiProposal &lhs, const AiProposal &rhs)
              {
                  if (lhs.created_unix_ms != rhs.created_unix_ms)
                      return lhs.created_unix_ms < rhs.created_unix_ms;
                  return lhs.id < rhs.id;
              });
    return listed;
}

Result<AiProposal> CatalogService::reject_ai_proposal(const std::string_view proposal_id)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto loaded = ensure_ai_proposals_loaded();
    if (!loaded)
        return loaded.error();
    auto found = ai_proposals_.find(std::string(proposal_id));
    if (found == ai_proposals_.end())
    {
        return make_error(ErrorCode::kNotFound, "AI proposal does not exist",
                          {{"proposal_id", std::string(proposal_id)}});
    }
    if (found->second.status != AiProposalStatus::kPending)
    {
        return make_error(ErrorCode::kConflict, "AI proposal is not pending",
                          {{"proposal_id", std::string(proposal_id)},
                           {"status", std::string(ai_proposal_status_name(found->second.status))},
                           {"reason", "ai_proposal_not_pending"}});
    }
    found->second.status = AiProposalStatus::kRejected;
    auto persisted = persist_ai_proposal(found->second);
    if (!persisted)
        return persisted.error();
    return found->second;
}

Result<AiProposal> CatalogService::cancel_ai_proposal(const std::string_view proposal_id)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto loaded = ensure_ai_proposals_loaded();
    if (!loaded)
        return loaded.error();
    auto found = ai_proposals_.find(std::string(proposal_id));
    if (found == ai_proposals_.end())
    {
        return make_error(ErrorCode::kNotFound, "AI proposal does not exist",
                          {{"proposal_id", std::string(proposal_id)}});
    }
    if (found->second.status != AiProposalStatus::kPending)
    {
        return make_error(ErrorCode::kConflict, "AI proposal is not pending",
                          {{"proposal_id", std::string(proposal_id)},
                           {"status", std::string(ai_proposal_status_name(found->second.status))},
                           {"reason", "ai_proposal_not_pending"}});
    }
    found->second.status = AiProposalStatus::kCancelled;
    auto persisted = persist_ai_proposal(found->second);
    if (!persisted)
        return persisted.error();
    return found->second;
}

Result<AiProposalApplyResult>
CatalogService::apply_ai_proposal(const std::string_view proposal_id,
                                  const std::optional<std::int64_t> expected_catalog_revision)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto loaded = ensure_ai_proposals_loaded();
    if (!loaded)
        return loaded.error();
    auto found = ai_proposals_.find(std::string(proposal_id));
    if (found == ai_proposals_.end())
    {
        return make_error(ErrorCode::kNotFound, "AI proposal does not exist",
                          {{"proposal_id", std::string(proposal_id)}});
    }
    AiProposal &proposal = found->second;
    if (proposal.status != AiProposalStatus::kPending)
    {
        return make_error(ErrorCode::kConflict, "AI proposal is not pending",
                          {{"proposal_id", std::string(proposal_id)},
                           {"status", std::string(ai_proposal_status_name(proposal.status))},
                           {"reason", "ai_proposal_not_pending"}});
    }
    auto provider_ok =
        require_stub_provider(proposal.provider.provider_id, proposal.provider.model_id);
    if (!provider_ok)
        return provider_ok.error();
    if (proposal.provider.model_id.empty() || proposal.provider.weight_content_hash.empty())
    {
        return make_error(ErrorCode::kValidation, "AI proposal is missing provider or model",
                          {{"proposal_id", std::string(proposal_id)},
                           {"reason", "missing_ai_provider_or_model"}});
    }

    auto snapshot = this->snapshot();
    if (!snapshot)
        return snapshot.error();
    const auto expected = expected_catalog_revision.value_or(proposal.observed_catalog_revision);
    if (expected != snapshot.value().revision)
    {
        return make_error(ErrorCode::kConflict, "Catalog revision is stale",
                          {{"reason", "stale_catalog_revision"},
                           {"expected_revision", std::to_string(expected)},
                           {"revision", std::to_string(snapshot.value().revision)},
                           {"proposal_id", std::string(proposal_id)}});
    }
    auto recovery = recovery_state(proposal.asset_id);
    if (!recovery)
        return recovery.error();
    if (recovery.value().generation != proposal.observed_recovery_generation)
    {
        return make_error(
            ErrorCode::kConflict, "Asset recovery generation is stale",
            {{"reason", "stale_recovery_generation"},
             {"expected_generation", std::to_string(proposal.observed_recovery_generation)},
             {"generation", std::to_string(recovery.value().generation)},
             {"proposal_id", std::string(proposal_id)},
             {"asset_id", proposal.asset_id}});
    }

    auto recipe = load_recipe(proposal.asset_id);
    if (!recipe)
        return recipe.error();
    auto current = develop_from_recipe(recipe.value());
    if (!current)
        return current.error();
    auto proposed = apply_ai_proposal_fields(current.value(), proposal.fields);
    if (!proposed)
        return proposed.error();

    auto saved = save_develop_with_history(proposal.asset_id, proposed.value());
    if (!saved)
        return saved.error();

    proposal.status = AiProposalStatus::kApplied;
    proposal.applied_history_id = saved.value().history_id;
    proposal.field_diff = develop_change_summary(current.value(), proposed.value());
    auto persisted = persist_ai_proposal(proposal);
    if (!persisted)
        return persisted.error();

    AiProposalApplyResult result;
    result.proposal = proposal;
    result.asset = saved.value().asset;
    result.revision = saved.value().revision;
    result.history_id = saved.value().history_id;
    return result;
}

} // namespace ravo
