#include "ravo/services/develop_service.h"

#include <set>
#include <string>
#include <utility>

#include "ravo/domain/types.h"
#include "ravo/domain/catalog_repository.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/develop_patch.h"
#include "ravo/services/recovery_service.h"

namespace ravo
{

Result<DevelopApplyResult>
DevelopService::apply_develop_selection(const DevelopApplyRequest &request,
                                        const DevelopApplyProgressCallback &progress)
{
    return apply_selection_impl(request, progress, nullptr);
}

Result<DevelopApplyResult> DevelopService::apply_patch_selection(
    const std::vector<std::string> &ids, const JsonValue &fields, const std::int64_t revision,
    const CancellationToken &cancellation, const DevelopApplyProgressCallback &progress)
{
    if (ids.empty() || ids.size() > 256 || !fields.array_if() || fields.array_if()->empty() ||
        fields.array_if()->size() > 256)
        return make_error(ErrorCode::kInvalidArgument,
                          "Automation batch requires 1 through 256 photos and fields");
    DevelopApplyRequest request;
    request.asset_ids = ids;
    request.expected_revision = revision;
    request.cancellation = cancellation;
    return apply_selection_impl(request, progress, &fields);
}

Result<DevelopApplyResult>
DevelopService::apply_selection_impl(const DevelopApplyRequest &request,
                                     const DevelopApplyProgressCallback &progress,
                                     const JsonValue *patch)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto cancelled = request.cancellation.check();
    if (!cancelled)
        return cancelled.error();
    if (request.asset_ids.empty() || request.asset_ids.size() > kExportBatchMaxAssets)
    {
        return make_error(ErrorCode::kInvalidArgument, "Develop apply batch size is invalid",
                          {{"asset_count", std::to_string(request.asset_ids.size())},
                           {"max_assets", std::to_string(kExportBatchMaxAssets)},
                           {"reason", "invalid_develop_apply_batch_size"}});
    }

    if (!patch)
    {
        DevelopParams field_probe;
        auto valid_fields =
            apply_develop_selected_fields(field_probe, request.source, request.fields);
        if (!valid_fields)
            return valid_fields.error();
    }

    std::set<std::string, std::less<>> unique_assets;
    for (std::size_t index = 0; index < request.asset_ids.size(); ++index)
    {
        cancelled = request.cancellation.check();
        if (!cancelled)
            return cancelled.error();
        const auto &asset_id = request.asset_ids[index];
        if (asset_id.empty() || !unique_assets.emplace(asset_id).second)
        {
            return make_error(ErrorCode::kValidation,
                              "Develop apply asset IDs must be nonempty and unique",
                              {{"asset_id", asset_id},
                               {"batch_index", std::to_string(index + 1U)},
                               {"reason", "duplicate_develop_apply_asset_id"}});
        }
        auto asset = repository_->find_asset_by_id(asset_id);
        if (!asset)
            return asset.error();
        if (!asset.value())
        {
            return make_error(
                ErrorCode::kNotFound, "Asset does not exist",
                {{"asset_id", asset_id}, {"batch_index", std::to_string(index + 1U)}});
        }
        if (is_video_media_type(asset.value()->media_type))
            return make_error(
                ErrorCode::kUnsupported, "Video does not support photo Develop",
                {{"asset_id", asset_id}, {"reason", "video_photo_operation_unsupported"}});
    }

    auto current = repository_->snapshot();
    if (!current)
        return current.error();
    if (request.expected_revision && *request.expected_revision != current.value().revision)
    {
        return make_error(ErrorCode::kConflict, "Catalog revision is stale",
                          {{"reason", "stale_catalog_revision"},
                           {"expected_revision", std::to_string(*request.expected_revision)},
                           {"revision", std::to_string(current.value().revision)}});
    }

