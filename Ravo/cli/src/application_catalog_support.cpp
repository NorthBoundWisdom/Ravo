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
[[nodiscard]] Result<std::unique_ptr<CatalogService>>
open_catalog_session(const EngineFacade &engine, const std::string_view path, const bool create)
{
    auto repository =
        create ? SqliteCatalogRepository::create(path) : SqliteCatalogRepository::open(path);
    if (!repository)
    {
        return repository.error();
    }
    auto cache = FilesystemPreviewCache::create(std::string(path) + ".preview");
    if (!cache)
    {
        return cache.error();
    }
    auto recovery = FilesystemRecoveryStore::create_for_catalog(path);
    if (!recovery)
    {
        return recovery.error();
    }
    auto service = std::make_unique<CatalogService>(
        engine, std::move(repository).value(), std::make_unique<QtRasterDecoder>(),
        std::move(cache).value(), std::move(recovery).value());
    auto resumed = service->sync_recovery(std::nullopt);
    if (!resumed)
    {
        return resumed.error();
    }
    return service;
}

[[nodiscard]] JsonValue optional_string_json(const std::optional<std::string> &value)
{
    if (!value)
    {
        return nullptr;
    }
    return *value;
}

[[nodiscard]] JsonValue asset_to_json(const AssetRecord &asset)
{
    JsonValue::Array tags;
    for (const auto &tag : asset.tags)
    {
        tags.push_back(tag);
    }
    JsonValue::Object metadata{
        {"city", optional_string_json(asset.metadata.city)},
        {"copyright", optional_string_json(asset.metadata.copyright)},
        {"country", optional_string_json(asset.metadata.country)},
        {"creator", optional_string_json(asset.metadata.creator)},
        {"description", optional_string_json(asset.metadata.description)},
        {"province_state", optional_string_json(asset.metadata.province_state)},
        {"sublocation", optional_string_json(asset.metadata.sublocation)},
        {"title", optional_string_json(asset.metadata.title)},
    };
    JsonValue captured_at{nullptr};
    if (asset.capture.captured_datetime)
    {
        captured_at = format_capture_datetime_iso(*asset.capture.captured_datetime);
    }
    JsonValue gps{nullptr};
    if (asset.capture.location)
    {
        JsonValue::Object gps_object{
            {"latitude",
             JsonValue::number(format_scaled_decimal(asset.capture.location->latitude_e6, 6))},
            {"longitude",
             JsonValue::number(format_scaled_decimal(asset.capture.location->longitude_e6, 6))},
        };
        if (asset.capture.location->altitude)
        {
            const auto &altitude = *asset.capture.location->altitude;
            std::int64_t signed_mm = static_cast<std::int64_t>(altitude.magnitude_mm);
            if (altitude.reference == CaptureAltitudeReference::kBelowSeaLevel)
            {
                signed_mm = -signed_mm;
            }
            gps_object.emplace("altitude_m",
                               JsonValue::number(format_scaled_decimal(signed_mm, 3)));
        }
        gps = std::move(gps_object);
    }
    JsonValue::Object capture{
        {"aperture", asset.capture.aperture ?
                         JsonValue::number(std::to_string(*asset.capture.aperture)) :
                         JsonValue{nullptr}},
        {"camera_make", optional_string_json(asset.capture.camera_make)},
        {"camera_model", optional_string_json(asset.capture.camera_model)},
        {"lens_make", optional_string_json(asset.capture.lens_make)},
        {"lens_model", optional_string_json(asset.capture.lens_model)},
        {"captured_at", std::move(captured_at)},
        {"focal_length_mm", asset.capture.focal_length_mm ?
                                JsonValue::number(std::to_string(*asset.capture.focal_length_mm)) :
                                JsonValue{nullptr}},
        {"gps", std::move(gps)},
        {"iso", asset.capture.iso ? JsonValue::number(std::to_string(*asset.capture.iso)) :
                                    JsonValue{nullptr}},
        {"shutter_s", asset.capture.shutter_s ?
                          JsonValue::number(std::to_string(*asset.capture.shutter_s)) :
                          JsonValue{nullptr}},
    };
    return JsonValue::Object{
        {"capture", std::move(capture)},
        {"color_label", std::string(color_label_name(asset.review.color_label))},
        {"has_edits", asset.has_edits},
        {"id", asset.id},
        {"import_state", asset.import_state},
        {"media_type", asset.media_type},
        {"metadata", std::move(metadata)},
        {"rating", JsonValue::number(std::to_string(asset.review.rating))},
        {"rejected", asset.review.rejected},
        {"source_asset_id", optional_string_json(asset.source_asset_id)},
        {"stack_count", JsonValue::number(std::to_string(asset.stack_count))},
        {"stack_id", optional_string_json(asset.stack_id)},
        {"stack_pick", asset.stack_pick},
        {"stack_position", JsonValue::number(std::to_string(asset.stack_position))},
        {"tags", std::move(tags)},
        {"uri", asset.normalized_uri},
        {"version_ordinal", JsonValue::number(std::to_string(asset.version_ordinal))},
    };
}

