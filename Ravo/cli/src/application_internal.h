#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ravo/adapters/crs_xmp.h"
#include "ravo/cli/application.h"
#include "ravo/foundation/json.h"
#include "ravo/recipe/develop.h"
#include "ravo/services/catalog_service.h"

namespace ravo::cli_internal
{

[[nodiscard]] JsonValue error_object(const TaskError &error);
[[nodiscard]] Result<std::uint32_t> parse_dimension(std::string_view text, std::string_view option);

struct CatalogCliArguments
{
    bool baseline = false;
    std::string_view catalog;
    std::vector<std::string_view> inputs;
    std::string_view import_mode;
    std::string_view import_destination;
    std::string_view import_organization;
    std::string_view import_preview;
    std::string_view import_filename_template;
    std::string_view import_second_copy;
    bool import_recursive = true;
    std::string_view asset_id;
    std::vector<std::string_view> asset_ids;
    std::optional<int> rating;
    std::optional<double> exposure_ev;
    std::optional<double> saturation;
    std::optional<double> contrast;
    std::optional<std::uint32_t> max_edge;
    std::vector<std::pair<std::string, double>> develop_sets;
    std::vector<std::pair<std::string, std::string>> develop_text_sets;
    std::optional<std::pair<double, double>> pick_white;
    std::optional<std::string_view> watermark_text;
    std::string_view from_xmp;
    std::string_view from_asset;
    std::string_view fields;
    std::string_view output;
    std::string_view output_directory;
    std::string_view filename_template;
    std::string_view format;
    std::string_view metadata_mode;
    std::string_view quality;
    std::string_view jpeg_subsampling;
    std::string_view tiff_sample_type;
    std::string_view tiff_compression;
    std::string_view tiff_compression_level;
    std::string_view tiff_resolution_dpi;
    bool tiff_grayscale_if_neutral = false;
    std::string_view png_bit_depth;
    std::string_view png_compression;
    std::string_view tag;
    std::string_view add;
    std::string_view remove;
    std::string_view title;
    std::string_view description;
    std::string_view creator;
    std::string_view copyright;
    std::string_view label;
    std::string_view backup;
    std::string_view schedule_directory;
    std::string_view schedule_interval_minutes;
    std::string_view schedule_retention_count;
    std::string_view schedule_enabled;
    std::string_view folder_id;
    std::string_view folder_uri;
    std::string_view replacement_directory;
    std::string_view set_id;
    std::string_view set_name;
    std::string_view set_kind;
    std::string_view query_json;
    std::string_view stack_id;
    std::string_view pick_id;
    bool stack_expanded = false;
    std::optional<std::int64_t> expected_revision;
    std::optional<std::int64_t> history_id;
};

struct AppliedDevelopOverride
{
    std::string name;
    std::variant<double, std::string> value;
};

[[nodiscard]] Result<int> parse_int_flag(std::string_view text, std::string_view option);
[[nodiscard]] Result<std::uint64_t> parse_uint64_flag(std::string_view text,
                                                      std::string_view option);
[[nodiscard]] Result<double> parse_double_flag(std::string_view text, std::string_view option);
[[nodiscard]] Result<CatalogCliArguments>
parse_catalog_flags(std::span<const std::string_view> positional);
[[nodiscard]] Result<ExportFormat> resolved_export_format(const CatalogCliArguments &flags);
[[nodiscard]] Result<ExportOptions> resolved_export_options(const CatalogCliArguments &flags);
[[nodiscard]] Result<void> reject_scoped_export_options(const CatalogCliArguments &flags,
                                                        std::string_view subcommand);
[[nodiscard]] Result<std::vector<AppliedDevelopOverride>>
apply_develop_overrides(DevelopParams &params, const CatalogCliArguments &flags);

[[nodiscard]] Result<std::unique_ptr<CatalogService>>
open_catalog_session(const EngineFacade &engine, std::string_view path, bool create);
[[nodiscard]] JsonValue asset_to_json(const AssetRecord &asset);
[[nodiscard]] JsonValue recovery_state_to_json(const AssetRecoveryState &state);
[[nodiscard]] JsonValue recovery_artifact_to_json(const RecoveryArtifact &artifact);
[[nodiscard]] JsonValue backup_artifact_to_json(const CatalogBackupArtifact &artifact,
                                                bool verified);
[[nodiscard]] JsonValue restore_result_to_json(const CatalogRestoreResult &result);
[[nodiscard]] JsonValue preview_rebuild_to_json(const PreviewRebuildResult &result);
[[nodiscard]] JsonValue backup_policy_to_json(const CatalogBackupPolicy &policy);
[[nodiscard]] JsonValue backup_schedule_to_json(const CatalogBackupScheduleResult &result);
[[nodiscard]] JsonValue folder_to_json(const FolderRecord &folder);
[[nodiscard]] Result<JsonValue> library_set_to_json(const LibrarySetRecord &set);
[[nodiscard]] Result<JsonValue> library_set_mutation_to_json(const LibrarySetMutation &mutation);
[[nodiscard]] JsonValue library_stack_mutation_to_json(const LibraryStackMutation &mutation);
[[nodiscard]] JsonValue asset_version_mutation_to_json(const AssetVersionMutation &mutation);
[[nodiscard]] JsonValue folder_relink_to_json(const FolderRelinkResult &result);
[[nodiscard]] Result<JsonValue> probe_statistics_json(const PreviewResult &preview);
[[nodiscard]] JsonValue crs_omissions_json(const std::vector<CrsOmission> &omitted);
[[nodiscard]] JsonValue develop_fields_json();
[[nodiscard]] Result<JsonValue>
run_perspective_analysis(const EngineFacade &engine, std::span<const std::string_view> positional);
[[nodiscard]] Result<JsonValue> run_noise_command(std::span<const std::string_view> positional);
[[nodiscard]] bool ends_with_png(std::string_view path) noexcept;
[[nodiscard]] Result<JsonValue> run_studio_command(const EngineFacade &engine,
                                                   std::span<const std::string_view> positional);
[[nodiscard]] Result<JsonValue> run_catalog_command(const EngineFacade &engine,
                                                    std::span<const std::string_view> positional);

} // namespace ravo::cli_internal