    DevelopApplyResult result;
    result.revision = current.value().revision;
    result.items.reserve(request.asset_ids.size());
    bool skipping = false;
    for (std::size_t index = 0; index < request.asset_ids.size(); ++index)
    {
        DevelopApplyItemResult item;
        item.asset_id = request.asset_ids[index];
        if (!skipping)
        {
            cancelled = request.cancellation.check();
            if (!cancelled)
                skipping = true;
        }
        if (skipping)
        {
            item.status = DevelopApplyItemStatus::kSkipped;
            if (patch)
                item.error = make_error(request.cancellation.is_cancellation_requested() ?
                                            ErrorCode::kCancelled :
                                            ErrorCode::kConflict,
                                        "Batch stopped before this photo");
            ++result.skipped;
            result.items.push_back(std::move(item));
            if (progress)
                progress(result.items.size(), request.asset_ids.size(), &result.items.back());
            continue;
        }
        if (repository_ == nullptr)
        {
            item.status = DevelopApplyItemStatus::kFailed;
            item.error = make_error(ErrorCode::kIo, "Catalog session is closed");
            ++result.failed;
            result.items.push_back(std::move(item));
            if (progress)
                progress(result.items.size(), request.asset_ids.size(), &result.items.back());
            continue;
        }
        auto loaded = load_recipe(item.asset_id);
        if (!loaded)
        {
            item.status = DevelopApplyItemStatus::kFailed;
            item.error = loaded.error();
            ++result.failed;
            result.items.push_back(std::move(item));
            if (progress)
                progress(result.items.size(), request.asset_ids.size(), &result.items.back());
            continue;
        }
        auto destination = develop_from_recipe(loaded.value());
        if (!destination)
        {
            item.status = DevelopApplyItemStatus::kFailed;
            item.error = destination.error();
            ++result.failed;
            result.items.push_back(std::move(item));
            if (progress)
                progress(result.items.size(), request.asset_ids.size(), &result.items.back());
            continue;
        }
        const auto before = destination.value();
        Result<void> overlay;
        if (patch)
        {
            auto changed = apply_develop_patch(before, *patch);
            if (!changed)
                overlay = changed.error();
            else
                destination = std::move(changed).value();
        }
        else
            overlay =
                apply_develop_selected_fields(destination.value(), request.source, request.fields);
        if (!overlay)
        {
            item.status = DevelopApplyItemStatus::kFailed;
            item.error = overlay.error();
            ++result.failed;
            result.items.push_back(std::move(item));
            if (progress)
                progress(result.items.size(), request.asset_ids.size(), &result.items.back());
            continue;
        }
        RecipeSaveOptions options;
        if (patch)
        {
            options.expected_revision = result.revision;
            options.expected_base = before;
            options.expected_source = loaded.value().asset;
            options.cancellation = request.cancellation;
            options.defer_recovery_publication = true;
        }
        auto saved = save_develop_with_history(item.asset_id, destination.value(), options);
        if (!saved)
        {
            item.status = DevelopApplyItemStatus::kFailed;
            item.error = saved.error();
            ++result.failed;
            if (patch && (saved.error().code == ErrorCode::kConflict ||
                          saved.error().code == ErrorCode::kCancelled))
                skipping = true;
            result.items.push_back(std::move(item));
            if (progress)
                progress(result.items.size(), request.asset_ids.size(), &result.items.back());
            continue;
        }
        item.status = DevelopApplyItemStatus::kApplied;
        item.history_id = saved.value().history_id;
        item.revision = saved.value().revision;
        result.revision = saved.value().revision;
        ++result.applied;
        if (patch)
        {
            auto recovered = recovery_service_.synchronize_committed_change(item.asset_id);
            if (!recovered)
                item.error = recovered.error(); // committed, with an explicit recovery warning
        }
        result.items.push_back(std::move(item));
        if (progress)
            progress(result.items.size(), request.asset_ids.size(), &result.items.back());
    }
    return result;
}

} // namespace ravo