[[nodiscard]] JsonValue recovery_state_to_json(const AssetRecoveryState &state)
{
    return JsonValue::Object{
        {"asset_id", state.asset_id},
        {"generation", JsonValue::number(std::to_string(state.generation))},
        {"pending", state.pending()},
        {"synchronized_generation",
         JsonValue::number(std::to_string(state.synchronized_generation))},
    };
}

[[nodiscard]] JsonValue recovery_artifact_to_json(const RecoveryArtifact &artifact)
{
    return JsonValue::Object{
        {"asset_id", artifact.asset_id},
        {"bytes", JsonValue::number(std::to_string(artifact.bytes))},
        {"generation", JsonValue::number(std::to_string(artifact.generation))},
        {"path", artifact.path},
        {"sha256", artifact.sha256},
    };
}

[[nodiscard]] JsonValue backup_artifact_to_json(const CatalogBackupArtifact &artifact,
                                                const bool verified)
{
    return JsonValue::Object{
        {"catalog",
         JsonValue::Object{
             {"bytes", JsonValue::number(std::to_string(artifact.catalog.bytes))},
             {"catalog_id", artifact.catalog.catalog_id},
             {"path", artifact.catalog.path},
             {"revision", JsonValue::number(std::to_string(artifact.catalog.revision))},
             {"schema_version", JsonValue::number(std::to_string(artifact.catalog.schema_version))},
             {"sha256", artifact.catalog.sha256},
         }},
        {"created_unix_ms", JsonValue::number(std::to_string(artifact.created_unix_ms))},
        {"excludes", JsonValue::Array{JsonValue{"originals"}, JsonValue{"previews"}}},
        {"format_version", JsonValue::number(std::to_string(kCatalogBackupFormatVersion))},
        {"manifest", artifact.manifest_path},
        {"path", artifact.path},
        {"sidecar_bytes", JsonValue::number(std::to_string(artifact.sidecar_bytes))},
        {"sidecar_count", JsonValue::number(std::to_string(artifact.sidecar_count))},
        {"verified", verified},
    };
}

[[nodiscard]] JsonValue restore_result_to_json(const CatalogRestoreResult &result)
{
    return JsonValue::Object{
        {"backup", backup_artifact_to_json(result.source_backup, true)},
        {"catalog",
         JsonValue::Object{
             {"catalog_id", result.catalog.catalog_id},
             {"path", result.catalog.database_path},
             {"revision", JsonValue::number(std::to_string(result.catalog.revision))},
             {"schema_version", JsonValue::number(std::to_string(result.catalog.schema_version))},
         }},
        {"previews_rebuild_required", result.previews_rebuild_required},
        {"published", result.published},
        {"support_root", result.support_root},
    };
}

