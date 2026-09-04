#include "application_internal.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ravo/foundation/json.h"
#include "ravo/services/catalog_service.h"
#include "ravo/services/ingest_transport.h"

namespace ravo::cli_internal
{
namespace
{

[[nodiscard]] JsonValue native_support_json(const NativeIngestPlatformSupport &support)
{
    return JsonValue{JsonValue::Object{
        {"schema", support.schema},
        {"platform", support.platform},
        {"adapter_packaged", support.adapter_packaged},
        {"ptp_usb", std::string(native_ingest_support_state_name(support.ptp_usb))},
        {"mtp", std::string(native_ingest_support_state_name(support.mtp))},
        {"reason", support.reason},
        {"ptp_planned_stack", support.ptp_planned_stack},
        {"mtp_planned_stack", support.mtp_planned_stack},
    }};
}

[[nodiscard]] std::string_view item_status_name(const ImportItemStatus status) noexcept
{
    switch (status)
    {
    case ImportItemStatus::kImported:
        return "imported";
    case ImportItemStatus::kDuplicate:
        return "duplicate";
    case ImportItemStatus::kUnsupported:
        return "unsupported";
    case ImportItemStatus::kSkipped:
        return "skipped";
    case ImportItemStatus::kFailed:
        return "failed";
    }
    return "failed";
}

[[nodiscard]] JsonValue import_item_json(const ImportItemResult &item)
{
    JsonValue::Object object{
        {"status", std::string(item_status_name(item.status))},
        {"input_path", item.input_path},
        {"copies_verified", item.copies_verified},
    };
    if (item.destination_path)
        object.emplace("destination_path", *item.destination_path);
    if (item.asset)
        object.emplace("asset_id", item.asset->id);
    if (item.error)
        object.emplace("error", error_object(*item.error));
    return JsonValue{std::move(object)};
}

} // namespace

Result<JsonValue> run_catalog_ingest_command(CatalogService &service, std::string_view subcommand,
                                             const CatalogCliArguments &flags)
{
    if (subcommand == "ingest-probe")
    {
        auto support = service.probe_ingest_native_support();
        if (!support)
            return support.error();
        return native_support_json(support.value());
    }
    if (subcommand == "ingest")
    {
        if (flags.inputs.empty() && flags.ingest_transport != "ptp-usb" &&
            flags.ingest_transport != "mtp")
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog ingest requires --input source root (or native session "
                              "flags when packaged)",
                              {{"reason", "missing_ingest_input"}});
        }
        IngestRequest request;
        if (!flags.inputs.empty())
            request.source_root = std::string(flags.inputs.front());
        request.transport = flags.ingest_transport.empty() ? std::string("filesystem-card") :
                                                             std::string(flags.ingest_transport);
        if (!flags.resume_batch_id.empty())
            request.resume_batch_id = std::string(flags.resume_batch_id);
        request.mode = ImportTransferMode::kCopy;
        if (!flags.import_mode.empty() && flags.import_mode != "copy")
        {
            return make_error(
                ErrorCode::kInvalidArgument, "catalog ingest is Copy-only; Move/Add are rejected",
                {{"mode", std::string(flags.import_mode)}, {"reason", "ingest_move_unsupported"}});
        }
        request.destination_directory = std::string(flags.import_destination);
        request.filename_template = std::string(flags.import_filename_template);
        request.second_copy_directory = std::string(flags.import_second_copy);
        request.recursive = flags.import_recursive;
        if (!flags.import_organization.empty())
        {
            if (flags.import_organization == "single-folder")
                request.organization = ImportOrganization::kSingleFolder;
            else if (flags.import_organization == "preserve-hierarchy")
                request.organization = ImportOrganization::kPreserveHierarchy;
            else if (flags.import_organization == "capture-date")
                request.organization = ImportOrganization::kCaptureDate;
            else if (flags.import_organization == "capture-month")
                request.organization = ImportOrganization::kCaptureMonth;
            else
            {
                return make_error(ErrorCode::kInvalidArgument, "Unknown import organization",
                                  {{"organization", std::string(flags.import_organization)}});
            }
        }
        auto detailed = service.execute_ingest_detailed(request);
        if (!detailed)
            return detailed.error();

        JsonValue::Array items;
        items.reserve(detailed.value().import.items.size());
        for (const auto &item : detailed.value().import.items)
            items.push_back(import_item_json(item));

        JsonValue::Object object{
            {"transport", detailed.value().transport},
            {"source_uri", detailed.value().source_uri},
            {"resume_checkpoint_cleared", detailed.value().resume_checkpoint_cleared},
            {"imported", JsonValue::number(std::to_string(detailed.value().import.imported))},
            {"duplicates", JsonValue::number(std::to_string(detailed.value().import.duplicates))},
            {"unsupported", JsonValue::number(std::to_string(detailed.value().import.unsupported))},
            {"failed", JsonValue::number(std::to_string(detailed.value().import.failed))},
            {"skipped", JsonValue::number(std::to_string(detailed.value().import.skipped))},
            {"verified_second_copies",
             JsonValue::number(std::to_string(detailed.value().import.verified_second_copies))},
            {"items", JsonValue{std::move(items)}},
            {"support", native_support_json(detailed.value().support)},
        };
        if (detailed.value().resume_batch_id)
            object.emplace("resume_batch_id", *detailed.value().resume_batch_id);
        return JsonValue{std::move(object)};
    }
    return make_error(ErrorCode::kInvalidArgument, "Unknown catalog ingest subcommand",
                      {{"subcommand", std::string(subcommand)}});
}

} // namespace ravo::cli_internal
