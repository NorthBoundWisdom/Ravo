#include "ravo/services/video.h"
#include "ravo/services/catalog_service.h"
#include "ravo/domain/uri.h"
#include "ravo/engine/video_color.h"
#include "ravo/services/preview_service.h"
#include "ravo/services/display_presentation.h"
#include "catalog_internal.h"
#include <new>
namespace ravo
{
Result<std::string> video_source_path(const AssetRecord &asset,
                                      const CancellationToken &cancellation)
{
    if (auto active = cancellation.check(); !active)
        return active.error();
    if (!asset.video || !is_video_media_type(asset.media_type))
        return make_error(ErrorCode::kUnsupported, "Asset is not a video",
                          {{"reason", "asset_not_video"}});
    auto location = normalize_local_input(asset.normalized_uri);
    if (!location)
        return location.error();
    auto identity = read_file_identity(location.value().path);
    if (!identity)
        return identity.error();
    if (identity.value().size_bytes != asset.size_bytes ||
        identity.value().mtime_unix_ms != asset.mtime_unix_ms)
        return make_error(ErrorCode::kConflict, "Video source changed since import",
                          {{"reason", "video_source_changed"}});
    return location.value().path;
}
Result<DecodedRaster> render_video_frame(const VideoFrame &frame,
                                         const CancellationToken &cancellation)
try
{
    if (frame.rgb.size() != static_cast<std::size_t>(frame.width) * frame.height * 3U)
        return make_error(ErrorCode::kValidation, "Video frame dimensions do not match its pixels",
                          {{"reason", "video_frame_invalid"}});
    auto rgb = video_rgb_to_sdr(frame.rgb, frame.info.transfer, frame.info.primaries,
                                frame.info.peak_nits, cancellation);
    if (!rgb)
        return rgb.error();
    DecodedRaster result;
    result.width = frame.width;
    result.height = frame.height;
    const bool swapped = frame.info.rotation == 90 || frame.info.rotation == 270;
    result.source_width = swapped ? frame.info.height : frame.info.width;
    result.source_height = swapped ? frame.info.width : frame.info.height;
    result.rgb = std::move(rgb).value();
    auto profile = make_srgb_color_profile();
    if (!profile)
        return profile.error();
    result.color_profile = std::move(profile).value();
    return result;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kInternal, "Video presentation exhausted memory",
                      {{"reason", "video_allocation_failed"}});
}

Result<VideoPreview> decode_video_preview(const VideoDecoder &decoder, std::string_view path,
                                          std::int64_t timestamp_us, std::uint32_t max_edge,
                                          const CancellationToken &cancellation)
{
    auto frame = decoder.frame(path, timestamp_us, max_edge, cancellation);
    if (!frame)
        return frame.error();
    auto rendered = render_video_frame(frame.value(), cancellation);
    if (!rendered)
        return rendered.error();
    return VideoPreview{std::move(rendered).value(), frame.value().timestamp_us};
}
Result<VideoPreview> decode_video_asset_preview(const VideoDecoder &decoder,
                                                const AssetRecord &asset, std::int64_t timestamp_us,
                                                std::uint32_t max_edge,
                                                const CancellationToken &cancellation)
{
    auto path = video_source_path(asset, cancellation);
    if (!path)
        return path.error();
    auto preview =
        decode_video_preview(decoder, path.value(), timestamp_us, max_edge, cancellation);
    if (!preview)
        return preview.error();
    auto after = video_source_path(asset, cancellation);
    if (!after)
        return after.error();
    return preview;
}
Result<VideoInfo> CatalogService::video_info(std::string_view asset_id,
                                             const CancellationToken &cancellation) const
{
    if (auto active = cancellation.check(); !active)
        return active.error();
    if (!repository_)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
        return asset.error();
    if (!asset.value())
        return make_error(ErrorCode::kNotFound, "Video asset does not exist");
    if (!asset.value()->video)
        return make_error(ErrorCode::kUnsupported, "Asset is not a video",
                          {{"reason", "asset_not_video"}});
    return *asset.value()->video;
}
Result<VideoPreview> CatalogService::video_frame(std::string_view asset_id, std::int64_t time_us,
                                                 std::uint32_t max_edge,
                                                 const CancellationToken &cancellation) const
{
    if (!repository_)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    if (!video_)
        return make_error(ErrorCode::kUnsupported, "Video decoder is unavailable",
                          {{"reason", "video_decoder_unavailable"}});
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
        return asset.error();
    if (!asset.value())
        return make_error(ErrorCode::kNotFound, "Video asset does not exist");
    if (!asset.value()->video)
        return make_error(ErrorCode::kUnsupported, "Asset is not a video",
                          {{"reason", "asset_not_video"}});
    return decode_video_asset_preview(*video_, *asset.value(), time_us, max_edge, cancellation);
}
Result<PreviewResult> PreviewService::generate_video_preview(const AssetRecord &asset,
                                                             const PreviewRequest &request,
                                                             std::int64_t generation)
{
    if (!video_ || !raster_ || !cache_)
        return make_error(ErrorCode::kUnsupported, "Video decoder is unavailable",
                          {{"reason", "video_decoder_unavailable"}});
    if (request.roi || request.crop_workspace || request.overlay_mask_id || request.ignore_edits)
        return make_error(ErrorCode::kUnsupported, "Photo preview options do not apply to video",
                          {{"reason", "video_photo_operation_unsupported"}});
    auto decoded =
        decode_video_asset_preview(*video_, asset, 0, request.max_edge, request.cancellation);
    if (!decoded)
        return decoded.error();
    auto &image = decoded.value().image;
    PreviewResult result;
    result.asset_id = asset.id;
    result.request_revision = request.request_revision;
    result.width = image.width;
    result.height = image.height;
    result.color_profile = image.color_profile;
    result.rgb = std::move(image.rgb);
    result.preview_apply_mode = "video_sdr";
    result.pixel_provenance = "video_frame_sdr_srgb8";
    if (request.persist_preview_record)
    {
        result.cache_key = make_preview_cache_key(asset.id, result.width, result.height,
                                                  asset.content_fingerprint.value_or("none")) +
                           "_video_sdr_v1";
        auto bytes = raster_->encode(result.width, result.height, result.rgb, result.color_profile,
                                     ExportFormat::kPng, {}, request.cancellation);
        if (!bytes)
            return bytes.error();
        if (auto active = request.cancellation.check(); !active)
            return active.error();
        if (auto current = require_preview_generation(asset.id, generation); !current)
            return current.error();
        auto published = cache_->commit_png_bytes(result.cache_key, bytes.value());
        if (!published)
            return published.error();
        result.cache_path = published.value();
        PreviewRecord record;
        record.asset_id = asset.id;
        record.cache_key = result.cache_key;
        record.width = result.width;
        record.height = result.height;
        record.state = std::string(kPreviewStateReady);
        record.cache_relpath = cache_->relative_png_path(result.cache_key);
        auto saved = repository_->upsert_preview(record, generation);
        if (!saved)
            return saved.error();
        result.rgb.clear(); // The immutable profiled PNG is the persistent preview resource.
    }
    return result;
}
} // namespace ravo