[[nodiscard]] JsonValue preview_rebuild_to_json(const PreviewRebuildResult &result)
{
    JsonValue::Array items;
    items.reserve(result.items.size());
    for (const auto &item : result.items)
    {
        JsonValue::Object value{
            {"asset_id", item.asset_id},
            {"browse_cache_path",
             item.browse_cache_path ? JsonValue{*item.browse_cache_path} : JsonValue{nullptr}},
            {"develop_cache_path",
             item.develop_cache_path ? JsonValue{*item.develop_cache_path} : JsonValue{nullptr}},
            {"status", item.error ? JsonValue{"failed"} : JsonValue{"rebuilt"}},
        };
        if (item.error)
            value.emplace("error", error_object(*item.error));
        else
            value.emplace("error", nullptr);
        items.emplace_back(std::move(value));
    }
    return JsonValue::Object{
        {"completed", JsonValue::number(std::to_string(result.completed))},
        {"failed", JsonValue::number(std::to_string(result.failed))},
        {"items", std::move(items)},
        {"succeeded", JsonValue::number(std::to_string(result.succeeded))},
        {"total", JsonValue::number(std::to_string(result.total))},
    };
}

[[nodiscard]] JsonValue backup_policy_to_json(const CatalogBackupPolicy &policy)
{
    return JsonValue::Object{
        {"destination_directory", policy.destination_directory},
        {"enabled", policy.enabled},
        {"interval_minutes", JsonValue::number(std::to_string(policy.interval_minutes))},
        {"last_backup_bytes", JsonValue::number(std::to_string(policy.last_backup_bytes))},
        {"last_error", policy.last_error ? JsonValue{*policy.last_error} : JsonValue{nullptr}},
        {"last_success_unix_ms", policy.last_success_unix_ms ? JsonValue::number(std::to_string(
                                                                   *policy.last_success_unix_ms)) :
                                                               JsonValue{nullptr}},
        {"next_run_unix_ms", policy.next_run_unix_ms ?
                                 JsonValue::number(std::to_string(*policy.next_run_unix_ms)) :
                                 JsonValue{nullptr}},
        {"retention_count", JsonValue::number(std::to_string(policy.retention_count))},
    };
}

[[nodiscard]] JsonValue backup_schedule_to_json(const CatalogBackupScheduleResult &result)
{
    JsonValue::Array removed;
    for (const auto &path : result.removed_backups)
        removed.emplace_back(path);
    JsonValue::Array retained;
    for (const auto &path : result.retained_unverified_paths)
        retained.emplace_back(path);
    return JsonValue::Object{
        {"backup",
         result.backup ? backup_artifact_to_json(*result.backup, true) : JsonValue{nullptr}},
        {"policy", backup_policy_to_json(result.policy)},
        {"ran", result.ran},
        {"removed_backups", std::move(removed)},
        {"retained_unverified_paths", std::move(retained)},
    };
}

[[nodiscard]] JsonValue folder_to_json(const FolderRecord &folder)
{
    return JsonValue::Object{
        {"asset_count", JsonValue::number(std::to_string(folder.asset_count))},
        {"depth", JsonValue::number(std::to_string(folder.depth))},
        {"display_name", folder.display_name},
        {"folder_id", folder.id.empty() ? JsonValue{nullptr} : JsonValue{folder.id}},
        {"missing", folder.missing},
        {"uri", folder.uri},
    };
}

[[nodiscard]] Result<JsonValue> library_set_to_json(const LibrarySetRecord &set)
{
    JsonValue::Object object{
        {"asset_count", JsonValue::number(std::to_string(set.asset_count))},
        {"created_unix_ms", JsonValue::number(std::to_string(set.created_unix_ms))},
        {"id", set.id},
        {"kind", std::string(library_set_kind_name(set.kind))},
        {"name", set.name},
        {"updated_unix_ms", JsonValue::number(std::to_string(set.updated_unix_ms))},
    };
    if (set.query)
    {
        auto serialized = serialize_library_query_document(*set.query);
        if (!serialized)
            return serialized.error();
        auto parsed = parse_json(serialized.value());
        if (!parsed)
            return parsed.error();
        object.emplace("query", std::move(parsed).value());
    }
    else
        object.emplace("query", JsonValue{nullptr});
    return JsonValue{std::move(object)};
}

