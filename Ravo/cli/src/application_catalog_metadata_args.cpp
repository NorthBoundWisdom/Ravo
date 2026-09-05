#include "application_internal.h"

#include <charconv>
#include <string>

namespace ravo::cli_internal
{

Result<bool> parse_catalog_metadata_flag(CatalogCliArguments &result, const std::string_view option,
                                         const std::string_view value)
{
    if (option == "--tag")
    {
        result.tag = value;
    }
    else if (option == "--camera")
    {
        result.camera = value;
    }
    else if (option == "--camera-make")
    {
        result.camera_make = value;
    }
    else if (option == "--camera-model")
    {
        result.camera_model = value;
    }
    else if (option == "--lens-make")
    {
        result.lens_make = value;
    }
    else if (option == "--lens-model")
    {
        result.lens_model = value;
    }
    else if (option == "--focal-length-mm")
    {
        result.focal_length_mm = value;
    }
    else if (option == "--captured-local-date")
    {
        result.captured_local_date = value;
    }
    else if (option == "--captured-after")
    {
        result.captured_after_unix_s = value;
    }
    else if (option == "--captured-before")
    {
        result.captured_before_unix_s = value;
    }
    else if (option == "--add")
    {
        result.add = value;
    }
    else if (option == "--remove")
    {
        result.remove = value;
    }
    else if (option == "--title")
    {
        result.title = value;
    }
    else if (option == "--description")
    {
        result.description = value;
    }
    else if (option == "--creator")
    {
        result.creator = value;
    }
    else if (option == "--copyright")
    {
        result.copyright = value;
    }
    else if (option == "--country")
    {
        result.country = value;
    }
    else if (option == "--province-state")
    {
        result.province_state = value;
    }
    else if (option == "--city")
    {
        result.city = value;
    }
    else if (option == "--sublocation")
    {
        result.sublocation = value;
    }
    else if (option == "--headline")
    {
        result.headline = value;
    }
    else if (option == "--credit")
    {
        result.credit = value;
    }
    else if (option == "--source")
    {
        result.source = value;
    }
    else if (option == "--instructions")
    {
        result.instructions = value;
    }
    else if (option == "--usage-terms")
    {
        result.usage_terms = value;
    }
    // --job-id is parsed earlier (also used by export-job-create) and maps to
    // WritableMetadata::job_id / TransmissionReference for catalog metadata.
    else if (option == "--label")
    {
        result.label = value;
    }
    else if (option == "--history-id")
    {
        auto parsed = parse_int_flag(value, option);
        if (!parsed)
        {
            return parsed.error();
        }
        result.history_id = parsed.value();
    }
    else if (option == "--backup")
    {
        if (!result.backup.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "Backup path was specified twice");
        }
        result.backup = value;
    }
    else if (option == "--schedule-dir")
    {
        if (!result.schedule_directory.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "Scheduled backup directory was specified twice");
        result.schedule_directory = value;
    }
    else if (option == "--interval-minutes")
    {
        if (!result.schedule_interval_minutes.empty())
            return make_error(ErrorCode::kInvalidArgument, "Backup interval was specified twice");
        result.schedule_interval_minutes = value;
    }
    else if (option == "--retention-count")
    {
        if (!result.schedule_retention_count.empty())
            return make_error(ErrorCode::kInvalidArgument, "Backup retention was specified twice");
        result.schedule_retention_count = value;
    }
    else if (option == "--enabled")
    {
        if (!result.schedule_enabled.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "Backup enabled state was specified twice");
        result.schedule_enabled = value;
    }
    else if (option == "--folder-id")
    {
        if (!result.folder_id.empty())
            return make_error(ErrorCode::kInvalidArgument, "Folder ID was specified twice");
        result.folder_id = value;
    }
    else if (option == "--folder-uri")
    {
        if (!result.folder_uri.empty())
            return make_error(ErrorCode::kInvalidArgument, "Folder URI was specified twice");
        result.folder_uri = value;
    }
    else if (option == "--replacement")
    {
        if (!result.replacement_directory.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "Replacement folder was specified twice");
        result.replacement_directory = value;
    }
    else if (option == "--keyword-id")
    {
        result.keyword_id = value;
    }
    else if (option == "--keyword-name")
    {
        result.keyword_name = value;
    }
    else if (option == "--parent-id")
    {
        result.parent_id = value;
    }
    else if (option == "--set-id")
    {
        if (!result.set_id.empty())
            return make_error(ErrorCode::kInvalidArgument, "Library set ID was specified twice");
        result.set_id = value;
    }
    else if (option == "--name")
    {
        if (!result.set_name.empty())
            return make_error(ErrorCode::kInvalidArgument, "Library set name was specified twice");
        result.set_name = value;
    }
    else if (option == "--kind")
    {
        if (!result.set_kind.empty())
            return make_error(ErrorCode::kInvalidArgument, "Library set kind was specified twice");
        result.set_kind = value;
    }
    else if (option == "--query")
    {
        if (!result.query_json.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "Library query document was specified twice");
        result.query_json = value;
    }
    else if (option == "--stack-id")
    {
        if (!result.stack_id.empty())
            return make_error(ErrorCode::kInvalidArgument, "Stack ID was specified twice");
        result.stack_id = value;
    }
    else if (option == "--pick-id")
    {
        if (!result.pick_id.empty())
            return make_error(ErrorCode::kInvalidArgument, "Stack pick ID was specified twice");
        result.pick_id = value;
    }
    else if (option == "--from-asset")
    {
        if (!result.from_asset.empty())
            return make_error(ErrorCode::kInvalidArgument, "Source asset ID was specified twice");
        result.from_asset = value;
    }
    else if (option == "--fields")
    {
        if (!result.fields.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "Develop field list was specified twice");
        result.fields = value;
    }
    else if (option == "--proposal-id")
    {
        if (!result.proposal_id.empty())
            return make_error(ErrorCode::kInvalidArgument, "--proposal-id was specified twice");
        result.proposal_id = value;
    }
    else if (option == "--provider")
    {
        if (!result.provider_id.empty())
            return make_error(ErrorCode::kInvalidArgument, "--provider was specified twice");
        result.provider_id = value;
    }
    else if (option == "--model")
    {
        if (!result.model_id.empty())
            return make_error(ErrorCode::kInvalidArgument, "--model was specified twice");
        result.model_id = value;
    }
    else if (option == "--proposal-kind")
    {
        if (!result.proposal_kind.empty())
            return make_error(ErrorCode::kInvalidArgument, "--proposal-kind was specified twice");
        result.proposal_kind = value;
    }
    else if (option == "--suggestion-kind")
    {
        if (!result.suggestion_kind.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "--suggestion-kind was specified twice");
        }
        result.suggestion_kind = value;
    }
    else if (option == "--suggestion-id")
    {
        if (!result.suggestion_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "--suggestion-id was specified twice");
        }
        result.suggestion_id = value;
    }
    else if (option == "--peer-asset")
    {
        if (!result.peer_asset.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "--peer-asset was specified twice");
        }
        result.peer_asset = value;
    }

    else if (option == "--reference-asset")
    {
        if (!result.reference_asset.empty())
            return make_error(ErrorCode::kInvalidArgument, "--reference-asset was specified twice");
        result.reference_asset = value;
    }
    else if (option == "--destination-assets")
    {
        result.destination_assets.push_back(value);
    }
    else if (option == "--semantic-label")
    {
        if (!result.semantic_label.empty())
            return make_error(ErrorCode::kInvalidArgument, "--semantic-label was specified twice");
        result.semantic_label = value;
    }
    else if (option == "--revision")
    {
        if (result.expected_revision)
            return make_error(ErrorCode::kInvalidArgument, "Catalog revision was specified twice");
        std::int64_t parsed = 0;
        const auto converted = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size() ||
            parsed < 0)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "--revision requires a non-negative integer",
                              {{"value", std::string(value)}});
        }
        result.expected_revision = parsed;
    }
    else
    {
        return false;
    }
    return true;
}

} // namespace ravo::cli_internal
