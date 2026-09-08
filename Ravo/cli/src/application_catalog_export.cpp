#include "application_internal.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ravo/adapters/text_file.h"
#include "ravo/domain/types.h"
#include "ravo/foundation/json.h"
#include "ravo/services/catalog_service.h"

namespace ravo::cli_internal
{
Result<JsonValue> run_catalog_export_command(CatalogService &service, std::string_view subcommand,
                                             const CatalogCliArguments &flags)
{
    if (subcommand == "export-preset-save")
    {
        if (flags.output.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog export-preset-save requires --output");
        }
        auto options = resolved_export_options(flags);
        if (!options)
            return options.error();
        ExportPreset preset;
        preset.schema_version = kExportPresetSchemaVersion;
        preset.options = std::move(options).value();
        auto serialized = serialize_export_preset(preset);
        if (!serialized)
            return serialized.error();
        auto written = write_utf8_text_file_atomically(flags.output, serialized.value());
        if (!written)
            return written.error();
        return JsonValue{JsonValue::Object{
            {"format", std::string(export_format_name(preset.options.format))},
            {"output", std::string(flags.output)},
            {"schema", std::string(kExportPresetSchema)},
            {"schema_version", JsonValue::number(std::to_string(preset.schema_version))},
        }};
    }
    if (subcommand == "export-job-create")
    {
        if (flags.asset_ids.empty() || flags.output_directory.empty() || flags.export_job.empty() ||
            flags.job_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog export-job-create requires --asset-id, --output-dir, "
                              "--export-job, and --job-id");
        }
        auto options = resolved_export_options(flags);
        if (!options)
            return options.error();
        ExportBatchRequest request;
        request.asset_ids.reserve(flags.asset_ids.size());
        for (const auto asset_id : flags.asset_ids)
            request.asset_ids.emplace_back(asset_id);
        request.output_directory = std::string(flags.output_directory);
        if (!flags.filename_template.empty())
            request.filename_template = std::string(flags.filename_template);
        request.options = std::move(options).value();
        auto job = service.create_export_job(request, std::string(flags.job_id));
        if (!job)
            return job.error();
        auto serialized = serialize_export_job(job.value());
        if (!serialized)
            return serialized.error();
        auto written = write_utf8_text_file_atomically(flags.export_job, serialized.value());
        if (!written)
            return written.error();
        return JsonValue{JsonValue::Object{
            {"job_id", job.value().job_id},
            {"items", JsonValue::number(std::to_string(job.value().items.size()))},
            {"output", std::string(flags.export_job)},
            {"schema", std::string(kExportJobSchema)},
        }};
    }
    if (subcommand == "export-job-resume")
    {
        if (flags.export_job.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog export-job-resume requires --export-job");
        }
        auto text_body = read_utf8_text_file(flags.export_job, kExportJobFileMaxBytes);
        if (!text_body)
            return text_body.error();
        auto parsed = parse_export_job_json(text_body.value());
        if (!parsed)
            return parsed.error();
        auto resumed = service.resume_export_job(std::move(parsed).value());
        if (!resumed)
            return resumed.error();
        auto serialized = serialize_export_job(resumed.value());
        if (!serialized)
            return serialized.error();
        auto written = write_utf8_text_file_atomically(flags.export_job, serialized.value());
        if (!written)
            return written.error();
        std::size_t delivered = 0;
        std::size_t pending = 0;
        std::size_t failed = 0;
        std::optional<std::string> failed_asset;
        std::optional<std::string> failed_reason;
        std::optional<std::string> failed_message;
        for (const auto &item : resumed.value().items)
        {
            switch (item.status)
            {
            case ExportJobItemStatus::kDelivered:
                ++delivered;
                break;
            case ExportJobItemStatus::kPending:
                ++pending;
                break;
            case ExportJobItemStatus::kFailed:
                ++failed;
                if (!failed_asset)
                {
                    failed_asset = item.asset_id;
                    failed_reason = item.error_reason;
                    failed_message = item.error_message;
                }
                break;
            }
        }
        if (failed > 0)
        {
            return make_error(ErrorCode::kIo, failed_message.value_or("Export job item failed"),
                              {{"job_id", resumed.value().job_id},
                               {"asset_id", failed_asset.value_or("")},
                               {"completed_count", std::to_string(delivered)},
                               {"total_count", std::to_string(resumed.value().items.size())},
                               {"partial_batch", delivered > 0 ? "true" : "false"},
                               {"reason", failed_reason.value_or("export_job_item_failed")}});
        }
        return JsonValue{JsonValue::Object{
            {"delivered", JsonValue::number(std::to_string(delivered))},
            {"failed", JsonValue::number(std::to_string(failed))},
            {"job_id", resumed.value().job_id},
            {"output", std::string(flags.export_job)},
            {"pending", JsonValue::number(std::to_string(pending))},
        }};
    }
    if (subcommand == "export-batch")
    {
        if (flags.asset_ids.empty() || flags.output_directory.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog export-batch requires --asset-id and --output-dir");
        }
        auto options = resolved_export_options(flags);
        if (!options)
            return options.error();
        ExportBatchRequest request;
        request.asset_ids.reserve(flags.asset_ids.size());
        for (const auto asset_id : flags.asset_ids)
            request.asset_ids.emplace_back(asset_id);
        request.output_directory = std::string(flags.output_directory);
        if (!flags.filename_template.empty())
            request.filename_template = std::string(flags.filename_template);
        request.options = std::move(options).value();
        auto exported = service.export_assets(request);
        if (!exported)
            return exported.error();
        JsonValue::Array items;
        items.reserve(exported.value().size());
        for (const auto &item : exported.value())
        {
            items.emplace_back(JsonValue::Object{
                {"asset_id", item.asset_id},
                {"bytes", JsonValue::number(std::to_string(item.bytes_written))},
                {"height", JsonValue::number(std::to_string(item.height))},
                {"output", item.output_path},
                {"width", JsonValue::number(std::to_string(item.width))},
            });
        }
        return JsonValue{JsonValue::Object{
            {"exported", JsonValue::number(std::to_string(exported.value().size()))},
            {"filename_template", request.filename_template},
            {"format", std::string(export_format_name(request.options.format))},
            {"items", std::move(items)},
            {"metadata_mode",
             std::string(export_metadata_mode_name(request.options.metadata_mode))},
            {"output_directory", request.output_directory},
        }};
    }
    if (subcommand == "export")
    {
        if (flags.asset_id.empty() || flags.output.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog export requires --asset-id and --output");
        }
        ExportRequest request;
        request.asset_id = std::string(flags.asset_id);
        request.output_path = std::string(flags.output);
        auto options = resolved_export_options(flags);
        if (!options)
            return options.error();
        static_cast<ExportOptions &>(request) = std::move(options).value();
        auto exported = service.export_asset(request);
        if (!exported)
        {
            return exported.error();
        }
        return JsonValue{JsonValue::Object{
            {"asset_id", exported.value().asset_id},
            {"bytes", JsonValue::number(std::to_string(exported.value().bytes_written))},
            {"format", std::string(export_format_name(exported.value().format))},
            {"height", JsonValue::number(std::to_string(exported.value().height))},
            {"metadata_mode", std::string(export_metadata_mode_name(request.metadata_mode))},
            {"output", exported.value().output_path},
            {"width", JsonValue::number(std::to_string(exported.value().width))},
        }};
    }
    return make_error(ErrorCode::kInvalidArgument, "Unknown catalog export subcommand",
                      {{"subcommand", std::string(subcommand)}});
}

} // namespace ravo::cli_internal