[[nodiscard]] Result<JsonValue> library_set_mutation_to_json(const LibrarySetMutation &mutation)
{
    auto set = library_set_to_json(mutation.set);
    if (!set)
        return set.error();
    return JsonValue{JsonValue::Object{
        {"revision", JsonValue::number(std::to_string(mutation.revision))},
        {"set", std::move(set).value()},
    }};
}

[[nodiscard]] JsonValue library_stack_to_json(const LibraryStackRecord &stack)
{
    JsonValue::Array members;
    members.reserve(stack.member_ids.size());
    for (const auto &id : stack.member_ids)
        members.push_back(id);
    return JsonValue::Object{
        {"created_unix_ms", JsonValue::number(std::to_string(stack.created_unix_ms))},
        {"id", stack.id},
        {"member_ids", std::move(members)},
        {"pick_asset_id", stack.pick_asset_id},
    };
}

[[nodiscard]] JsonValue library_stack_mutation_to_json(const LibraryStackMutation &mutation)
{
    return JsonValue::Object{
        {"revision", JsonValue::number(std::to_string(mutation.revision))},
        {"stack", library_stack_to_json(mutation.stack)},
    };
}

[[nodiscard]] JsonValue ai_proposal_field_to_json(const AiProposalFieldChange &change)
{
    JsonValue::Object object{
        {"field", change.field},
        {"value", JsonValue::number(std::to_string(change.value))},
    };
    if (change.confidence)
        object.emplace("confidence", JsonValue::number(std::to_string(*change.confidence)));
    else
        object.emplace("confidence", nullptr);
    return object;
}

[[nodiscard]] JsonValue ai_proposal_alternative_to_json(const AiProposalAlternative &alternative)
{
    JsonValue::Array fields;
    fields.reserve(alternative.fields.size());
    for (const auto &change : alternative.fields)
        fields.push_back(ai_proposal_field_to_json(change));
    JsonValue::Object object{
        {"label", alternative.label},
        {"fields", std::move(fields)},
    };
    if (alternative.confidence)
        object.emplace("confidence", JsonValue::number(std::to_string(*alternative.confidence)));
    else
        object.emplace("confidence", nullptr);
    return object;
}

JsonValue ai_proposal_to_json(const AiProposal &proposal)
{
    JsonValue::Array fields;
    fields.reserve(proposal.fields.size());
    for (const auto &change : proposal.fields)
        fields.push_back(ai_proposal_field_to_json(change));
    JsonValue::Array diffs;
    diffs.reserve(proposal.field_diff.size());
    for (const auto &diff : proposal.field_diff)
        diffs.push_back(JsonValue::Object{{"field", diff.field}, {"value", diff.value}});
    JsonValue::Array alternatives;
    alternatives.reserve(proposal.alternatives.size());
    for (const auto &alternative : proposal.alternatives)
        alternatives.push_back(ai_proposal_alternative_to_json(alternative));
    JsonValue::Object parameters;
    for (const auto &[key, value] : proposal.provider.parameters)
        parameters.emplace(key, value);
    JsonValue::Object provider{
        {"model_id", proposal.provider.model_id},
        {"model_version", proposal.provider.model_version},
        {"parameters", std::move(parameters)},
        {"provider_id", proposal.provider.provider_id},
        {"weight_content_hash", proposal.provider.weight_content_hash},
    };
    JsonValue::Object object{
        {"alternatives", std::move(alternatives)},
        {"asset_id", proposal.asset_id},
        {"contract_version", proposal.contract_version},
        {"created_unix_ms", JsonValue::number(std::to_string(proposal.created_unix_ms))},
        {"field_diff", std::move(diffs)},
        {"fields", std::move(fields)},
        {"id", proposal.id},
        {"kind", std::string(ai_proposal_kind_name(proposal.kind))},
        {"observed_catalog_revision",
         JsonValue::number(std::to_string(proposal.observed_catalog_revision))},
        {"observed_recovery_generation",
         JsonValue::number(std::to_string(proposal.observed_recovery_generation))},
        {"provider", std::move(provider)},
        {"status", std::string(ai_proposal_status_name(proposal.status))},
    };
    if (proposal.semantic_label)
        object.emplace("semantic_label", *proposal.semantic_label);
    else
        object.emplace("semantic_label", nullptr);
    if (proposal.applied_history_id)
        object.emplace("applied_history_id",
                       JsonValue::number(std::to_string(*proposal.applied_history_id)));
    else
        object.emplace("applied_history_id", nullptr);
    return object;
}

