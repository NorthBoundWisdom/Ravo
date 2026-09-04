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
        {"group_kind", group.group_kind},
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
                {"group_kind", group.group_kind},
                {"non_authoritative", group.non_authoritative},
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
            {"max_assets", JsonValue::number(std::to_string(report.value().max_assets))},
            {"non_authoritative", report.value().non_authoritative},
            {"group_kind", std::string(kCullGroupKindHeuristicAHash)},
            {"cache_used", report.value().cache_used},
            {"throttled", report.value().throttled},
            {"assets_considered",
             JsonValue::number(std::to_string(report.value().assets_considered))},
            {"assets_fingerprinted",
             JsonValue::number(std::to_string(report.value().assets_fingerprinted))},
            {"assets_from_cache",
             JsonValue::number(std::to_string(report.value().assets_from_cache))},
            {"assets_computed", JsonValue::number(std::to_string(report.value().assets_computed))},
            {"cache_entries", JsonValue::number(std::to_string(report.value().cache_entries))},
            {"dismissed_groups_omitted",
             JsonValue::number(std::to_string(report.value().dismissed_groups_omitted))},
            {"groups", JsonValue{std::move(groups)}},
            {"skipped", JsonValue{std::move(skipped)}},
        }};
    }

    if (subcommand == "cull-review")
    {
        if (flags.asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog cull-review requires --asset-id");
        }
        const int flag_count = static_cast<int>(flags.cull_pick) +
                               static_cast<int>(flags.cull_reject_flag) +
                               static_cast<int>(flags.cull_unflag);
        if (flag_count > 1)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog cull-review accepts only one of --pick, --reject, --unflag",
                              {{"reason", "cull_review_flag_conflict"}});
        }
        CullReviewRequest request;
        request.asset_id = std::string(flags.asset_id);
        if (flags.cull_pick)
            request.flag_action = CullReviewFlagAction::kPick;
        else if (flags.cull_reject_flag)
            request.flag_action = CullReviewFlagAction::kReject;
        else if (flags.cull_unflag)
            request.flag_action = CullReviewFlagAction::kUnflag;
        if (flags.rating)
            request.rating = *flags.rating;
        if (!flags.color_label.empty())
        {
            auto label = parse_color_label(flags.color_label);
            if (!label)
                return label.error();
            request.color_label = label.value();
        }
        request.auto_advance = flags.auto_advance;
        request.selection_asset_ids.reserve(flags.selection_asset_ids.size());
        for (const auto id : flags.selection_asset_ids)
            request.selection_asset_ids.emplace_back(id);
        if (!flags.query_json.empty())
        {
            auto parsed = parse_library_query_document(flags.query_json);
            if (!parsed)
                return parsed.error();
            request.query = std::move(parsed).value();
        }
        request.expected_catalog_revision = flags.expected_revision;
        auto applied = service.apply_cull_review(request);
        if (!applied)
            return applied.error();
        JsonValue::Object previous{
            {"color_label",
             std::string(color_label_name(applied.value().previous_review.color_label))},
            {"picked", applied.value().previous_review.picked},
            {"rating", JsonValue::number(std::to_string(applied.value().previous_review.rating))},
            {"rejected", applied.value().previous_review.rejected},
        };
        JsonValue::Object review{
            {"color_label", std::string(color_label_name(applied.value().review.color_label))},
            {"picked", applied.value().review.picked},
            {"rating", JsonValue::number(std::to_string(applied.value().review.rating))},
            {"rejected", applied.value().review.rejected},
        };
        JsonValue::Object object{
            {"asset", asset_to_json(applied.value().asset)},
            {"catalog_mutated", applied.value().catalog_mutated},
            {"flag_action", std::string(cull_review_flag_action_name(request.flag_action))},
            {"previous_review", std::move(previous)},
            {"review", std::move(review)},
            {"revision", JsonValue::number(std::to_string(applied.value().revision))},
            {"schema", applied.value().schema},
            {"schema_version", JsonValue::number(std::to_string(applied.value().schema_version))},
        };
        if (applied.value().next_asset_id)
            object.emplace("next_asset_id", *applied.value().next_asset_id);
        else
            object.emplace("next_asset_id", nullptr);
        return JsonValue{std::move(object)};
    }

    if (subcommand == "cull-burst-compare")
    {
        if (flags.asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog cull-burst-compare requires --asset-id");
        }
        BurstCompareRequest request;
        request.asset_id = std::string(flags.asset_id);
        if (flags.compare_step)
        {
            auto step = burst_compare_step_from_name(*flags.compare_step);
            if (!step)
            {
                return make_error(
                    ErrorCode::kInvalidArgument, "--step must be current, previous, or next",
                    {{"value", *flags.compare_step}, {"reason", "invalid_burst_compare_step"}});
            }
            request.step = *step;
        }
        auto pair = service.resolve_burst_compare_pair(request);
        if (!pair)
            return pair.error();
        JsonValue::Array members;
        members.reserve(pair.value().member_ids.size());
        for (const auto &id : pair.value().member_ids)
            members.push_back(JsonValue{id});
        return JsonValue{JsonValue::Object{
            {"schema", pair.value().schema},
            {"schema_version", JsonValue::number(std::to_string(pair.value().schema_version))},
            {"stack_id", pair.value().stack_id},
            {"member_ids", JsonValue{std::move(members)}},
            {"focus_index", JsonValue::number(std::to_string(pair.value().focus_index))},
            {"focus_asset_id", pair.value().focus_asset_id},
            {"compare_asset_id", pair.value().compare_asset_id},
            {"step", std::string(burst_compare_step_name(pair.value().step))},
        }};
    }

    return make_error(ErrorCode::kInvalidArgument, "Unknown catalog cull subcommand",
                      {{"subcommand", std::string(subcommand)}});
}

} // namespace ravo::cli_internal
