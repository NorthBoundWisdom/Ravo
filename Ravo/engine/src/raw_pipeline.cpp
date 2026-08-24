#include "raw_pipeline.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>
#include <QUrl>

#include <png.h>

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

[[nodiscard]] double exposure_scale(const Recipe &recipe)
{
    double scale = 1.0;
    for (const auto &operation : recipe.operations)
    {
        if (!operation.enabled || operation.id != "ravo.core.exposure")
        {
            continue;
        }
        const auto iterator = operation.parameters.find("exposure_ev");
        if (iterator == operation.parameters.end())
        {
            continue;
        }
        const auto &value = iterator->second.value;
        const double ev = std::holds_alternative<double>(value) ?
                              std::get<double>(value) :
                              static_cast<double>(std::get<std::int64_t>(value));
        scale *= std::exp2(ev);
    }
    return scale;
}

[[nodiscard]] float srgb_encode(const float value)
{
    const float clamped = std::clamp(value, 0.0F, 1.0F);
    return clamped <= 0.0031308F ? 12.92F * clamped :
                                   1.055F * std::pow(clamped, 1.0F / 2.4F) - 0.055F;
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

} // namespace

Result<InspectionResult> identify_raw(const std::string_view input_uri)
{
    auto decoder = open_libraw_file(input_uri);
    if (!decoder)
    {
        return decoder.error();
    }
    const auto &raw = decoder.value()->imgdata;
    if (raw.sizes.width == 0 || raw.sizes.height == 0)
    {
        return make_error(ErrorCode::kValidation, "LibRaw returned invalid RAW dimensions",
                          {{"input_uri", std::string(input_uri)}});
    }
    return InspectionResult{std::string(input_uri),
                            "raw",
                            raw.idata.make,
                            raw.idata.model,
                            static_cast<std::uint32_t>(raw.sizes.width),
                            static_cast<std::uint32_t>(raw.sizes.height),
                            true};
}

Result<EmbeddedPreview> extract_libraw_preview(const std::string_view input_uri,
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
    const int thumb_status = decoder.value()->unpack_thumb();
    if (thumb_status != LIBRAW_SUCCESS)
    {
        return make_error(ErrorCode::kUnsupported, "RAW file has no embedded preview",
                          {{"detail", libraw_strerror(thumb_status)},
                           {"input_uri", std::string(input_uri)}});
    }
    const auto &thumb = decoder.value()->imgdata.thumbnail;
    if (thumb.tformat != LIBRAW_THUMBNAIL_JPEG || thumb.thumb == nullptr || thumb.tlength == 0)
    {
        return make_error(ErrorCode::kUnsupported, "RAW embedded preview is not JPEG",
                          {{"input_uri", std::string(input_uri)}});
    }
    EmbeddedPreview preview;
    preview.mime_type = "image/jpeg";
    preview.width = static_cast<std::uint32_t>(thumb.twidth);
    preview.height = static_cast<std::uint32_t>(thumb.theight);
    const auto *begin = reinterpret_cast<const std::uint8_t *>(thumb.thumb);
    preview.bytes.assign(begin, begin + thumb.tlength);
    return preview;
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
        std::copy_n(source, result.width,
                    result.pixels.begin() + static_cast<std::size_t>(y) * result.width);
    }
    return result;
}

