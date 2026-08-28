#include "raw_pipeline.h"

#include "image_ops.h"
#include "mask_evaluator.h"
#include "recursive_gaussian.h"

#include <algorithm>
#include <cctype>
#include <array>
#include <cmath>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>
#include <QUrl>

#include <exception>
#include <exiv2/exiv2.hpp>
#include <zlib.h>
#include <libraw/libraw.h>

#include "ravo/recipe/color_checker.h"
#include "ravo/recipe/color_harmonizer.h"
#include "ravo/recipe/primaries.h"
#include "ravo/recipe/profile_gamma.h"

namespace ravo
{
namespace
{

[[nodiscard]] QString local_path(const std::string_view uri)
{
    const QString text = QString::fromUtf8(uri.data(), static_cast<qsizetype>(uri.size()));
    const QUrl parsed(text);
    return parsed.isLocalFile() ? parsed.toLocalFile() : text;
}

[[nodiscard]] constexpr std::uint64_t saturating_add(const std::uint64_t left,
                                                     const std::uint64_t right) noexcept
{
    constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    return right > maximum - left ? maximum : left + right;
}

[[nodiscard]] constexpr std::uint64_t saturating_multiply(const std::uint64_t left,
                                                          const std::uint64_t right) noexcept
{
    constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    return left != 0U && right > maximum / left ? maximum : left * right;
}

[[nodiscard]] Result<std::uint8_t> channel_for(const int color)
{
    switch (color)
    {
    case 0:
        return std::uint8_t{0};
    case 1:
    case 3:
        return std::uint8_t{1};
    case 2:
        return std::uint8_t{2};
    default:
        return make_error(ErrorCode::kUnsupported,
                          "The first Ravo RAW slice supports Bayer RGB CFA sensors only");
    }
}

[[nodiscard]] bool normalize_white_balance(const float source[4], const int colors,
                                           std::array<float, 4> &output) noexcept
{
    const float green = source[1] > 0.0F ? source[1] : source[3] > 0.0F ? source[3] : 0.0F;
    if (!std::isfinite(green) || green <= 0.0F)
    {
        return false;
    }
    const int required = colors >= 4 ? 4 : 3;
    for (int channel = 0; channel < required; ++channel)
    {
        if (!std::isfinite(source[channel]) || source[channel] <= 0.0F)
        {
            return false;
        }
        output[static_cast<std::size_t>(channel)] = source[channel] / green;
    }
    if (required == 3)
    {
        output[3] = 1.0F;
    }
    return true;
}

[[nodiscard]] Result<std::unique_ptr<LibRaw>> open_libraw_file(const std::string_view input_uri)
{
    const QString path = local_path(input_uri);
    if (path.isEmpty())
    {
        return make_error(ErrorCode::kInvalidArgument, "RAW input path must not be empty",
                          {{"input_uri", std::string(input_uri)}, {"reason", "empty_raw_path"}});
    }
    const QFileInfo info(path);
    if (!info.exists())
    {
        return make_error(ErrorCode::kNotFound, "RAW input does not exist",
                          {{"input_uri", std::string(input_uri)}, {"reason", "raw_not_found"}});
    }
    if (!info.isFile())
    {
        return make_error(
            ErrorCode::kInvalidArgument, "RAW input must be a regular file",
            {{"input_uri", std::string(input_uri)}, {"reason", "raw_not_regular_file"}});
    }

    auto decoder = std::make_unique<LibRaw>();
    const QByteArray utf8 = path.toUtf8();
    const int open_status = decoder->open_file(utf8.constData());
    if (open_status != LIBRAW_SUCCESS)
    {
        const bool unsupported = open_status == LIBRAW_FILE_UNSUPPORTED;
        return make_error(
            unsupported ? ErrorCode::kUnsupported : ErrorCode::kValidation,
            "LibRaw could not identify the RAW input",
            {{"detail", libraw_strerror(open_status)},
             {"input_uri", std::string(input_uri)},
             {"reason", unsupported ? "libraw_unsupported_file" : "libraw_open_failed"}});
    }
    return decoder;
}

// LibRaw/dcraw flip: 0 none, 3 180°, 5 90° CCW, 6 90° CW.
[[nodiscard]] int clockwise_quarters_from_libraw_flip(const int flip) noexcept
{
    switch (flip)
    {
    case 3:
        return 2;
    case 5:
        return 3;
    case 6:
        return 1;
    default:
        return 0;
    }
}

[[nodiscard]] RawExposureMetadata failed_exposure_metadata(const std::string_view detail)
{
    RawExposureMetadata result;
    result.status = RawExposureMetadataStatus::kReadFailed;
    result.failure_detail = std::string(detail);
    return result;
}

[[nodiscard]] RawExposureMetadata read_legacy_exposure_metadata(const QString &path)
{
    try
    {
        const QByteArray utf8 = path.toUtf8();
        auto image = Exiv2::ImageFactory::open(utf8.constData());
        if (!image)
        {
            return failed_exposure_metadata("Exiv2 did not create an image reader");
        }
        image->readMetadata();
        const auto &exif = image->exifData();
        LegacyExposureMetadataTags tags;

        const auto find = [&exif](const char *key) { return exif.findKey(Exiv2::ExifKey(key)); };
        const auto present = [&exif](const Exiv2::ExifData::const_iterator position)
        { return position != exif.end() && position->size() != 0U; };
        const auto scalar_float = [&](const char *key) -> std::optional<double>
        {
            const auto position = find(key);
            if (!present(position))
            {
                return std::nullopt;
            }
            return static_cast<double>(position->toFloat());
        };
        const auto scalar_integer = [&](const Exiv2::ExifData::const_iterator position)
        { return position->toInt64(0U); };
        const auto integer_values = [&](const Exiv2::ExifData::const_iterator position,
                                        const std::size_t required, const std::size_t maximum)
        {
            std::vector<std::int64_t> values;
            if (position->count() < required)
            {
                return values;
            }
            const std::size_t count = std::min<std::size_t>(position->count(), maximum);
            values.reserve(count);
            for (std::size_t index = 0; index < count; ++index)
            {
                values.push_back(position->toInt64(index));
            }
            return values;
        };

        tags.photo_exposure_bias_ev = scalar_float("Exif.Photo.ExposureBiasValue");
        if (!tags.photo_exposure_bias_ev)
        {
            tags.image_exposure_bias_ev = scalar_float("Exif.Image.ExposureBiasValue");
        }

        auto position = find("Exif.Canon.LightingOpt");
        if (present(position))
        {
            tags.canon_lighting_opt = integer_values(position, 3U, 3U);
        }
        else if ((position = find("Exif.CanonLiOp.AutoLightingOptimizer")), present(position))
        {
            tags.canon_auto_lighting_optimizer = integer_values(position, 1U, 1U);
        }
        else if ((position = find("Exif.Fujifilm.DevelopmentDynamicRange")), present(position))
        {
            tags.fuji_development_dynamic_range = scalar_integer(position);
        }
        else if ((position = find("Exif.Fujifilm.AutoDynamicRange")), present(position))
        {
            tags.fuji_auto_dynamic_range = scalar_integer(position);
        }
        else if ((position = find("Exif.Nikon3.ColorSpace")), present(position))
        {
            tags.nikon_color_space = scalar_integer(position);
        }
        else if ((position = find("Exif.Nikon3.ActiveDLighting")), present(position))
        {
            tags.nikon_active_d_lighting = scalar_integer(position);
        }
        else if ((position = find("Exif.OlympusCs.Gradation")), present(position))
        {
            tags.olympus_camera_settings_gradation = integer_values(position, 3U, 4U);
        }
        else if ((position = find("Exif.OlympusRd2.Gradation")), present(position))
        {
            tags.olympus_raw_development_gradation = integer_values(position, 3U, 4U);
        }
        else if ((position = find("Exif.Pentax.DynamicRangeExpansion")), present(position))
        {
            const Exiv2::DataBuf bytes = position->dataArea();
            std::vector<std::uint8_t> values;
            if (!bytes.empty())
            {
                values.push_back(bytes.read_uint8(0U));
            }
            tags.pentax_dynamic_range_expansion = std::move(values);
        }

        auto resolved = resolve_legacy_exposure_metadata(tags);
        if (!resolved)
        {
            const auto reason = resolved.error().context.find("reason");
            return failed_exposure_metadata(reason == resolved.error().context.end() ?
                                                resolved.error().message :
                                                reason->second);
        }
        return std::move(resolved).value();
    }
    catch (const std::bad_alloc &)
    {
        throw;
    }
    catch (const Exiv2::Error &error)
    {
        return failed_exposure_metadata(error.what());
    }
    catch (const std::exception &error)
    {
        return failed_exposure_metadata(error.what());
    }
}

} // namespace

Result<RawExposureMetadata> resolve_legacy_exposure_metadata(const LegacyExposureMetadataTags &tags)
{
    RawExposureMetadata result;
    result.status = RawExposureMetadataStatus::kReady;
    const std::optional<double> exposure_bias =
        tags.photo_exposure_bias_ev ? tags.photo_exposure_bias_ev : tags.image_exposure_bias_ev;
    if (exposure_bias)
    {
        if (!std::isfinite(*exposure_bias))
        {
            return make_error(ErrorCode::kValidation, "RAW exposure-bias metadata is not finite",
                              {{"reason", "non_finite_exposure_bias_metadata"}});
        }
        result.exposure_bias_ev = std::clamp(*exposure_bias, -5.0, 5.0);
    }

    double highlight = 0.0;
    const auto malformed = [](const std::string_view tag)
    {
        return make_error(ErrorCode::kValidation,
                          "RAW highlight-preservation metadata is malformed",
                          {{"metadata_tag", std::string(tag)},
                           {"reason", "malformed_highlight_preservation_metadata"}});
    };
    if (tags.canon_lighting_opt)
    {
        if (tags.canon_lighting_opt->size() < 3U)
        {
            return malformed("Exif.Canon.LightingOpt");
        }
        switch ((*tags.canon_lighting_opt)[2])
        {
        case 0:
            highlight = 0.50;
            break;
        case 1:
            highlight = 0.33;
            break;
        case 2:
            highlight = 0.66;
            break;
        default:
            break;
        }
    }
    else if (tags.canon_auto_lighting_optimizer)
    {
        if (tags.canon_auto_lighting_optimizer->empty())
        {
            return malformed("Exif.CanonLiOp.AutoLightingOptimizer");
        }
        switch (tags.canon_auto_lighting_optimizer->front())
        {
        case 0:
            highlight = 0.50;
            break;
        case 1:
            highlight = 0.33;
            break;
        case 2:
            highlight = 0.66;
            break;
        default:
            break;
        }
    }
    else if (tags.fuji_development_dynamic_range || tags.fuji_auto_dynamic_range)
    {
        const auto range = tags.fuji_development_dynamic_range ?
                               *tags.fuji_development_dynamic_range :
                               *tags.fuji_auto_dynamic_range;
        highlight = range == 200 ? 1.0 : range == 400 ? 2.0 : 0.0;
    }
    else if (tags.nikon_color_space)
    {
        highlight = *tags.nikon_color_space == 4 ? 2.0 : 0.0;
    }
    else if (tags.nikon_active_d_lighting)
    {
        switch (*tags.nikon_active_d_lighting)
        {
        case 3:
            highlight = 0.33;
            break;
        case 5:
            highlight = 0.66;
            break;
        case 7:
            highlight = 1.00;
            break;
        case 8:
            highlight = 1.10;
            break;
        case 9:
            highlight = 1.20;
            break;
        case 10:
            highlight = 1.30;
            break;
        case 11:
            highlight = 1.33;
            break;
        default:
            break;
        }
    }
    else if (tags.olympus_camera_settings_gradation || tags.olympus_raw_development_gradation)
    {
        const auto &gradation = tags.olympus_camera_settings_gradation ?
                                    *tags.olympus_camera_settings_gradation :
                                    *tags.olympus_raw_development_gradation;
        if (gradation.size() < 3U)
        {
            return malformed(tags.olympus_camera_settings_gradation ? "Exif.OlympusCs.Gradation" :
                                                                      "Exif.OlympusRd2.Gradation");
        }
        const std::int64_t override_state = gradation.size() > 3U ? gradation[3] : 0;
        if (override_state == 1)
        {
            highlight = 0.33;
        }
        else if (gradation[0] == -1 && gradation[1] == -1 && gradation[2] == 1)
        {
            highlight = 0.66;
        }
    }
    else if (tags.pentax_dynamic_range_expansion)
    {
        if (tags.pentax_dynamic_range_expansion->empty())
        {
            return malformed("Exif.Pentax.DynamicRangeExpansion");
        }
        highlight = tags.pentax_dynamic_range_expansion->front() == 1U ? 1.0 : 0.0;
    }
    result.highlight_preservation_ev = std::clamp(highlight, -1.0, 4.0);
    return result;
}

Result<InspectionResult> inspection_from_libraw(LibRaw &decoder, const std::string_view input_uri)
{
    const auto &raw = decoder.imgdata;
    if (raw.sizes.width == 0 || raw.sizes.height == 0)
    {
        return make_error(ErrorCode::kValidation, "LibRaw returned invalid RAW dimensions",
                          {{"input_uri", std::string(input_uri)}});
    }
    InspectionResult result;
    result.input_uri = std::string(input_uri);
    result.format = "raw";
    result.make = raw.idata.make;
    result.model = raw.idata.model;
    result.width = static_cast<std::uint32_t>(raw.sizes.width);
    result.height = static_cast<std::uint32_t>(raw.sizes.height);
    apply_display_rotation_to_size(result.width, result.height,
                                   clockwise_quarters_from_libraw_flip(raw.sizes.flip));
    result.is_raw = true;
    if (raw.other.iso_speed > 0.0F)
    {
        result.iso = static_cast<double>(raw.other.iso_speed);
    }
    if (raw.other.aperture > 0.0F)
    {
        result.aperture = static_cast<double>(raw.other.aperture);
    }
    if (raw.other.focal_len > 0.0F)
    {
        result.focal_length_mm = static_cast<double>(raw.other.focal_len);
    }
    if (raw.other.shutter > 0.0F)
    {
        result.shutter_s = static_cast<double>(raw.other.shutter);
    }
    if (raw.other.timestamp > 0)
    {
        result.captured_unix_s = static_cast<std::int64_t>(raw.other.timestamp);
    }
    return result;
}

Result<InspectionResult> identify_raw(const std::string_view input_uri)
{
    auto decoder = open_libraw_file(input_uri);
    if (!decoder)
    {
        return decoder.error();
    }
    return inspection_from_libraw(*decoder.value(), input_uri);
}

[[nodiscard]] int select_libraw_jpeg_thumb_index(const libraw_thumbnail_list_t &list,
                                                 const std::uint32_t max_edge)
{
    int best_sufficient = -1;
    std::uint32_t best_sufficient_span = 0;
    int best_any = -1;
    std::uint32_t best_any_span = 0;
    const int count = std::min(list.thumbcount, LIBRAW_THUMBNAIL_MAXCOUNT);
    for (int index = 0; index < count; ++index)
    {
        const auto &item = list.thumblist[index];
        if (item.tformat != LIBRAW_INTERNAL_THUMBNAIL_JPEG)
        {
            continue;
        }
        const auto width = static_cast<std::uint32_t>(item.twidth);
        const auto height = static_cast<std::uint32_t>(item.theight);
        const auto span = std::max(width, height);
        if (span == 0)
        {
            continue;
        }
        if (span > best_any_span)
        {
            best_any_span = span;
            best_any = index;
        }
        if (max_edge > 0 && span >= max_edge &&
            (best_sufficient < 0 || span < best_sufficient_span))
        {
            best_sufficient_span = span;
            best_sufficient = index;
        }
    }
    return best_sufficient >= 0 ? best_sufficient : best_any;
}

Result<EmbeddedPreview> copy_unpacked_jpeg_thumb(LibRaw &decoder, const std::string_view input_uri)
{
    const auto &thumb = decoder.imgdata.thumbnail;
    if (thumb.tformat != LIBRAW_THUMBNAIL_JPEG || thumb.thumb == nullptr || thumb.tlength == 0)
    {
        return make_error(ErrorCode::kUnsupported, "RAW embedded preview is not JPEG",
                          {{"input_uri", std::string(input_uri)}});
    }
    EmbeddedPreview preview;
    preview.mime_type = "image/jpeg";
    preview.color_profile.kind = ColorProfileKind::kBuiltin;
    preview.color_profile.model = ColorModel::kRgb;
    preview.color_profile.identifier = "srgb";
    preview.width = static_cast<std::uint32_t>(thumb.twidth);
    preview.height = static_cast<std::uint32_t>(thumb.theight);
    const int quarters = clockwise_quarters_from_libraw_flip(decoder.imgdata.sizes.flip);
    const bool jpeg_portrait = preview.height > preview.width;
    const bool sensor_portrait = decoder.imgdata.sizes.height > decoder.imgdata.sizes.width;
    if (quarters % 2 != 0 && jpeg_portrait != sensor_portrait)
    {
        preview.rotate_quarters = 0;
    }
    else
    {
        preview.rotate_quarters = quarters;
        apply_display_rotation_to_size(preview.width, preview.height, quarters);
    }
    const auto *begin = reinterpret_cast<const std::uint8_t *>(thumb.thumb);
    preview.bytes.assign(begin, begin + thumb.tlength);
    return preview;
}

Result<EmbeddedPreview> unpack_open_jpeg_thumb(LibRaw &decoder, const std::string_view input_uri,
                                               const std::uint32_t max_edge)
{
    const int selected = select_libraw_jpeg_thumb_index(decoder.imgdata.thumbs_list, max_edge);
    int thumb_status = LIBRAW_UNSPECIFIED_ERROR;
    if (selected >= 0)
    {
        thumb_status = decoder.unpack_thumb_ex(selected);
    }
    if (thumb_status != LIBRAW_SUCCESS)
    {
        thumb_status = decoder.unpack_thumb();
    }
    if (thumb_status != LIBRAW_SUCCESS)
    {
        return make_error(
            ErrorCode::kUnsupported, "RAW file has no embedded preview",
            {{"detail", libraw_strerror(thumb_status)}, {"input_uri", std::string(input_uri)}});
    }
    return copy_unpacked_jpeg_thumb(decoder, input_uri);
}

Result<EmbeddedPreview> extract_libraw_preview(const std::string_view input_uri,
                                               const std::uint32_t max_edge,
                                               const CancellationToken &cancellation)
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    auto decoder = open_libraw_file(input_uri);
    if (!decoder)
    {
        return decoder.error();
    }
    cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    return unpack_open_jpeg_thumb(*decoder.value(), input_uri, max_edge);
}