JsonValue ai_proposal_apply_to_json(const AiProposalApplyResult &result)
{
    JsonValue::Object object{
        {"asset", asset_to_json(result.asset)},
        {"proposal", ai_proposal_to_json(result.proposal)},
        {"revision", JsonValue::number(std::to_string(result.revision))},
    };
    if (result.history_id)
        object.emplace("history_id", JsonValue::number(std::to_string(*result.history_id)));
    else
        object.emplace("history_id", nullptr);
    return object;
}

JsonValue keyword_to_json(const KeywordRecord &keyword)
{
    JsonValue::Object object{
        {"id", keyword.id},
        {"name", keyword.name},
        {"path", keyword.path},
        {"depth", JsonValue::number(std::to_string(keyword.depth))},
        {"created_unix_ms", JsonValue::number(std::to_string(keyword.created_unix_ms))},
        {"updated_unix_ms", JsonValue::number(std::to_string(keyword.updated_unix_ms))},
    };
    if (keyword.parent_id)
        object.emplace("parent_id", *keyword.parent_id);
    else
        object.emplace("parent_id", nullptr);
    return object;
}

JsonValue keyword_mutation_to_json(const KeywordMutation &mutation)
{
    return JsonValue::Object{
        {"keyword", keyword_to_json(mutation.keyword)},
        {"revision", JsonValue::number(std::to_string(mutation.revision))},
    };
}

[[nodiscard]] JsonValue asset_version_mutation_to_json(const AssetVersionMutation &mutation)
{
    return JsonValue::Object{
        {"asset", asset_to_json(mutation.version)},
        {"revision", JsonValue::number(std::to_string(mutation.revision))},
    };
}

[[nodiscard]] JsonValue folder_relink_to_json(const FolderRelinkResult &result)
{
    return JsonValue::Object{
        {"asset_count", JsonValue::number(std::to_string(result.asset_count))},
        {"folder_id", result.folder_id},
        {"previous_uri", result.previous_uri},
        {"recovery_pending", JsonValue::number(std::to_string(result.recovery_pending))},
        {"replacement_uri", result.replacement_uri},
    };
}

[[nodiscard]] Result<JsonValue> probe_statistics_json(const PreviewResult &preview)
{
    RasterBuffer raster;
    raster.width = preview.width;
    raster.height = preview.height;
    raster.srgb = preview.rgb;
    raster.color_profile = preview.color_profile;
    auto histogram = collect_rgb_histogram(raster);
    if (!histogram)
    {
        return histogram.error();
    }

    const std::uint64_t pixels = static_cast<std::uint64_t>(preview.width) * preview.height;
    const std::array<const std::array<std::uint32_t, kRgbHistogramBins> *, 3> channels{
        &histogram.value().red,
        &histogram.value().green,
        &histogram.value().blue,
    };
    std::array<std::uint64_t, 3> sums{};
    std::array<std::uint32_t, 3> minima{};
    std::array<std::uint32_t, 3> maxima{};
    std::array<std::uint32_t, 3> zeros{};
    std::array<std::uint32_t, 3> full{};
    for (std::size_t channel = 0; channel < channels.size(); ++channel)
    {
        bool found = false;
        for (std::uint32_t bin = 0; bin < kRgbHistogramBins; ++bin)
        {
            const auto count = (*channels[channel])[bin];
            sums[channel] += static_cast<std::uint64_t>(bin) * count;
            if (count != 0)
            {
                if (!found)
                {
                    minima[channel] = bin;
                    found = true;
                }
                maxima[channel] = bin;
            }
        }
        zeros[channel] = (*channels[channel])[0];
        full[channel] = (*channels[channel])[kRgbHistogramBins - 1U];
    }

    const auto integer_array = [](const auto &values)
    {
        JsonValue::Array result;
        result.reserve(values.size());
        for (const auto value : values)
        {
            result.push_back(JsonValue::number(std::to_string(value)));
        }
        return result;
    };
    JsonValue::Array means;
    means.reserve(sums.size());
    for (const auto sum : sums)
    {
        means.push_back(JsonValue::number(
            std::to_string(static_cast<double>(sum) / static_cast<double>(pixels))));
    }
    const double luma_mean =
        (0.2126 * static_cast<double>(sums[0]) + 0.7152 * static_cast<double>(sums[1]) +
         0.0722 * static_cast<double>(sums[2])) /
        static_cast<double>(pixels);
    return JsonValue{JsonValue::Object{
        {"channel_full_counts", integer_array(full)},
        {"channel_maxima", integer_array(maxima)},
        {"channel_means", std::move(means)},
        {"channel_minima", integer_array(minima)},
        {"channel_sums", integer_array(sums)},
        {"channel_zero_counts", integer_array(zeros)},
        {"display_luma_mean", JsonValue::number(std::to_string(luma_mean))},
        {"histogram_peak", JsonValue::number(std::to_string(histogram.value().max_count))},
        {"pixels", JsonValue::number(std::to_string(pixels))},
    }};
}

