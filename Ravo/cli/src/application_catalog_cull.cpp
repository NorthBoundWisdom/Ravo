#include "application_internal.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ravo/foundation/json.h"
#include "ravo/services/catalog_service.h"
#include "ravo/services/cull_assistance.h"

namespace ravo::cli_internal
{
namespace
{

[[nodiscard]] JsonValue exact_member_json(const ExactDuplicateMember &member)
{
    JsonValue::Object object{
        {"asset_id", member.asset_id},
        {"normalized_uri", member.normalized_uri},
        {"version_ordinal", JsonValue::number(std::to_string(member.version_ordinal))},
        {"size_bytes", JsonValue::number(std::to_string(member.size_bytes))},
    };
    if (member.source_asset_id)
        object.emplace("source_asset_id", *member.source_asset_id);
    return JsonValue{std::move(object)};
}

[[nodiscard]] JsonValue exact_group_json(const ExactDuplicateGroup &group)
{
    JsonValue::Array members;
    members.reserve(group.members.size());
    for (const auto &member : group.members)
        members.push_back(exact_member_json(member));
    return JsonValue{JsonValue::Object{
        {"schema", group.schema},
        {"schema_version", JsonValue::number(std::to_string(group.schema_version))},
        {"sha256", group.sha256},
        {"outcome", std::string(exact_duplicate_outcome_name(group.outcome))},
        {"members", JsonValue{std::move(members)}},
    }};
}

[[nodiscard]] JsonValue burst_member_json(const BurstGroupMember &member)
{
    JsonValue::Object object{{"asset_id", member.asset_id}};
    if (member.captured_unix_s)
        object.emplace("captured_unix_s",
                       JsonValue::number(std::to_string(*member.captured_unix_s)));
    if (member.camera_make)
        object.emplace("camera_make", *member.camera_make);
    if (member.camera_model)
        object.emplace("camera_model", *member.camera_model);
    return JsonValue{std::move(object)};
}

[[nodiscard]] JsonValue burst_proposal_json(const BurstGroupProposal &proposal)
{
    JsonValue::Array members;
    members.reserve(proposal.members.size());
    for (const auto &member : proposal.members)
        members.push_back(burst_member_json(member));
    return JsonValue{JsonValue::Object{
        {"schema", proposal.schema},
        {"schema_version", JsonValue::number(std::to_string(proposal.schema_version))},
        {"proposal_id", proposal.proposal_id},
        {"camera_key", proposal.camera_key},
        {"window_start_unix_s", JsonValue::number(std::to_string(proposal.window_start_unix_s))},
        {"window_end_unix_s", JsonValue::number(std::to_string(proposal.window_end_unix_s))},
        {"members", JsonValue{std::move(members)}},
    }};
}

} // namespace

Result<JsonValue> run_catalog_cull_command(CatalogService &service,
                                           const std::string_view subcommand,
                                           const CatalogCliArguments &flags)
{
    if (subcommand == "cull-exact-duplicates")
    {
        ExactDuplicateRequest request;
        auto report = service.find_exact_duplicate_groups(request);
        if (!report)
            return report.error();
        JsonValue::Array groups;
        groups.reserve(report.value().groups.size());
        for (const auto &group : report.value().groups)
            groups.push_back(exact_group_json(group));
        JsonValue::Array skipped;
        skipped.reserve(report.value().skipped.size());
        for (const auto &item : report.value().skipped)
        {
            JsonValue::Object object{{"asset_id", item.asset_id}, {"reason", item.reason}};
            if (item.path)
                object.emplace("path", *item.path);
            skipped.push_back(JsonValue{std::move(object)});
        }
        return JsonValue{JsonValue::Object{
            {"schema", report.value().schema},
            {"schema_version", JsonValue::number(std::to_string(report.value().schema_version))},
            {"assets_considered",
             JsonValue::number(std::to_string(report.value().assets_considered))},
            {"groups", JsonValue{std::move(groups)}},
            {"skipped", JsonValue{std::move(skipped)}},
        }};
    }
    if (subcommand == "cull-burst-propose")
    {
        BurstProposeRequest request;
        if (flags.burst_window_seconds)
            request.window_seconds = *flags.burst_window_seconds;
        auto report = service.propose_burst_groups(request);
        if (!report)
            return report.error();
        JsonValue::Array proposals;
        proposals.reserve(report.value().proposals.size());
        for (const auto &proposal : report.value().proposals)
            proposals.push_back(burst_proposal_json(proposal));
        return JsonValue{JsonValue::Object{
            {"schema", report.value().schema},
            {"schema_version", JsonValue::number(std::to_string(report.value().schema_version))},
            {"window_seconds", JsonValue::number(std::to_string(report.value().window_seconds))},
            {"assets_considered",
             JsonValue::number(std::to_string(report.value().assets_considered))},
            {"assets_skipped_missing_capture",
             JsonValue::number(std::to_string(report.value().assets_skipped_missing_capture))},
            {"proposals", JsonValue{std::move(proposals)}},
        }};
    }
    if (subcommand == "cull-burst-accept")
    {
        if (!flags.user_initiated)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog cull-burst-accept requires --user-initiated",
                              {{"reason", "missing_user_initiated"}});
        }
        if (flags.asset_ids.empty() && flags.asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog cull-burst-accept requires --asset-id members");
        }
        BurstAcceptRequest request;
        request.user_initiated = true;
        request.expected_catalog_revision = flags.expected_revision;
        if (!flags.asset_id.empty())
            request.asset_ids.push_back(std::string(flags.asset_id));
        for (const auto &id : flags.asset_ids)
            request.asset_ids.push_back(std::string(id));
        if (!flags.pick_id.empty())
            request.pick_asset_id = std::string(flags.pick_id);
        auto accepted = service.accept_burst_group_proposal(request);
        if (!accepted)
            return accepted.error();
        return JsonValue{JsonValue::Object{
            {"catalog_mutated", accepted.value().catalog_mutated},
            {"stack", library_stack_mutation_to_json(accepted.value().stack)},
        }};
    }
    if (subcommand == "cull-near-duplicates")
    {
        NearDuplicateRequest request;
        if (flags.near_dup_max_hamming)
            request.max_hamming = *flags.near_dup_max_hamming;
        auto report = service.find_near_duplicate_groups(request);
        if (!report)
            return report.error();
        JsonValue::Array groups;
        groups.reserve(report.value().groups.size());
        for (const auto &group : report.value().groups)
        {
            JsonValue::Array members;
            members.reserve(group.members.size());
            for (const auto &member : group.members)
            {
                JsonValue::Object object{
                    {"asset_id", member.asset_id},
                    {"normalized_uri", member.normalized_uri},
                    {"fingerprint_hex", member.fingerprint_hex},
                    {"version_ordinal", JsonValue::number(std::to_string(member.version_ordinal))},
                };
                if (member.source_asset_id)
                    object.emplace("source_asset_id", *member.source_asset_id);
                members.push_back(JsonValue{std::move(object)});
            }
            groups.push_back(JsonValue{JsonValue::Object{
                {"schema", group.schema},
                {"schema_version", JsonValue::number(std::to_string(group.schema_version))},
                {"fingerprint_hex", group.fingerprint_hex},
                {"max_hamming_in_group",
                 JsonValue::number(std::to_string(group.max_hamming_in_group))},
                {"members", JsonValue{std::move(members)}},
            }});
        }
        JsonValue::Array skipped;
        for (const auto &item : report.value().skipped)
        {
            JsonValue::Object object{{"asset_id", item.asset_id}, {"reason", item.reason}};
            if (item.path)
                object.emplace("path", *item.path);
            skipped.push_back(JsonValue{std::move(object)});
        }
        return JsonValue{JsonValue::Object{
            {"schema", report.value().schema},
            {"schema_version", JsonValue::number(std::to_string(report.value().schema_version))},
            {"max_hamming", JsonValue::number(std::to_string(report.value().max_hamming))},
            {"max_groups", JsonValue::number(std::to_string(report.value().max_groups))},
            {"assets_considered",
             JsonValue::number(std::to_string(report.value().assets_considered))},
            {"assets_fingerprinted",
             JsonValue::number(std::to_string(report.value().assets_fingerprinted))},
            {"groups", JsonValue{std::move(groups)}},
            {"skipped", JsonValue{std::move(skipped)}},
        }};
    }
    return make_error(ErrorCode::kInvalidArgument, "Unknown catalog cull subcommand",
                      {{"subcommand", std::string(subcommand)}});
}

} // namespace ravo::cli_internal
