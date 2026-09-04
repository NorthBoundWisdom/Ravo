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
    std::optional<std::uint32_t> max_width;
    std::optional<std::uint32_t> max_height;
    bool output_sharpen = false;
    std::string_view sharpen_amount;
    std::string_view sharpen_radius;
    std::string_view sharpen_threshold;
    bool delivery_watermark = false;
    std::string_view delivery_watermark_text;
    std::string_view delivery_watermark_opacity;
    std::string_view delivery_watermark_scale;
    std::string_view delivery_watermark_alignment;
    bool delivery_frame = false;
    std::string_view delivery_frame_size;
    std::string_view delivery_frame_border; // r,g,b 0-1 joined by commas optional; or leave default
    bool delivery_color = false;
    std::string_view delivery_output_profile;
    std::string_view delivery_rendering_intent;
    std::string_view export_preset;
    std::string_view export_job;
    std::string_view job_id;
    std::optional<PreviewNormRect> roi;
    std::vector<std::pair<std::string, double>> develop_sets;
    std::vector<std::pair<std::string, std::string>> develop_text_sets;
    std::optional<std::pair<double, double>> pick_white;
    std::optional<std::string_view> watermark_text;
    std::string_view from_xmp;
    std::string_view xmp_path;
    std::string_view xmp_resolve;
    std::string_view editor_id;
    std::string_view editor_version;
    bool editor_auto_stack = false;
    bool editor_invoke_os_open = false;
    std::string_view foreign_source;
    std::string_view foreign_source_kind;
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
    std::string_view camera;
    std::string_view camera_make;
    std::string_view camera_model;
    std::string_view lens_make;
    std::string_view lens_model;
    std::string_view focal_length_mm;
    std::string_view captured_local_date;
    std::string_view captured_after_unix_s;
    std::string_view captured_before_unix_s;
    std::string_view add;
    std::string_view remove;
    std::string_view title;
    std::string_view description;
    std::string_view creator;
    std::string_view copyright;
    std::string_view country;
    std::string_view province_state;
    std::string_view city;
    std::string_view sublocation;
    std::string_view headline;
    std::string_view credit;
    std::string_view source;
    std::string_view instructions;
    std::string_view usage_terms;
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
    std::string_view keyword_id;
    std::string_view keyword_name;
    std::string_view parent_id;
    bool keyword_recursive = false;
    std::string_view set_name;
    std::string_view set_kind;
    std::string_view query_json;
    std::string_view stack_id;
    std::string_view pick_id;
    bool stack_expanded = false;
    std::string_view proposal_id;
    std::string_view provider_id;
    std::string_view model_id;
    std::string_view proposal_kind;
    std::string_view semantic_label;
    std::string_view reference_asset;
    std::vector<std::string_view> destination_assets;
    bool user_initiated = false;
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
[[nodiscard]] JsonValue keyword_to_json(const KeywordRecord &keyword);
[[nodiscard]] JsonValue keyword_mutation_to_json(const KeywordMutation &mutation);
[[nodiscard]] JsonValue ai_proposal_to_json(const AiProposal &proposal);
[[nodiscard]] JsonValue ai_proposal_apply_to_json(const AiProposalApplyResult &result);
[[nodiscard]] JsonValue asset_version_mutation_to_json(const AssetVersionMutation &mutation);
[[nodiscard]] JsonValue folder_relink_to_json(const FolderRelinkResult &result);
[[nodiscard]] Result<JsonValue> probe_statistics_json(const PreviewResult &preview);
[[nodiscard]] JsonValue crs_omissions_json(const std::vector<CrsOmission> &omitted);
[[nodiscard]] JsonValue develop_fields_json();
[[nodiscard]] Result<JsonValue> run_catalog_xmp_command(CatalogService &service,
                                                        std::string_view subcommand,
                                                        const CatalogCliArguments &flags);
[[nodiscard]] Result<JsonValue> run_catalog_editor_command(CatalogService &service,
                                                           std::string_view subcommand,
                                                           const CatalogCliArguments &flags);
[[nodiscard]] Result<LibraryQuery> build_library_query(const CatalogCliArguments &flags);
[[nodiscard]] Result<JsonValue> run_catalog_facets_command(CatalogService &service,
                                                           const CatalogCliArguments &flags);
[[nodiscard]] Result<JsonValue> run_catalog_convert_command(CatalogService &service,
                                                            std::string_view subcommand,
                                                            const CatalogCliArguments &flags);
[[nodiscard]] Result<JsonValue>
run_perspective_analysis(const EngineFacade &engine, std::span<const std::string_view> positional);
[[nodiscard]] Result<JsonValue> run_noise_command(std::span<const std::string_view> positional);
[[nodiscard]] bool ends_with_png(std::string_view path) noexcept;
[[nodiscard]] Result<JsonValue> run_studio_command(const EngineFacade &engine,
                                                   std::span<const std::string_view> positional);
[[nodiscard]] Result<JsonValue> run_catalog_command(const EngineFacade &engine,
                                                    std::span<const std::string_view> positional);

} // namespace ravo::cli_internal
