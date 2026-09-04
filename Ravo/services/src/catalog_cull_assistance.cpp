#include "ravo/services/catalog_service.h"

#include "catalog_internal.h"

#include <algorithm>
#include <array>
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

Result<NearDuplicateReport>
CatalogService::find_near_duplicate_groups(const NearDuplicateRequest &request) const
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");

    auto cancelled = request.cancellation.check();
    if (!cancelled)
        return cancelled.error();
    if (request.max_hamming < 0 || request.max_hamming > 64)
    {
        return make_error(ErrorCode::kInvalidArgument, "near-dup max Hamming must be 0..64",
                          {{"max_hamming", std::to_string(request.max_hamming)},
                           {"reason", "invalid_near_dup_max_hamming"}});
    }
    if (request.max_groups == 0U)
    {
        return make_error(ErrorCode::kInvalidArgument, "near-dup max_groups must be positive",
                          {{"reason", "invalid_near_dup_max_groups"}});
    }

    auto assets = repository_->list_assets();
    if (!assets)
        return assets.error();

    NearDuplicateReport report;
    report.max_hamming = request.max_hamming;
    report.max_groups = request.max_groups;
    report.assets_considered = assets.value().size();

    struct Fingerprinted
    {
        NearDuplicateMember member;
        std::uint64_t hash = 0;
    };
    std::vector<Fingerprinted> fingerprinted;
    fingerprinted.reserve(assets.value().size());

    const auto popcount64 = [](std::uint64_t value) noexcept -> int
    {
        int count = 0;
        while (value != 0U)
        {
            count += static_cast<int>(value & 1U);
            value >>= 1U;
        }
        return count;
    };
    const auto hamming = [&](std::uint64_t left, std::uint64_t right) noexcept -> int
    { return popcount64(left ^ right); };
    const auto hash_to_hex = [](std::uint64_t value) -> std::string
    {
        static constexpr char kHex[] = "0123456789abcdef";
        std::string out(16, '0');
        for (int i = 15; i >= 0; --i)
        {
            out[static_cast<std::size_t>(i)] = kHex[value & 0xFU];
            value >>= 4U;
        }
        return out;
    };
    const auto average_hash_raster = [&](const RasterBuffer &raster) -> Result<std::uint64_t>
    {
        if (raster.width == 0U || raster.height == 0U ||
            raster.srgb.size() < static_cast<std::size_t>(raster.width) * raster.height * 3U)
        {
            return make_error(ErrorCode::kUnsupported, "Raster is empty for near-duplicate hash",
                              {{"reason", "near_dup_empty_raster"}});
        }
        // Box-sample to 8x8 grayscale using integer means.
        std::array<std::uint64_t, 64> sums{};
        std::array<std::uint32_t, 64> counts{};
        for (std::uint32_t y = 0; y < raster.height; ++y)
        {
            const auto cell_y = static_cast<std::size_t>((y * 8U) / raster.height);
            for (std::uint32_t x = 0; x < raster.width; ++x)
            {
                const auto cell_x = static_cast<std::size_t>((x * 8U) / raster.width);
                const auto index = (static_cast<std::size_t>(y) * raster.width + x) * 3U;
                const auto r = raster.srgb[index];
                const auto g = raster.srgb[index + 1U];
                const auto b = raster.srgb[index + 2U];
                // Rec.709-ish integer luma.
                const auto luma =
                    (static_cast<std::uint32_t>(r) * 54U + static_cast<std::uint32_t>(g) * 183U +
                     static_cast<std::uint32_t>(b) * 19U) /
                    256U;
                const auto cell = cell_y * 8U + cell_x;
                sums[cell] += luma;
                ++counts[cell];
            }
        }
        std::array<std::uint8_t, 64> pixels{};
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < 64U; ++i)
        {
            const auto value =
                counts[i] == 0U ? 0U : static_cast<std::uint8_t>(sums[i] / counts[i]);
            pixels[i] = value;
            total += value;
        }
        const auto mean = static_cast<std::uint8_t>(total / 64U);
        std::uint64_t hash = 0;
        for (const auto pixel : pixels)
        {
            hash <<= 1U;
            if (pixel >= mean)
                hash |= 1U;
        }
        return hash;
    };

    for (const auto &asset : assets.value())
    {
        cancelled = request.cancellation.check();
        if (!cancelled)
            return cancelled.error();

        auto location = normalize_local_input(asset.normalized_uri);
        if (!location)
        {
            NearDuplicateSkip skip;
            skip.asset_id = asset.id;
            skip.reason = "invalid_uri";
            report.skipped.push_back(std::move(skip));
            continue;
        }
        if (!file_is_regular(location.value().path))
        {
            NearDuplicateSkip skip;
            skip.asset_id = asset.id;
            skip.reason = "original_missing";
            skip.path = location.value().path;
            report.skipped.push_back(std::move(skip));
            continue;
        }

        auto raster =
            decode_import_candidate_thumbnail(location.value().path, request.cancellation);
        if (!raster)
        {
            NearDuplicateSkip skip;
            skip.asset_id = asset.id;
            skip.reason = "near_dup_decode_failed";
            skip.path = location.value().path;
            report.skipped.push_back(std::move(skip));
            continue;
        }
        auto hash = average_hash_raster(raster.value());
        if (!hash)
        {
            NearDuplicateSkip skip;
            skip.asset_id = asset.id;
            skip.reason = hash.error().context.count("reason") ? hash.error().context.at("reason") :
                                                                 "near_dup_hash_failed";
            skip.path = location.value().path;
            report.skipped.push_back(std::move(skip));
            continue;
        }

        NearDuplicateMember member;
        member.asset_id = asset.id;
        member.normalized_uri = asset.normalized_uri;
        member.fingerprint_hex = hash_to_hex(hash.value());
        member.version_ordinal = asset.version_ordinal;
        member.source_asset_id = asset.source_asset_id;
        fingerprinted.push_back(Fingerprinted{std::move(member), hash.value()});
    }
    report.assets_fingerprinted = fingerprinted.size();
    if (fingerprinted.size() < 2U)
        return report;

    std::vector<std::size_t> parent(fingerprinted.size());
    std::vector<int> rank(fingerprinted.size(), 0);
    for (std::size_t i = 0; i < parent.size(); ++i)
        parent[i] = i;
    const auto find_root = [&](std::size_t index) -> std::size_t
    {
        while (parent[index] != index)
        {
            parent[index] = parent[parent[index]];
            index = parent[index];
        }
        return index;
    };
    const auto unite = [&](std::size_t left, std::size_t right)
    {
        left = find_root(left);
        right = find_root(right);
        if (left == right)
            return;
        if (rank[left] < rank[right])
            parent[left] = right;
        else if (rank[left] > rank[right])
            parent[right] = left;
        else
        {
            parent[right] = left;
            ++rank[left];
        }
    };

    for (std::size_t i = 0; i < fingerprinted.size(); ++i)
    {
        cancelled = request.cancellation.check();
        if (!cancelled)
            return cancelled.error();
        for (std::size_t j = i + 1U; j < fingerprinted.size(); ++j)
        {
            if (hamming(fingerprinted[i].hash, fingerprinted[j].hash) <= request.max_hamming)
                unite(i, j);
        }
    }

    std::map<std::size_t, std::vector<std::size_t>> clusters;
    for (std::size_t i = 0; i < fingerprinted.size(); ++i)
        clusters[find_root(i)].push_back(i);

    for (auto &entry : clusters)
    {
        auto &members = entry.second;
        if (members.size() < 2U)
            continue;
        if (report.groups.size() >= request.max_groups)
            break;

        NearDuplicateGroup group;
        int max_distance = 0;
        for (std::size_t a = 0; a < members.size(); ++a)
        {
            for (std::size_t b = a + 1U; b < members.size(); ++b)
            {
                max_distance = std::max(max_distance, hamming(fingerprinted[members[a]].hash,
                                                              fingerprinted[members[b]].hash));
            }
        }
        group.max_hamming_in_group = max_distance;
        std::string canonical = fingerprinted[members.front()].member.fingerprint_hex;
        for (const auto index : members)
        {
            group.members.push_back(fingerprinted[index].member);
            if (fingerprinted[index].member.fingerprint_hex < canonical)
                canonical = fingerprinted[index].member.fingerprint_hex;
        }
        group.fingerprint_hex = std::move(canonical);
        std::sort(group.members.begin(), group.members.end(),
                  [](const NearDuplicateMember &left, const NearDuplicateMember &right)
                  { return left.asset_id < right.asset_id; });
        report.groups.push_back(std::move(group));
    }

    std::sort(report.groups.begin(), report.groups.end(),
              [](const NearDuplicateGroup &left, const NearDuplicateGroup &right)
              {
                  if (left.fingerprint_hex != right.fingerprint_hex)
                      return left.fingerprint_hex < right.fingerprint_hex;
                  return left.members.front().asset_id < right.members.front().asset_id;
              });
    return report;
}