Result<RawInspectPreview> inspect_raw_with_embedded_preview(const std::string_view input_uri,
                                                            const std::uint32_t max_edge,
                                                            const CancellationToken &cancellation)
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    auto decoder = open_libraw_file(input_uri);
    if (!decoder)
    {
        return decoder.error();
    }
    auto inspection = inspection_from_libraw(*decoder.value(), input_uri);
    if (!inspection)
    {
        return inspection.error();
    }
    cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    RawInspectPreview result;
    result.inspection = std::move(inspection).value();
    auto preview = unpack_open_jpeg_thumb(*decoder.value(), input_uri, max_edge);
    if (preview)
    {
        result.embedded_preview = std::move(preview).value();
    }
    return result;
}

Result<DecodedRaw> decode_raw(const std::string_view input_uri,
                              const CancellationToken &cancellation)
try
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    auto decoder = open_libraw_file(input_uri);
    if (!decoder)
    {
        return decoder.error();
    }
    active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    const int unpack_status = decoder.value()->unpack();
    if (unpack_status != LIBRAW_SUCCESS)
    {
        return make_error(ErrorCode::kValidation, "LibRaw could not unpack the RAW input",
                          {{"detail", libraw_strerror(unpack_status)},
                           {"input_uri", std::string(input_uri)},
                           {"reason", "libraw_unpack_failed"}});
    }

    const auto &raw = decoder.value()->imgdata;
    const auto &sizes = raw.sizes;
    const bool xtrans = raw.idata.filters == LIBRAW_XTRANS;
    const bool missing_raw = raw.rawdata.raw_image == nullptr;
    const bool bayer = !xtrans && raw.idata.filters != 0 && !missing_raw;
    if (!bayer)
    {
        const char *sensor = "non_bayer";
        if (xtrans)
        {
            sensor = "xtrans";
        }
        else if (missing_raw)
        {
            sensor = "missing_raw_image";
        }
        return make_error(ErrorCode::kUnsupported,
                          "The first-frame RAW decoder requires a 16-bit Bayer CFA",
                          {{"input_uri", std::string(input_uri)},
                           {"reason", "unsupported_raw_sensor"},
                           {"sensor", sensor}});
    }
    if (sizes.width == 0 || sizes.height == 0 || sizes.raw_pitch == 0 ||
        static_cast<std::uint32_t>(sizes.left_margin) + sizes.width > sizes.raw_width ||
        static_cast<std::uint32_t>(sizes.top_margin) + sizes.height > sizes.raw_height)
    {
        return make_error(
            ErrorCode::kValidation, "LibRaw returned invalid RAW dimensions",
            {{"input_uri", std::string(input_uri)}, {"reason", "invalid_raw_dimensions"}});
    }
    constexpr std::uint32_t kMaxRawDimension = 65535U;
    constexpr std::uint64_t kMaxRawCfaBytes = 512ULL * 1024ULL * 1024ULL;
    const auto pixel_count =
        saturating_multiply(static_cast<std::uint64_t>(sizes.width), sizes.height);
    const auto cfa_bytes = saturating_multiply(pixel_count, sizeof(std::uint16_t));
    if (sizes.width > kMaxRawDimension || sizes.height > kMaxRawDimension ||
        static_cast<std::uint32_t>(sizes.raw_width) > kMaxRawDimension ||
        static_cast<std::uint32_t>(sizes.raw_height) > kMaxRawDimension ||
        cfa_bytes > kMaxRawCfaBytes)
    {
        return make_error(ErrorCode::kUnsupported, "RAW frame exceeds the first-frame size limit",
                          {{"height", std::to_string(sizes.height)},
                           {"input_uri", std::string(input_uri)},
                           {"reason", "oversized_raw_frame"},
                           {"width", std::to_string(sizes.width)}});
    }

    DecodedRaw result;
    result.width = sizes.width;
    result.height = sizes.height;
    result.rotate_quarters = clockwise_quarters_from_libraw_flip(sizes.flip);
    result.black_level = static_cast<std::int32_t>(
        std::min(raw.color.black, static_cast<unsigned>(std::numeric_limits<std::int32_t>::max())));
    result.white_level = raw.color.maximum > 0 ? raw.color.maximum : 65535U;
    result.make = raw.idata.make;
    result.model = raw.idata.model;
    float deflicker_black = 0.0F;
    for (std::size_t channel = 0; channel < 4U; ++channel)
    {
        const auto separate =
            static_cast<std::uint16_t>(raw.rawdata.color.black + raw.rawdata.color.cblack[channel]);
        deflicker_black += static_cast<float>(separate);
    }
    result.exposure_deflicker_black_level =
        static_cast<std::uint16_t>(std::round(deflicker_black / 4.0F));
    const unsigned deflicker_white = raw.rawdata.color.linear_max[0] != 0U ?
                                         raw.rawdata.color.linear_max[0] :
                                         raw.rawdata.color.maximum;
    result.exposure_deflicker_white_level = static_cast<std::uint16_t>(deflicker_white);
    active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    result.exposure_metadata = read_legacy_exposure_metadata(local_path(input_uri));

    if (raw.color.as_shot_wb_applied)
    {
        result.as_shot_white_balance = {1.0F, 1.0F, 1.0F, 1.0F};
        result.has_as_shot_white_balance = true;
    }
    else
    {
        result.has_as_shot_white_balance = normalize_white_balance(
            raw.color.cam_mul, raw.idata.colors, result.as_shot_white_balance);
    }
    result.has_camera_reference_white_balance = normalize_white_balance(
        raw.color.pre_mul, raw.idata.colors, result.camera_reference_white_balance);
    constexpr std::array<float, 9> linear_srgb_to_xyz_d50{0.4360747F, 0.3850649F, 0.1430804F,
                                                          0.2225045F, 0.7168786F, 0.0606169F,
                                                          0.0139322F, 0.0971045F, 0.7141733F};
    result.color_profile.kind = ColorProfileKind::kMatrix;
    result.color_profile.model = ColorModel::kRgb;
    result.color_profile.identifier = "enhanced_matrix";
    result.color_profile.has_matrix = true;
    result.color_profile.camera_input = true;
    for (std::size_t xyz_channel = 0; xyz_channel < 3; ++xyz_channel)
    {
        for (std::size_t camera_channel = 0; camera_channel < 3; ++camera_channel)
        {
            float value = 0.0F;
            for (std::size_t srgb_channel = 0; srgb_channel < 3; ++srgb_channel)
            {
                value += linear_srgb_to_xyz_d50[xyz_channel * 3U + srgb_channel] *
                         raw.color.rgb_cam[srgb_channel][camera_channel];
            }
            if (!std::isfinite(value))
            {
                return make_error(ErrorCode::kValidation,
                                  "LibRaw camera colour matrix contains a non-finite value");
            }
            result.color_profile.matrix_to_xyz_d50[xyz_channel * 3U + camera_channel] = value;
        }
    }

    result.cfa_width = 2;
    result.cfa_height = 2;
    result.cfa_channels.reserve(4);
    for (std::uint32_t y = 0; y < result.cfa_height; ++y)
    {
        for (std::uint32_t x = 0; x < result.cfa_width; ++x)
        {
            auto channel = channel_for(decoder.value()->COLOR(
                static_cast<int>(sizes.top_margin + y), static_cast<int>(sizes.left_margin + x)));
            if (!channel)
            {
                return channel.error();
            }
            result.cfa_channels.push_back(channel.value());
        }
    }

    const std::size_t pitch = sizes.raw_pitch / sizeof(std::uint16_t);
    result.pixels.resize(static_cast<std::size_t>(result.width) * result.height);
    for (std::uint32_t y = 0; y < result.height; ++y)
    {
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        const auto *source = raw.rawdata.raw_image +
                             (static_cast<std::size_t>(sizes.top_margin) + y) * pitch +
                             sizes.left_margin;
        const auto row_offset =
            static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * result.width);
        std::copy_n(source, result.width, result.pixels.begin() + row_offset);
    }
    return result;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "RAW decode allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<std::shared_ptr<const ExposureAnalysisContext>>
