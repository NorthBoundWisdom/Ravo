#include "ravo/services/catalog_service.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <new>
#include <set>

#include "catalog_internal.h"
#include "catalog_service_internal.h"
#include "ravo/adapters/text_file.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/json.h"
#include "ravo/recipe/color_output.h"
#include "ravo/services/artifact_publication.h"

namespace ravo
{
namespace
{
struct MergeSource
{
    AssetRecord asset;
    std::string path;
    std::string sha256;
    Recipe baseline;
    std::string observed_recipe;
};
void cleanup_owned(const std::vector<std::string> &paths, TaskError &error)
{
    for (auto it = paths.rbegin(); it != paths.rend(); ++it)
    {
        std::error_code ec;
        std::filesystem::remove(utf8_path(*it), ec);
        if (ec)
        {
            error.context.insert_or_assign("cleanup_failed", "true");
            error.context.insert_or_assign("cleanup_path", *it);
            error.context.insert_or_assign("cleanup_detail", ec.message());
        }
    }
}
Result<void> unchanged(const MergeSource &source, const CancellationToken &cancellation)
{
    auto identity = read_file_identity(source.path);
    if (!identity)
        return identity.error();
    auto digest = sha256_file_hex(source.path, cancellation);
    if (!digest)
        return digest.error();
    if (digest.value() != source.sha256 || identity.value().size_bytes != source.asset.size_bytes ||
        identity.value().mtime_unix_ms != source.asset.mtime_unix_ms)
        return make_error(ErrorCode::kConflict, "Merge source changed during processing",
                          {{"reason", "merge_source_changed"}, {"asset_id", source.asset.id}});
    return {};
}
} // namespace

Result<PhotoMergeResult> CatalogService::merge_selected_photos(const PhotoMergeRequest &request)
{
    std::vector<std::string> owned;
    bool catalog_committed = false;
    std::string retained_asset_id, retained_output;
    try
    {
        if (!repository_ || !engine_ || !raster_)
            return make_error(ErrorCode::kIo, "Catalog session is closed");
        if (auto active = request.cancellation.check(); !active)
            return active.error();
        if (request.asset_ids.size() < 2 || request.asset_ids.size() > 16 ||
            request.max_edge > 16000 || request.options.memory_budget_bytes < 64ULL * 1024 * 1024 ||
            request.options.memory_budget_bytes > 4ULL * 1024 * 1024 * 1024 ||
            (request.options.kind != PhotoMergeKind::kHdr &&
             request.options.kind != PhotoMergeKind::kPanorama))
            return make_error(ErrorCode::kInvalidArgument,
                              "Merge requires 2 to 16 photos and bounded options",
                              {{"reason", "invalid_merge_options"}});
        auto snapshot = repository_->snapshot();
        if (!snapshot)
            return snapshot.error();
        if (request.expected_catalog_revision &&
            *request.expected_catalog_revision != snapshot.value().revision)
            return make_error(ErrorCode::kConflict, "Catalog revision is stale",
                              {{"reason", "stale_catalog_revision"}});
        std::set<std::string> unique;
        std::vector<MergeSource> sources;
        std::uint64_t input_pixels = 0, largest_pixels = 0, largest_native = 0;
        for (const auto &id : request.asset_ids)
        {
            if (!unique.insert(id).second)
                return make_error(ErrorCode::kInvalidArgument,
                                  "Merge selection contains duplicate assets",
                                  {{"reason", "merge_duplicate_asset"}, {"asset_id", id}});
            auto found = repository_->find_asset_by_id(id);
            if (!found)
                return found.error();
            if (!found.value())
                return make_error(ErrorCode::kNotFound, "Merge photo does not exist",
                                  {{"asset_id", id}});
            const auto &asset = *found.value();
            if (!is_raw_media_type(asset.media_type) && !is_raster_media_type(asset.media_type))
                return make_error(ErrorCode::kUnsupported, "Merge input format is unsupported",
                                  {{"asset_id", id}});
            auto location = normalize_local_input(asset.normalized_uri);
            if (!location)
                return location.error();
            auto identity = read_file_identity(location.value().path);
            if (!identity)
                return identity.error();
            if (identity.value().size_bytes != asset.size_bytes ||
                identity.value().mtime_unix_ms != asset.mtime_unix_ms)
                return make_error(ErrorCode::kConflict, "Merge input no longer matches the catalog",
                                  {{"reason", "merge_source_changed"}, {"asset_id", id}});
            std::uint32_t width = 0, height = 0;
            if (is_raw_media_type(asset.media_type))
            {
                auto inspection = engine_->inspect(location.value().path, request.cancellation);
                if (!inspection)
                    return inspection.error();
                width = inspection.value().width;
                height = inspection.value().height;
            }
            else
            {
                auto inspection = raster_->probe(location.value().path);
                if (!inspection)
                    return inspection.error();
                width = inspection.value().width;
                height = inspection.value().height;
            }
            if (width == 0 || height == 0 || std::uint64_t(width) * height > 80000000)
                return make_error(ErrorCode::kValidation,
                                  "Merge input dimensions exceed the bound");
            largest_native = std::max(largest_native, std::uint64_t(width) * height);
            if (request.max_edge && std::max(width, height) > request.max_edge)
            {
                const auto longest = std::max(width, height);
                width =
                    std::max(1U, std::uint32_t(std::uint64_t(width) * request.max_edge / longest));
                height =
                    std::max(1U, std::uint32_t(std::uint64_t(height) * request.max_edge / longest));
            }
            const auto pixels = std::uint64_t(width) * height;
            input_pixels += pixels;
            largest_pixels = std::max(largest_pixels, pixels);
            auto baseline = load_baseline_recipe(id);
            if (!baseline)
                return baseline.error();
            auto saved = load_recipe(id);
            if (!saved)
                return saved.error();
            auto serialized = serialize_recipe(saved.value());
            if (!serialized)
                return serialized.error();
            auto digest = sha256_file_hex(location.value().path, request.cancellation);
            if (!digest)
                return digest.error();
            sources.push_back({asset, location.value().path, std::move(digest).value(),
                               std::move(baseline).value(), std::move(serialized).value()});
        }
        if (std::max(input_pixels * 12 + largest_pixels * 48,
                     input_pixels * 12 + largest_native * 24 + largest_pixels * 12) +
                64ULL * 1024 * 1024 >
            request.options.memory_budget_bytes)
            return make_error(ErrorCode::kValidation, "Merge decode exceeds memory budget",
                              {{"reason", "merge_memory_budget_exceeded"}});
        auto options = request.options;
        if (options.kind == PhotoMergeKind::kHdr && options.exposure_ev.empty())
        {
            // Read exposure metadata through the Engine, never guess from pixel
            // brightness or filenames. Explicit CLI EVs override absent metadata.
            for (const auto &source : sources)
            {
                auto capture =
                    engine_->read_embedded_capture_metadata(source.path, request.cancellation);
                if (!capture)
                    return capture.error();
                const auto &m = capture.value();
                if (!m.shutter_s || !m.aperture || !m.iso || *m.shutter_s <= 0 ||
                    *m.aperture <= 0 || *m.iso <= 0)
                    return make_error(
                        ErrorCode::kValidation,
                        "Capture exposure metadata is missing; supply exposure stops",
                        {{"reason", "hdr_exposure_required"}, {"asset_id", source.asset.id}});
                options.exposure_ev.push_back(
                    std::log2(*m.shutter_s * *m.iso / (*m.aperture * *m.aperture)));
            }
            const double base = options.exposure_ev.front();
            for (auto &ev : options.exposure_ev)
                ev -= base;
        }
        const std::string asset_id = generate_asset_id();
        const std::string kind = options.kind == PhotoMergeKind::kHdr ? "hdr" : "panorama";
        std::string output = request.output_path;
        if (output.empty())
            output =
                snapshot.value().database_path + ".ravo/derived/" + asset_id + "/" + kind + ".tiff";
        auto destination = normalize_local_input(output);
        if (!destination)
            return destination.error();
        output = destination.value().path;
        const auto suffix = extension_lower(utf8_path(output));
        if (suffix != ".tif" && suffix != ".tiff")
            return make_error(ErrorCode::kInvalidArgument, "Photo merge output must be a TIFF");
        const std::string provenance = output + ".ravo-merge.json";
        for (const auto &path : {output, provenance})
        {
            std::error_code ec;
            const bool exists = std::filesystem::exists(utf8_path(path), ec);
            if (ec)
                return make_error(ErrorCode::kIo, "Cannot inspect merge output",
                                  {{"path", path}, {"detail", ec.message()}});
            if (exists)
                return make_error(ErrorCode::kConflict, "Merge output already exists",
                                  {{"reason", "merge_output_exists"}, {"path", path}});
        }
        std::vector<LinearWorkingBuffer> frames;
        for (const auto &source : sources)
        {
            Result<LinearWorkingBuffer> decoded =
                make_error(ErrorCode::kInternal, "Merge decode was not dispatched");
            if (is_raw_media_type(source.asset.media_type))
            {
                auto raw = engine_->decode_raw_frame(source.path, request.cancellation);
                if (!raw)
                    return raw.error();
                std::uint32_t width = raw.value().width, height = raw.value().height;
                apply_display_rotation_to_size(width, height, raw.value().rotate_quarters);
                if (request.max_edge && std::max(width, height) > request.max_edge)
                {
                    const auto edge = std::max(width, height);
                    width =
                        std::max(1U, std::uint32_t(std::uint64_t(width) * request.max_edge / edge));
                    height = std::max(
                        1U, std::uint32_t(std::uint64_t(height) * request.max_edge / edge));
                }
                decoded = engine_->linear_working_from_raw(raw.value(), source.baseline, width,
                                                           height, request.cancellation);
            }
            else
            {
                auto raster = raster_->decode(source.path, request.max_edge, request.cancellation);
                if (!raster)
                    return raster.error();
                RasterBuffer buffer;
                buffer.width = raster.value().width;
                buffer.height = raster.value().height;
                buffer.source_width = source.asset.width.value_or(buffer.width);
                buffer.source_height = source.asset.height.value_or(buffer.height);
                buffer.srgb = std::move(raster.value().rgb);
                buffer.color_profile = std::move(raster.value().color_profile);
                decoded = engine_->linear_working_from_raster(buffer, source.baseline,
                                                              request.cancellation);
            }
            if (!decoded)
                return decoded.error();
            frames.push_back(std::move(decoded).value());
        }
        auto merged = merge_photos(frames, options, request.cancellation);
        if (!merged)
            return merged.error();
        frames.clear();
        frames.shrink_to_fit();
        auto working = std::move(merged.value().image);
        if (std::uint64_t(working.width) * working.height * 80 + 64ULL * 1024 * 1024 >
            options.memory_budget_bytes)
            return make_error(ErrorCode::kValidation, "Merge delivery exceeds memory budget",
                              {{"reason", "merge_memory_budget_exceeded"}});
        if (options.kind == PhotoMergeKind::kHdr)
        {
            auto mapped = tone_map_merged_hdr(working, request.cancellation);
            if (!mapped)
                return mapped.error();
            working = std::move(mapped).value();
        }
        Recipe delivery;
        delivery.asset = {asset_id, output, {}};
        delivery.operations.push_back({"ravo.color.output", 1, "merge-output-1", true,
                                       output_color_to_parameters(OutputColorParams{}),
                                       std::nullopt});
        auto rendered = engine_->render_linear_working_export(
            working, delivery, RenderSampleKind::kRgb16, request.cancellation);
        if (!rendered)
            return rendered.error();
        ExportPixelBuffer pixels{rendered.value().width, rendered.value().height,
                                 std::move(rendered.value().color_profile),
                                 std::move(rendered.value().samples)};
        TiffExportOptions tiff;
        tiff.sample_type = TiffSampleType::kUint16;
        tiff.compression = TiffCompression::kDeflatePredictor;
        ExportMetadataSnapshot metadata;
        metadata.destination_document_name = kind + ".tiff";
        auto encoded = raster_->encode(pixels, ExportFormat::kTiff, {}, request.cancellation, {},
                                       tiff, metadata);
        if (!encoded)
            return encoded.error();
        ImageArtifactExpectation expected;
        expected.mime_type = "image/tiff";
        expected.width = pixels.width;
        expected.height = pixels.height;
        expected.max_encoded_bytes = static_cast<std::size_t>(options.memory_budget_bytes);
        auto verified = verify_encoded_image_artifact(*raster_, encoded.value(), expected,
                                                      request.cancellation);
        if (!verified)
            return verified.error();
        JsonValue::Array inputs, transforms, exposures;
        for (std::size_t i = 0; i < sources.size(); ++i)
        {
            const auto &source = sources[i];
            auto safe = unchanged(source, request.cancellation);
            if (!safe)
                return safe.error();
            inputs.emplace_back(
                JsonValue::Object{{"asset_id", source.asset.id},
                                  {"uri", source.asset.normalized_uri},
                                  {"sha256", source.sha256},
                                  {"observed_recipe_json", source.observed_recipe}});
            JsonValue::Array matrix;
            for (const auto v : merged.value().alignments[i].source_to_reference)
            {
                auto number = parameter_value_to_json(ParameterValue{v});
                if (!number)
                    return number.error();
                matrix.push_back(std::move(number).value());
            }
            transforms.emplace_back(JsonValue::Object{
                {"matrix", std::move(matrix)},
                {"inliers",
                 JsonValue::number(std::to_string(merged.value().alignments[i].inliers))}});
        }
        for (const auto ev : options.exposure_ev)
        {
            auto number = parameter_value_to_json(ParameterValue{ev});
            if (!number)
                return number.error();
            exposures.push_back(std::move(number).value());
        }
        JsonValue manifest{JsonValue::Object{
            {"schema", "ravo.photo_merge_provenance"},
            {"version", JsonValue::number("1")},
            {"kind", kind},
            {"derived_asset_id", asset_id},
            {"catalog_id", snapshot.value().catalog_id},
            {"catalog_revision", JsonValue::number(std::to_string(snapshot.value().revision))},
            {"input_policy", "baseline_linear_originals"},
            {"output_sha256", verified.value().content_sha256},
            {"output_profile", verified.value().color_profile},
            {"sample_type", "uint16"},
            {"tone_map", options.kind == PhotoMergeKind::kHdr ? "luminance_reinhard_v1" : "none"},
            {"projection", options.kind == PhotoMergeKind::kPanorama ? "planar" : "reference"},
            {"auto_align", options.auto_align},
            {"auto_crop", options.auto_crop},
            {"origin_x", JsonValue::number(std::to_string(merged.value().origin_x))},
            {"origin_y", JsonValue::number(std::to_string(merged.value().origin_y))},
            {"exposure_normalization_ev",
             JsonValue::number(std::to_string(merged.value().exposure_normalization_ev))},
            {"max_edge", JsonValue::number(std::to_string(request.max_edge))},
            {"deghost_threshold", JsonValue::number(std::to_string(options.deghost_threshold))},
            {"sources", std::move(inputs)},
            {"alignments", std::move(transforms)},
            {"exposure_ev", std::move(exposures)}}};
        const auto text = serialize_json(manifest);
        std::error_code ec;
        std::filesystem::create_directories(utf8_path(output).parent_path(), ec);
        if (ec)
            return make_error(ErrorCode::kIo, "Unable to create merge output directory",
                              {{"detail", ec.message()}});
        owned.reserve(2);
        const auto fail = [&](TaskError error) -> Result<PhotoMergeResult>
        {
            cleanup_owned(owned, error);
            return error;
        };
        if (testing_merge_checkpoint_)
        {
            auto checked = testing_merge_checkpoint_("before_publication");
            if (!checked)
                return checked.error();
        }
        auto image_published =
            publish_bytes_artifact_no_replace(output, encoded.value(), request.cancellation);
        if (!image_published)
            return image_published.error();
        owned.push_back(output);
        auto provenance_published =
            publish_text_artifact_no_replace(provenance, text, request.cancellation);
        if (!provenance_published)
            return fail(provenance_published.error());
        owned.push_back(provenance);
        if (testing_merge_checkpoint_)
        {
            auto checked = testing_merge_checkpoint_("before_catalog_commit");
            if (!checked)
                return fail(checked.error());
        }
        for (const auto &source : sources)
        {
            auto safe = unchanged(source, request.cancellation);
            if (!safe)
                return fail(safe.error());
        }
        auto identity = read_file_identity(output);
        if (!identity)
            return fail(identity.error());
        AssetRecord asset;
        asset.id = asset_id;
        asset.normalized_uri = destination.value().uri;
        asset.media_type = "image/tiff";
        asset.width = pixels.width;
        asset.height = pixels.height;
        asset.size_bytes = identity.value().size_bytes;
        asset.mtime_unix_ms = identity.value().mtime_unix_ms;
        asset.content_fingerprint = make_content_fingerprint(identity.value());
        asset.created_unix_ms = now_unix_ms();
        retained_asset_id = asset_id;
        retained_output = output;
        // The transaction owns both revision preflight and its last cancel point.
        // Everything after successful commit reports committed state and retains artifacts.
        auto committed =
            repository_->commit_imported_asset(asset, verified.value().content_sha256, false,
                                               snapshot.value().revision, request.cancellation);
        if (!committed)
            return fail(committed.error());
        catalog_committed = true;
        owned.clear();
        auto recovered = synchronize_committed_change(asset_id, CancellationToken{});
        if (!recovered)
        {
            auto error = recovered.error();
            error.context.insert_or_assign("catalog_committed", "true");
            error.context.insert_or_assign("asset_id", asset_id);
            error.context.insert_or_assign("output", output);
            return error;
        }
        PhotoMergeResult result;
        result.kind = options.kind;
        result.asset = std::move(asset);
        result.output_path = output;
        result.provenance_path = provenance;
        result.artifact = std::move(verified).value();
        result.alignments = std::move(merged.value().alignments);
        result.deghosted_pixels = merged.value().deghosted_pixels;
        result.origin_x = merged.value().origin_x;
        result.origin_y = merged.value().origin_y;
        result.exposure_normalization_ev = merged.value().exposure_normalization_ev;
        return result;
    }
    catch (const std::bad_alloc &)
    {
        auto error = make_error(ErrorCode::kIo, "Unable to allocate merge resources",
                                {{"reason", "merge_allocation_failed"}});
        if (catalog_committed)
        {
            error.context.insert_or_assign("catalog_committed", "true");
            error.context.insert_or_assign("asset_id", retained_asset_id);
            error.context.insert_or_assign("output", retained_output);
        }
        cleanup_owned(owned, error);
        return error;
    }
}
} // namespace ravo