Result<BurstComparePair> resolve_burst_compare_pair(const LibraryStackRecord &stack,
                                                    const std::string_view focus_asset_id,
                                                    const BurstCompareStep step)
{
    if (stack.id.empty())
    {
        return make_error(ErrorCode::kValidation, "Library stack id is empty",
                          {{"reason", "burst_compare_missing_stack"}});
    }
    if (stack.member_ids.size() < 2U)
    {
        return make_error(ErrorCode::kValidation,
                          "Burst compare requires a stack with at least two members",
                          {{"reason", "burst_compare_singleton_stack"},
                           {"stack_id", stack.id},
                           {"member_count", std::to_string(stack.member_ids.size())}});
    }
    if (focus_asset_id.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Burst compare requires an asset id",
                          {{"reason", "burst_compare_missing_asset"}});
    }
    std::size_t focus_index = stack.member_ids.size();
    for (std::size_t i = 0; i < stack.member_ids.size(); ++i)
    {
        if (stack.member_ids[i] == focus_asset_id)
        {
            focus_index = i;
            break;
        }
    }
    if (focus_index >= stack.member_ids.size())
    {
        return make_error(ErrorCode::kNotFound, "Asset is not a member of the library stack",
                          {{"reason", "burst_compare_asset_not_in_stack"},
                           {"stack_id", stack.id},
                           {"asset_id", std::string(focus_asset_id)}});
    }

    if (step == BurstCompareStep::kPrevious && focus_index > 0U)
    {
        --focus_index;
    }
    else if (step == BurstCompareStep::kNext && focus_index + 1U < stack.member_ids.size())
    {
        ++focus_index;
    }

    std::size_t compare_index = focus_index;
    if (step == BurstCompareStep::kPrevious)
    {
        compare_index = focus_index > 0U ? focus_index - 1U : focus_index + 1U;
    }
    else
    {
        // current and next prefer the following member, else the previous.
        compare_index =
            focus_index + 1U < stack.member_ids.size() ? focus_index + 1U : focus_index - 1U;
    }

    BurstComparePair pair;
    pair.stack_id = stack.id;
    pair.member_ids = stack.member_ids;
    pair.focus_index = focus_index;
    pair.focus_asset_id = stack.member_ids[focus_index];
    pair.compare_asset_id = stack.member_ids[compare_index];
    pair.step = step;
    return pair;
}

Result<BurstComparePair>
CatalogService::resolve_burst_compare_pair(const BurstCompareRequest &request) const
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    if (request.asset_id.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Burst compare requires an asset id",
                          {{"reason", "burst_compare_missing_asset"}});
    }
    auto asset = repository_->find_asset_by_id(request.asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(
            ErrorCode::kNotFound, "Asset was not found",
            {{"reason", "burst_compare_asset_missing"}, {"asset_id", request.asset_id}});
    }
    if (!asset.value()->stack_id.has_value() || asset.value()->stack_id->empty())
    {
        return make_error(
            ErrorCode::kValidation, "Asset is not in a library stack for burst compare",
            {{"reason", "burst_compare_not_stacked"}, {"asset_id", request.asset_id}});
    }
    auto stack = repository_->find_library_stack(*asset.value()->stack_id);
    if (!stack)
    {
        return stack.error();
    }
    if (!stack.value())
    {
        return make_error(
            ErrorCode::kNotFound, "Library stack was not found",
            {{"reason", "burst_compare_stack_missing"}, {"stack_id", *asset.value()->stack_id}});
    }
    return ::ravo::resolve_burst_compare_pair(*stack.value(), request.asset_id, request.step);
}

} // namespace ravo