build_exposure_analysis_context(const DecodedRaw &raw, const CancellationToken &cancellation)
try
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    const std::uint64_t pixel_count = static_cast<std::uint64_t>(raw.width) * raw.height;
    if (raw.width == 0U || raw.height == 0U ||
        pixel_count > std::numeric_limits<std::size_t>::max() ||
        raw.pixels.size() != static_cast<std::size_t>(pixel_count))
    {
        return make_error(ErrorCode::kValidation,
                          "Exposure RAW analysis input does not match its dimensions",
                          {{"reason", "invalid_exposure_raw_dimensions"}});
    }
    if (pixel_count > std::numeric_limits<std::uint32_t>::max())
    {
        return make_error(ErrorCode::kUnsupported, "Exposure RAW histogram counters would overflow",
                          {{"reason", "exposure_raw_histogram_overflow"}});
    }

    auto context = std::make_shared<ExposureAnalysisContext>();
    context->raw_histogram.assign(kExposureRawHistogramBins, 0U);
    context->raw_pixel_count = pixel_count;
    context->raw_black_level = raw.exposure_deflicker_black_level;
    context->raw_white_level = raw.exposure_deflicker_white_level;
    context->metadata = raw.exposure_metadata;
    for (std::uint32_t row = 0; row < raw.height; ++row)
    {
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        const std::size_t begin = static_cast<std::size_t>(row) * raw.width;
        const std::size_t end = begin + raw.width;
        for (std::size_t index = begin; index < end; ++index)
        {
            ++context->raw_histogram[raw.pixels[index]];
        }
    }
    std::shared_ptr<const ExposureAnalysisContext> published = std::move(context);
    return published;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Exposure RAW analysis allocation failed",
                      {{"reason", "allocation_failed"}});
}

