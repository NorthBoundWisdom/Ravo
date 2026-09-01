#include "ravo/services/catalog_service.h"

#include <set>
#include <string>
#include <utility>

#include "ravo/domain/types.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/develop.h"

namespace ravo
{

Result<DevelopApplyResult>
CatalogService::apply_develop_selection(const DevelopApplyRequest &request,
                                        const DevelopApplyProgressCallback &progress)
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

    DevelopParams field_probe;
    auto valid_fields =
        apply_develop_selected_fields(field_probe, request.source, request.fields);
    if (!valid_fields)
        return valid_fields.error();

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
    }

    auto current = snapshot();
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
        auto overlay =
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
        auto saved = save_develop_with_history(item.asset_id, destination.value());
        if (!saved)
        {
            item.status = DevelopApplyItemStatus::kFailed;
            item.error = saved.error();
            ++result.failed;
            result.items.push_back(std::move(item));
            if (progress)
                progress(result.items.size(), request.asset_ids.size(), &result.items.back());
            continue;
        }
        item.status = DevelopApplyItemStatus::kApplied;
        item.history_id = saved.value().history_id;
        result.revision = saved.value().revision;
        ++result.applied;
        result.items.push_back(std::move(item));
        if (progress)
            progress(result.items.size(), request.asset_ids.size(), &result.items.back());
    }
    return result;
}

} // namespace ravo