[[nodiscard]] JsonValue crs_omissions_json(const std::vector<CrsOmission> &omitted)
{
    JsonValue::Array items;
    for (const auto &item : omitted)
    {
        items.push_back(JsonValue{JsonValue::Object{
            {"key", item.key},
            {"reason", item.reason},
            {"value", item.value},
        }});
    }
    return JsonValue{std::move(items)};
}

[[nodiscard]] JsonValue develop_fields_json()
{
    JsonValue::Array fields;
    for (const auto &field : list_develop_set_fields())
    {
        JsonValue::Object item{{"kind", std::string(develop_set_field_kind_name(field.kind))},
                               {"name", field.name}};
        if (field.minimum)
        {
            item.emplace("minimum", JsonValue::number(std::to_string(*field.minimum)));
        }
        if (field.maximum)
        {
            item.emplace("maximum", JsonValue::number(std::to_string(*field.maximum)));
        }
        fields.push_back(JsonValue{std::move(item)});
    }
    JsonValue::Array prefixes;
    for (const auto prefix : develop_set_field_prefixes())
    {
        prefixes.push_back(JsonValue{JsonValue::Object{
            {"kind", "number"},
            {"prefix", std::string(prefix)},
        }});
    }
    return JsonValue{JsonValue::Object{
        {"fields", std::move(fields)},
        {"prefixes", std::move(prefixes)},
        {"set", "--set name=value"},
        {"set_text", "--set-text name=value"},
        {"watermark_text", "--watermark-text"},
    }};
}

[[nodiscard]] Result<PerspectiveAnalysisMode>
perspective_analysis_mode(const std::string_view value)
{
    if (value == "vertical")
        return PerspectiveAnalysisMode::kVertical;
    if (value == "horizontal")
        return PerspectiveAnalysisMode::kHorizontal;
    if (value == "full")
        return PerspectiveAnalysisMode::kFull;
    return make_error(ErrorCode::kInvalidArgument, "Perspective analysis mode is unsupported",
                      {{"mode", std::string(value)}});
}