std::uint64_t estimate_raw_render_memory(const DecodedRaw &raw, const Recipe &recipe,
                                         const std::uint32_t width, const std::uint32_t height,
                                         const std::size_t output_bytes_per_pixel) noexcept
{
    const std::uint64_t output_pixels = static_cast<std::uint64_t>(width) * height;
    const std::uint64_t output_bytes =
        saturating_multiply(output_pixels, static_cast<std::uint64_t>(output_bytes_per_pixel));
    const std::uint64_t raw_pixels = raw.pixels.size();
    const std::uint64_t raw_bytes = saturating_multiply(raw_pixels, sizeof(std::uint16_t));
    // Worst case: source, working, normalization, output, and proof profiles
    // each own three input and three output shaper curves.
    constexpr std::uint64_t color_lut_bytes = 30U * 0x10000U * sizeof(float);
    const std::uint64_t failure_detail_bytes = saturating_add(
        static_cast<std::uint64_t>(raw.exposure_metadata.failure_detail.capacity()), 1U);
    std::uint64_t exposure_analysis_bytes = sizeof(ExposureAnalysisContext);
    exposure_analysis_bytes = saturating_add(
        exposure_analysis_bytes, 2U * sizeof(std::shared_ptr<const ExposureAnalysisContext>));
    exposure_analysis_bytes =
        saturating_add(exposure_analysis_bytes, kExposureRawHistogramBins * sizeof(std::uint32_t));
    exposure_analysis_bytes =
        saturating_add(exposure_analysis_bytes, saturating_multiply(2U, failure_detail_bytes));
    const std::uint64_t float_rgb_bytes = saturating_multiply(output_pixels, 3U * sizeof(float));
    std::uint64_t working_bytes = 0U;
    const auto add_working_bytes = [&working_bytes](const std::uint64_t bytes) noexcept
    { working_bytes = saturating_add(working_bytes, bytes); };
    add_working_bytes(output_bytes);
    add_working_bytes(saturating_multiply(2U, float_rgb_bytes));
    add_working_bytes(color_lut_bytes);
    add_working_bytes(raw_bytes);
    add_working_bytes(exposure_analysis_bytes);
    bool owns_raw_copy = false;
    for (const auto &operation : recipe.operations)
    {
        if (!operation.enabled)
        {
            continue;
        }
        if (operation.id == "ravo.raw.hotpixels" || operation.id == "ravo.raw.highlights" ||
            operation.id == "ravo.raw.cacorrect")
        {
            owns_raw_copy = true;
        }
        if (operation.id == "ravo.raw.hotpixels")
        {
            add_working_bytes(raw_bytes);
        }
        if (operation.id == "ravo.raw.cacorrect")
        {
            add_working_bytes(
                saturating_multiply(raw_pixels, 3U * sizeof(float) + sizeof(std::uint16_t)));
            const auto avoid = operation.parameters.find("avoid_color_shift");
            if (avoid != operation.parameters.end())
            {
                if (const auto *flag = std::get_if<bool>(&avoid->second.value);
                    flag != nullptr && *flag)
                {
                    add_working_bytes(saturating_multiply(raw_pixels, sizeof(float)));
                }
            }
        }
        if (operation.id == kPrimariesOperationId)
        {
            add_working_bytes(float_rgb_bytes);
        }
        if (operation.id == kProfileGammaOperationId)
        {
            add_working_bytes(float_rgb_bytes);
            add_working_bytes(0x10000U * sizeof(float));
        }
        if (operation.id == kColorCheckerOperationId)
        {
            std::uint64_t patch_count = kColorCheckerMaxPatchCount;
            const auto patches = operation.parameters.find("patches");
            if (patches != operation.parameters.end())
            {
                if (const auto *array = std::get_if<ParameterValue::Array>(&patches->second.value);
                    array != nullptr && array->size() <= kColorCheckerMaxPatchCount)
                {
                    patch_count = array->size();
                }
            }
            const std::uint64_t fit_size = patch_count + 4U;
            add_working_bytes(saturating_multiply(patch_count, sizeof(ColorCheckerPatch)));
            add_working_bytes(saturating_multiply(patch_count, sizeof(std::array<float, 3>)));
            add_working_bytes(saturating_multiply(3U * sizeof(float), fit_size));
            if (patch_count >= 2U && patch_count <= 4U)
            {
                const std::uint64_t solve_size = patch_count;
                add_working_bytes(saturating_multiply(saturating_multiply(solve_size, solve_size),
                                                      sizeof(double)));
                add_working_bytes(saturating_multiply(solve_size, sizeof(int)));
                add_working_bytes(saturating_multiply(solve_size, sizeof(double)));
            }
            else if (patch_count > 4U)
            {
                add_working_bytes(
                    saturating_multiply(saturating_multiply(fit_size, fit_size), sizeof(double)));
                add_working_bytes(saturating_multiply(fit_size, sizeof(int)));
                add_working_bytes(saturating_multiply(fit_size, sizeof(double)));
            }
        }
        if (operation.id == kColorHarmonizerOperationId)
        {
            bool positive_smoothing = false;
            if (const auto smoothing = operation.parameters.find("smoothing");
                smoothing != operation.parameters.end())
            {
                if (const auto *value = std::get_if<double>(&smoothing->second.value);
                    value != nullptr)
                {
                    positive_smoothing = std::isfinite(*value) && *value > 0.0;
                }
                else if (const auto *integer = std::get_if<std::int64_t>(&smoothing->second.value);
                         integer != nullptr)
                {
                    positive_smoothing = *integer > 0;
                }
            }
            if (positive_smoothing)
            {
                // The normal two-RGB-buffer estimate already covers borrowed
                // working pixels and the eventual owned RGB output.  Positive
                // C14 smoothing additionally owns JCH (3c); the S2.2 owner is
                // the one authority for its consumed correction (2c) plus
                // recurrence scratch (2c) bytes.
                add_working_bytes(saturating_multiply(output_pixels, 3U * sizeof(float)));
                add_working_bytes(detail::recursive_gaussian_zero_2c_bytes(width, height));
            }
        }
        if (operation.mask_id.has_value() && (operation.id == kColorHarmonizerOperationId ||
                                              operation.id == "ravo.effect.graduatednd"))
        {
            // Masked dispatch moves the current working image into an owned
            // pre-operation snapshot, creates a distinct operation-output
            // image, then owns one alpha result and depth-first evaluator
            // group scratch before the normal mix can publish.  Keep these
            // terms explicit rather than relying on a later bad_alloc.
            add_working_bytes(float_rgb_bytes); // pre-operation snapshot
            add_working_bytes(float_rgb_bytes); // operation output
            const MaskEvaluatorMemoryEstimate mask_memory =
                estimate_mask_evaluator_memory(recipe.masks, *operation.mask_id, width, height);
            add_working_bytes(mask_memory.alpha_plane_bytes);
            add_working_bytes(mask_memory.evaluator_scratch_bytes);
        }
    }
    // A RAW repair operation copies the decoded frame before mutation. Its
    // metadata string allocation coexists with the source and analysis copy.
    if (owns_raw_copy)
    {
        add_working_bytes(raw_bytes);
        add_working_bytes(failure_detail_bytes);
    }
    return working_bytes;
}

Result<void> write_png_atomically(const std::string_view output_uri, const RenderedImage &image)
{
    const QString path = local_path(output_uri);
    if (QFileInfo::exists(path))
    {
        return make_error(ErrorCode::kConflict, "Render output already exists",
                          {{"output_uri", std::string(output_uri)}});
    }

    auto encoded = encode_png_bytes(image);
    if (!encoded)
    {
        return encoded.error();
    }

    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    const auto &bytes = encoded.value();
    if (!output.open(QIODevice::WriteOnly) ||
        output.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<qint64>(bytes.size())) != static_cast<qint64>(bytes.size()) ||
        !output.commit())
    {
        return make_error(ErrorCode::kIo, "Unable to atomically write PNG output",
                          {{"output_uri", std::string(output_uri)},
                           {"qt_error", output.errorString().toStdString()}});
    }
    return {};
}

namespace
{

[[nodiscard]] TaskError capture_read_error(const std::string_view message,
                                           const std::string_view reason,
                                           const std::string_view field = {},
                                           const std::string_view path = {},
                                           const std::string_view value = {})
{
    std::map<std::string, std::string, std::less<>> context{{"reason", std::string(reason)}};
    if (!field.empty())
    {
        context.emplace("field", std::string(field));
    }
    if (!path.empty())
    {
        context.emplace("path", std::string(path));
    }
    if (!value.empty())
    {
        context.emplace("value", std::string(value));
    }
    return make_error(ErrorCode::kValidation, std::string(message), std::move(context));
}

[[nodiscard]] std::string sanitize_error_value(const std::string_view raw,
                                               const std::size_t maximum = 64U)
{
    if (raw.empty() || raw.size() > maximum)
    {
        return {};
    }
    for (const char raw_character : raw)
    {
        const auto ch = static_cast<unsigned char>(raw_character);
        if (ch < 0x20U || ch > 0x7EU)
        {
            return {};
        }
    }
    return std::string(raw);
}

[[nodiscard]] std::map<std::string, std::string, std::less<>>
capture_source_context(const std::string_view input_uri, const std::string_view reason = {},
                       const std::string_view detail = {})
{
    std::map<std::string, std::string, std::less<>> context;
    if (!reason.empty())
    {
        context.emplace("reason", std::string(reason));
    }
    const auto safe_uri = sanitize_error_value(input_uri, 512U);
    if (!safe_uri.empty())
    {
        context.emplace("input_uri", safe_uri);
    }
    const auto safe_detail = sanitize_error_value(detail, 256U);
    if (!safe_detail.empty())
    {
        context.emplace("detail", safe_detail);
    }
    return context;
}

[[nodiscard]] bool is_ascii_digit(const char ch) noexcept
{
    return ch >= '0' && ch <= '9';
}

[[nodiscard]] int digit_value(const char ch) noexcept
{
    return ch - '0';
}

[[nodiscard]] bool is_leap_year(const int year) noexcept
{
    if (year % 4 != 0)
    {
        return false;
    }
    if (year % 100 != 0)
    {
        return true;
    }
    return year % 400 == 0;
}

[[nodiscard]] int days_in_month(const int year, const int month) noexcept
{
    static constexpr int kDays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year))
    {
        return 29;
    }
    return kDays[month];
}

struct EngineUnsignedRational
{
    std::uint32_t numerator = 0;
    std::uint32_t denominator = 1;
};

struct UInt128
{
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
};

[[nodiscard]] UInt128 mul_u64_u64(const std::uint64_t left, const std::uint64_t right) noexcept
{
    const std::uint64_t left_lo = left & 0xffffffffU;
    const std::uint64_t left_hi = left >> 32U;
    const std::uint64_t right_lo = right & 0xffffffffU;
    const std::uint64_t right_hi = right >> 32U;
    const std::uint64_t p0 = left_lo * right_lo;
    const std::uint64_t p1 = left_lo * right_hi;
    const std::uint64_t p2 = left_hi * right_lo;
    const std::uint64_t p3 = left_hi * right_hi;
    const std::uint64_t mid = (p0 >> 32U) + (p1 & 0xffffffffU) + (p2 & 0xffffffffU);
    UInt128 result;
    result.lo = (p0 & 0xffffffffU) | (mid << 32U);
    result.hi = p3 + (p1 >> 32U) + (p2 >> 32U) + (mid >> 32U);
    return result;
}

[[nodiscard]] bool add_u128_checked(const UInt128 left, const UInt128 right, UInt128 &out) noexcept
{
    out.lo = left.lo + right.lo;
    const std::uint64_t carry = out.lo < left.lo ? 1U : 0U;
    if (right.hi > std::numeric_limits<std::uint64_t>::max() - left.hi)
    {
        return false;
    }
    const std::uint64_t hi = left.hi + right.hi;
    if (carry != 0U && hi == std::numeric_limits<std::uint64_t>::max())
    {
        return false;
    }
    out.hi = hi + carry;
    return true;
}

[[nodiscard]] bool mul_u128_u64(const UInt128 left, const std::uint64_t right,
                                UInt128 &out) noexcept
{
    const UInt128 lo = mul_u64_u64(left.lo, right);
    const UInt128 hi = mul_u64_u64(left.hi, right);
    if (hi.hi != 0U)
    {
        return false;
    }
    out.lo = lo.lo;
    if (lo.hi > std::numeric_limits<std::uint64_t>::max() - hi.lo)
    {
        return false;
    }
    out.hi = lo.hi + hi.lo;
    return true;
}

[[nodiscard]] int cmp_u128(const UInt128 left, const UInt128 right) noexcept
{
    if (left.hi != right.hi)
    {
        return left.hi > right.hi ? 1 : -1;
    }
    if (left.lo != right.lo)
    {
        return left.lo > right.lo ? 1 : -1;
    }
    return 0;
}

[[nodiscard]] int bit_of128(const UInt128 value, const int index) noexcept
{
    if (index >= 64)
    {
        return static_cast<int>((value.hi >> (index - 64)) & 1U);
    }
    return static_cast<int>((value.lo >> index) & 1U);
}

