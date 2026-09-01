#include "catalog_service_internal.h"

#include <utility>

#include "catalog_internal.h"

namespace ravo::catalog_service_internal
{

[[nodiscard]] bool media_type_has_embedded_capture(const std::string_view media_type) noexcept
{
    return is_raw_media_type(media_type) || media_type == kMediaTypeJpeg ||
           media_type == kMediaTypePng || media_type == kMediaTypeTiff;
}

[[nodiscard]] bool is_common_raster_media(const std::string_view media_type) noexcept
{
    return media_type == kMediaTypeJpeg || media_type == kMediaTypePng ||
           media_type == kMediaTypeTiff;
}

void merge_engine_capture(CaptureMetadata &target, const EngineCaptureMetadata &source)
{
    if (source.camera_make)
        target.camera_make = source.camera_make;
    if (source.camera_model)
        target.camera_model = source.camera_model;
    if (source.iso)
        target.iso = source.iso;
    if (source.aperture)
        target.aperture = source.aperture;
    if (source.focal_length_mm)
        target.focal_length_mm = source.focal_length_mm;
    if (source.shutter_s)
        target.shutter_s = source.shutter_s;
    if (source.captured_datetime)
    {
        CaptureDateTime captured;
        captured.local_exif = source.captured_datetime->local_exif;
        captured.subsecond_digits = source.captured_datetime->subsecond_digits;
        captured.utc_offset_minutes = source.captured_datetime->utc_offset_minutes;
        target.captured_datetime = std::move(captured);
    }
    if (source.location)
    {
        CaptureLocation copied;
        copied.latitude_e6 = source.location->latitude_e6;
        copied.longitude_e6 = source.location->longitude_e6;
        if (source.location->altitude)
        {
            CaptureAltitude altitude;
            altitude.magnitude_mm = source.location->altitude->magnitude_mm;
            altitude.reference = source.location->altitude->reference ==
                                         EngineCaptureAltitudeReference::kBelowSeaLevel ?
                                     CaptureAltitudeReference::kBelowSeaLevel :
                                     CaptureAltitudeReference::kAboveSeaLevel;
            copied.altitude = altitude;
        }
        target.location = copied;
    }
}

[[nodiscard]] std::string_view context_value(const TaskError &error, const std::string_view key)
{
    const auto found = error.context.find(std::string(key));
    if (found == error.context.end())
    {
        return {};
    }
    return found->second;
}

[[nodiscard]] bool is_recognized_raster_probe_error(const TaskError &error) noexcept
{
    const auto format = context_value(error, "format");
    return format == "jpeg" || format == "jpg" || format == "png" || format == "tiff" ||
           format == "tif" || format == "bmp" || format == "gif" || format == "webp" ||
           format == "qoi" || format == "rgbe";
}

[[nodiscard]] bool should_try_raw_after_raster(const TaskError &error) noexcept
{
    const auto format = context_value(error, "format");
    const auto reason = context_value(error, "reason");
    if ((format == "tiff" || format == "tif") && reason == "unsupported_tiff_raw_container")
    {
        return true;
    }
    if (is_recognized_raster_probe_error(error))
    {
        return false;
    }
    return error.code == ErrorCode::kUnsupported;
}

[[nodiscard]] std::string utf8_string(const std::u8string &value)
{
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}

[[nodiscard]] TaskError
annotate_batch_export_error(TaskError error, const std::size_t completed_count,
                            const std::size_t total_count, const std::size_t failed_index,
                            const std::string_view asset_id, const std::string_view output)
{
    error.context.insert_or_assign("asset_id", std::string(asset_id));
    error.context.insert_or_assign("batch_index", std::to_string(failed_index + 1U));
    error.context.insert_or_assign("completed_count", std::to_string(completed_count));
    error.context.insert_or_assign("output", std::string(output));
    error.context.insert_or_assign("partial_batch", completed_count == 0 ? "false" : "true");
    error.context.insert_or_assign("total_count", std::to_string(total_count));
    return error;
}

} // namespace ravo::catalog_service_internal