[[nodiscard]] Result<JsonValue>
run_perspective_analysis(const EngineFacade &engine,
                         const std::span<const std::string_view> positional)
{
    if (positional.size() != 3U && positional.size() != 5U)
        return make_error(
            ErrorCode::kInvalidArgument,
            "Usage: ravo perspective analyze <input> [--mode vertical|horizontal|full]");
    if (positional[0] != "perspective" || positional[1] != "analyze")
        return make_error(ErrorCode::kInvalidArgument, "Unknown Perspective command");
    std::string_view mode_name = "full";
    if (positional.size() == 5U)
    {
        if (positional[3] != "--mode")
            return make_error(ErrorCode::kInvalidArgument, "Unknown Perspective option",
                              {{"option", std::string(positional[3])}});
        mode_name = positional[4];
    }
    auto mode = perspective_analysis_mode(mode_name);
    if (!mode)
        return mode.error();
    const CancellationToken cancellation;
    constexpr std::uint32_t kAnalysisRenderMaxEdge = 900U;
    RasterBuffer raster;
    QtRasterDecoder raster_decoder;
    auto decoded = raster_decoder.decode(positional[2], kAnalysisRenderMaxEdge, cancellation);
    if (decoded)
    {
        if (decoded.value().pixel_format != RasterPixelFormat::kRgb8)
            return make_error(ErrorCode::kUnsupported,
                              "Perspective analysis requires an RGB8 raster",
                              {{"reason", "unsupported_analysis_pixel_format"}});
        raster.width = decoded.value().width;
        raster.height = decoded.value().height;
        raster.source_width = decoded.value().source_width;
        raster.source_height = decoded.value().source_height;
        raster.color_profile = decoded.value().color_profile;
        raster.srgb = std::move(decoded).value().rgb;
    }
    else
    {
        if (decoded.error().code != ErrorCode::kUnsupported)
            return decoded.error();
        auto inspection = engine.inspect(positional[2], cancellation);
        if (!inspection)
            return inspection.error();
        if (inspection.value().width == 0U || inspection.value().height == 0U)
            return make_error(ErrorCode::kValidation,
                              "Perspective input dimensions are unavailable",
                              {{"reason", "invalid_dimensions"}});
        const double scale = std::min(
            1.0,
            static_cast<double>(kAnalysisRenderMaxEdge) /
                static_cast<double>(std::max(inspection.value().width, inspection.value().height)));
        const auto width = std::max<std::uint32_t>(
            16U, static_cast<std::uint32_t>(std::lround(inspection.value().width * scale)));
        const auto height = std::max<std::uint32_t>(
            16U, static_cast<std::uint32_t>(std::lround(inspection.value().height * scale)));
        DevelopParams develop;
        auto recipe = recipe_from_develop(
            {"perspective-analysis", std::string(positional[2]), std::nullopt}, develop);
        if (!recipe)
            return recipe.error();
        RenderRequest request;
        request.asset = recipe.value().asset;
        request.recipe = std::move(recipe).value();
        request.output_width = width;
        request.output_height = height;
        request.memory_budget_bytes = 1024ULL * 1024ULL * 1024ULL;
        request.worker_count = 1U;
        request.deterministic = true;
        request.cancellation = cancellation;
        request.correlation_id = "perspective-analysis";
        auto rendered = engine.render_to_image(request);
        if (!rendered)
            return rendered.error();
        raster.width = rendered.value().width;
        raster.height = rendered.value().height;
        raster.source_width = raster.width;
        raster.source_height = raster.height;
        raster.color_profile = rendered.value().color_profile;
        raster.srgb = std::move(rendered).value().rgb;
    }
    auto analysis = engine.analyze_perspective(raster, mode.value(), cancellation);
    if (!analysis)
        return analysis.error();

    const auto &params = analysis.value().params;
    JsonValue::Array lines;
    lines.reserve(analysis.value().lines.size());
    const double normalized_width = static_cast<double>(std::max(1U, raster.width - 1U));
    const double normalized_height = static_cast<double>(std::max(1U, raster.height - 1U));
    for (const auto &line : analysis.value().lines)
    {
        lines.emplace_back(JsonValue::Object{
            {"orientation", line.orientation == PerspectiveGuideOrientation::kVertical ?
                                "vertical" :
                                "horizontal"},
            {"weight", JsonValue::number(std::to_string(line.weight))},
            {"x1", JsonValue::number(std::to_string(line.x1 / normalized_width))},
            {"x2", JsonValue::number(std::to_string(line.x2 / normalized_width))},
            {"y1", JsonValue::number(std::to_string(line.y1 / normalized_height))},
            {"y2", JsonValue::number(std::to_string(line.y2 / normalized_height))},
        });
    }
    return JsonValue{JsonValue::Object{
        {"algorithm", "bounded_hough_robust_fit_v1"},
        {"analyzed_height", JsonValue::number(std::to_string(analysis.value().analyzed_height))},
        {"analyzed_width", JsonValue::number(std::to_string(analysis.value().analyzed_width))},
        {"horizontal_line_count",
         JsonValue::number(std::to_string(analysis.value().horizontal_line_count))},
        {"input", std::string(positional[2])},
        {"lines", std::move(lines)},
        {"mode", std::string(mode_name)},
        {"params",
         JsonValue::Object{
             {"constrain_crop", params.constrain_crop},
             {"horizontal_shift", JsonValue::number(std::to_string(params.horizontal_shift))},
             {"interpolation", params.interpolation},
             {"rotation_degrees", JsonValue::number(std::to_string(params.rotation_degrees))},
             {"shear", JsonValue::number(std::to_string(params.shear))},
             {"vertical_shift", JsonValue::number(std::to_string(params.vertical_shift))},
         }},
        {"residual_degrees", JsonValue::number(std::to_string(analysis.value().residual_degrees))},
        {"vertical_line_count",
         JsonValue::number(std::to_string(analysis.value().vertical_line_count))},
    }};
}