void set_bit128(UInt128 &value, const int index) noexcept
{
    if (index >= 64)
    {
        value.hi |= 1ULL << (index - 64);
        return;
    }
    value.lo |= 1ULL << index;
}

[[nodiscard]] bool is_zero(const UInt128 value) noexcept
{
    return value.lo == 0U && value.hi == 0U;
}

[[nodiscard]] UInt128 subtract_u128(const UInt128 left, const UInt128 right) noexcept
{
    UInt128 result;
    result.lo = left.lo - right.lo;
    result.hi = left.hi - right.hi - (left.lo < right.lo ? 1U : 0U);
    return result;
}

[[nodiscard]] bool divmod_u128(const UInt128 numerator, const UInt128 denominator,
                               UInt128 &quotient, UInt128 &remainder) noexcept
{
    if (is_zero(denominator))
    {
        return false;
    }
    quotient = {};
    remainder = {};
    for (int index = 127; index >= 0; --index)
    {
        const bool overflow = (remainder.hi >> 63U) != 0U;
        remainder.hi = (remainder.hi << 1U) | (remainder.lo >> 63U);
        remainder.lo =
            (remainder.lo << 1U) | static_cast<std::uint64_t>(bit_of128(numerator, index));
        if (overflow || cmp_u128(remainder, denominator) >= 0)
        {
            remainder = subtract_u128(remainder, denominator);
            set_bit128(quotient, index);
        }
    }
    return true;
}

[[nodiscard]] Result<std::uint64_t> round_div_ties_away(const UInt128 numerator,
                                                        const UInt128 denominator)
{
    UInt128 quotient{};
    UInt128 remainder{};
    if (!divmod_u128(numerator, denominator, quotient, remainder))
    {
        return capture_read_error("Capture rational overflowed the conversion range",
                                  "capture_rational_overflow");
    }
    if (quotient.hi != 0U)
    {
        return capture_read_error("Capture rational overflowed the conversion range",
                                  "capture_rational_overflow");
    }
    std::uint64_t rounded = quotient.lo;
    if (!is_zero(remainder) && cmp_u128(remainder, subtract_u128(denominator, remainder)) >= 0)
    {
        if (rounded == std::numeric_limits<std::uint64_t>::max())
        {
            return capture_read_error("Capture rational overflowed the conversion range",
                                      "capture_rational_overflow");
        }
        ++rounded;
    }
    return rounded;
}

[[nodiscard]] Result<void> parse_local_exif(const std::string_view text,
                                            const std::string_view field,
                                            const std::string_view path)
{
    if (text.size() != 19U)
    {
        return capture_read_error("Capture local time must be exactly YYYY:MM:DD HH:MM:SS",
                                  "invalid_capture_datetime", field, path,
                                  sanitize_error_value(text));
    }
    for (const std::size_t index : {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U, 11U, 12U, 14U, 15U, 17U, 18U})
    {
        if (!is_ascii_digit(text[index]))
        {
            return capture_read_error("Capture local time must use ASCII digits",
                                      "invalid_capture_datetime", field, path,
                                      sanitize_error_value(text));
        }
    }
    if (text[4] != ':' || text[7] != ':' || text[10] != ' ' || text[13] != ':' || text[16] != ':')
    {
        return capture_read_error("Capture local time must use Exif separators",
                                  "invalid_capture_datetime", field, path,
                                  sanitize_error_value(text));
    }
    const int year = digit_value(text[0]) * 1000 + digit_value(text[1]) * 100 +
                     digit_value(text[2]) * 10 + digit_value(text[3]);
    const int month = digit_value(text[5]) * 10 + digit_value(text[6]);
    const int day = digit_value(text[8]) * 10 + digit_value(text[9]);
    const int hour = digit_value(text[11]) * 10 + digit_value(text[12]);
    const int minute = digit_value(text[14]) * 10 + digit_value(text[15]);
    const int second = digit_value(text[17]) * 10 + digit_value(text[18]);
    if (year < 1 || year > 9999 || month < 1 || month > 12 || day < 1 ||
        day > days_in_month(year, month) || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59)
    {
        return capture_read_error("Capture local time is outside the Gregorian calendar",
                                  "invalid_capture_datetime", field, path,
                                  sanitize_error_value(text));
    }
    return {};
}

[[nodiscard]] Result<void> parse_subsecond(const std::string_view text,
                                           const std::string_view field,
                                           const std::string_view path)
{
    if (text.empty() || text.size() > 9U || !std::all_of(text.begin(), text.end(), is_ascii_digit))
    {
        return capture_read_error("Capture subsecond must be 1 to 9 ASCII digits",
                                  "invalid_capture_subsecond", field, path,
                                  sanitize_error_value(text));
    }
    return {};
}

[[nodiscard]] Result<std::int32_t> parse_utc_offset(const std::string_view text,
                                                    const std::string_view field,
                                                    const std::string_view path)
{
    if (text.size() != 6U || (text[0] != '+' && text[0] != '-') || text[3] != ':' ||
        !is_ascii_digit(text[1]) || !is_ascii_digit(text[2]) || !is_ascii_digit(text[4]) ||
        !is_ascii_digit(text[5]))
    {
        return capture_read_error("Capture UTC offset must be +HH:MM or -HH:MM",
                                  "invalid_capture_utc_offset", field, path,
                                  sanitize_error_value(text));
    }
    const int hours = digit_value(text[1]) * 10 + digit_value(text[2]);
    const int minutes = digit_value(text[4]) * 10 + digit_value(text[5]);
    if (minutes > 59 || hours > 14 || (hours == 14 && minutes != 0))
    {
        return capture_read_error("Capture UTC offset is outside -14:00..+14:00",
                                  "invalid_capture_utc_offset", field, path,
                                  sanitize_error_value(text));
    }
    if (text[0] == '-' && hours == 0 && minutes == 0)
    {
        return capture_read_error("Capture UTC offset rejects -00:00", "invalid_capture_utc_offset",
                                  field, path, sanitize_error_value(text));
    }
    const std::int32_t value = static_cast<std::int32_t>(hours * 60 + minutes);
    return text[0] == '-' ? -value : value;
}

[[nodiscard]] Result<std::int32_t> engine_dms_to_microdegrees(
    const EngineUnsignedRational degrees, const EngineUnsignedRational minutes,
    const EngineUnsignedRational seconds, const bool negative, const std::int32_t minimum,
    const std::int32_t maximum, const std::string_view field, const std::string_view path)
{
    if (degrees.denominator == 0U || minutes.denominator == 0U || seconds.denominator == 0U)
    {
        return capture_read_error("Capture GPS rational has a zero denominator",
                                  "invalid_capture_gps_rational", field, path);
    }
    const auto below_60 = [](const EngineUnsignedRational value) noexcept
    {
        return static_cast<std::uint64_t>(value.numerator) <
               60ULL * static_cast<std::uint64_t>(value.denominator);
    };
    if (!below_60(minutes) || !below_60(seconds))
    {
        return capture_read_error("Capture GPS minutes or seconds are not below 60",
                                  "invalid_capture_gps_dms", field, path);
    }
    const std::uint64_t deg_n = degrees.numerator;
    const std::uint64_t deg_d = degrees.denominator;
    const std::uint64_t min_n = minutes.numerator;
    const std::uint64_t min_d = minutes.denominator;
    const std::uint64_t sec_n = seconds.numerator;
    const std::uint64_t sec_d = seconds.denominator;
    UInt128 term0{};
    UInt128 term1{};
    UInt128 term2{};
    if (!mul_u128_u64(mul_u64_u64(deg_n, min_d), sec_d, term0) ||
        !mul_u128_u64(term0, 3600U, term0) ||
        !mul_u128_u64(mul_u64_u64(min_n, deg_d), sec_d, term1) ||
        !mul_u128_u64(term1, 60U, term1) || !mul_u128_u64(mul_u64_u64(sec_n, deg_d), min_d, term2))
    {
        return capture_read_error("Capture GPS rational overflowed the conversion range",
                                  "capture_rational_overflow", field, path);
    }
    UInt128 exact_num{};
    if (!add_u128_checked(term0, term1, exact_num) ||
        !add_u128_checked(exact_num, term2, exact_num))
    {
        return capture_read_error("Capture GPS rational overflowed the conversion range",
                                  "capture_rational_overflow", field, path);
    }
    UInt128 exact_den{};
    if (!mul_u128_u64(mul_u64_u64(deg_d, min_d), sec_d, exact_den) ||
        !mul_u128_u64(exact_den, 3600U, exact_den))
    {
        return capture_read_error("Capture GPS rational overflowed the conversion range",
                                  "capture_rational_overflow", field, path);
    }
    const std::uint64_t bound_degrees =
        static_cast<std::uint64_t>(maximum < 0 ? 0 : maximum) / 1000000ULL;
    UInt128 bound_num{};
    if (!mul_u128_u64(exact_den, bound_degrees, bound_num))
    {
        return capture_read_error("Capture GPS rational overflowed the conversion range",
                                  "capture_rational_overflow", field, path);
    }
    if (cmp_u128(exact_num, bound_num) > 0)
    {
        return capture_read_error("Capture coordinate is outside its legal bound",
                                  "invalid_capture_gps_bounds", field, path);
    }
    UInt128 whole{};
    UInt128 remainder{};
    if (!divmod_u128(exact_num, exact_den, whole, remainder) || whole.hi != 0U)
    {
        return capture_read_error("Capture GPS rational overflowed the conversion range",
                                  "capture_rational_overflow", field, path);
    }
    UInt128 scaled_remainder{};
    if (!mul_u128_u64(remainder, 1000000U, scaled_remainder))
    {
        return capture_read_error("Capture GPS rational overflowed the conversion range",
                                  "capture_rational_overflow", field, path);
    }
    auto fraction = round_div_ties_away(scaled_remainder, exact_den);
    if (!fraction || whole.lo > std::numeric_limits<std::uint64_t>::max() / 1000000U ||
        fraction.value() > std::numeric_limits<std::uint64_t>::max() - whole.lo * 1000000U)
    {
        return fraction ? capture_read_error("Capture GPS rational overflowed the conversion range",
                                             "capture_rational_overflow", field, path) :
                          fraction.error();
    }
    const std::uint64_t magnitude = whole.lo * 1000000U + fraction.value();
    const std::uint64_t allowed =
        negative ? static_cast<std::uint64_t>(-static_cast<std::int64_t>(minimum)) :
                   static_cast<std::uint64_t>(maximum);
    if (magnitude > allowed)
    {
        return capture_read_error("Capture coordinate is outside its legal bound",
                                  "invalid_capture_gps_bounds", field, path);
    }
    const std::int64_t signed_value =
        negative ? -static_cast<std::int64_t>(magnitude) : static_cast<std::int64_t>(magnitude);
    if (signed_value < minimum || signed_value > maximum)
    {
        return capture_read_error("Capture coordinate is outside its legal bound",
                                  "invalid_capture_gps_bounds", field, path);
    }
    return signed_value == 0 ? 0 : static_cast<std::int32_t>(signed_value);
}

