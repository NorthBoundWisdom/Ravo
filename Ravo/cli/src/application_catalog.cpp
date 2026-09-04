#include "ravo/cli/application.h"
#include "application_internal.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <QByteArrayView>
#include <QCryptographicHash>

#ifdef emit
#undef emit
#endif

#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/filesystem_recovery_store.h"
#include "ravo/adapters/camera_noise_profile.h"
#include "ravo/adapters/crs_xmp.h"
#include "ravo/adapters/legacy_xmp.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/adapters/text_file.h"
#include "ravo/control/live_control.h"
#include "ravo/foundation/json.h"
#include "ravo/engine/noise_calibration.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/style.h"
#include "ravo/services/catalog_service.h"
#include "ravo/services/artifact_publication.h"

namespace ravo::cli_internal
{
[[nodiscard]] Result<std::vector<std::string>>
parse_develop_apply_fields(const std::string_view text)
{
    std::vector<std::string> fields;
    std::size_t begin = 0;
    while (begin <= text.size())
    {
        const auto comma = text.find(',', begin);
        const auto token = text.substr(
            begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin);
        const auto first = token.find_first_not_of(" \t");
        if (first == std::string_view::npos)
        {
            return make_error(
                ErrorCode::kInvalidArgument,
                "catalog develop-apply --fields requires comma-separated names",
                {{"value", std::string(text)}, {"reason", "empty_develop_apply_field_token"}});
        }
        const auto last = token.find_last_not_of(" \t");
        fields.emplace_back(token.substr(first, last - first + 1));
        if (comma == std::string_view::npos)
            break;
        begin = comma + 1;
    }
    if (fields.empty())
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "catalog develop-apply --fields requires comma-separated names",
                          {{"reason", "empty_develop_field_selection"}});
    }
    return fields;
}

