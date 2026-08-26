#include "raw_pipeline.h"

#include "capability_ops.h"
#include "image_ops.h"
#include "raw_ca.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>
#include <QUrl>

#include <libraw/libraw.h>

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

[[nodiscard]] Result<std::unique_ptr<LibRaw>> open_libraw_file(const std::string_view input_uri)
{
    const QString path = local_path(input_uri);
    if (path.isEmpty())
    {
        return make_error(ErrorCode::kInvalidArgument, "RAW input path must not be empty",
                          {{"input_uri", std::string(input_uri)}});
    }
    if (!QFileInfo::exists(path))
    {
        return make_error(ErrorCode::kNotFound, "RAW input does not exist",
                          {{"input_uri", std::string(input_uri)}});
    }

    auto decoder = std::make_unique<LibRaw>();
    const QByteArray utf8 = path.toUtf8();
    const int open_status = decoder->open_file(utf8.constData());
    if (open_status != LIBRAW_SUCCESS)
    {
        const ErrorCode code = open_status == LIBRAW_FILE_UNSUPPORTED ? ErrorCode::kUnsupported :
                                                                        ErrorCode::kValidation;
        return make_error(
            code, "LibRaw could not identify the RAW input",
            {{"detail", libraw_strerror(open_status)}, {"input_uri", std::string(input_uri)}});
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

} // namespace

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

Result<DecodedRaw> decode_raw(const std::string_view input_uri)
{
    auto decoder = open_libraw_file(input_uri);
    if (!decoder)
    {
        return decoder.error();
    }
    const int unpack_status = decoder.value()->unpack();
    if (unpack_status != LIBRAW_SUCCESS)
    {
        return make_error(
            ErrorCode::kValidation, "LibRaw could not unpack the RAW input",
            {{"detail", libraw_strerror(unpack_status)}, {"input_uri", std::string(input_uri)}});
    }

    const auto &raw = decoder.value()->imgdata;
    const auto &sizes = raw.sizes;
    if (raw.idata.filters == 0 || raw.rawdata.raw_image == nullptr)
    {
        return make_error(ErrorCode::kUnsupported,
                          "The first Ravo RAW slice requires a 16-bit Bayer CFA");
    }
    if (sizes.width == 0 || sizes.height == 0 || sizes.raw_pitch == 0 ||
        static_cast<std::uint32_t>(sizes.left_margin) + sizes.width > sizes.raw_width ||
        static_cast<std::uint32_t>(sizes.top_margin) + sizes.height > sizes.raw_height)
    {
        return make_error(ErrorCode::kValidation, "LibRaw returned invalid RAW dimensions");
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

    const float green = raw.color.cam_mul[1] > 0.0F ? raw.color.cam_mul[1] :
                        raw.color.cam_mul[3] > 0.0F ? raw.color.cam_mul[3] :
                                                      1.0F;
    if (raw.color.cam_mul[0] > 0.0F && raw.color.cam_mul[2] > 0.0F)
    {
        result.white_balance = {raw.color.cam_mul[0] / green, 1.0F, raw.color.cam_mul[2] / green};
    }
    for (std::size_t output_channel = 0; output_channel < 3; ++output_channel)
    {
        for (std::size_t input_channel = 0; input_channel < 3; ++input_channel)
        {
            result.camera_to_srgb[output_channel * 3U + input_channel] =
                raw.color.rgb_cam[output_channel][input_channel];
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
        const auto *source = raw.rawdata.raw_image +
                             (static_cast<std::size_t>(sizes.top_margin) + y) * pitch +
                             sizes.left_margin;
        const auto row_offset =
            static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * result.width);
        std::copy_n(source, result.width, result.pixels.begin() + row_offset);
    }
    return result;
}

std::uint64_t estimate_raw_render_memory(const DecodedRaw &raw, const Recipe &recipe,
                                         const std::uint32_t width,
                                         const std::uint32_t height) noexcept
{
    const std::uint64_t output_pixels = static_cast<std::uint64_t>(width) * height;
    const std::uint64_t output_bytes = output_pixels * 3U;
    const std::uint64_t raw_pixels = raw.pixels.size();
    const std::uint64_t raw_bytes = raw_pixels * sizeof(std::uint16_t);
    std::uint64_t working_bytes = output_bytes + output_pixels * 3U * sizeof(float) + raw_bytes;
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
            working_bytes += raw_bytes;
        }
        if (operation.id == "ravo.raw.cacorrect")
        {
            working_bytes += raw_pixels * (3U * sizeof(float) + sizeof(std::uint16_t));
            const auto avoid = operation.parameters.find("avoid_color_shift");
            if (avoid != operation.parameters.end())
            {
                if (const auto *flag = std::get_if<bool>(&avoid->second.value);
                    flag != nullptr && *flag)
                {
                    working_bytes += raw_pixels * sizeof(float);
                }
            }
        }
    }
    return owns_raw_copy ? working_bytes + raw_bytes : working_bytes;
}

Result<RenderedImage> render_raw(const DecodedRaw &raw, const RenderRequest &request)
{
    std::uint32_t default_width = raw.width;
    std::uint32_t default_height = raw.height;
    apply_display_rotation_to_size(default_width, default_height, raw.rotate_quarters);
    const std::uint32_t width = request.output_width.value_or(default_width);
    const std::uint32_t height = request.output_height.value_or(default_height);
    const std::uint64_t working_bytes =
        estimate_raw_render_memory(raw, request.recipe, width, height);
    if (request.memory_budget_bytes != 0 && working_bytes > request.memory_budget_bytes)
    {
        return make_error(ErrorCode::kValidation, "Render memory budget is too small",
                          {{"required_bytes", std::to_string(working_bytes)}});
    }
    DecodedRaw prepared = raw;
    Recipe rgb_recipe = request.recipe;
    for (auto &operation : rgb_recipe.operations)
    {
        if (!operation.enabled ||
            (operation.id != "ravo.raw.hotpixels" && operation.id != "ravo.raw.highlights" &&
             operation.id != "ravo.raw.cacorrect"))
        {
            continue;
        }
        Result<void> applied = operation.id == "ravo.raw.hotpixels" ?
                                   apply_raw_hotpixels(prepared, operation, request.cancellation) :
                               operation.id == "ravo.raw.highlights" ?
                                   apply_raw_highlights(prepared, operation, request.cancellation) :
                                   apply_raw_cacorrect(prepared, operation, request.cancellation);
        if (!applied)
        {
            return applied.error();
        }
        operation.enabled = false;
    }
    auto working = working_from_raw(prepared, width, height, request.cancellation);
    if (!working)
    {
        return working.error();
    }
    auto adjusted = apply_recipe_ops(std::move(working).value(), rgb_recipe, request.cancellation);
    if (!adjusted)
    {
        return adjusted.error();
    }
    return encode_working_srgb(adjusted.value());
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