[[nodiscard]] Result<std::uint32_t> engine_altitude_to_mm(const EngineUnsignedRational value,
                                                          const std::uint32_t maximum_mm,
                                                          const std::string_view field,
                                                          const std::string_view path)
{
    if (value.denominator == 0U)
    {
        return capture_read_error("Capture altitude rational has a zero denominator",
                                  "invalid_capture_gps_rational", field, path);
    }
    UInt128 exact_mm{};
    if (!mul_u128_u64(mul_u64_u64(value.numerator, 1000U), 1U, exact_mm))
    {
        return capture_read_error("Capture altitude overflowed the conversion range",
                                  "capture_rational_overflow", field, path);
    }
    const UInt128 bound = mul_u64_u64(maximum_mm, value.denominator);
    if (cmp_u128(exact_mm, bound) > 0)
    {
        return capture_read_error("Capture altitude is outside its legal bound",
                                  "invalid_capture_altitude", field, path);
    }
    auto mm = round_div_ties_away(exact_mm, UInt128{value.denominator, 0U});
    if (!mm)
    {
        return mm.error();
    }
    if (mm.value() > maximum_mm)
    {
        return capture_read_error("Capture altitude is outside its legal bound",
                                  "invalid_capture_altitude", field, path);
    }
    return static_cast<std::uint32_t>(mm.value());
}

enum class AsciiTagStatus
{
    kAbsent,
    kPresent,
    kDuplicate,
    kWrongType,
    kMultiValue,
    kOversized,
    kContainsNul,
};

struct AsciiTag
{
    AsciiTagStatus status = AsciiTagStatus::kAbsent;
    std::string value;
    std::string path;
};

struct Rational3Tag
{
    AsciiTagStatus status = AsciiTagStatus::kAbsent;
    std::array<EngineUnsignedRational, 3> values{};
    std::string path;
};

struct ByteTag
{
    AsciiTagStatus status = AsciiTagStatus::kAbsent;
    std::uint8_t value = 0;
    std::string path;
};

struct RationalTag
{
    AsciiTagStatus status = AsciiTagStatus::kAbsent;
    EngineUnsignedRational value;
    std::string path;
};

[[nodiscard]] Result<void> require_present_ok(const AsciiTag &tag, const std::string_view field)
{
    switch (tag.status)
    {
    case AsciiTagStatus::kAbsent:
    case AsciiTagStatus::kPresent:
        return {};
    case AsciiTagStatus::kDuplicate:
        return capture_read_error("Embedded capture tag is duplicated", "duplicate_capture_tag",
                                  field, tag.path, sanitize_error_value(tag.value));
    case AsciiTagStatus::kWrongType:
        return capture_read_error("Embedded capture tag has the wrong type", "wrong_type", field,
                                  tag.path, sanitize_error_value(tag.value));
    case AsciiTagStatus::kMultiValue:
        return capture_read_error("Embedded capture tag has multiple values", "multi_value", field,
                                  tag.path, sanitize_error_value(tag.value));
    case AsciiTagStatus::kOversized:
        return capture_read_error("Embedded capture tag exceeds its accepted byte count",
                                  "invalid_capture_tag_count", field, tag.path);
    case AsciiTagStatus::kContainsNul:
        return capture_read_error("Embedded capture tag contains a NUL", "contains_nul", field,
                                  tag.path);
    }
    return capture_read_error("Embedded capture tag is malformed", "malformed_capture_tag", field,
                              tag.path);
}

[[nodiscard]] Result<void> require_rational_ok(const AsciiTagStatus status,
                                               const std::string_view field,
                                               const std::string_view path)
{
    switch (status)
    {
    case AsciiTagStatus::kAbsent:
    case AsciiTagStatus::kPresent:
        return {};
    case AsciiTagStatus::kDuplicate:
        return capture_read_error("Embedded GPS tag is duplicated", "duplicate_capture_tag", field,
                                  path);
    case AsciiTagStatus::kWrongType:
        return capture_read_error("Embedded GPS tag has the wrong type", "wrong_type", field, path);
    case AsciiTagStatus::kMultiValue:
        return capture_read_error("Embedded GPS tag has the wrong count", "multi_value", field,
                                  path);
    case AsciiTagStatus::kOversized:
        return capture_read_error("Embedded GPS tag exceeds its accepted byte count",
                                  "invalid_capture_tag_count", field, path);
    case AsciiTagStatus::kContainsNul:
        return capture_read_error("Embedded GPS tag contains a NUL", "contains_nul", field, path);
    }
    return capture_read_error("Embedded GPS tag is malformed", "malformed_capture_tag", field,
                              path);
}

[[nodiscard]] Result<std::optional<EngineCaptureDateTime>>
resolve_engine_datetime(const AsciiTag &photo, const AsciiTag &image, const AsciiTag &fraction,
                        const AsciiTag &offset)
{
    const std::array<std::pair<const AsciiTag *, std::string_view>, 4> fields{{
        {&photo, "captured_datetime"},
        {&image, "captured_datetime"},
        {&fraction, "captured_subsecond_digits"},
        {&offset, "captured_utc_offset_minutes"},
    }};
    for (const auto &[tag, field] : fields)
    {
        auto ok = require_present_ok(*tag, field);
        if (!ok)
        {
            return ok.error();
        }
    }
    const bool has_photo = photo.status == AsciiTagStatus::kPresent;
    const bool has_image = image.status == AsciiTagStatus::kPresent;
    const bool has_fraction = fraction.status == AsciiTagStatus::kPresent;
    const bool has_offset = offset.status == AsciiTagStatus::kPresent;
    if (!has_photo && !has_image)
    {
        if (has_fraction || has_offset)
        {
            return capture_read_error("Capture fraction or offset is present without a base time",
                                      "orphan_capture_datetime_component", "captured_datetime",
                                      has_fraction ? fraction.path : offset.path);
        }
        return std::optional<EngineCaptureDateTime>{};
    }
    EngineCaptureDateTime value;
    if (has_photo)
    {
        auto parsed = parse_local_exif(photo.value, "captured_datetime", photo.path);
        if (!parsed)
        {
            return parsed.error();
        }
        value.local_exif = photo.value;
        if (has_image)
        {
            auto other = parse_local_exif(image.value, "captured_datetime", image.path);
            if (!other)
            {
                return other.error();
            }
            if (image.value != photo.value)
            {
                return capture_read_error("Photo and Image DateTimeOriginal conflict",
                                          "conflicting_capture_datetime", "captured_datetime",
                                          photo.path, sanitize_error_value(photo.value));
            }
        }
    }
    else
    {
        auto parsed = parse_local_exif(image.value, "captured_datetime", image.path);
        if (!parsed)
        {
            return parsed.error();
        }
        value.local_exif = image.value;
    }
    if (has_fraction)
    {
        auto parsed = parse_subsecond(fraction.value, "captured_subsecond_digits", fraction.path);
        if (!parsed)
        {
            return parsed.error();
        }
        value.subsecond_digits = fraction.value;
    }
    if (has_offset)
    {
        auto parsed = parse_utc_offset(offset.value, "captured_utc_offset_minutes", offset.path);
        if (!parsed)
        {
            return parsed.error();
        }
        value.utc_offset_minutes = parsed.value();
    }
    return std::optional<EngineCaptureDateTime>{std::move(value)};
}

[[nodiscard]] Result<std::optional<EngineCaptureLocation>>
resolve_engine_location(const AsciiTag &lat_ref, const Rational3Tag &lat, const AsciiTag &lon_ref,
                        const Rational3Tag &lon, const ByteTag &alt_ref, const RationalTag &alt)
{
    auto lat_ref_ok = require_present_ok(lat_ref, "gps_latitude_ref");
    if (!lat_ref_ok)
    {
        return lat_ref_ok.error();
    }
    auto lon_ref_ok = require_present_ok(lon_ref, "gps_longitude_ref");
    if (!lon_ref_ok)
    {
        return lon_ref_ok.error();
    }
    auto lat_ok = require_rational_ok(lat.status, "gps_latitude_e6", lat.path);
    if (!lat_ok)
    {
        return lat_ok.error();
    }
    auto lon_ok = require_rational_ok(lon.status, "gps_longitude_e6", lon.path);
    if (!lon_ok)
    {
        return lon_ok.error();
    }
    auto alt_ref_ok = require_rational_ok(alt_ref.status, "gps_altitude_ref", alt_ref.path);
    if (!alt_ref_ok)
    {
        return alt_ref_ok.error();
    }
    auto alt_ok = require_rational_ok(alt.status, "gps_altitude", alt.path);
    if (!alt_ok)
    {
        return alt_ok.error();
    }
    const bool any_pair =
        lat_ref.status == AsciiTagStatus::kPresent || lat.status == AsciiTagStatus::kPresent ||
        lon_ref.status == AsciiTagStatus::kPresent || lon.status == AsciiTagStatus::kPresent;
    const bool all_pair =
        lat_ref.status == AsciiTagStatus::kPresent && lat.status == AsciiTagStatus::kPresent &&
        lon_ref.status == AsciiTagStatus::kPresent && lon.status == AsciiTagStatus::kPresent;
    const bool any_alt =
        alt_ref.status == AsciiTagStatus::kPresent || alt.status == AsciiTagStatus::kPresent;
    const bool all_alt =
        alt_ref.status == AsciiTagStatus::kPresent && alt.status == AsciiTagStatus::kPresent;
    if (!any_pair && !any_alt)
    {
        return std::optional<EngineCaptureLocation>{};
    }
    if (any_pair && !all_pair)
    {
        return capture_read_error(
            "Capture location requires latitude, longitude, and both references",
            "incomplete_capture_location", "gps");
    }
    if (any_alt && !all_alt)
    {
        return capture_read_error("Capture altitude requires both the value and reference 0 or 1",
                                  "incomplete_capture_altitude", "gps_altitude");
    }
    if (any_alt && !all_pair)
    {
        return capture_read_error("Capture altitude cannot exist without a complete location",
                                  "orphan_capture_altitude", "gps_altitude");
    }
    if (lat_ref.value != "N" && lat_ref.value != "S")
    {
        return capture_read_error("Capture latitude reference must be N or S",
                                  "invalid_capture_gps_ref", "gps_latitude_ref", lat_ref.path,
                                  sanitize_error_value(lat_ref.value));
    }
    if (lon_ref.value != "E" && lon_ref.value != "W")
    {
        return capture_read_error("Capture longitude reference must be E or W",
                                  "invalid_capture_gps_ref", "gps_longitude_ref", lon_ref.path,
                                  sanitize_error_value(lon_ref.value));
    }
    auto latitude = engine_dms_to_microdegrees(lat.values[0], lat.values[1], lat.values[2],
                                               lat_ref.value == "S", -90000000, 90000000,
                                               "gps_latitude_e6", lat.path);
    if (!latitude)
    {
        return latitude.error();
    }
    auto longitude = engine_dms_to_microdegrees(lon.values[0], lon.values[1], lon.values[2],
                                                lon_ref.value == "W", -180000000, 180000000,
                                                "gps_longitude_e6", lon.path);
    if (!longitude)
    {
        return longitude.error();
    }
    EngineCaptureLocation location;
    location.latitude_e6 = latitude.value();
    location.longitude_e6 = longitude.value();
    if (all_alt)
    {
        if (alt_ref.value != 0U && alt_ref.value != 1U)
        {
            return capture_read_error("Capture altitude reference must be 0 or 1",
                                      "invalid_capture_altitude_ref", "gps_altitude_ref",
                                      alt_ref.path);
        }
        const std::uint32_t maximum = alt_ref.value == 1U ? 12000000U : 100000000U;
        auto mm = engine_altitude_to_mm(alt.value, maximum, "gps_altitude", alt.path);
        if (!mm)
        {
            return mm.error();
        }
        EngineCaptureAltitude altitude;
        altitude.magnitude_mm = mm.value();
        altitude.reference = alt_ref.value == 1U ? EngineCaptureAltitudeReference::kBelowSeaLevel :
                                                   EngineCaptureAltitudeReference::kAboveSeaLevel;
        location.altitude = altitude;
    }
    return std::optional<EngineCaptureLocation>{location};
}

