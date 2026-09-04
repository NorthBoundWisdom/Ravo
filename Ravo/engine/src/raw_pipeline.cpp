#include "raw_pipeline.h"

#include "image_ops.h"
#include "canvas_frame.h"
#include "color_zones.h"
#include "mask_evaluator.h"
#include "monochrome.h"
#include "perspective_transform.h"
#include "recursive_gaussian.h"
#include "retouch.h"

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
#include <mutex>
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
#include "ravo/recipe/develop.h"
#include "ravo/recipe/primaries.h"
#include "ravo/recipe/profile_gamma.h"

#include "color_reconstruction.h"
#include "dehaze.h"
#include "dng_opcodes.h"
#include "bayer_demosaic.h"
#include "sharpen.h"
#include "split_toning.h"
#include "texture.h"
#include "xtrans_demosaic.h"

#include "raw_pipeline_internal.h"

namespace ravo
{
namespace raw_pipeline_internal
{
void configure_exiv2_diagnostics()
{
    // The private profile deliberately omits Exiv2's XMP SDK. Its default
    // handler prints a warning for every source that merely contains XMP,
    // even though the owned Exif fields remain readable. Ravo maps actual
    // Exiv2 failures from exceptions and validation into structured errors;
    // non-fatal third-party diagnostics must not bypass that contract onto a
    // CLI or Studio stderr stream.
    static std::once_flag configured;
    std::call_once(configured, [] { Exiv2::LogMsg::setHandler(nullptr); });
}

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
    configure_exiv2_diagnostics();
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

[[nodiscard]] Result<DngOpcodeListView> libraw_dng_opcode_view(const libraw_data_t &raw,
                                                               const std::size_t index,
                                                               const unsigned parsed_field,
                                                               const std::uint32_t list_number)
{
    const auto &opcode = raw.color.dng_levels.rawopcodes[index];
    // LibRaw copies the selected RAW IFD's opcode payload into dng_levels but,
    // in releases through 0.21, does not copy that IFD's parsedfields bits.
    // Treat an owned payload as authoritative presence while retaining the bit
    // for builds which do propagate it.
    const bool present = (raw.color.dng_levels.parsedfields & parsed_field) != 0U ||
                         opcode.len != 0U || opcode.data != nullptr;
    if (!present)
    {
        return DngOpcodeListView{};
    }
    if (opcode.len == 0U || opcode.data == nullptr)
    {
        return make_error(
            ErrorCode::kUnsupported,
            "LibRaw identified a DNG opcode list but did not retain its bounded payload",
            {{"dng_opcode_list", std::to_string(list_number)},
             {"reason", "unavailable_dng_opcode_payload"}});
    }
    if (opcode.len > kMaxDngOpcodeListBytes)
    {
        return make_error(ErrorCode::kUnsupported, "DNG opcode payload exceeds Ravo's byte limit",
                          {{"bytes", std::to_string(opcode.len)},
                           {"dng_opcode_list", std::to_string(list_number)},
                           {"reason", "oversized_dng_opcode_list"}});
    }
    const auto *bytes = static_cast<const std::uint8_t *>(opcode.data);
    return DngOpcodeListView{true, std::span<const std::uint8_t>(bytes, opcode.len)};
}

[[nodiscard]] Result<std::shared_ptr<const DngOpcodeMetadata>>
parse_libraw_dng_opcodes(const libraw_data_t &raw)
{
    auto list2 = libraw_dng_opcode_view(raw, 1U, LIBRAW_DNGFM_OPCODE2, 2U);
    if (!list2)
    {
        return list2.error();
    }
    auto list3 = libraw_dng_opcode_view(raw, 2U, LIBRAW_DNGFM_OPCODE3, 3U);
    if (!list3)
    {
        return list3.error();
    }
    return parse_dng_opcode_metadata(list2.value(), list3.value(), raw.sizes.width,
                                     raw.sizes.height);
}

} // namespace raw_pipeline_internal

using namespace raw_pipeline_internal;

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
    if (raw.idata.filters == LIBRAW_XTRANS)
    {
        result.raw_sensor = "xtrans";
        result.cfa_width = 6U;
        result.cfa_height = 6U;
        result.default_demosaic_mode = std::string(kDemosaicModeMarkesteijn3);
    }
    else if (raw.idata.filters != 0U)
    {
        result.raw_sensor = "bayer";
        result.cfa_width = 2U;
        result.cfa_height = 2U;
        result.default_demosaic_mode = std::string(kDemosaicModeRcd);
    }
    else
    {
        result.raw_sensor = "unsupported";
    }
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
    std::array<float, 4> as_shot{};
    std::array<float, 4> camera_reference{};
    if (normalize_white_balance(raw.color.cam_mul, raw.idata.colors, as_shot))
    {
        result.has_as_shot_white_balance = true;
        result.as_shot_white_balance = {as_shot[0], as_shot[1], as_shot[2], as_shot[3]};
    }
    else if (raw.color.as_shot_wb_applied)
    {
        result.has_as_shot_white_balance = true;
        result.as_shot_white_balance = {1.0, 1.0, 1.0, 1.0};
    }
    if (normalize_white_balance(raw.color.pre_mul, raw.idata.colors, camera_reference))
    {
        result.has_camera_reference_white_balance = true;
        result.camera_reference_white_balance = {camera_reference[0], camera_reference[1],
                                                 camera_reference[2], camera_reference[3]};
    }
    auto dng_opcodes = parse_libraw_dng_opcodes(raw);
    if (!dng_opcodes)
    {
        return dng_opcodes.error();
    }
    if (dng_opcodes.value())
    {
        const auto &metadata = *dng_opcodes.value();
        result.dng_opcode_list2_present = metadata.list2_present;
        result.dng_opcode_list3_present = metadata.list3_present;
        result.dng_gain_map_count = static_cast<std::uint32_t>(dng_gain_map_count(metadata));
        result.dng_has_warp_rectilinear =
            std::any_of(metadata.list3_operations.begin(), metadata.list3_operations.end(),
                        [](const DngOpcodeList3Operation &operation)
                        { return std::holds_alternative<DngWarpRectilinear>(operation); });
        result.dng_has_fix_vignette_radial =
            std::any_of(metadata.list3_operations.begin(), metadata.list3_operations.end(),
                        [](const DngOpcodeList3Operation &operation)
                        { return std::holds_alternative<DngFixVignetteRadial>(operation); });
        for (const auto &skipped : metadata.skipped_optional)
        {
            auto &destination = skipped.list == 2U ? result.dng_skipped_optional_opcode_list2 :
                                                     result.dng_skipped_optional_opcode_list3;
            destination.push_back(skipped.id);
        }
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
    if ((!bayer && !xtrans) || missing_raw)
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
                          "The first-frame RAW decoder requires a 16-bit Bayer or X-Trans CFA",
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

    auto dng_opcodes = parse_libraw_dng_opcodes(raw);
    if (!dng_opcodes)
    {
        return dng_opcodes.error();
    }

    DecodedRaw result;
    result.width = sizes.width;
    result.height = sizes.height;
    result.rotate_quarters = clockwise_quarters_from_libraw_flip(sizes.flip);
    result.black_level = static_cast<std::int32_t>(
        std::min(raw.color.black, static_cast<unsigned>(std::numeric_limits<std::int32_t>::max())));
    result.white_level = raw.color.maximum > 0 ? raw.color.maximum : 65535U;
    std::copy(std::begin(raw.rawdata.color.linear_max), std::end(raw.rawdata.color.linear_max),
              result.linear_response_limits.begin());
    if (result.linear_response_limits[3] != 0U)
    {
        result.linear_response_limits[1] =
            result.linear_response_limits[1] == 0U ?
                result.linear_response_limits[3] :
                std::min(result.linear_response_limits[1], result.linear_response_limits[3]);
        result.linear_response_limits[3] = result.linear_response_limits[1];
    }
    result.make = raw.idata.make;
    result.model = raw.idata.model;
    result.dng_opcodes = std::move(dng_opcodes).value();
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

    if (normalize_white_balance(raw.color.cam_mul, raw.idata.colors, result.as_shot_white_balance))
    {
        result.has_as_shot_white_balance = true;
    }
    else if (raw.color.as_shot_wb_applied)
    {
        result.as_shot_white_balance = {1.0F, 1.0F, 1.0F, 1.0F};
        result.has_as_shot_white_balance = true;
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

    result.cfa_width = xtrans ? 6U : 2U;
    result.cfa_height = xtrans ? 6U : 2U;
    result.cfa_channels.reserve(static_cast<std::size_t>(result.cfa_width) * result.cfa_height);
    for (std::uint32_t y = 0; y < result.cfa_height; ++y)
    {
        for (std::uint32_t x = 0; x < result.cfa_width; ++x)
        {
            // LibRaw's xtrans matrix is already shifted to the active crop.
            // Bayer FC(), by contrast, needs the absolute sensor coordinates.
            const int color = xtrans ?
                                  raw.idata.xtrans[y][x] :
                                  decoder.value()->COLOR(static_cast<int>(sizes.top_margin + y),
                                                         static_cast<int>(sizes.left_margin + x));
            auto channel = channel_for(color);
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
    BayerDemosaicMode bayer_demosaic_mode = BayerDemosaicMode::kRcd;
    XTransDemosaicMode xtrans_demosaic_mode = XTransDemosaicMode::kMarkesteijn3;
    for (const auto &operation : recipe.operations)
    {
        if (!operation.enabled || operation.id != kDemosaicOperationId)
        {
            continue;
        }
        const auto found = operation.parameters.find("mode");
        if (found != operation.parameters.end())
        {
            if (const auto *mode = std::get_if<std::string>(&found->second.value);
                mode != nullptr && *mode == kDemosaicModePpg)
            {
                bayer_demosaic_mode = BayerDemosaicMode::kPpg;
            }
            else if (const auto *xtrans_mode = std::get_if<std::string>(&found->second.value);
                     xtrans_mode != nullptr && *xtrans_mode == kDemosaicModeMarkesteijn1)
            {
                xtrans_demosaic_mode = XTransDemosaicMode::kMarkesteijn1;
            }
        }
        break;
    }
    const std::uint64_t demosaic_bytes =
        raw.cfa_width == 6U && raw.cfa_height == 6U ?
            estimate_xtrans_demosaic_memory(width, height, xtrans_demosaic_mode) :
            estimate_bayer_demosaic_memory(width, height, bayer_demosaic_mode);
    add_working_bytes(demosaic_bytes > float_rgb_bytes ? demosaic_bytes - float_rgb_bytes : 0U);
    if (raw.dng_opcodes)
    {
        add_working_bytes(estimate_dng_opcode_memory(*raw.dng_opcodes));
    }
    bool owns_raw_copy = false;
    for (const auto &operation : recipe.operations)
    {
        if (!operation.enabled)
        {
            continue;
        }
        if (operation.id == "ravo.raw.hotpixels" || operation.id == "ravo.raw.highlights" ||
            operation.id == "ravo.raw.cacorrect" || operation.id == "ravo.raw.denoise")
        {
            owns_raw_copy = true;
        }
        if (operation.id == "ravo.raw.hotpixels")
        {
            add_working_bytes(raw_bytes);
        }
        if (operation.id == "ravo.raw.highlights")
        {
            add_working_bytes(saturating_multiply(raw_pixels, sizeof(float)));
            const auto mode = operation.parameters.find("mode");
            const auto *name = mode == operation.parameters.end() ?
                                   nullptr :
                                   std::get_if<std::string>(&mode->second.value);
            if (name != nullptr &&
                (*name == kRawHighlightsModeInpaint || *name == kRawHighlightsModeLch))
            {
                add_working_bytes(saturating_multiply(raw_pixels, sizeof(float)));
            }
            else if ((name == nullptr || *name == kRawHighlightsModeOpposed) &&
                     raw.width / 3U > 7U && raw.height / 3U > 7U)
            {
                const auto mask_pixels = saturating_multiply(raw.width / 3U, raw.height / 3U);
                add_working_bytes(saturating_multiply(mask_pixels, 6U));
            }
        }
        if (operation.id == "ravo.raw.denoise")
        {
            add_working_bytes(saturating_multiply(raw_pixels, 4U * sizeof(float)));
        }
        if (operation.id == "ravo.detail.denoiseprofile")
        {
            // Adaptive Y0U0V0 wavelets own accumulator, current, coarse, and detail/output RGB
            // planes. Robust MAD sampling and the per-scale clamped coordinate tables do not
            // overlap, so budget only their larger bounded peak.
            add_working_bytes(saturating_multiply(4U, float_rgb_bytes));
            constexpr std::uint64_t sample_limit = 1U << 18U;
            const std::uint64_t sample_bytes =
                saturating_multiply(std::min(output_pixels, sample_limit), sizeof(float));
            const std::uint64_t coordinate_count =
                saturating_multiply(saturating_add(width, height), 5U);
            const std::uint64_t coordinate_bytes =
                saturating_multiply(coordinate_count, sizeof(int));
            add_working_bytes(std::max(sample_bytes, coordinate_bytes));
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
        if (operation.id == kColorZonesOperationId)
        {
            add_working_bytes(float_rgb_bytes);
            add_working_bytes(detail::kColorZonesLutBytes);
        }
        if (operation.id == "ravo.core.toneequal")
        {
            // One log-EV mask plus the four additional live planes at the peak of the
            // self-guided filter. The normalized correction LUT spans [-8, 0] EV at
            // 10,000 samples per stop, including both endpoints.
            add_working_bytes(saturating_multiply(output_pixels, 5U * sizeof(float)));
            add_working_bytes((8U * 10000U + 1U) * sizeof(float));
        }
        if (operation.id == kMonochromeOperationId)
        {
            std::uint32_t original_width = raw.width;
            std::uint32_t original_height = raw.height;
            const int turns = ((raw.rotate_quarters % 4) + 4) % 4;
            if (turns % 2 != 0)
                std::swap(original_width, original_height);
            const CanonicalRoiScale scale = CanonicalRoiScale::from_scaled_dimensions(
                width, height, original_width, original_height);
            add_working_bytes(scale.valid() ?
                                  detail::monochrome_working_bytes(width, height, scale.value()) :
                                  std::numeric_limits<std::uint64_t>::max());
        }
        if (operation.id == kSplitToningOperationId)
            add_working_bytes(float_rgb_bytes);
        if (operation.id == kColorReconstructionOperationId)
        {
            ColorReconstructionParams params;
            const auto assign_number = [&](const std::string_view name, double &target) noexcept
            {
                const auto found = operation.parameters.find(std::string(name));
                if (found == operation.parameters.end())
                {
                    return;
                }
                if (const auto *value = std::get_if<double>(&found->second.value); value != nullptr)
                {
                    target = *value;
                }
                else if (const auto *integer = std::get_if<std::int64_t>(&found->second.value);
                         integer != nullptr)
                {
                    target = static_cast<double>(*integer);
                }
            };
            assign_number("threshold", params.threshold);
            assign_number("spatial", params.spatial);
            assign_number("range", params.range);
            assign_number("hue", params.hue);
            std::uint32_t original_width = raw.width;
            std::uint32_t original_height = raw.height;
            const int turns = ((raw.rotate_quarters % 4) + 4) % 4;
            if (turns % 2 != 0)
            {
                std::swap(original_width, original_height);
            }
            const CanonicalRoiScale scale = CanonicalRoiScale::from_scaled_dimensions(
                width, height, original_width, original_height);
            add_working_bytes(
                detail::color_reconstruction_grid_bytes(width, height, scale.value(), params));
        }
        if (operation.id == kSharpenOperationId)
        {
            SharpenParams params;
            const auto assign_number = [&](const std::string_view name, double &target) noexcept
            {
                const auto found = operation.parameters.find(std::string(name));
                if (found == operation.parameters.end())
                {
                    return;
                }
                if (const auto *value = std::get_if<double>(&found->second.value); value != nullptr)
                {
                    target = *value;
                }
                else if (const auto *integer = std::get_if<std::int64_t>(&found->second.value);
                         integer != nullptr)
                {
                    target = static_cast<double>(*integer);
                }
            };
            assign_number("radius", params.radius);
            assign_number("amount", params.amount);
            assign_number("threshold", params.threshold);
            add_working_bytes(detail::sharpen_working_bytes(width, height, params));
        }
        if (operation.id == kTextureOperationId)
        {
            TextureParams params;
            const auto assign_number = [&](const std::string_view name, double &target) noexcept
            {
                const auto found = operation.parameters.find(std::string(name));
                if (found == operation.parameters.end())
                {
                    return;
                }
                if (const auto *value = std::get_if<double>(&found->second.value); value != nullptr)
                {
                    target = *value;
                }
                else if (const auto *integer = std::get_if<std::int64_t>(&found->second.value);
                         integer != nullptr)
                {
                    target = static_cast<double>(*integer);
                }
            };
            assign_number("strength", params.strength);
            assign_number("detail_threshold", params.detail_threshold);
            if (const auto iterations = operation.parameters.find("iterations");
                iterations != operation.parameters.end())
            {
                if (const auto *value = std::get_if<std::int64_t>(&iterations->second.value);
                    value != nullptr)
                {
                    params.iterations = *value;
                }
            }
            add_working_bytes(detail::texture_working_bytes(width, height, params));
        }
        if (operation.id == kDehazeOperationId)
        {
            DehazeParams params;
            const auto assign_number = [&](const std::string_view name, double &target) noexcept
            {
                const auto found = operation.parameters.find(std::string(name));
                if (found == operation.parameters.end())
                {
                    return;
                }
                if (const auto *value = std::get_if<double>(&found->second.value); value != nullptr)
                {
                    target = *value;
                }
                else if (const auto *integer = std::get_if<std::int64_t>(&found->second.value);
                         integer != nullptr)
                {
                    target = static_cast<double>(*integer);
                }
            };
            if (operation.schema_version == 1)
            {
                params.strength = 0.0;
                assign_number("amount", params.strength);
            }
            else
            {
                assign_number("strength", params.strength);
                assign_number("distance", params.distance);
                if (const auto adaptive = operation.parameters.find("adaptive");
                    adaptive != operation.parameters.end())
                {
                    if (const auto *value = std::get_if<bool>(&adaptive->second.value);
                        value != nullptr)
                    {
                        params.adaptive = *value;
                    }
                }
            }
            std::uint32_t original_width = raw.width;
            std::uint32_t original_height = raw.height;
            const int turns = ((raw.rotate_quarters % 4) + 4) % 4;
            if (turns % 2 != 0)
            {
                std::swap(original_width, original_height);
            }
            const CanonicalRoiScale scale = CanonicalRoiScale::from_scaled_dimensions(
                width, height, original_width, original_height);
            add_working_bytes(detail::dehaze_working_bytes(width, height, scale.value(), params));
        }
        if (operation.id == kRetouchOperationId)
        {
            auto params = retouch_from_parameters(operation.parameters);
            if (params)
            {
                add_working_bytes(detail::retouch_working_bytes(width, height, params.value()));
                std::uint64_t peak_alpha = 0U;
                std::uint64_t peak_scratch = 0U;
                for (const auto &region : params.value().regions)
                {
                    const MaskEvaluatorMemoryEstimate mask_memory =
                        estimate_mask_evaluator_memory(recipe.masks, region.mask_id, width, height);
                    peak_alpha = std::max(peak_alpha, mask_memory.alpha_plane_bytes);
                    peak_scratch = std::max(peak_scratch, mask_memory.evaluator_scratch_bytes);
                }
                add_working_bytes(peak_alpha);
                add_working_bytes(peak_scratch);
            }
        }
        if (operation.mask_id.has_value() &&
            (operation.id == kColorHarmonizerOperationId ||
             operation.id == kColorZonesOperationId || operation.id == kMonochromeOperationId ||
             operation.id == kSplitToningOperationId || operation.id == "ravo.effect.graduatednd"))
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
    std::uint32_t decorated_width = width;
    std::uint32_t decorated_height = height;
    std::uint64_t peak_decorated_pixels = output_pixels;
    for (const auto &operation : recipe.operations)
    {
        if (!operation.enabled)
            continue;
        if (operation.id == kCanvasOperationId)
        {
            auto params = canvas_from_parameters(operation.parameters);
            if (!params)
                return std::numeric_limits<std::uint64_t>::max();
            auto layout = compute_canvas_layout(decorated_width, decorated_height, params.value());
            if (!layout)
                return std::numeric_limits<std::uint64_t>::max();
            decorated_width = layout.value().output_width;
            decorated_height = layout.value().output_height;
        }
        else if (operation.id == kFrameOperationId)
        {
            auto params = frame_from_parameters(operation.parameters);
            if (!params)
                return std::numeric_limits<std::uint64_t>::max();
            auto layout = compute_frame_layout(decorated_width, decorated_height, params.value());
            if (!layout)
                return std::numeric_limits<std::uint64_t>::max();
            decorated_width = layout.value().output_width;
            decorated_height = layout.value().output_height;
        }
        else if (operation.id == kPerspectiveOperationId)
        {
            auto params = perspective_from_parameters(operation.parameters);
            if (!params)
                return std::numeric_limits<std::uint64_t>::max();
            auto layout =
                compute_perspective_layout(decorated_width, decorated_height, params.value());
            if (!layout)
                return std::numeric_limits<std::uint64_t>::max();
            decorated_width = layout.value().output_width;
            decorated_height = layout.value().output_height;
        }
        peak_decorated_pixels = std::max(
            peak_decorated_pixels, static_cast<std::uint64_t>(decorated_width) * decorated_height);
    }
    if (output_pixels == 0U)
        return std::numeric_limits<std::uint64_t>::max();
    const std::uint64_t decoration_ratio =
        saturating_add(peak_decorated_pixels, output_pixels - 1U) / output_pixels;
    working_bytes =
        saturating_multiply(working_bytes, std::max<std::uint64_t>(1U, decoration_ratio));
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

} // namespace ravo
