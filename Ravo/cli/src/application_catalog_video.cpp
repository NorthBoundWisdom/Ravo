#include "application_internal.h"
#include "ravo/services/catalog_service.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/services/artifact_publication.h"
#include "ravo/services/image_artifact_verification.h"
namespace ravo::cli_internal
{
Result<JsonValue> run_catalog_video_command(const EngineFacade &engine, CatalogService &service,
                                            std::string_view command,
                                            const CatalogCliArguments &flags)
{
    if (flags.asset_id.empty())
        return make_error(ErrorCode::kInvalidArgument, "Video command requires --asset-id");
    if (command == "video-info")
    {
        auto info = service.video_info(flags.asset_id);
        if (!info)
            return info.error();
        return video_info_json(info.value());
    }
    if (flags.output.empty() || !ends_with_png(flags.output))
        return make_error(ErrorCode::kInvalidArgument,
                          "video-frame requires --output <absent.png>");
    auto frame = service.video_frame(flags.asset_id, flags.video_time_us.value_or(0),
                                     flags.max_edge.value_or(512));
    if (!frame)
        return frame.error();
    QtRasterDecoder raster;
    const auto &image = frame.value().image;
    RenderedImage rendered;
    rendered.width = image.width;
    rendered.height = image.height;
    rendered.rgb = image.rgb;
    rendered.color_profile = image.color_profile;
    auto png = engine.encode_png(rendered);
    if (!png)
        return png.error();
    ImageArtifactExpectation expected;
    expected.width = image.width;
    expected.height = image.height;
    expected.color_profile = "srgb";
    auto verified = verify_encoded_image_artifact(raster, png.value(), expected);
    if (!verified)
        return verified.error();
    auto written = publish_bytes_artifact_no_replace(flags.output, png.value());
    if (!written)
        return written.error();
    const auto &artifact = verified.value();
    return JsonValue{JsonValue::Object{
        {"schema", "ravo.video_frame/v1"},
        {"asset_id", std::string(flags.asset_id)},
        {"timestamp_us", JsonValue::number(std::to_string(frame.value().timestamp_us))},
        {"width", JsonValue::number(std::to_string(artifact.width))},
        {"height", JsonValue::number(std::to_string(artifact.height))},
        {"artifact",
         JsonValue::Object{{"path", std::string(flags.output)},
                           {"mime_type", artifact.mime_type},
                           {"width", JsonValue::number(std::to_string(artifact.width))},
                           {"height", JsonValue::number(std::to_string(artifact.height))},
                           {"color_profile", artifact.color_profile},
                           {"color_profile_fingerprint", artifact.color_profile_fingerprint},
                           {"type", artifact.type},
                           {"version", JsonValue::number(std::to_string(artifact.version))},
                           {"content_sha256", artifact.content_sha256},
                           {"byte_count", JsonValue::number(std::to_string(artifact.byte_count))},
                           {"lifecycle", "caller_owned_no_replace"}}}}};
}
} // namespace ravo::cli_internal