[[nodiscard]] Result<JsonValue>
run_catalog_command(const EngineFacade &engine, const std::span<const std::string_view> positional)
{
    if (positional.size() < 2)
    {
        return make_error(
            ErrorCode::kInvalidArgument,
            "Usage: ravo catalog <create|import|list|facets|preview|probe|recipe|develop|develop-apply|"
            "fields|rate|"
            "export|export-batch|export-preset-save|export-job-create|export-job-resume|tag|metadata|refresh-metadata|history|snapshot|restore|"
            "sidecar-status|sidecar-sync|backup|backup-verify|backup-restore|backup-policy|"
            "backup-run|preview-rebuild|folders|folder-relink|folder-remove|sets|set-create|set-rename|"
            "set-delete|set-add|set-remove|version-create|stack|unstack|stack-pick|xmp-status|xmp-import|xmp-export|editor-register|editor-show|editor-open|convert-foreign|dng-convert|dng-status|smart-preview|"
            "ai-propose|ai-proposal|ai-proposals|ai-proposal-apply|ai-proposal-reject|ai-proposal-cancel> "
            "--catalog <path>; backup-verify/backup-restore use --backup <directory>");
    }
    const auto subcommand = positional[1];
    auto flags = parse_catalog_flags(positional);
    if (!flags)
    {
        return flags.error();
    }
    if (subcommand == "fields")
    {
        return develop_fields_json();
    }
    if (!flags.value().output.empty() && subcommand != "export" && subcommand != "probe" &&
        subcommand != "backup-restore" && subcommand != "preview")
    {
        return make_error(
            ErrorCode::kInvalidArgument,
            "--output is only valid for catalog export, probe, preview, or backup-restore",
            {{"subcommand", std::string(subcommand)}});
    }
    if (subcommand == "preview" && !flags.value().output.empty() && !flags.value().roi.has_value())
    {
        return make_error(ErrorCode::kInvalidArgument, "catalog preview --output requires --roi",
                          {{"reason", "preview_output_requires_roi"}});
    }
    if (flags.value().baseline && subcommand != "probe")
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "--baseline is only valid for catalog probe");
    }
    if (!flags.value().backup.empty() && subcommand != "backup" && subcommand != "backup-verify" &&
        subcommand != "backup-restore")
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "--backup is only valid for catalog backup, backup-verify, or "
                          "backup-restore");
    }
    const bool has_schedule_options = !flags.value().schedule_directory.empty() ||
                                      !flags.value().schedule_interval_minutes.empty() ||
                                      !flags.value().schedule_retention_count.empty() ||
                                      !flags.value().schedule_enabled.empty();
    if (has_schedule_options && subcommand != "backup-policy")
        return make_error(ErrorCode::kInvalidArgument,
                          "Backup schedule options are only valid for catalog backup-policy");
    const bool has_folder_relink_options =
        !flags.value().folder_id.empty() || !flags.value().replacement_directory.empty();
    if (has_folder_relink_options && subcommand != "folder-relink")
        return make_error(ErrorCode::kInvalidArgument,
                          "Folder relink options are only valid for catalog folder-relink");
    if (!flags.value().folder_uri.empty() && subcommand != "folder-remove")
        return make_error(ErrorCode::kInvalidArgument,
                          "--folder-uri is only valid for catalog folder-remove");
    const bool has_set_options = !flags.value().set_id.empty() || !flags.value().set_name.empty() ||
                                 !flags.value().set_kind.empty() ||
                                 !flags.value().query_json.empty();
    const bool set_command = subcommand == "sets" || subcommand == "set-create" ||
                             subcommand == "set-rename" || subcommand == "set-delete" ||
                             subcommand == "set-add" || subcommand == "set-remove" ||
                             subcommand == "list" || subcommand == "facets";
    const bool keyword_command = subcommand == "keywords" || subcommand == "keyword-create" ||
                                 subcommand == "keyword-rename" || subcommand == "keyword-move" ||
                                 subcommand == "keyword-delete" || subcommand == "tag";
    if (has_set_options && !set_command)
        return make_error(ErrorCode::kInvalidArgument,
                          "Library set options are only valid for catalog set commands or list");
    if (!flags.value().query_json.empty() && subcommand != "set-create" && subcommand != "facets")
        return make_error(ErrorCode::kInvalidArgument,
                          "--query is only valid for catalog set-create or facets");
    const bool version_command = subcommand == "version-create";
    const bool stack_command =
        subcommand == "stack" || subcommand == "unstack" || subcommand == "stack-pick";
    const bool develop_apply_command = subcommand == "develop-apply";
    const bool ai_command = subcommand == "ai-propose" || subcommand == "ai-proposal" ||
                            subcommand == "ai-proposals" || subcommand == "ai-proposal-apply" ||
                            subcommand == "ai-proposal-reject" ||
                            subcommand == "ai-proposal-cancel";
    const bool xmp_command =
        subcommand == "xmp-status" || subcommand == "xmp-import" || subcommand == "xmp-export";
    const bool editor_command = subcommand == "editor-register" || subcommand == "editor-show" ||
                                subcommand == "editor-open";
    const bool convert_command = subcommand == "convert-foreign" || subcommand == "dng-convert" ||
                                 subcommand == "smart-preview";
    if ((!flags.value().foreign_source.empty() || !flags.value().foreign_source_kind.empty()) &&
        subcommand != "convert-foreign")
        return make_error(
            ErrorCode::kInvalidArgument,
            "--foreign-source/--source-kind are only valid for catalog convert-foreign");
    if (flags.value().ensure && subcommand != "smart-preview")
        return make_error(ErrorCode::kInvalidArgument,
                          "--ensure is only valid for catalog smart-preview");
    if (!flags.value().xmp_path.empty() && !xmp_command)
        return make_error(ErrorCode::kInvalidArgument,
                          "--xmp is only valid for catalog xmp-status, xmp-import, or xmp-export");
    if (!flags.value().xmp_resolve.empty() && subcommand != "xmp-import" &&
        subcommand != "xmp-export")
        return make_error(ErrorCode::kInvalidArgument,
                          "--resolve is only valid for catalog xmp-import or xmp-export");
    if ((!flags.value().editor_id.empty() || !flags.value().editor_version.empty()) &&
        subcommand != "editor-register" && subcommand != "editor-open")
        return make_error(ErrorCode::kInvalidArgument,
                          "--editor/--editor-version are only valid for catalog editor-register "
                          "or editor-open");
    if (flags.value().editor_auto_stack && subcommand != "editor-register")
        return make_error(ErrorCode::kInvalidArgument,
                          "--auto-stack is only valid for catalog editor-register");
    if (flags.value().editor_invoke_os_open && subcommand != "editor-open")
        return make_error(ErrorCode::kInvalidArgument,
                          "--invoke-os-open is only valid for catalog editor-open");
    if (flags.value().expected_revision && !set_command && !version_command && !stack_command &&
        !develop_apply_command && !keyword_command && !ai_command && !editor_command)
        return make_error(ErrorCode::kInvalidArgument,
                          "--revision is only valid for catalog set, version, stack, "
                          "keyword, tag, develop-apply, ai proposal, or editor commands");
    if (flags.value().user_initiated && subcommand != "ai-propose" && subcommand != "editor-open")
        return make_error(ErrorCode::kInvalidArgument,
                          "--user-initiated is only valid for catalog ai-propose or editor-open");
    if (!flags.value().proposal_id.empty() && subcommand != "ai-proposal" &&
        subcommand != "ai-proposal-apply" && subcommand != "ai-proposal-reject" &&
        subcommand != "ai-proposal-cancel")
        return make_error(ErrorCode::kInvalidArgument,
                          "--proposal-id is only valid for catalog ai-proposal commands");
    if ((!flags.value().provider_id.empty() || !flags.value().model_id.empty()) &&
        subcommand != "ai-propose")
        return make_error(ErrorCode::kInvalidArgument,
                          "--provider/--model are only valid for catalog ai-propose");
    if ((!flags.value().proposal_kind.empty() || !flags.value().semantic_label.empty() ||
         !flags.value().reference_asset.empty() || !flags.value().destination_assets.empty()) &&
        subcommand != "ai-propose")
        return make_error(ErrorCode::kInvalidArgument,
                          "--proposal-kind/--semantic-label/--reference-asset/"
                          "--destination-assets are only valid for catalog ai-propose");
    if (!flags.value().from_asset.empty() && !develop_apply_command)
        return make_error(ErrorCode::kInvalidArgument,
                          "--from-asset is only valid for catalog develop-apply");
    if (!flags.value().fields.empty() && !develop_apply_command)
        return make_error(ErrorCode::kInvalidArgument,
                          "--fields is only valid for catalog develop-apply");
    if (!flags.value().stack_id.empty() && subcommand != "unstack" && subcommand != "stack-pick")
        return make_error(ErrorCode::kInvalidArgument,
                          "--stack-id is only valid for catalog unstack or stack-pick");
    if (!flags.value().pick_id.empty() && subcommand != "stack")
        return make_error(ErrorCode::kInvalidArgument, "--pick-id is only valid for catalog stack");
    if (flags.value().stack_expanded && subcommand != "list")
        return make_error(ErrorCode::kInvalidArgument,
                          "--stack-expanded is only valid for catalog list");
    const bool has_facet_list_options =
        !flags.value().camera.empty() || !flags.value().camera_make.empty() ||
        !flags.value().camera_model.empty() || !flags.value().lens_make.empty() ||
        !flags.value().lens_model.empty() || !flags.value().focal_length_mm.empty() ||
        !flags.value().captured_local_date.empty() ||
        !flags.value().captured_after_unix_s.empty() ||
        !flags.value().captured_before_unix_s.empty();
    if (has_facet_list_options && subcommand != "list" && subcommand != "facets")
        return make_error(ErrorCode::kInvalidArgument,
                          "Capture facet filters are only valid for catalog list or facets");
    const bool has_location_list_options =
        !flags.value().country.empty() || !flags.value().province_state.empty() ||
        !flags.value().city.empty() || !flags.value().sublocation.empty();
    if (has_location_list_options && subcommand != "list" && subcommand != "metadata" &&
        subcommand != "facets")
        return make_error(ErrorCode::kInvalidArgument,
                          "Location fields are only valid for catalog list, metadata, or facets");
    const bool has_extension_metadata_options =
        !flags.value().headline.empty() || !flags.value().credit.empty() ||
        !flags.value().source.empty() || !flags.value().instructions.empty() ||
        !flags.value().usage_terms.empty();
    // --job-id is shared with export-job-create; only gate the Extension-only flags here.
    if (has_extension_metadata_options && subcommand != "metadata")
        return make_error(ErrorCode::kInvalidArgument,
                          "IPTC Extension fields are only valid for catalog metadata");
    if (!flags.value().job_id.empty() && subcommand != "metadata" &&
        subcommand != "export-job-create")
        return make_error(ErrorCode::kInvalidArgument,
                          "--job-id is only valid for catalog metadata or export-job-create");
    const bool has_import_options =
        !flags.value().import_mode.empty() || !flags.value().import_destination.empty() ||
        !flags.value().import_organization.empty() || !flags.value().import_preview.empty() ||
        !flags.value().import_filename_template.empty() ||
        !flags.value().import_second_copy.empty() || !flags.value().import_recursive;
    if (has_import_options && subcommand != "import" && subcommand != "editor-register")
        return make_error(ErrorCode::kInvalidArgument,
                          "Import options are only valid for catalog import or editor-register");
    if (subcommand == "editor-register" &&
        (!flags.value().import_mode.empty() || !flags.value().import_organization.empty() ||
         !flags.value().import_preview.empty() || !flags.value().import_filename_template.empty() ||
         !flags.value().import_second_copy.empty() || !flags.value().import_recursive))
        return make_error(
            ErrorCode::kInvalidArgument,
            "catalog editor-register only accepts --destination among import options");
    if (!flags.value().from_xmp.empty() && subcommand != "develop")
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "--from-xmp is only valid for catalog develop");
    }
    auto scoped = reject_scoped_export_options(flags.value(), subcommand);
    if (!scoped)
    {
        return scoped.error();
    }

    if (subcommand == "backup-verify")
    {
        if (!flags.value().catalog.empty())
        {
            return make_error(
                ErrorCode::kInvalidArgument,
                "catalog backup-verify is self-contained and does not accept --catalog or --path");
        }
        if (flags.value().backup.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog backup-verify requires --backup <directory>");
        }
        const auto sidecar_root = filesystem_path_from_utf8(flags.value().backup) /
                                  filesystem_path_from_utf8(kCatalogBackupSidecarDirectory);
        auto recovery =
            FilesystemRecoveryStore::open_existing(filesystem_path_to_utf8(sidecar_root));
        if (!recovery)
        {
            return recovery.error();
        }
        const SqliteCatalogBackupVerifier database_verifier;
        auto verified = verify_catalog_backup(database_verifier, *recovery.value(),
                                              flags.value().backup, CancellationToken{});
        if (!verified)
        {
            return verified.error();
        }
        return backup_artifact_to_json(verified.value().artifact, true);
    }

    if (subcommand == "backup-restore")
    {
        if (!flags.value().catalog.empty())
            return make_error(
                ErrorCode::kInvalidArgument,
                "catalog backup-restore is self-contained and does not accept --catalog or --path");
        if (flags.value().backup.empty() || flags.value().output.empty())
            return make_error(
                ErrorCode::kInvalidArgument,
                "catalog backup-restore requires --backup <directory> --output <absent-catalog>");
        const auto sidecar_root = filesystem_path_from_utf8(flags.value().backup) /
                                  filesystem_path_from_utf8(kCatalogBackupSidecarDirectory);
        auto recovery =
            FilesystemRecoveryStore::open_existing(filesystem_path_to_utf8(sidecar_root));
        if (!recovery)
            return recovery.error();
        const SqliteCatalogBackupVerifier verifier;
        CatalogRestoreRequest request;
        request.backup_directory = std::string(flags.value().backup);
        request.destination_catalog = std::string(flags.value().output);
        auto restored = restore_catalog_backup(verifier, verifier, *recovery.value(), request);
        if (!restored)
            return restored.error();
        return restore_result_to_json(restored.value());
    }

    if (flags.value().catalog.empty())
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Catalog commands require --catalog or --path");
    }

    if (subcommand == "create")
    {
        auto session = open_catalog_session(engine, flags.value().catalog, true);
        if (!session)
        {
            return session.error();
        }
        auto snapshot = session.value()->snapshot();
        if (!snapshot)
        {
            return snapshot.error();
        }
        return JsonValue{JsonValue::Object{
            {"catalog_id", snapshot.value().catalog_id},
            {"path", snapshot.value().database_path},
            {"schema_version", JsonValue::number(std::to_string(snapshot.value().schema_version))},
        }};
    }

    auto session = open_catalog_session(engine, flags.value().catalog, false);
    if (!session)
    {
        return session.error();
    }
    auto &service = *session.value();

    if (subcommand == "sidecar-status")
    {
        JsonValue::Array states;
        std::size_t pending_count = 0U;
        if (!flags.value().asset_id.empty())
        {
            auto state = service.recovery_state(flags.value().asset_id);
            if (!state)
            {
                return state.error();
            }
            pending_count = state.value().pending() ? 1U : 0U;
            states.push_back(recovery_state_to_json(state.value()));
        }
        else
        {
            auto pending = service.pending_recovery();
            if (!pending)
            {
                return pending.error();
            }
            for (const auto &state : pending.value())
            {
                states.push_back(recovery_state_to_json(state));
            }
            pending_count = states.size();
        }
        return JsonValue{JsonValue::Object{
            {"pending", JsonValue::number(std::to_string(pending_count))},
            {"states", std::move(states)},
        }};
    }
    if (subcommand == "sidecar-sync")
    {
        const std::optional<std::string_view> asset_id =
            flags.value().asset_id.empty() ?
                std::nullopt :
                std::optional<std::string_view>{flags.value().asset_id};
        auto synchronized = service.sync_recovery(asset_id, CancellationToken{});
        if (!synchronized)
        {
            return synchronized.error();
        }
        JsonValue::Array artifacts;
        for (const auto &artifact : synchronized.value().artifacts)
        {
            artifacts.push_back(recovery_artifact_to_json(artifact));
        }
        return JsonValue{JsonValue::Object{
            {"artifacts", std::move(artifacts)},
            {"pending_after",
             JsonValue::number(std::to_string(synchronized.value().pending_after))},
            {"pending_before",
             JsonValue::number(std::to_string(synchronized.value().pending_before))},
            {"root", synchronized.value().root},
        }};
    }
    if (subcommand == "backup")
    {
        if (flags.value().backup.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog backup requires --backup <absent-directory>");
        }
        auto backup = service.create_backup(flags.value().backup, CancellationToken{});
        if (!backup)
        {
            return backup.error();
        }
        return backup_artifact_to_json(backup.value(), true);
    }
    if (subcommand == "backup-policy")
    {
        auto policy = service.backup_policy();
        if (!policy)
            return policy.error();
        if (has_schedule_options)
        {
            if (!flags.value().schedule_directory.empty())
                policy.value().destination_directory =
                    std::string(flags.value().schedule_directory);
            if (!flags.value().schedule_interval_minutes.empty())
            {
                auto interval =
                    parse_int_flag(flags.value().schedule_interval_minutes, "--interval-minutes");
                if (!interval)
                    return interval.error();
                policy.value().interval_minutes = interval.value();
            }
            if (!flags.value().schedule_retention_count.empty())
            {
                auto retention =
                    parse_int_flag(flags.value().schedule_retention_count, "--retention-count");
                if (!retention)
                    return retention.error();
                policy.value().retention_count = retention.value();
            }
            if (!flags.value().schedule_enabled.empty())
            {
                if (flags.value().schedule_enabled != "true" &&
                    flags.value().schedule_enabled != "false")
                    return make_error(ErrorCode::kInvalidArgument,
                                      "--enabled must be true or false");
                policy.value().enabled = flags.value().schedule_enabled == "true";
            }
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
            policy = service.set_backup_policy(std::move(policy).value(), now);
            if (!policy)
                return policy.error();
        }
        return backup_policy_to_json(policy.value());
    }
    if (subcommand == "backup-run")
    {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        auto scheduled = service.run_scheduled_backup(now, CancellationToken{}, true);
        if (!scheduled)
            return scheduled.error();
        return backup_schedule_to_json(scheduled.value());
    }
    if (subcommand == "preview-rebuild")
    {
        std::vector<std::string> asset_ids;
        asset_ids.reserve(flags.value().asset_ids.size());
        for (const auto asset_id : flags.value().asset_ids)
            asset_ids.emplace_back(asset_id);
        auto rebuilt = service.rebuild_previews(asset_ids, CancellationToken{});
        if (!rebuilt)
            return rebuilt.error();
        return preview_rebuild_to_json(rebuilt.value());
    }
    if (subcommand == "folders")
    {
        auto folders = service.list_folders();
        if (!folders)
            return folders.error();
        JsonValue::Array items;
        items.reserve(folders.value().size());
        for (const auto &folder : folders.value())
            items.push_back(folder_to_json(folder));
        return JsonValue{JsonValue::Object{{"folders", std::move(items)}}};
    }
    if (subcommand == "folder-relink")
    {
        if (flags.value().folder_id.empty() || flags.value().replacement_directory.empty())
            return make_error(
                ErrorCode::kInvalidArgument,
                "catalog folder-relink requires --folder-id <id> --replacement <directory>");
        auto relinked = service.relink_folder(
            flags.value().folder_id, flags.value().replacement_directory, CancellationToken{});
        if (!relinked)
            return relinked.error();
        return folder_relink_to_json(relinked.value());
    }
    if (subcommand == "folder-remove")
    {
        if (flags.value().folder_uri.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog folder-remove requires --folder-uri <uri>");
        auto removed =
            service.remove_folder_from_catalog(flags.value().folder_uri, CancellationToken{});
        if (!removed)
            return removed.error();
        return JsonValue{JsonValue::Object{
            {"asset_count", JsonValue::number(std::to_string(removed.value().asset_count))},
            {"folder_uri", removed.value().folder_uri},
        }};
    }
    if (subcommand == "sets")
    {
        auto sets = service.list_library_sets();
        if (!sets)
            return sets.error();
        JsonValue::Array items;
        items.reserve(sets.value().size());
        for (const auto &set : sets.value())
        {
            auto json = library_set_to_json(set);
            if (!json)
                return json.error();
            items.push_back(std::move(json).value());
        }
        return JsonValue{JsonValue::Object{{"sets", std::move(items)}}};
    }
    if (subcommand == "set-create")
    {
        if (flags.value().set_name.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog set-create requires --name <name>");
        auto kind = parse_library_set_kind(flags.value().set_kind.empty() ? kLibrarySetKindManual :
                                                                            flags.value().set_kind);
        if (!kind)
            return kind.error();
        std::optional<LibraryQuery> query;
        if (!flags.value().query_json.empty())
        {
            auto parsed = parse_library_query_document(flags.value().query_json);
            if (!parsed)
                return parsed.error();
            query = std::move(parsed).value();
        }
        std::vector<std::string> asset_ids;
        if (!flags.value().asset_id.empty())
            asset_ids.emplace_back(flags.value().asset_id);
        for (const auto asset_id : flags.value().asset_ids)
            asset_ids.emplace_back(asset_id);
        auto created = service.create_library_set(kind.value(), flags.value().set_name, query,
                                                  asset_ids, flags.value().expected_revision);
        if (!created)
            return created.error();
        return library_set_mutation_to_json(created.value());
    }
    if (subcommand == "set-rename")
    {
        if (flags.value().set_id.empty() || flags.value().set_name.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog set-rename requires --set-id <id> --name <name>");
        auto renamed = service.rename_library_set(flags.value().set_id, flags.value().set_name,
                                                  flags.value().expected_revision);
        if (!renamed)
            return renamed.error();
        return library_set_mutation_to_json(renamed.value());
    }
    if (subcommand == "set-delete")
    {
        if (flags.value().set_id.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog set-delete requires --set-id <id>");
        auto deleted =
            service.delete_library_set(flags.value().set_id, flags.value().expected_revision);
        if (!deleted)
            return deleted.error();
        return JsonValue{JsonValue::Object{
            {"revision", JsonValue::number(std::to_string(deleted.value()))},
            {"set_id", std::string(flags.value().set_id)},
        }};
    }
    if (subcommand == "set-add" || subcommand == "set-remove")
    {
        if (flags.value().set_id.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              std::string("catalog ") + std::string(subcommand) +
                                  " requires --set-id <id> --asset-id <id>");
        std::vector<std::string> asset_ids;
        if (!flags.value().asset_id.empty())
            asset_ids.emplace_back(flags.value().asset_id);
        for (const auto asset_id : flags.value().asset_ids)
            asset_ids.emplace_back(asset_id);
        if (asset_ids.empty())
            return make_error(ErrorCode::kInvalidArgument, std::string("catalog ") +
                                                               std::string(subcommand) +
                                                               " requires --asset-id <id>");
        auto mutated = subcommand == "set-add" ?
                           service.add_library_set_members(flags.value().set_id, asset_ids,
                                                           flags.value().expected_revision) :
                           service.remove_library_set_members(flags.value().set_id, asset_ids,
                                                              flags.value().expected_revision);
        if (!mutated)
            return mutated.error();
        return library_set_mutation_to_json(mutated.value());
    }
    if (subcommand == "version-create")
    {
        if (flags.value().asset_id.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog version-create requires --asset-id <id>");
        auto created =
            service.create_asset_version(flags.value().asset_id, flags.value().expected_revision);
        if (!created)
            return created.error();
        return asset_version_mutation_to_json(created.value());
    }
    if (subcommand == "stack")
    {
        std::vector<std::string> asset_ids;
        if (!flags.value().asset_id.empty())
            asset_ids.emplace_back(flags.value().asset_id);
        for (const auto asset_id : flags.value().asset_ids)
            asset_ids.emplace_back(asset_id);
        if (asset_ids.size() < 2)
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog stack requires at least two --asset-id values");
        const auto pick = flags.value().pick_id.empty() ? std::string_view{asset_ids.front()} :
                                                          flags.value().pick_id;
        auto stacked = service.stack_assets(asset_ids, pick, flags.value().expected_revision);
        if (!stacked)
            return stacked.error();
        return library_stack_mutation_to_json(stacked.value());
    }
    if (subcommand == "unstack")
    {
        if (flags.value().stack_id.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog unstack requires --stack-id <id>");
        auto unstacked =
            service.unstack_assets(flags.value().stack_id, flags.value().expected_revision);
        if (!unstacked)
            return unstacked.error();
        return JsonValue{JsonValue::Object{
            {"revision", JsonValue::number(std::to_string(unstacked.value()))},
            {"stack_id", std::string(flags.value().stack_id)},
        }};
    }
    if (subcommand == "stack-pick")
    {
        if (flags.value().stack_id.empty() || flags.value().asset_id.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog stack-pick requires --stack-id <id> --asset-id <id>");
        auto picked = service.set_stack_pick(flags.value().stack_id, flags.value().asset_id,
                                             flags.value().expected_revision);
        if (!picked)
            return picked.error();
        return library_stack_mutation_to_json(picked.value());
    }
    if (subcommand == "import")
    {
        if (flags.value().inputs.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "catalog import requires --input");
        }
        ImportRequest request;
        request.inputs.reserve(flags.value().inputs.size());
        for (const auto input : flags.value().inputs)
            request.inputs.emplace_back(input);
        const auto mode =
            flags.value().import_mode.empty() ? std::string_view{"add"} : flags.value().import_mode;
        if (mode == "add")
            request.mode = ImportTransferMode::kAdd;
        else if (mode == "copy")
            request.mode = ImportTransferMode::kCopy;
        else if (mode == "move")
            request.mode = ImportTransferMode::kMove;
        else
            return make_error(ErrorCode::kInvalidArgument, "Unknown import mode",
                              {{"mode", std::string(mode)}});
        const auto organization = flags.value().import_organization.empty() ?
                                      std::string_view{"single"} :
                                      flags.value().import_organization;
        if (organization == "single")
            request.organization = ImportOrganization::kSingleFolder;
        else if (organization == "hierarchy")
            request.organization = ImportOrganization::kPreserveHierarchy;
        else if (organization == "date")
            request.organization = ImportOrganization::kCaptureDate;
        else if (organization == "month")
            request.organization = ImportOrganization::kCaptureMonth;
        else
            return make_error(ErrorCode::kInvalidArgument, "Unknown import organization",
                              {{"organization", std::string(organization)}});
        const auto preview = flags.value().import_preview.empty() ? std::string_view{"standard"} :
                                                                    flags.value().import_preview;
        if (preview == "minimal")
            request.preview = ImportPreviewPolicy::kMinimal;
        else if (preview == "standard")
            request.preview = ImportPreviewPolicy::kStandard;
        else if (preview == "one-to-one")
            request.preview = ImportPreviewPolicy::kOneToOne;
        else
            return make_error(ErrorCode::kInvalidArgument, "Unknown import preview policy",
                              {{"preview", std::string(preview)}});
        request.destination_directory = std::string(flags.value().import_destination);
        request.filename_template = std::string(flags.value().import_filename_template);
        request.second_copy_directory = std::string(flags.value().import_second_copy);
        request.source_root = request.inputs.front();
        request.recursive = flags.value().import_recursive;
        request.cancellation = CancellationToken{};
        auto imported = service.execute_import(request);
        if (!imported)
        {
            return imported.error();
        }
        JsonValue::Array items;
        for (const auto &item : imported.value().items)
        {
            JsonValue::Object row{
                {"input", item.input_path},
                {"status",
                 std::string(item.status == ImportItemStatus::kImported    ? "imported" :
                             item.status == ImportItemStatus::kDuplicate   ? "duplicate" :
                             item.status == ImportItemStatus::kUnsupported ? "unsupported" :
                                                                             "failed")}};
            if (item.asset)
            {
                row.emplace("asset", asset_to_json(*item.asset));
            }
            if (item.error)
            {
                row.emplace("error", error_object(*item.error));
            }
            if (item.destination_path)
                row.emplace("destination", *item.destination_path);
            if (item.sidecar_destination_path)
                row.emplace("sidecar_destination", *item.sidecar_destination_path);
            if (item.jpeg_companion_destination_path)
                row.emplace("jpeg_companion_destination", *item.jpeg_companion_destination_path);
            if (item.second_copy_destination_path)
                row.emplace("second_copy_destination", *item.second_copy_destination_path);
            if (item.second_copy_sidecar_destination_path)
                row.emplace("second_copy_sidecar_destination",
                            *item.second_copy_sidecar_destination_path);
            if (item.second_copy_jpeg_companion_destination_path)
                row.emplace("second_copy_jpeg_companion_destination",
                            *item.second_copy_jpeg_companion_destination_path);
            row.emplace("copies_verified", item.copies_verified);
            if (item.source_cleanup_error)
                row.emplace("source_cleanup_error", error_object(*item.source_cleanup_error));
            items.emplace_back(std::move(row));
        }
        return JsonValue{JsonValue::Object{
            {"mode", std::string(mode)},
            {"preview", std::string(preview)},
            {"rename_template", std::string(flags.value().import_filename_template)},
            {"second_copy_destination", std::string(flags.value().import_second_copy)},
            {"imported", JsonValue::number(std::to_string(imported.value().imported))},
            {"duplicates", JsonValue::number(std::to_string(imported.value().duplicates))},
            {"unsupported", JsonValue::number(std::to_string(imported.value().unsupported))},
            {"failed", JsonValue::number(std::to_string(imported.value().failed))},
            {"source_cleanup_failed",
             JsonValue::number(std::to_string(imported.value().source_cleanup_failed))},
            {"verified_second_copies",
             JsonValue::number(std::to_string(imported.value().verified_second_copies))},
            {"items", std::move(items)},
        }};
    }
    if (subcommand == "facets")
    {
        return run_catalog_facets_command(service, flags.value());
    }
    if (subcommand == "list")
    {
        auto snapshot = service.snapshot();
        if (!snapshot)
        {
            return snapshot.error();
        }
        auto query = build_library_query(flags.value());
        if (!query)
        {
            return query.error();
        }
        auto listed = service.list_assets(query.value(), !flags.value().stack_expanded);
        if (!listed)
        {
            return listed.error();
        }
        JsonValue::Array assets;
        for (const auto &asset : listed.value())
        {
            assets.push_back(asset_to_json(asset));
        }
        return JsonValue{JsonValue::Object{
            {"assets", std::move(assets)},
            {"catalog_id", snapshot.value().catalog_id},
            {"schema_version", JsonValue::number(std::to_string(snapshot.value().schema_version))},
        }};
    }
    if (subcommand == "preview")
    {
        if (flags.value().asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "catalog preview requires --asset-id");
        }
        PreviewRequest request;
        request.asset_id = std::string(flags.value().asset_id);
        request.max_edge = flags.value().max_edge.value_or(kDefaultPreviewMaxEdge);
        if (flags.value().roi.has_value())
        {
            request.roi = flags.value().roi;
            request.persist_preview_record = false;
        }
        auto previewed = service.request_preview(request);
        if (!previewed)
        {
            return previewed.error();
        }
        if (flags.value().roi.has_value() && !flags.value().output.empty())
        {
            if (!ends_with_png(flags.value().output))
            {
                return make_error(ErrorCode::kInvalidArgument,
                                  "catalog preview --output must be a .png path",
                                  {{"path", std::string(flags.value().output)}});
            }
            if (std::filesystem::exists(std::filesystem::path(std::string(flags.value().output))))
            {
                return make_error(ErrorCode::kConflict, "Output path already exists",
                                  {{"path", std::string(flags.value().output)}});
            }
            RenderedImage rendered;
            rendered.width = previewed.value().width;
            rendered.height = previewed.value().height;
            rendered.rgb = previewed.value().rgb;
            rendered.color_profile = previewed.value().color_profile;
            auto encoded = engine.encode_png(rendered);
            if (!encoded)
            {
                return encoded.error();
            }
            auto written = write_file_bytes_atomically(flags.value().output, encoded.value());
            if (!written)
            {
                return written.error();
            }
            previewed.value().cache_path = std::string(flags.value().output);
        }
        JsonValue::Object body{
            {"asset_id", previewed.value().asset_id},
            {"cache_path", previewed.value().cache_path},
            {"gpu_backend",
             previewed.value().gpu_backend.empty() ? "cpu" : previewed.value().gpu_backend},
            {"height", JsonValue::number(std::to_string(previewed.value().height))},
            {"original_missing", previewed.value().original_missing},
            {"width", JsonValue::number(std::to_string(previewed.value().width))},
        };
        if (flags.value().roi.has_value())
        {
            body.emplace("roi", JsonValue{JsonValue::Array{
                                    JsonValue::number(std::to_string(flags.value().roi->x)),
                                    JsonValue::number(std::to_string(flags.value().roi->y)),
                                    JsonValue::number(std::to_string(flags.value().roi->width)),
                                    JsonValue::number(std::to_string(flags.value().roi->height)),
                                }});
        }
        return JsonValue{std::move(body)};
    }
    if (subcommand == "probe")
    {
        if (flags.value().asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "catalog probe requires --asset-id");
        }
        if (!flags.value().output.empty())
        {
            if (!ends_with_png(flags.value().output))
            {
                return make_error(ErrorCode::kInvalidArgument,
                                  "catalog probe --output must be a .png path",
                                  {{"path", std::string(flags.value().output)}});
            }
            if (std::filesystem::exists(std::filesystem::path(std::string(flags.value().output))))
            {
                return make_error(ErrorCode::kConflict, "Output path already exists",
                                  {{"path", std::string(flags.value().output)}});
            }
        }
        auto stored_before = service.load_recipe(flags.value().asset_id);
        if (!stored_before)
        {
            return stored_before.error();
        }
        auto serialized_before = serialize_recipe(stored_before.value());
        if (!serialized_before)
        {
            return serialized_before.error();
        }
        auto previews_before = service.list_previews();
        if (!previews_before)
        {
            return previews_before.error();
        }
        auto source = flags.value().baseline ?
                          service.load_baseline_recipe(flags.value().asset_id) :
                          stored_before;
        if (!source)
        {
            return source.error();
        }
        auto params = develop_from_recipe(source.value());
        if (!params)
        {
            return params.error();
        }
        auto applied = apply_develop_overrides(params.value(), flags.value());
        if (!applied)
        {
            return applied.error();
        }

        PreviewRequest request;
        request.asset_id = std::string(flags.value().asset_id);
        request.max_edge = flags.value().max_edge.value_or(512U);
        request.prefer_embedded_preview = false;
        request.persist_preview_record = false;
        auto previewed = service.request_preview(request, params.value());
        if (!previewed)
        {
            return previewed.error();
        }
        if (!previewed.value().cache_path.empty() || previewed.value().rgb.empty())
        {
            return make_error(ErrorCode::kIo,
                              "Develop probe did not return a non-persistent memory preview");
        }
        auto statistics = probe_statistics_json(previewed.value());
        if (!statistics)
        {
            return statistics.error();
        }
        auto stored_after = service.load_recipe(flags.value().asset_id);
        if (!stored_after)
        {
            return stored_after.error();
        }
        auto serialized_after = serialize_recipe(stored_after.value());
        if (!serialized_after)
        {
            return serialized_after.error();
        }
        if (serialized_before.value() != serialized_after.value())
        {
            return make_error(ErrorCode::kIo, "Develop probe unexpectedly changed the recipe");
        }
        auto previews_after = service.list_previews();
        if (!previews_after)
        {
            return previews_after.error();
        }
        if (previews_before.value() != previews_after.value())
        {
            return make_error(ErrorCode::kIo, "Develop probe unexpectedly changed preview records");
        }

        JsonValue::Object overrides;
        for (const auto &item : applied.value())
        {
            if (const auto *number = std::get_if<double>(&item.value); number != nullptr)
                overrides.emplace(item.name, JsonValue::number(std::to_string(*number)));
            else
                overrides.emplace(item.name, JsonValue{std::get<std::string>(item.value)});
        }
        JsonValue::Object payload{
            {"asset_id", previewed.value().asset_id},
            {"baseline", flags.value().baseline},
            {"color_profile", previewed.value().color_profile.identifier},
            {"height", JsonValue::number(std::to_string(previewed.value().height))},
            {"original_missing", previewed.value().original_missing},
            {"overrides", std::move(overrides)},
            {"preview_records_unchanged", true},
            {"recipe_unchanged", true},
            {"statistics", std::move(statistics).value()},
            {"width", JsonValue::number(std::to_string(previewed.value().width))},
            {"gpu_backend",
             previewed.value().gpu_backend.empty() ? "cpu" : previewed.value().gpu_backend},
        };
        if (!flags.value().output.empty())
        {
            RenderedImage image;
            image.width = previewed.value().width;
            image.height = previewed.value().height;
            image.rgb = previewed.value().rgb;
            image.color_profile = previewed.value().color_profile;
            auto encoded = engine.encode_png(image);
            if (!encoded)
            {
                return encoded.error();
            }
            auto written = write_file_bytes_atomically(flags.value().output, encoded.value());
            if (!written)
            {
                return written.error();
            }
            payload.emplace("output", std::string(flags.value().output));
        }
        return JsonValue{std::move(payload)};
    }
    if (subcommand == "recipe")
    {
        if (flags.value().asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "catalog recipe requires --asset-id");
        }
        auto recipe = service.load_recipe(flags.value().asset_id);
        if (!recipe)
        {
            return recipe.error();
        }
        auto serialized = serialize_recipe(recipe.value());
        if (!serialized)
        {
            return serialized.error();
        }
        auto parsed = parse_json(serialized.value());
        if (!parsed)
        {
            return parsed.error();
        }
        auto has_edits = service.asset_has_edits(flags.value().asset_id);
        if (!has_edits)
        {
            return has_edits.error();
        }
        return JsonValue{JsonValue::Object{
            {"asset_id", std::string(flags.value().asset_id)},
            {"has_edits", has_edits.value()},
            {"recipe", std::move(parsed).value()},
        }};
    }
    if (subcommand == "develop")
    {
        if (flags.value().asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "catalog develop requires --asset-id");
        }
        auto loaded = service.load_recipe(flags.value().asset_id);
        if (!loaded)
        {
            return loaded.error();
        }
        auto params = develop_from_recipe(loaded.value());
        if (!params)
        {
            return params.error();
        }
        std::string crs_name;
        std::vector<CrsOmission> crs_omitted;
        if (!flags.value().from_xmp.empty())
        {
            auto xmp = read_utf8_text_file(flags.value().from_xmp);
            if (!xmp)
                return xmp.error();
            if (!is_crs_xmp_document(xmp.value()))
            {
                return make_error(ErrorCode::kUnsupported,
                                  "catalog develop --from-xmp requires Camera Raw settings XMP",
                                  {{"reason", "unsupported_xmp_dialect"}});
            }
            auto imported = import_crs_xmp({xmp.value(), loaded.value().asset});
            if (!imported)
                return imported.error();
            apply_crs_look(params.value(), imported.value().look, imported.value().mask);
            crs_name = imported.value().name;
            crs_omitted = imported.value().omitted;
        }
        if (flags.value().pick_white)
        {
            if (std::abs(params.value().straighten_degrees) > 1.0e-4 ||
                params.value().canvas_enabled)
            {
                return make_error(ErrorCode::kUnsupported,
                                  "White-balance pick is unavailable with straighten or Canvas");
            }
            WhiteBalancePickRequest request;
            request.preview_x = flags.value().pick_white->first;
            request.preview_y = flags.value().pick_white->second;
            request.crop_x = params.value().crop_x;
            request.crop_y = params.value().crop_y;
            request.crop_width = params.value().crop_width;
            request.crop_height = params.value().crop_height;
            request.rotate_quarters = static_cast<int>(params.value().rotate_quarters);
            request.flip_horizontal = params.value().flip_horizontal != 0;
            request.flip_vertical = params.value().flip_vertical != 0;
            auto sampled =
                service.sample_white_balance(flags.value().asset_id, request, CancellationToken{});
            if (!sampled)
            {
                return sampled.error();
            }
            params.value().temperature.mode = std::string(kTemperatureModeManual);
            params.value().temperature.coefficients = sampled.value();
        }
        auto applied = apply_develop_overrides(params.value(), flags.value());
        if (!applied)
        {
            return applied.error();
        }
        auto saved = service.save_develop(flags.value().asset_id, params.value());
        if (!saved)
        {
            return saved.error();
        }
        auto json_asset = asset_to_json(saved.value());
        if (flags.value().from_xmp.empty())
            return json_asset;
        JsonValue::Object object =
            json_asset.object_if() != nullptr ? *json_asset.object_if() : JsonValue::Object{};
        object.emplace("omitted", crs_omissions_json(crs_omitted));
        object.emplace("preset_name", crs_name);
        return JsonValue{std::move(object)};
    }
    if (subcommand == "develop-apply")
    {
        if (flags.value().from_asset.empty() || flags.value().asset_ids.empty() ||
            flags.value().fields.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog develop-apply requires --from-asset, --asset-id, and "
                              "--fields");
        }
        if (!flags.value().develop_sets.empty() || !flags.value().develop_text_sets.empty() ||
            flags.value().exposure_ev || flags.value().saturation || flags.value().contrast ||
            flags.value().pick_white || flags.value().watermark_text ||
            !flags.value().from_xmp.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog develop-apply does not accept develop --set overrides");
        }
        auto fields = parse_develop_apply_fields(flags.value().fields);
        if (!fields)
            return fields.error();
        auto source_recipe = service.load_recipe(flags.value().from_asset);
        if (!source_recipe)
            return source_recipe.error();
        auto source = develop_from_recipe(source_recipe.value());
        if (!source)
            return source.error();
        DevelopApplyRequest request;
        request.source = std::move(source).value();
        request.fields = std::move(fields).value();
        request.asset_ids.reserve(flags.value().asset_ids.size());
        for (const auto asset_id : flags.value().asset_ids)
            request.asset_ids.emplace_back(asset_id);
        request.expected_revision = flags.value().expected_revision;
        auto applied = service.apply_develop_selection(request);
        if (!applied)
            return applied.error();
        JsonValue::Array items;
        items.reserve(applied.value().items.size());
        for (const auto &item : applied.value().items)
        {
            JsonValue::Object row{
                {"asset_id", item.asset_id},
                {"status", std::string(develop_apply_item_status_name(item.status))}};
            if (item.history_id)
                row.emplace("history_id", JsonValue::number(std::to_string(*item.history_id)));
            if (item.error)
                row.emplace("error", error_object(*item.error));
            items.emplace_back(std::move(row));
        }
        JsonValue::Array field_json;
        field_json.reserve(request.fields.size());
        for (const auto &field : request.fields)
            field_json.emplace_back(field);
        return JsonValue{JsonValue::Object{
            {"applied", JsonValue::number(std::to_string(applied.value().applied))},
            {"failed", JsonValue::number(std::to_string(applied.value().failed))},
            {"fields", std::move(field_json)},
            {"from_asset", std::string(flags.value().from_asset)},
            {"items", std::move(items)},
            {"revision", JsonValue::number(std::to_string(applied.value().revision))},
            {"skipped", JsonValue::number(std::to_string(applied.value().skipped))},
        }};
    }
    if (subcommand == "rate")
    {
        if (flags.value().asset_id.empty() || !flags.value().rating)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog rate requires --asset-id and --rating");
        }
        auto rated = service.set_rating(flags.value().asset_id, *flags.value().rating);
        if (!rated)
        {
            return rated.error();
        }
        return asset_to_json(rated.value());
    }
    if (subcommand == "refresh-metadata")
    {
        if (flags.value().asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog refresh-metadata requires --asset-id");
        }
        auto refreshed =
            service.refresh_capture_metadata(flags.value().asset_id, CancellationToken{});
        if (!refreshed)
        {
            return refreshed.error();
        }
        return asset_to_json(refreshed.value());
    }
    if (subcommand == "export-preset-save")
    {
        if (flags.value().output.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog export-preset-save requires --output");
        }
        auto options = resolved_export_options(flags.value());
        if (!options)
            return options.error();
        ExportPreset preset;
        preset.schema_version = kExportPresetSchemaVersion;
        preset.options = std::move(options).value();
        auto serialized = serialize_export_preset(preset);
        if (!serialized)
            return serialized.error();
        auto written = write_utf8_text_file_atomically(flags.value().output, serialized.value());
        if (!written)
            return written.error();
        return JsonValue{JsonValue::Object{
            {"format", std::string(export_format_name(preset.options.format))},
            {"output", std::string(flags.value().output)},
            {"schema", std::string(kExportPresetSchema)},
            {"schema_version", JsonValue::number(std::to_string(preset.schema_version))},
        }};
    }
    if (subcommand == "export-job-create")
    {
        if (flags.value().asset_ids.empty() || flags.value().output_directory.empty() ||
            flags.value().export_job.empty() || flags.value().job_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog export-job-create requires --asset-id, --output-dir, "
                              "--export-job, and --job-id");
        }
        auto options = resolved_export_options(flags.value());
        if (!options)
            return options.error();
        ExportBatchRequest request;
        request.asset_ids.reserve(flags.value().asset_ids.size());
        for (const auto asset_id : flags.value().asset_ids)
            request.asset_ids.emplace_back(asset_id);
        request.output_directory = std::string(flags.value().output_directory);
        if (!flags.value().filename_template.empty())
            request.filename_template = std::string(flags.value().filename_template);
        request.options = std::move(options).value();
        auto job = service.create_export_job(request, std::string(flags.value().job_id));
        if (!job)
            return job.error();
        auto serialized = serialize_export_job(job.value());
        if (!serialized)
            return serialized.error();
        auto written =
            write_utf8_text_file_atomically(flags.value().export_job, serialized.value());
        if (!written)
            return written.error();
        return JsonValue{JsonValue::Object{
            {"job_id", job.value().job_id},
            {"items", JsonValue::number(std::to_string(job.value().items.size()))},
            {"output", std::string(flags.value().export_job)},
            {"schema", std::string(kExportJobSchema)},
        }};
    }
    if (subcommand == "export-job-resume")
    {
        if (flags.value().export_job.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog export-job-resume requires --export-job");
        }
        auto text_body = read_utf8_text_file(flags.value().export_job, kExportJobFileMaxBytes);
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
        auto written =
            write_utf8_text_file_atomically(flags.value().export_job, serialized.value());
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
            {"output", std::string(flags.value().export_job)},
            {"pending", JsonValue::number(std::to_string(pending))},
        }};
    }
    if (subcommand == "export-batch")
    {
        if (flags.value().asset_ids.empty() || flags.value().output_directory.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog export-batch requires --asset-id and --output-dir");
        }
        auto options = resolved_export_options(flags.value());
        if (!options)
            return options.error();
        ExportBatchRequest request;
        request.asset_ids.reserve(flags.value().asset_ids.size());
        for (const auto asset_id : flags.value().asset_ids)
            request.asset_ids.emplace_back(asset_id);
        request.output_directory = std::string(flags.value().output_directory);
        if (!flags.value().filename_template.empty())
            request.filename_template = std::string(flags.value().filename_template);
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
        if (flags.value().asset_id.empty() || flags.value().output.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog export requires --asset-id and --output");
        }
        ExportRequest request;
        request.asset_id = std::string(flags.value().asset_id);
        request.output_path = std::string(flags.value().output);
        auto options = resolved_export_options(flags.value());
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
    if (subcommand == "tag")
    {
        if (flags.value().asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "catalog tag requires --asset-id");
        }
        auto asset = service.list_assets();
        if (!asset)
        {
            return asset.error();
        }
        const AssetRecord *selected = nullptr;
        for (const auto &item : asset.value())
        {
            if (item.id == flags.value().asset_id)
            {
                selected = &item;
                break;
            }
        }
        if (selected == nullptr)
        {
            return make_error(ErrorCode::kNotFound, "Asset does not exist",
                              {{"asset_id", std::string(flags.value().asset_id)}});
        }
        std::vector<std::string> tags = selected->tags;
        if (!flags.value().add.empty())
        {
            auto parsed = parse_tag_list(flags.value().add);
            if (!parsed)
            {
                return parsed.error();
            }
            for (auto &tag : parsed.value())
            {
                if (std::find(tags.begin(), tags.end(), tag) == tags.end())
                {
                    tags.push_back(std::move(tag));
                }
            }
        }
        if (!flags.value().remove.empty())
        {
            auto parsed = parse_tag_list(flags.value().remove);
            if (!parsed)
            {
                return parsed.error();
            }
            tags.erase(std::remove_if(tags.begin(), tags.end(),
                                      [&](const std::string &tag)
                                      {
                                          return std::find(parsed.value().begin(),
                                                           parsed.value().end(),
                                                           tag) != parsed.value().end();
                                      }),
                       tags.end());
        }
        if (!flags.value().add.empty() || !flags.value().remove.empty())
        {
            auto saved = service.set_tags(flags.value().asset_id, tags);
            if (!saved)
            {
                return saved.error();
            }
            return asset_to_json(saved.value());
        }
        return asset_to_json(*selected);
    }
    if (subcommand == "metadata")
    {
        if (flags.value().asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "catalog metadata requires --asset-id");
        }
        auto loaded = service.list_assets();
        if (!loaded)
        {
            return loaded.error();
        }
        const AssetRecord *selected = nullptr;
        for (const auto &item : loaded.value())
        {
            if (item.id == flags.value().asset_id)
            {
                selected = &item;
                break;
            }
        }
        if (selected == nullptr)
        {
            return make_error(ErrorCode::kNotFound, "Asset does not exist",
                              {{"asset_id", std::string(flags.value().asset_id)}});
        }
        WritableMetadata metadata = selected->metadata;
        bool write = false;
        const auto assign = [&](const std::string_view text, std::optional<std::string> &field)
        {
            if (!text.empty())
            {
                field = std::string(text);
                write = true;
            }
        };
        assign(flags.value().title, metadata.title);
        assign(flags.value().description, metadata.description);
        assign(flags.value().creator, metadata.creator);
        assign(flags.value().copyright, metadata.copyright);
        assign(flags.value().country, metadata.country);
        assign(flags.value().province_state, metadata.province_state);
        assign(flags.value().city, metadata.city);
        assign(flags.value().sublocation, metadata.sublocation);
        assign(flags.value().headline, metadata.headline);
        assign(flags.value().credit, metadata.credit);
        assign(flags.value().source, metadata.source);
        assign(flags.value().instructions, metadata.instructions);
        assign(flags.value().usage_terms, metadata.usage_terms);
        assign(flags.value().job_id, metadata.job_id);
        if (write)
        {
            auto saved = service.set_writable_metadata(flags.value().asset_id, metadata);
            if (!saved)
            {
                return saved.error();
            }
            return asset_to_json(saved.value());
        }
        return asset_to_json(*selected);
    }
    if (subcommand == "history")
    {
        if (flags.value().asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "catalog history requires --asset-id");
        }
        auto history = service.list_recipe_history(flags.value().asset_id);
        if (!history)
        {
            return history.error();
        }
        JsonValue::Array entries;
        for (const auto &entry : history.value())
        {
            entries.push_back(JsonValue::Object{
                {"id", JsonValue::number(std::to_string(entry.id))},
                {"kind", entry.kind},
                {"label", entry.label ? JsonValue{*entry.label} : JsonValue{nullptr}},
                {"seq", JsonValue::number(std::to_string(entry.seq))},
            });
        }
        return JsonValue{JsonValue::Object{
            {"asset_id", std::string(flags.value().asset_id)},
            {"history", std::move(entries)},
        }};
    }
    if (subcommand == "snapshot")
    {
        if (flags.value().asset_id.empty() || flags.value().label.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog snapshot requires --asset-id and --label");
        }
        auto saved = service.create_recipe_snapshot(flags.value().asset_id, flags.value().label);
        if (!saved)
        {
            return saved.error();
        }
        return asset_to_json(saved.value());
    }
    if (subcommand == "restore")
    {
        if (flags.value().asset_id.empty() || !flags.value().history_id)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog restore requires --asset-id and --history-id");
        }
        auto restored =
            service.restore_recipe_history(flags.value().asset_id, *flags.value().history_id);
        if (!restored)
        {
            return restored.error();
        }
        return asset_to_json(restored.value());
    }

    if (xmp_command)
        return run_catalog_xmp_command(service, subcommand, flags.value());
    if (editor_command)
        return run_catalog_editor_command(service, subcommand, flags.value());
    if (convert_command)
        return run_catalog_convert_command(service, subcommand, flags.value());
    if (subcommand == "keywords")
    {
        auto listed = service.list_keywords();
        if (!listed)
            return listed.error();
        JsonValue::Array rows;
        rows.reserve(listed.value().size());
        for (const auto &keyword : listed.value())
            rows.push_back(keyword_to_json(keyword));
        return JsonValue{JsonValue::Object{{"keywords", std::move(rows)}}};
    }
    if (subcommand == "keyword-create")
    {
        const auto name = !flags.value().keyword_name.empty() ? flags.value().keyword_name :
                                                                flags.value().set_name;
        if (name.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog keyword-create requires --keyword-name or --name");
        std::optional<std::string_view> parent;
        if (!flags.value().parent_id.empty())
            parent = flags.value().parent_id;
        auto created = service.create_keyword(name, parent, flags.value().expected_revision);
        if (!created)
            return created.error();
        return keyword_mutation_to_json(created.value());
    }
    if (subcommand == "keyword-rename")
    {
        if (flags.value().keyword_id.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog keyword-rename requires --keyword-id");
        const auto name = !flags.value().keyword_name.empty() ? flags.value().keyword_name :
                                                                flags.value().set_name;
        if (name.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog keyword-rename requires --keyword-name or --name");
        auto renamed =
            service.rename_keyword(flags.value().keyword_id, name, flags.value().expected_revision);
        if (!renamed)
            return renamed.error();
        return keyword_mutation_to_json(renamed.value());
    }
    if (subcommand == "keyword-move")
    {
        if (flags.value().keyword_id.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog keyword-move requires --keyword-id");
        std::optional<std::string_view> parent;
        if (!flags.value().parent_id.empty())
            parent = flags.value().parent_id;
        auto moved =
            service.move_keyword(flags.value().keyword_id, parent, flags.value().expected_revision);
        if (!moved)
            return moved.error();
        return keyword_mutation_to_json(moved.value());
    }
    if (subcommand == "keyword-delete")
    {
        if (flags.value().keyword_id.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog keyword-delete requires --keyword-id");
        auto deleted =
            service.delete_keyword(flags.value().keyword_id, flags.value().keyword_recursive,
                                   flags.value().expected_revision);
        if (!deleted)
            return deleted.error();
        return JsonValue{JsonValue::Object{
            {"keyword_id", std::string(flags.value().keyword_id)},
            {"revision", JsonValue::number(std::to_string(deleted.value()))},
        }};
    }

    if (subcommand == "ai-propose")
    {
        AiProposalKind kind = AiProposalKind::kGlobal;
        if (!flags.value().proposal_kind.empty())
        {
            auto parsed = parse_ai_proposal_kind(flags.value().proposal_kind);
            if (!parsed)
                return parsed.error();
            kind = parsed.value();
        }
        if (kind == AiProposalKind::kShootConsistency)
        {
            if (flags.value().reference_asset.empty())
                return make_error(
                    ErrorCode::kInvalidArgument,
                    "catalog ai-propose shoot-consistency requires --reference-asset");
            if (flags.value().destination_assets.empty())
                return make_error(
                    ErrorCode::kInvalidArgument,
                    "catalog ai-propose shoot-consistency requires --destination-assets");
            if (!flags.value().asset_id.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "shoot-consistency uses --reference-asset/--destination-assets, "
                                  "not --asset-id");
            AiShootConsistencyRequest request;
            request.reference_asset_id = std::string(flags.value().reference_asset);
            request.destination_asset_ids.reserve(flags.value().destination_assets.size());
            for (const auto destination : flags.value().destination_assets)
                request.destination_asset_ids.emplace_back(destination);
            request.user_initiated = flags.value().user_initiated;
            request.expected_catalog_revision = flags.value().expected_revision;
            if (!flags.value().provider_id.empty())
                request.provider_id = std::string(flags.value().provider_id);
            if (!flags.value().model_id.empty())
                request.model_id = std::string(flags.value().model_id);
            auto created = service.create_shoot_consistency_proposals(request);
            if (!created)
                return created.error();
            JsonValue::Array rows;
            rows.reserve(created.value().size());
            for (const auto &proposal : created.value())
                rows.push_back(ai_proposal_to_json(proposal));
            return JsonValue{JsonValue::Object{
                {"kind", "shoot-consistency"},
                {"proposals", std::move(rows)},
                {"reference_asset_id", request.reference_asset_id},
            }};
        }
        if (flags.value().asset_id.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog ai-propose requires --asset-id");
        AiProposalCreateRequest request;
        request.asset_id = std::string(flags.value().asset_id);
        request.user_initiated = flags.value().user_initiated;
        request.expected_catalog_revision = flags.value().expected_revision;
        request.kind = kind;
        if (!flags.value().semantic_label.empty())
            request.semantic_label = std::string(flags.value().semantic_label);
        if (!flags.value().provider_id.empty())
            request.provider_id = std::string(flags.value().provider_id);
        if (!flags.value().model_id.empty())
            request.model_id = std::string(flags.value().model_id);
        auto created = service.create_ai_proposal(request);
        if (!created)
            return created.error();
        return ai_proposal_to_json(created.value());
    }
    if (subcommand == "ai-proposal")
    {
        if (flags.value().proposal_id.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog ai-proposal requires --proposal-id");
        auto proposal = service.get_ai_proposal(flags.value().proposal_id);
        if (!proposal)
            return proposal.error();
        return ai_proposal_to_json(proposal.value());
    }
    if (subcommand == "ai-proposals")
    {
        std::optional<std::string_view> asset_id;
        if (!flags.value().asset_id.empty())
            asset_id = flags.value().asset_id;
        auto listed = service.list_ai_proposals(asset_id);
        if (!listed)
            return listed.error();
        JsonValue::Array rows;
        rows.reserve(listed.value().size());
        for (const auto &proposal : listed.value())
            rows.push_back(ai_proposal_to_json(proposal));
        return JsonValue{JsonValue::Object{{"proposals", std::move(rows)}}};
    }
    if (subcommand == "ai-proposal-apply")
    {
        if (flags.value().proposal_id.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog ai-proposal-apply requires --proposal-id");
        auto applied =
            service.apply_ai_proposal(flags.value().proposal_id, flags.value().expected_revision);
        if (!applied)
            return applied.error();
        return ai_proposal_apply_to_json(applied.value());
    }
    if (subcommand == "ai-proposal-reject")
    {
        if (flags.value().proposal_id.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog ai-proposal-reject requires --proposal-id");
        auto rejected = service.reject_ai_proposal(flags.value().proposal_id);
        if (!rejected)
            return rejected.error();
        return ai_proposal_to_json(rejected.value());
    }
    if (subcommand == "ai-proposal-cancel")
    {
        if (flags.value().proposal_id.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog ai-proposal-cancel requires --proposal-id");
        auto cancelled = service.cancel_ai_proposal(flags.value().proposal_id);
        if (!cancelled)
            return cancelled.error();
        return ai_proposal_to_json(cancelled.value());
    }

    return make_error(ErrorCode::kInvalidArgument, "Unknown catalog subcommand",
                      {{"subcommand", std::string(subcommand)}});
}

} // namespace ravo::cli_internal
