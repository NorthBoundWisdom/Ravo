#include "ravo/services/catalog_service.h"

#include "catalog_internal.h"

#include <algorithm>
#include <limits>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ravo/adapters/text_file.h"
#include "ravo/domain/types.h"
#include "ravo/domain/uri.h"
#include "ravo/services/cull_assistance.h"

namespace ravo
{
namespace
{

[[nodiscard]] bool file_is_regular(const std::string_view path) noexcept
{
    std::error_code error;
    return std::filesystem::is_regular_file(utf8_path(path), error) && !error;
}

[[nodiscard]] std::string camera_key_for(const CaptureMetadata &capture)
{
    const auto make = capture.camera_make.value_or(std::string{});
    const auto model = capture.camera_model.value_or(std::string{});
    if (make.empty() && model.empty())
        return "unknown";
    return make + "\n" + model;
}

[[nodiscard]] std::string make_burst_proposal_id(const std::vector<std::string> &sorted_ids)
{
    std::string joined;
    for (const auto &id : sorted_ids)
    {
        if (!joined.empty())
            joined.push_back('|');
        joined += id;
    }
    return "burst_" + sha256_utf8_hex(joined).substr(0, 24);
}

} // namespace

Result<ExactDuplicateReport>
CatalogService::find_exact_duplicate_groups(const ExactDuplicateRequest &request) const
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");

    auto cancelled = request.cancellation.check();
    if (!cancelled)
        return cancelled.error();

    auto assets = repository_->list_assets();
    if (!assets)
        return assets.error();

    ExactDuplicateReport report;
    report.assets_considered = assets.value().size();

    struct HashBucket
    {
        std::string sha256;
        std::vector<ExactDuplicateMember> members;
    };
    std::map<std::string, HashBucket> by_hash;

    for (const auto &asset : assets.value())
    {
        cancelled = request.cancellation.check();
        if (!cancelled)
            return cancelled.error();

        auto location = normalize_local_input(asset.normalized_uri);
        if (!location)
        {
            ExactDuplicateSkip skip;
            skip.asset_id = asset.id;
            skip.reason = "invalid_uri";
            report.skipped.push_back(std::move(skip));
            continue;
        }
        if (!file_is_regular(location.value().path))
        {
            ExactDuplicateSkip skip;
            skip.asset_id = asset.id;
            skip.reason = "original_missing";
            skip.path = location.value().path;
            report.skipped.push_back(std::move(skip));
            continue;
        }
        auto digest = sha256_file_hex(location.value().path);
        if (!digest)
        {
            ExactDuplicateSkip skip;
            skip.asset_id = asset.id;
            skip.reason = "hash_failed";
            skip.path = location.value().path;
            report.skipped.push_back(std::move(skip));
            continue;
        }

        ExactDuplicateMember member;
        member.asset_id = asset.id;
        member.normalized_uri = asset.normalized_uri;
        member.version_ordinal = asset.version_ordinal;
        member.source_asset_id = asset.source_asset_id;
        member.size_bytes = asset.size_bytes;
        auto &bucket = by_hash[digest.value()];
        bucket.sha256 = digest.value();
        bucket.members.push_back(std::move(member));
    }

    for (auto &[sha, bucket] : by_hash)
    {
        (void)sha;
        if (bucket.members.size() < 2U)
            continue;

        // Partition by normalized_uri: virtual copies => same_file; distinct
        // URIs with the same bytes => same_bytes (one representative per URI).
        std::map<std::string, std::vector<ExactDuplicateMember>> by_uri;
        for (auto &member : bucket.members)
            by_uri[member.normalized_uri].push_back(std::move(member));

        std::vector<ExactDuplicateMember> same_bytes_reps;
        same_bytes_reps.reserve(by_uri.size());
        for (auto &[uri, uri_members] : by_uri)
        {
            (void)uri;
            std::sort(uri_members.begin(), uri_members.end(),
                      [](const ExactDuplicateMember &a, const ExactDuplicateMember &b)
                      {
                          if (a.version_ordinal != b.version_ordinal)
                              return a.version_ordinal < b.version_ordinal;
                          return a.asset_id < b.asset_id;
                      });
            if (uri_members.size() >= 2U)
            {
                ExactDuplicateGroup group;
                group.sha256 = bucket.sha256;
                group.outcome = ExactDuplicateOutcome::kSameFile;
                group.members = uri_members;
                report.groups.push_back(std::move(group));
            }
            same_bytes_reps.push_back(uri_members.front());
        }
        if (same_bytes_reps.size() >= 2U)
        {
            ExactDuplicateGroup group;
            group.sha256 = bucket.sha256;
            group.outcome = ExactDuplicateOutcome::kSameBytes;
            group.members = std::move(same_bytes_reps);
            std::sort(group.members.begin(), group.members.end(),
                      [](const ExactDuplicateMember &a, const ExactDuplicateMember &b)
                      { return a.asset_id < b.asset_id; });
            report.groups.push_back(std::move(group));
        }
    }

    std::sort(report.groups.begin(), report.groups.end(),
              [](const ExactDuplicateGroup &a, const ExactDuplicateGroup &b)
              {
                  if (a.sha256 != b.sha256)
                      return a.sha256 < b.sha256;
                  return static_cast<int>(a.outcome) < static_cast<int>(b.outcome);
              });
    return report;
}