[[nodiscard]] std::size_t count_exif_key(const Exiv2::ExifData &exif, const char *key)
{
    std::size_t count = 0;
    for (auto it = exif.begin(); it != exif.end(); ++it)
    {
        if (it->key() == key)
        {
            ++count;
        }
    }
    return count;
}

void extract_ascii(const Exiv2::ExifData &exif, AsciiTag &tag, const char *key,
                   const std::size_t maximum_bytes)
{
    tag.path = key;
    const std::size_t count = count_exif_key(exif, key);
    if (count == 0U)
    {
        tag.status = AsciiTagStatus::kAbsent;
        return;
    }
    if (count > 1U)
    {
        tag.status = AsciiTagStatus::kDuplicate;
        return;
    }
    const auto position = exif.findKey(Exiv2::ExifKey(key));
    if (position == exif.end())
    {
        tag.status = AsciiTagStatus::kAbsent;
        return;
    }
    if (position->typeId() != Exiv2::asciiString)
    {
        tag.status = AsciiTagStatus::kWrongType;
        return;
    }
    const auto &value = position->value();
    if (value.size() > maximum_bytes)
    {
        tag.status = AsciiTagStatus::kOversized;
        return;
    }
    std::string raw(static_cast<std::size_t>(value.size()), '\0');
    if (!raw.empty())
    {
        value.copy(reinterpret_cast<Exiv2::byte *>(raw.data()), Exiv2::littleEndian);
    }
    if (!raw.empty() && raw.back() == '\0')
    {
        raw.pop_back();
    }
    if (raw.find('\0') != std::string::npos)
    {
        tag.status = AsciiTagStatus::kContainsNul;
        tag.value = raw.substr(0, raw.find('\0'));
        return;
    }
    tag.status = AsciiTagStatus::kPresent;
    tag.value = std::move(raw);
}

void extract_urational3(const Exiv2::ExifData &exif, Rational3Tag &tag, const char *key)
{
    tag.path = key;
    const std::size_t count = count_exif_key(exif, key);
    if (count == 0U)
    {
        tag.status = AsciiTagStatus::kAbsent;
        return;
    }
    if (count > 1U)
    {
        tag.status = AsciiTagStatus::kDuplicate;
        return;
    }
    const auto position = exif.findKey(Exiv2::ExifKey(key));
    if (position == exif.end())
    {
        tag.status = AsciiTagStatus::kAbsent;
        return;
    }
    if (position->typeId() != Exiv2::unsignedRational)
    {
        tag.status = AsciiTagStatus::kWrongType;
        return;
    }
    const auto *urational = dynamic_cast<const Exiv2::URationalValue *>(&position->value());
    if (urational == nullptr)
    {
        tag.status = AsciiTagStatus::kWrongType;
        return;
    }
    if (urational->count() != 3U)
    {
        tag.status = AsciiTagStatus::kMultiValue;
        return;
    }
    for (std::size_t index = 0; index < 3U; ++index)
    {
        const Exiv2::URational rational = urational->value_.at(index);
        tag.values[index].numerator = rational.first;
        tag.values[index].denominator = rational.second;
    }
    tag.status = AsciiTagStatus::kPresent;
}

void extract_byte(const Exiv2::ExifData &exif, ByteTag &tag, const char *key)
{
    tag.path = key;
    const std::size_t count = count_exif_key(exif, key);
    if (count == 0U)
    {
        tag.status = AsciiTagStatus::kAbsent;
        return;
    }
    if (count > 1U)
    {
        tag.status = AsciiTagStatus::kDuplicate;
        return;
    }
    const auto position = exif.findKey(Exiv2::ExifKey(key));
    if (position == exif.end())
    {
        tag.status = AsciiTagStatus::kAbsent;
        return;
    }
    if (position->typeId() != Exiv2::unsignedByte)
    {
        tag.status = AsciiTagStatus::kWrongType;
        return;
    }
    if (position->count() != 1U)
    {
        tag.status = AsciiTagStatus::kMultiValue;
        return;
    }
    tag.status = AsciiTagStatus::kPresent;
    tag.value = static_cast<std::uint8_t>(position->toInt64(0U));
}

void extract_urational(const Exiv2::ExifData &exif, RationalTag &tag, const char *key)
{
    tag.path = key;
    const std::size_t count = count_exif_key(exif, key);
    if (count == 0U)
    {
        tag.status = AsciiTagStatus::kAbsent;
        return;
    }
    if (count > 1U)
    {
        tag.status = AsciiTagStatus::kDuplicate;
        return;
    }
    const auto position = exif.findKey(Exiv2::ExifKey(key));
    if (position == exif.end())
    {
        tag.status = AsciiTagStatus::kAbsent;
        return;
    }
    if (position->typeId() != Exiv2::unsignedRational)
    {
        tag.status = AsciiTagStatus::kWrongType;
        return;
    }
    const auto *urational = dynamic_cast<const Exiv2::URationalValue *>(&position->value());
    if (urational == nullptr || urational->count() != 1U)
    {
        tag.status =
            urational == nullptr ? AsciiTagStatus::kWrongType : AsciiTagStatus::kMultiValue;
        return;
    }
    const Exiv2::URational rational = urational->value_.at(0U);
    tag.status = AsciiTagStatus::kPresent;
    tag.value.numerator = rational.first;
    tag.value.denominator = rational.second;
}

[[nodiscard]] Result<EngineCaptureMetadata> extract_from_exif(const Exiv2::ExifData &exif)
{
    AsciiTag photo{};
    AsciiTag image{};
    AsciiTag fraction{};
    AsciiTag offset{};
    AsciiTag lat_ref{};
    AsciiTag lon_ref{};
    Rational3Tag lat{};
    Rational3Tag lon{};
    ByteTag alt_ref{};
    RationalTag alt{};
    extract_ascii(exif, photo, "Exif.Photo.DateTimeOriginal", 20U);
    extract_ascii(exif, image, "Exif.Image.DateTimeOriginal", 20U);
    extract_ascii(exif, fraction, "Exif.Photo.SubSecTimeOriginal", 10U);
    extract_ascii(exif, offset, "Exif.Photo.OffsetTimeOriginal", 7U);
    extract_ascii(exif, lat_ref, "Exif.GPSInfo.GPSLatitudeRef", 2U);
    extract_urational3(exif, lat, "Exif.GPSInfo.GPSLatitude");
    extract_ascii(exif, lon_ref, "Exif.GPSInfo.GPSLongitudeRef", 2U);
    extract_urational3(exif, lon, "Exif.GPSInfo.GPSLongitude");
    extract_byte(exif, alt_ref, "Exif.GPSInfo.GPSAltitudeRef");
    extract_urational(exif, alt, "Exif.GPSInfo.GPSAltitude");
    auto datetime = resolve_engine_datetime(photo, image, fraction, offset);
    if (!datetime)
    {
        return datetime.error();
    }
    auto location = resolve_engine_location(lat_ref, lat, lon_ref, lon, alt_ref, alt);
    if (!location)
    {
        return location.error();
    }
    EngineCaptureMetadata extracted;
    extracted.captured_datetime = std::move(datetime).value();
    extracted.location = std::move(location).value();
    return extracted;
}

[[nodiscard]] std::uint32_t read_be32(const unsigned char *bytes) noexcept
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) | static_cast<std::uint32_t>(bytes[3]);
}

[[nodiscard]] Result<bool> read_exact(QFile &file, char *buffer, const qint64 size,
                                      const char *truncated_reason, const char *read_reason)
{
    qint64 total = 0;
    while (total < size)
    {
        const qint64 got = file.read(buffer + total, size - total);
        if (got > 0)
        {
            total += got;
            continue;
        }
        if (got < 0 || file.error() != QFile::NoError)
        {
            std::map<std::string, std::string, std::less<>> context{{"reason", read_reason}};
            const auto detail = sanitize_error_value(file.errorString().toStdString());
            if (!detail.empty())
            {
                context.emplace("qt_error", detail);
            }
            return make_error(ErrorCode::kIo, "Unable to read the PNG container",
                              std::move(context));
        }
        return make_error(ErrorCode::kValidation, "PNG container is truncated",
                          {{"reason", truncated_reason}});
    }
    return true;
}

