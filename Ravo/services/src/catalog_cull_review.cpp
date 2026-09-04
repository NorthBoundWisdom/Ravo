#include "ravo/services/catalog_service.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/foundation/error.h"
#include "ravo/services/cull_assistance.h"

namespace ravo
{
namespace
{

[[nodiscard]] Result<AssetRecord> load_asset(CatalogRepository &repository,
                                             const std::string_view asset_id)
{
    auto asset = repository.find_asset_by_id(asset_id);
    if (!asset)
        return asset.error();
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    return *asset.value();
}

[[nodiscard]] Result<std::optional<std::string>>
compute_auto_advance_next(CatalogService &service, const CullReviewRequest &request)
{
    if (!request.auto_advance)
        return std::optional<std::string>{};

    std::vector<std::string> order;
    if (!request.selection_asset_ids.empty())
    {
        order = request.selection_asset_ids;
    }
    else
    {
        LibraryQuery query = request.query.value_or(LibraryQuery{});
        auto listed = service.list_assets(query);
        if (!listed)
            return listed.error();
        order.reserve(listed.value().size());
        for (const auto &row : listed.value())
            order.push_back(row.id);
    }
    const auto it = std::find(order.begin(), order.end(), request.asset_id);
    if (it == order.end())
        return std::optional<std::string>{};
    const auto next = std::next(it);
    if (next == order.end())
        return std::optional<std::string>{};
    return std::optional<std::string>{*next};
}

} // namespace

Result<AssetRecord> CatalogService::set_picked(const std::string_view asset_id, const bool picked)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto asset = load_asset(*repository_, asset_id);
    if (!asset)
        return asset.error();
    ReviewState review = asset.value().review;
    review.picked = picked;
    if (picked)
        review.rejected = false;
    auto valid = validate_review_state(review);
    if (!valid)
        return valid.error();
    const auto revision = repository_->commit_review(asset_id, review);
    if (!revision)
        return revision.error();
    asset.value().review = review;
    auto recovered = synchronize_committed_change(asset_id);
    if (!recovered)
    {
        // Catalog mutation is durable; surface committed vs recovery explicitly.
        return recovered.error();
    }
    return asset.value();
}

Result<CullReviewResult> CatalogService::apply_cull_review(const CullReviewRequest &request)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    if (request.asset_id.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Cull review requires an asset id",
                          {{"reason", "cull_review_asset_required"}});
    }
    if (request.flag_action == CullReviewFlagAction::kUnchanged && !request.rating &&
        !request.color_label)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Cull review requires pick, reject, unflag, rating, or color label",
                          {{"reason", "cull_review_mutation_required"}});
    }

    // Cancel and stale checks happen before the publication boundary.
    auto cancelled = request.cancellation.check();
    if (!cancelled)
    {
        auto error = cancelled.error();
        error.context.insert_or_assign("catalog_committed", "false");
        error.context.insert_or_assign("reason", "cull_review_cancelled_before_publish");
        return error;
    }

    if (request.expected_catalog_revision)
    {
        auto snap = snapshot();
        if (!snap)
            return snap.error();
        if (snap.value().revision != *request.expected_catalog_revision)
        {
            return make_error(ErrorCode::kConflict, "Catalog revision does not match",
                              {{"expected", std::to_string(*request.expected_catalog_revision)},
                               {"actual", std::to_string(snap.value().revision)},
                               {"reason", "stale_catalog_revision"},
                               {"catalog_committed", "false"}});
        }
    }

    auto asset = load_asset(*repository_, request.asset_id);
    if (!asset)
        return asset.error();

    CullReviewResult result;
    result.previous_review = asset.value().review;
    ReviewState review = asset.value().review;

    switch (request.flag_action)
    {
    case CullReviewFlagAction::kUnchanged:
        break;
    case CullReviewFlagAction::kPick:
        review.picked = true;
        review.rejected = false;
        break;
    case CullReviewFlagAction::kReject:
        review.rejected = true;
        review.picked = false;
        break;
    case CullReviewFlagAction::kUnflag:
        review.picked = false;
        review.rejected = false;
        break;
    }

    if (request.rating)
        review.rating = *request.rating;
    if (request.color_label)
        review.color_label = *request.color_label;

    auto valid = validate_review_state(review);
    if (!valid)
        return valid.error();

    // Navigation order is computed before mutation so a committed review is never
    // reported as a failed mutation due to list/query failure afterward.
    auto next_asset = compute_auto_advance_next(*this, request);
    if (!next_asset)
        return next_asset.error();

    cancelled = request.cancellation.check();
    if (!cancelled)
    {
        auto error = cancelled.error();
        error.context.insert_or_assign("catalog_committed", "false");
        error.context.insert_or_assign("reason", "cull_review_cancelled_before_publish");
        return error;
    }

    const auto bumped = repository_->commit_review(request.asset_id, review);
    if (!bumped)
    {
        auto error = bumped.error();
        error.context.insert_or_assign("catalog_committed", "false");
        return error;
    }
    asset.value().review = review;

    auto recovered = synchronize_committed_change(request.asset_id, request.cancellation);
    if (!recovered)
    {
        // Review + revision are committed; do not imply the mutation rolled back.
        auto error = recovered.error();
        error.context.insert_or_assign("catalog_committed", "true");
        error.context.insert_or_assign("committed_revision", std::to_string(bumped.value()));
        error.context.insert_or_assign("recovery_retryable", "true");
        error.context.insert_or_assign("reason", error.context.count("reason") ?
                                                     error.context.at("reason") :
                                                     "cull_review_recovery_failed");
        return error;
    }

    // After publication, always return the committed result. A late cancel must
    // not imply the review rolled back.
    result.asset = std::move(asset).value();
    result.review = review;
    result.revision = bumped.value();
    result.next_asset_id = std::move(next_asset).value();
    result.catalog_mutated = true;
    return result;
}

} // namespace ravo