Result<BurstProposeReport>
CatalogService::propose_burst_groups(const BurstProposeRequest &request) const
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");

    auto cancelled = request.cancellation.check();
    if (!cancelled)
        return cancelled.error();
    if (request.window_seconds <= 0)
    {
        return make_error(ErrorCode::kInvalidArgument, "Burst window must be positive",
                          {{"window_seconds", std::to_string(request.window_seconds)},
                           {"reason", "invalid_burst_window"}});
    }

    auto assets = repository_->list_assets();
    if (!assets)
        return assets.error();

    BurstProposeReport report;
    report.window_seconds = request.window_seconds;
    report.assets_considered = assets.value().size();

    struct TimedAsset
    {
        AssetRecord asset;
        std::int64_t captured = 0;
        std::string camera_key;
    };
    std::vector<TimedAsset> timed;
    timed.reserve(assets.value().size());
    for (auto &asset : assets.value())
    {
        if (!asset.capture.captured_unix_s)
        {
            ++report.assets_skipped_missing_capture;
            continue;
        }
        TimedAsset row;
        row.captured = *asset.capture.captured_unix_s;
        row.camera_key = camera_key_for(asset.capture);
        row.asset = std::move(asset);
        timed.push_back(std::move(row));
    }

    std::sort(timed.begin(), timed.end(),
              [](const TimedAsset &a, const TimedAsset &b)
              {
                  if (a.camera_key != b.camera_key)
                      return a.camera_key < b.camera_key;
                  if (a.captured != b.captured)
                      return a.captured < b.captured;
                  return a.asset.id < b.asset.id;
              });

    std::size_t index = 0;
    while (index < timed.size())
    {
        cancelled = request.cancellation.check();
        if (!cancelled)
            return cancelled.error();

        const auto key = timed[index].camera_key;
        std::size_t end = index + 1;
        while (end < timed.size() && timed[end].camera_key == key)
            ++end;

        std::size_t run_start = index;
        while (run_start < end)
        {
            std::size_t cluster_end = run_start + 1;
            while (cluster_end < end &&
                   timed[cluster_end].captured - timed[cluster_end - 1].captured <=
                       request.window_seconds)
                ++cluster_end;
            if (cluster_end - run_start >= 2U)
            {
                BurstGroupProposal proposal;
                proposal.camera_key = key;
                proposal.window_start_unix_s = timed[run_start].captured;
                proposal.window_end_unix_s = timed[cluster_end - 1].captured;
                std::vector<std::string> ids;
                for (std::size_t member_index = run_start; member_index < cluster_end;
                     ++member_index)
                {
                    BurstGroupMember member;
                    member.asset_id = timed[member_index].asset.id;
                    member.captured_unix_s = timed[member_index].captured;
                    member.camera_make = timed[member_index].asset.capture.camera_make;
                    member.camera_model = timed[member_index].asset.capture.camera_model;
                    ids.push_back(member.asset_id);
                    proposal.members.push_back(std::move(member));
                }
                std::sort(ids.begin(), ids.end());
                proposal.proposal_id = make_burst_proposal_id(ids);
                report.proposals.push_back(std::move(proposal));
            }
            run_start = cluster_end;
        }
        index = end;
    }

    return report;
}

Result<BurstAcceptResult>
CatalogService::accept_burst_group_proposal(const BurstAcceptRequest &request)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");

    auto cancelled = request.cancellation.check();
    if (!cancelled)
        return cancelled.error();
    if (!request.user_initiated)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Burst accept requires explicit user initiation",
                          {{"reason", "missing_user_initiated"}});
    }
    if (request.asset_ids.size() < 2U)
    {
        return make_error(ErrorCode::kInvalidArgument, "Burst accept requires at least two assets",
                          {{"reason", "burst_accept_too_few_members"}});
    }

    std::vector<std::string> unique_ids = request.asset_ids;
    std::sort(unique_ids.begin(), unique_ids.end());
    unique_ids.erase(std::unique(unique_ids.begin(), unique_ids.end()), unique_ids.end());
    if (unique_ids.size() != request.asset_ids.size())
    {
        return make_error(ErrorCode::kInvalidArgument, "Burst accept asset ids must be unique",
                          {{"reason", "duplicate_burst_member"}});
    }

    std::string pick = request.pick_asset_id.value_or(std::string{});
    struct Candidate
    {
        std::string id;
        std::optional<std::int64_t> captured;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(unique_ids.size());
    for (const auto &id : unique_ids)
    {
        auto asset = repository_->find_asset_by_id(id);
        if (!asset)
            return asset.error();
        if (!asset.value())
        {
            return make_error(ErrorCode::kNotFound, "Burst member asset was not found",
                              {{"asset_id", id}});
        }
        candidates.push_back(Candidate{id, asset.value()->capture.captured_unix_s});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b)
              {
                  const auto a_time = a.captured.value_or(std::numeric_limits<std::int64_t>::max());
                  const auto b_time = b.captured.value_or(std::numeric_limits<std::int64_t>::max());
                  if (a_time != b_time)
                      return a_time < b_time;
                  return a.id < b.id;
              });
    if (pick.empty())
        pick = candidates.front().id;
    else if (std::find(unique_ids.begin(), unique_ids.end(), pick) == unique_ids.end())
    {
        return make_error(ErrorCode::kInvalidArgument, "Burst pick must be a member asset",
                          {{"pick_asset_id", pick}, {"reason", "burst_pick_not_member"}});
    }

    auto stacked = stack_assets(unique_ids, pick, request.expected_catalog_revision);
    if (!stacked)
        return stacked.error();

    BurstAcceptResult result;
    result.stack = std::move(stacked).value();
    result.catalog_mutated = true;
    return result;
}

} // namespace ravo
