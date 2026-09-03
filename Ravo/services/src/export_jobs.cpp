#include "ravo/services/catalog_service.h"

#include <filesystem>
#include <set>
#include <utility>

#include "catalog_internal.h"
#include "catalog_service_internal.h"
#include "ravo/domain/uri.h"

namespace ravo
{
using namespace catalog_service_internal;
namespace
{

[[nodiscard]] Result<std::string> planned_output_path(const ExportJob &job, const std::size_t index,
                                                      const AssetRecord &asset)
{
    auto source = normalize_local_input(asset.normalized_uri);
    if (!source)
        return source.error();
    const auto source_path = utf8_path(source.value().path);
    const auto stem = utf8_string(source_path.stem().u8string());
    const auto extension = job.options.format == ExportFormat::kOriginalCopy ?
                               utf8_string(source_path.extension().u8string()) :
                               std::string(export_format_extension(job.options.format));
    auto filename = expand_export_filename_template(job.filename_template, stem, asset.id,
                                                    index + 1U, extension);
    if (!filename)
        return filename.error();
    auto root = normalize_local_input(job.output_directory);
    if (!root)
        return root.error();
    const auto output_path = utf8_path(root.value().path) / utf8_path(filename.value());
    return utf8_string(output_path.generic_u8string());
}

} // namespace

Result<ExportJob> CatalogService::create_export_job(const ExportBatchRequest &request,
                                                    std::string job_id)
{
    if (job_id.empty())
        return make_error(ErrorCode::kInvalidArgument, "Export job requires a job id",
                          {{"reason", "missing_export_job_id"}});
    auto valid = validate_export_options(request.options);
    if (!valid)
        return valid.error();
    if (request.asset_ids.empty() || request.asset_ids.size() > kExportBatchMaxAssets)
    {
        return make_error(ErrorCode::kInvalidArgument, "Export batch size is invalid",
                          {{"asset_count", std::to_string(request.asset_ids.size())},
                           {"reason", "invalid_export_batch_size"}});
    }
    if (request.output_directory.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Export batch requires an output directory");
    }
    ExportJob job;
    job.schema_version = kExportJobSchemaVersion;
    job.job_id = std::move(job_id);
    job.asset_ids = request.asset_ids;
    job.options = request.options;
    job.output_directory = request.output_directory;
    job.filename_template = request.filename_template.empty() ?
                                std::string("{stem}-{sequence}{ext}") :
                                request.filename_template;
    job.items.reserve(job.asset_ids.size());
    std::set<std::string, std::less<>> unique_assets;
    for (std::size_t index = 0; index < job.asset_ids.size(); ++index)
    {
        const auto &asset_id = job.asset_ids[index];
        if (asset_id.empty() || !unique_assets.emplace(asset_id).second)
        {
            return make_error(ErrorCode::kValidation,
                              "Export batch asset IDs must be nonempty and unique",
                              {{"asset_id", asset_id},
                               {"batch_index", std::to_string(index + 1U)},
                               {"reason", "duplicate_export_asset_id"}});
        }
        if (repository_ == nullptr)
            return make_error(ErrorCode::kIo, "Catalog session is closed");
        auto asset = repository_->find_asset_by_id(asset_id);
        if (!asset)
            return asset.error();
        if (!asset.value())
        {
            return make_error(
                ErrorCode::kNotFound, "Asset does not exist",
                {{"asset_id", asset_id}, {"batch_index", std::to_string(index + 1U)}});
        }
        auto output = planned_output_path(job, index, *asset.value());
        if (!output)
            return output.error();
        ExportJobItem item;
        item.asset_id = asset_id;
        item.status = ExportJobItemStatus::kPending;
        item.output_path = std::move(output).value();
        job.items.push_back(std::move(item));
    }
    return job;
}

Result<ExportJob> CatalogService::run_export_job(
    ExportJob job,
    const std::function<void(std::size_t, std::size_t, const ExportResult *)> &progress)
{
    return resume_export_job(std::move(job), progress);
}

Result<ExportJob> CatalogService::resume_export_job(
    ExportJob job,
    const std::function<void(std::size_t, std::size_t, const ExportResult *)> &progress)
{
    if (engine_ == nullptr || raster_ == nullptr || repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    if (job.schema_version != kExportJobSchemaVersion)
    {
        return make_error(ErrorCode::kUnsupported, "Export job schema version is unsupported",
                          {{"schema_version", std::to_string(job.schema_version)},
                           {"reason", "unsupported_export_job_schema"}});
    }
    auto valid = validate_export_options(job.options);
    if (!valid)
        return valid.error();
    if (job.asset_ids.size() != job.items.size() || job.asset_ids.empty() ||
        job.asset_ids.size() > kExportBatchMaxAssets)
    {
        return make_error(ErrorCode::kValidation, "Export job item list is invalid",
                          {{"reason", "invalid_export_job_items"}});
    }

    // Preflight only unfinished items for conflicts; retain delivered files.
    std::set<std::string, std::less<>> unique_outputs;
    for (std::size_t index = 0; index < job.items.size(); ++index)
    {
        auto &item = job.items[index];
        if (item.asset_id != job.asset_ids[index])
        {
            return make_error(ErrorCode::kValidation, "Export job item order must match asset_ids",
                              {{"reason", "export_job_item_order_mismatch"},
                               {"batch_index", std::to_string(index + 1U)}});
        }
        if (!unique_outputs.emplace(item.output_path).second)
        {
            return make_error(ErrorCode::kConflict,
                              "Export filename template resolves multiple assets to one output",
                              {{"asset_id", item.asset_id},
                               {"output", item.output_path},
                               {"reason", "duplicate_export_output"}});
        }
        if (item.status == ExportJobItemStatus::kDelivered)
            continue;
        std::error_code target_error;
        const auto target_status =
            std::filesystem::symlink_status(utf8_path(item.output_path), target_error);
        if (target_error && target_error != std::errc::no_such_file_or_directory)
        {
            return make_error(ErrorCode::kIo, "Unable to inspect export output path",
                              {{"asset_id", item.asset_id},
                               {"output", item.output_path},
                               {"detail", target_error.message()}});
        }
        if (!target_error && std::filesystem::exists(target_status))
        {
            return make_error(ErrorCode::kConflict, "Export output already exists",
                              {{"asset_id", item.asset_id},
                               {"output", item.output_path},
                               {"reason", "export_batch_preflight_conflict"},
                               {"partial_batch", "true"}});
        }
    }

    std::size_t completed = 0;
    for (const auto &item : job.items)
    {
        if (item.status == ExportJobItemStatus::kDelivered)
            ++completed;
    }

    for (std::size_t index = 0; index < job.items.size(); ++index)
    {
        auto &item = job.items[index];
        if (item.status == ExportJobItemStatus::kDelivered)
            continue;
        ExportRequest request;
        static_cast<ExportOptions &>(request) = job.options;
        request.asset_id = item.asset_id;
        request.output_path = item.output_path;
        request.correlation_id = job.job_id + ":" + std::to_string(index + 1U);
        auto exported = export_asset(request);
        if (!exported)
        {
            item.status = ExportJobItemStatus::kFailed;
            const auto reason = exported.error().context.find("reason");
            if (reason != exported.error().context.end())
                item.error_reason = reason->second;
            item.error_message = exported.error().message;
            // Return the durable job snapshot so callers can persist partial outcomes.
            return job;
        }
        item.status = ExportJobItemStatus::kDelivered;
        item.error_reason.reset();
        item.error_message.reset();
        ++completed;
        if (progress)
            progress(completed, job.items.size(), &exported.value());
    }
    return job;
}

} // namespace ravo