Result<RenderedImage> render_raw(const DecodedRaw &raw, const RenderRequest &request)
{
    for (const auto &operation : request.recipe.operations)
    {
        if (operation.enabled && operation.id != "ravo.core.identity" &&
            operation.id != "ravo.core.exposure")
        {
            return make_error(ErrorCode::kUnsupported,
                              "Operation has no first-slice CPU implementation",
                              {{"operation_id", operation.id}});
        }
    }

    const std::uint32_t width = request.output_width.value_or(raw.width);
    const std::uint32_t height = request.output_height.value_or(raw.height);
    if (width == 0 || height == 0)
    {
        return make_error(ErrorCode::kInvalidArgument, "Render output dimensions must be non-zero");
    }
    const std::uint64_t output_bytes = static_cast<std::uint64_t>(width) * height * 3U;
    const std::uint64_t working_bytes =
        output_bytes + static_cast<std::uint64_t>(raw.pixels.size()) * sizeof(std::uint16_t);
    if (request.memory_budget_bytes != 0 && working_bytes > request.memory_budget_bytes)
    {
        return make_error(ErrorCode::kValidation, "Render memory budget is too small",
                          {{"required_bytes", std::to_string(working_bytes)}});
    }

    RenderedImage result;
    result.width = width;
    result.height = height;
    result.rgb.resize(static_cast<std::size_t>(output_bytes));
    const float denominator = static_cast<float>(
        std::max<std::int64_t>(1, static_cast<std::int64_t>(raw.white_level) - raw.black_level));
    const float scale = static_cast<float>(exposure_scale(request.recipe));

    for (std::uint32_t output_y = 0; output_y < height; ++output_y)
    {
        auto cancelled = request.cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        const std::uint32_t source_y = std::min(
            raw.height - 1,
            static_cast<std::uint32_t>(static_cast<std::uint64_t>(output_y) * raw.height / height));
        for (std::uint32_t output_x = 0; output_x < width; ++output_x)
        {
            const std::uint32_t source_x = std::min(
                raw.width - 1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(output_x) *
                                                          raw.width / width));
            std::array<float, 3> sum{};
            std::array<std::uint32_t, 3> count{};
            for (int offset_y = -1; offset_y <= 1; ++offset_y)
            {
                const std::uint32_t y = static_cast<std::uint32_t>(std::clamp(
                    static_cast<int>(source_y) + offset_y, 0, static_cast<int>(raw.height) - 1));
                for (int offset_x = -1; offset_x <= 1; ++offset_x)
                {
                    const std::uint32_t x = static_cast<std::uint32_t>(std::clamp(
                        static_cast<int>(source_x) + offset_x, 0, static_cast<int>(raw.width) - 1));
                    const std::uint8_t channel =
                        raw.cfa_channels[(y % raw.cfa_height) * raw.cfa_width +
                                         (x % raw.cfa_width)];
                    sum[channel] +=
                        static_cast<float>(raw.pixels[static_cast<std::size_t>(y) * raw.width + x]);
                    ++count[channel];
                }
            }

            const std::size_t output_index =
                (static_cast<std::size_t>(output_y) * width + output_x) * 3U;
            std::array<float, 3> camera_rgb{};
            for (std::size_t channel = 0; channel < camera_rgb.size(); ++channel)
            {
                const float sample = count[channel] == 0 ? 0.0F : sum[channel] / count[channel];
                camera_rgb[channel] =
                    std::max(0.0F, (sample - static_cast<float>(raw.black_level)) / denominator) *
                    raw.white_balance[channel] * scale;
            }
            for (std::size_t output_channel = 0; output_channel < 3; ++output_channel)
            {
                float linear = 0.0F;
                for (std::size_t input_channel = 0; input_channel < 3; ++input_channel)
                {
                    linear += raw.camera_to_srgb[output_channel * 3U + input_channel] *
                              camera_rgb[input_channel];
                }
                result.rgb[output_index + output_channel] =
                    static_cast<std::uint8_t>(std::lround(srgb_encode(linear) * 255.0F));
            }
        }
    }
    return result;
}

Result<void> write_png_atomically(const std::string_view output_uri, const RenderedImage &image)
{
    const QString path = local_path(output_uri);
    if (QFileInfo::exists(path))
    {
        return make_error(ErrorCode::kConflict, "Render output already exists",
                          {{"output_uri", std::string(output_uri)}});
    }

    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    png.width = image.width;
    png.height = image.height;
    png.format = PNG_FORMAT_RGB;
    png_alloc_size_t encoded_size = 0;
    if (png_image_write_to_memory(&png, nullptr, &encoded_size, 0, image.rgb.data(), 0, nullptr) ==
        0)
    {
        return make_error(ErrorCode::kIo, "Unable to size PNG output",
                          {{"png_error", png.message}});
    }
    std::vector<png_byte> encoded(encoded_size);
    if (png_image_write_to_memory(&png, encoded.data(), &encoded_size, 0, image.rgb.data(), 0,
                                  nullptr) == 0)
    {
        return make_error(ErrorCode::kIo, "Unable to encode PNG output",
                          {{"png_error", png.message}});
    }

    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly) ||
        output.write(reinterpret_cast<const char *>(encoded.data()),
                     static_cast<qint64>(encoded_size)) != static_cast<qint64>(encoded_size) ||
        !output.commit())
    {
        return make_error(ErrorCode::kIo, "Unable to atomically write PNG output",
                          {{"output_uri", std::string(output_uri)},
                           {"qt_error", output.errorString().toStdString()}});
    }
    return {};
}

} // namespace ravo