[[nodiscard]] JsonValue camera_noise_profile_json(const CameraNoiseProfile &profile,
                                                  const std::string_view path)
{
    return JsonValue::Object{
        {"fit_policy", std::string(kCameraNoiseFitPolicy)},
        {"gaussian_variance", JsonValue::number(std::to_string(profile.fit.gaussian_variance))},
        {"input_sample_count", JsonValue::number(std::to_string(profile.fit.input_sample_count))},
        {"iso", JsonValue::number(std::to_string(profile.identity.iso))},
        {"make", profile.identity.make},
        {"model", profile.identity.model},
        {"path", std::string(path)},
        {"payload_sha256", profile.payload_sha256},
        {"poisson_slope", JsonValue::number(std::to_string(profile.fit.poisson_slope))},
        {"retained_sample_count",
         JsonValue::number(std::to_string(profile.fit.retained_sample_count))},
        {"schema", std::string(kCameraNoiseProfileSchema)},
        {"source_samples_sha256", profile.source_samples_sha256},
        {"units", std::string(kCameraNoiseSignalUnits)},
        {"version", JsonValue::number(std::to_string(kCameraNoiseProfileSchemaVersion))},
        {"weighted_r_squared", JsonValue::number(std::to_string(profile.fit.weighted_r_squared))},
        {"weighted_rmse", JsonValue::number(std::to_string(profile.fit.weighted_rmse))},
    };
}

[[nodiscard]] Result<JsonValue>
run_noise_command(const std::span<const std::string_view> positional)
{
    if (positional.size() == 3U && positional[1] == "inspect")
    {
        auto text = read_utf8_text_file(positional[2], kCameraNoiseDocumentMaximumBytes);
        if (!text)
            return text.error();
        auto profile = parse_camera_noise_profile_json(text.value());
        if (!profile)
            return profile.error();
        return camera_noise_profile_json(profile.value(), positional[2]);
    }
    if (positional.size() != 5U || positional[1] != "calibrate" || positional[3] != "--output")
        return make_error(ErrorCode::kInvalidArgument,
                          "Usage: ravo noise <calibrate <samples.json> --output <profile.json>|"
                          "inspect <profile.json>>");

    auto text = read_utf8_text_file(positional[2], kCameraNoiseDocumentMaximumBytes);
    if (!text)
        return text.error();
    auto document = parse_camera_noise_calibration_json(text.value());
    if (!document)
        return document.error();
    auto fit = fit_camera_noise(document.value().samples, CancellationToken{});
    if (!fit)
        return fit.error();
    auto source_sha = camera_noise_calibration_sha256(document.value());
    if (!source_sha)
        return source_sha.error();
    auto serialized = serialize_camera_noise_profile_json(document.value().identity, fit.value(),
                                                          source_sha.value());
    if (!serialized)
        return serialized.error();
    auto published = publish_text_artifact_no_replace(positional[4], serialized.value());
    if (!published)
        return published.error();
    auto profile = parse_camera_noise_profile_json(serialized.value());
    if (!profile)
        return profile.error();
    return camera_noise_profile_json(profile.value(), positional[4]);
}

} // namespace ravo::cli_internal