[[nodiscard]] bool is_png_chunk_letter(const unsigned char value) noexcept
{
    return (value >= static_cast<unsigned char>('A') && value <= static_cast<unsigned char>('Z')) ||
           (value >= static_cast<unsigned char>('a') && value <= static_cast<unsigned char>('z'));
}

[[nodiscard]] Result<std::optional<std::vector<std::uint8_t>>>
extract_png_exif_payload(const QString &path)
{
    static constexpr unsigned char kSignature[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    static constexpr std::uint32_t kMaxExifPayload = 16U * 1024U * 1024U;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        std::map<std::string, std::string, std::less<>> context;
        const auto detail = sanitize_error_value(file.errorString().toStdString());
        if (!detail.empty())
        {
            context.emplace("qt_error", detail);
        }
        return make_error(ErrorCode::kIo,
                          "Unable to read the PNG source for embedded capture metadata",
                          std::move(context));
    }
    unsigned char signature[8];
    auto header = read_exact(file, reinterpret_cast<char *>(signature), 8,
                             "truncated_png_container", "png_read_failed");
    if (!header)
    {
        return header.error();
    }
    if (std::memcmp(signature, kSignature, sizeof(kSignature)) != 0)
    {
        return std::optional<std::vector<std::uint8_t>>{};
    }
    bool saw_ihdr = false;
    bool saw_idat = false;
    bool idat_ended = false;
    bool saw_iend = false;
    std::optional<std::vector<std::uint8_t>> payload;
    while (!saw_iend)
    {
        unsigned char prefix[8];
        auto prefix_ok = read_exact(file, reinterpret_cast<char *>(prefix), 8,
                                    "truncated_png_chunk_header", "png_read_failed");
        if (!prefix_ok)
        {
            return prefix_ok.error();
        }
        const std::uint32_t length = read_be32(prefix);
        const unsigned char *type = prefix + 4;
        if (!std::all_of(type, type + 4, is_png_chunk_letter) || (type[2] & 0x20U) != 0U)
        {
            return make_error(ErrorCode::kValidation, "PNG chunk type is invalid",
                              {{"reason", "invalid_png_chunk_type"}});
        }
        const bool is_ihdr = std::memcmp(type, "IHDR", 4) == 0;
        const bool is_exif = std::memcmp(type, "eXIf", 4) == 0;
        const bool is_idat = std::memcmp(type, "IDAT", 4) == 0;
        const bool is_iend = std::memcmp(type, "IEND", 4) == 0;
        if (!saw_ihdr && !is_ihdr)
        {
            return make_error(ErrorCode::kValidation, "PNG IHDR must be the first chunk",
                              {{"reason", "missing_png_ihdr"}});
        }
        if (is_ihdr && (saw_ihdr || length != 13U))
        {
            return make_error(
                ErrorCode::kValidation,
                saw_ihdr ? "PNG contains more than one IHDR" : "PNG IHDR has the wrong length",
                {{"reason", saw_ihdr ? "duplicate_png_ihdr" : "invalid_png_ihdr_length"}});
        }
        if (is_exif && length > kMaxExifPayload)
        {
            return make_error(ErrorCode::kValidation, "PNG eXIf payload exceeds the accepted cap",
                              {{"reason", "png_exif_payload_too_large"}});
        }
        if (is_exif && payload)
        {
            return make_error(ErrorCode::kValidation, "PNG contains a duplicate eXIf chunk",
                              {{"reason", "duplicate_png_exif_chunk"}});
        }
        if (is_exif && saw_idat)
        {
            return make_error(ErrorCode::kValidation, "PNG eXIf must appear before IDAT",
                              {{"reason", "png_exif_after_idat"}});
        }
        if (is_exif && length == 0U)
        {
            return make_error(ErrorCode::kValidation, "PNG eXIf chunk is empty",
                              {{"reason", "empty_png_exif_chunk"}});
        }
        if (is_idat && idat_ended)
        {
            return make_error(ErrorCode::kValidation, "PNG IDAT chunks must be consecutive",
                              {{"reason", "nonconsecutive_png_idat"}});
        }
        if (is_iend && (length != 0U || !saw_idat))
        {
            return make_error(
                ErrorCode::kValidation,
                length != 0U ? "PNG IEND has the wrong length" :
                               "PNG IEND appears before image data",
                {{"reason", length != 0U ? "invalid_png_iend_length" : "png_iend_before_idat"}});
        }

        std::vector<std::uint8_t> exif_data;
        if (is_exif)
        {
            exif_data.resize(static_cast<std::size_t>(length));
        }
        std::array<unsigned char, 64U * 1024U> buffer{};
        std::uint32_t consumed = 0U;
        uLong calculated = crc32(0L, Z_NULL, 0);
        calculated = crc32(calculated, reinterpret_cast<const Bytef *>(type), 4U);
        while (consumed < length)
        {
            const std::uint32_t remaining = length - consumed;
            const std::uint32_t step =
                std::min<std::uint32_t>(remaining, static_cast<std::uint32_t>(buffer.size()));
            unsigned char *destination = is_exif ? exif_data.data() + consumed : buffer.data();
            auto data_ok =
                read_exact(file, reinterpret_cast<char *>(destination), static_cast<qint64>(step),
                           "truncated_png_chunk_payload", "png_read_failed");
            if (!data_ok)
            {
                return data_ok.error();
            }
            calculated = crc32(calculated, destination, static_cast<uInt>(step));
            consumed += step;
        }
        unsigned char crc_bytes[4];
        auto crc_ok = read_exact(file, reinterpret_cast<char *>(crc_bytes), 4,
                                 "truncated_png_chunk_crc", "png_read_failed");
        if (!crc_ok)
        {
            return crc_ok.error();
        }
        const std::uint32_t stored_crc = read_be32(crc_bytes);
        if (static_cast<std::uint32_t>(calculated) != stored_crc)
        {
            return make_error(ErrorCode::kValidation, "PNG chunk CRC does not match its payload",
                              {{"reason", "png_chunk_crc_mismatch"}});
        }
        if (is_ihdr)
        {
            saw_ihdr = true;
        }
        else if (is_exif)
        {
            static constexpr unsigned char kJpegPrefix[] = {'E', 'x', 'i', 'f', 0, 0};
            if (length >= 6U && std::memcmp(exif_data.data(), kJpegPrefix, 6) == 0)
            {
                return make_error(ErrorCode::kValidation,
                                  "PNG eXIf must not use the JPEG Exif prefix",
                                  {{"reason", "jpeg_exif_prefix_in_png"}});
            }
            payload = std::move(exif_data);
        }
        else if (is_idat)
        {
            saw_idat = true;
        }
        else if (is_iend)
        {
            saw_iend = true;
        }
        else if (saw_idat)
        {
            idat_ended = true;
        }
    }
    char trailing = 0;
    const qint64 trailing_size = file.read(&trailing, 1);
    if (trailing_size < 0 || file.error() != QFile::NoError)
    {
        return make_error(ErrorCode::kIo, "Unable to finish reading the PNG container",
                          {{"reason", "png_read_failed"}});
    }
    if (trailing_size != 0)
    {
        return make_error(ErrorCode::kValidation, "PNG contains trailing bytes after IEND",
                          {{"reason", "png_trailing_data"}});
    }
    return payload;
}

[[nodiscard]] bool peek_png_signature(const QString &path, bool &is_png)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return false;
    }
    unsigned char signature[8];
    if (file.read(reinterpret_cast<char *>(signature), 8) != 8)
    {
        is_png = false;
        return true;
    }
    static constexpr unsigned char kSignature[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    is_png = std::memcmp(signature, kSignature, sizeof(kSignature)) == 0;
    return true;
}

} // namespace

Result<EngineCaptureMetadata> read_embedded_capture_metadata(const std::string_view input_uri,
                                                             const CancellationToken &cancellation)
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    const QString path = local_path(input_uri);
    if (path.isEmpty())
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Capture metadata input path must not be empty",
                          capture_source_context(input_uri));
    }
    QFileInfo info(path);
    if (!info.exists())
    {
        return make_error(ErrorCode::kNotFound, "Capture metadata input does not exist",
                          capture_source_context(input_uri));
    }
    if (!info.isFile())
    {
        return make_error(ErrorCode::kValidation, "Capture metadata input must be a regular file",
                          capture_source_context(input_uri, "non_regular_capture_source"));
    }
    cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    try
    {
        bool is_png = false;
        if (!peek_png_signature(path, is_png))
        {
            return make_error(ErrorCode::kIo,
                              "Unable to read the source for embedded capture metadata",
                              capture_source_context(input_uri));
        }
        Exiv2::Image::UniquePtr image;
        std::vector<std::uint8_t> png_payload;
        if (is_png)
        {
            auto extracted = extract_png_exif_payload(path);
            if (!extracted)
            {
                return extracted.error();
            }
            if (!extracted.value())
            {
                cancelled = cancellation.check();
                if (!cancelled)
                {
                    return cancelled.error();
                }
                return EngineCaptureMetadata{};
            }
            png_payload = std::move(*extracted.value());
            image = Exiv2::ImageFactory::open(png_payload.data(), png_payload.size());
        }
        else
        {
            const QByteArray utf8 = path.toUtf8();
            image = Exiv2::ImageFactory::open(utf8.constData());
        }
        if (!image)
        {
            return make_error(
                ErrorCode::kValidation, "Exiv2 did not create an embedded-metadata reader",
                capture_source_context(input_uri, "embedded_capture_reader_unavailable"));
        }
        image->readMetadata();
        cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        auto extracted = extract_from_exif(image->exifData());
        if (!extracted)
        {
            return extracted.error();
        }
        cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        return extracted;
    }
    catch (const std::bad_alloc &)
    {
        return make_error(ErrorCode::kIo, "Embedded capture Exif allocation failed",
                          capture_source_context(input_uri, "embedded_capture_allocation_failed"));
    }
    catch (const Exiv2::Error &error)
    {
        return make_error(
            ErrorCode::kValidation, "Embedded capture Exif could not be read",
            capture_source_context(input_uri, "embedded_capture_exif_failed", error.what()));
    }
    catch (const std::exception &error)
    {
        return make_error(
            ErrorCode::kValidation, "Embedded capture Exif could not be read",
            capture_source_context(input_uri, "embedded_capture_exif_failed", error.what()));
    }
}

} // namespace ravo
