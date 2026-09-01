#include "ravo/adapters/qt_raster_decoder.h"

#include "jpeg_encoder.h"
#include "png_encoder.h"
#include "tiff_encoder.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <zlib.h>

#include <QtCore/QBuffer>
#include <QtCore/QByteArray>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QIODevice>
#include <QtGui/QColorSpace>
#include <QtGui/QColorTransform>
#include <QtGui/QImage>
#include <QtGui/QImageIOHandler>
#include <QtGui/QImageReader>
#include <QtGui/QTransform>

#include "qt_raster_internal.h"

namespace ravo
{
using namespace qt_raster_internal;

Result<RasterInfo> QtRasterDecoder::probe(const std::string_view path) const
{
    auto candidate = read_jpeg_file_candidate(path);
    if (!candidate)
    {
        return candidate.error();
    }
    if (candidate.value().recognized)
    {
        return probe_jpeg_bytes(candidate.value().bytes, path);
    }
    auto png_candidate = read_png_file_candidate(path);
    if (!png_candidate)
    {
        return png_candidate.error();
    }
    if (png_candidate.value().recognized)
    {
        return probe_png_bytes(png_candidate.value().bytes, path);
    }
    auto tiff_candidate = read_tiff_file_candidate(path);
    if (!tiff_candidate)
    {
        return tiff_candidate.error();
    }
    if (tiff_candidate.value().recognized)
    {
        // Probe deliberately performs a complete pixel decode. Catalog calls
        // this tokenless API before publication, so TIFF pays the same double
        // decode as strict PNG to preserve atomic corrupt-input failure.
        return probe_tiff_bytes(tiff_candidate.value().bytes, path);
    }
    auto qoi_candidate = read_qoi_file_candidate(path);
    if (!qoi_candidate)
    {
        return qoi_candidate.error();
    }
    if (qoi_candidate.value().recognized)
    {
        return qoi_unsupported_error(path);
    }
    auto rgbe_candidate = read_rgbe_file_candidate(path);
    if (!rgbe_candidate)
    {
        return rgbe_candidate.error();
    }
    if (rgbe_candidate.value().recognized)
    {
        return rgbe_unsupported_error(path);
    }
    QImageReader reader;
    auto prepared = prepare_raster_reader(reader, path);
    if (!prepared)
    {
        return prepared.error();
    }
    QSize size = reader.size();
    const auto transformation = reader.transformation();
    if (transformation == QImageIOHandler::TransformationRotate90 ||
        transformation == QImageIOHandler::TransformationRotate270 ||
        transformation == QImageIOHandler::TransformationMirrorAndRotate90 ||
        transformation == QImageIOHandler::TransformationFlipAndRotate90)
    {
        size.transpose();
    }
    if (size.width() <= 0 || size.height() <= 0)
    {
        return make_error(ErrorCode::kValidation, "Raster image has invalid dimensions",
                          {{"path", std::string(path)}});
    }
    RasterInfo info;
    info.media_type = media_type_for_format(reader.format());
    info.width = static_cast<std::uint32_t>(size.width());
    info.height = static_cast<std::uint32_t>(size.height());
    return info;
}

Result<DecodedRaster> QtRasterDecoder::decode(const std::string_view path,
                                              const std::uint32_t max_edge,
                                              const CancellationToken &cancellation) const
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    auto candidate = read_jpeg_file_candidate(path, &cancellation);
    if (!candidate)
    {
        return candidate.error();
    }
    if (candidate.value().recognized)
    {
        cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        return decode_jpeg_bytes(candidate.value().bytes, max_edge, cancellation, path, 0);
    }
    auto png_candidate = read_png_file_candidate(path, &cancellation);
    if (!png_candidate)
    {
        return png_candidate.error();
    }
    if (png_candidate.value().recognized)
    {
        return decode_png_bytes(png_candidate.value().bytes, max_edge, cancellation, path, 0);
    }
    auto tiff_candidate = read_tiff_file_candidate(path, &cancellation);
    if (!tiff_candidate)
    {
        return tiff_candidate.error();
    }
    if (tiff_candidate.value().recognized)
    {
        return decode_tiff_bytes(tiff_candidate.value().bytes, max_edge, cancellation, path, 0);
    }
    auto qoi_candidate = read_qoi_file_candidate(path, &cancellation);
    if (!qoi_candidate)
    {
        return qoi_candidate.error();
    }
    if (qoi_candidate.value().recognized)
    {
        cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        return qoi_unsupported_error(path);
    }
    auto rgbe_candidate = read_rgbe_file_candidate(path, &cancellation);
    if (!rgbe_candidate)
    {
        return rgbe_candidate.error();
    }
    if (rgbe_candidate.value().recognized)
    {
        cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        return rgbe_unsupported_error(path);
    }
    QImageReader reader;
    auto prepared = prepare_raster_reader(reader, path);
    if (!prepared)
    {
        return prepared.error();
    }
    const QSize source_size = transformed_reader_size(reader);
    apply_scaled_decode_size(reader, max_edge);
    return decode_raster(reader.read(), max_edge, cancellation, path, std::nullopt, source_size);
}

Result<DecodedRaster> QtRasterDecoder::decode_memory(const std::vector<std::uint8_t> &encoded,
                                                     const std::uint32_t max_edge,
                                                     const CancellationToken &cancellation,
                                                     const int rotate_quarters) const
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    if (encoded.empty() ||
        encoded.size() > static_cast<std::size_t>(std::numeric_limits<qsizetype>::max()))
    {
        return make_error(ErrorCode::kValidation, "Embedded preview payload is empty or too large");
    }
    const std::span<const std::uint8_t> encoded_bytes(encoded);
    if (is_png_payload(encoded_bytes) && encoded.size() > kPngMaxEncodedBytes)
    {
        return png_error(ErrorCode::kValidation, "PNG input is too large", "memory",
                         "oversized_png_input");
    }
    if (is_tiff_payload(encoded_bytes) && encoded.size() > kTiffMaxEncodedBytes)
    {
        return tiff_error(ErrorCode::kValidation, "TIFF input is too large", "memory",
                          "oversized_tiff_input");
    }
    if (is_qoi_payload(encoded_bytes))
    {
        cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        return qoi_unsupported_error("memory");
    }
    if (is_rgbe_payload(encoded_bytes))
    {
        cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        return rgbe_unsupported_error("memory");
    }
    QByteArray bytes(reinterpret_cast<const char *>(encoded.data()),
                     static_cast<qsizetype>(encoded.size()));
    if (is_jpeg_payload(byte_span(bytes)))
    {
        return decode_jpeg_bytes(bytes, max_edge, cancellation, "memory", rotate_quarters);
    }
    if (is_png_payload(byte_span(bytes)))
    {
        return decode_png_bytes(bytes, max_edge, cancellation, "memory", rotate_quarters);
    }
    if (is_tiff_payload(byte_span(bytes)))
    {
        return decode_tiff_bytes(bytes, max_edge, cancellation, "memory", rotate_quarters);
    }
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::ReadOnly))
    {
        return make_error(ErrorCode::kIo, "Unable to open embedded preview payload");
    }
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    reader.setDecideFormatFromContent(true);
    if (!reader.canRead())
    {
        return make_error(
            ErrorCode::kUnsupported, "Embedded preview is not a readable raster image",
            {{"source", "memory"}, {"qt_error", reader.errorString().toUtf8().toStdString()}});
    }
    if (!is_allowed_raster_format(reader.format()))
    {
        return make_error(ErrorCode::kUnsupported,
                          "Embedded preview format is not enabled for import",
                          {{"format", reader.format().toStdString()}, {"source", "memory"}});
    }
    const QSize source_size =
        apply_display_rotation_to_size(transformed_reader_size(reader), rotate_quarters);
    apply_scaled_decode_size(reader, max_edge);
    return decode_raster(apply_display_rotation(reader.read(), rotate_quarters), max_edge,
                         cancellation, "memory", std::nullopt, source_size);
}

Result<std::vector<std::uint8_t>> QtRasterDecoder::encode(
    const std::uint32_t width, const std::uint32_t height, const std::vector<std::uint8_t> &rgb,
    const ColorProfileState &color_profile, const ExportFormat format,
    const JpegExportOptions &jpeg_options, const CancellationToken &cancellation,
    const PngExportOptions &png_options) const
{
    return encode(width, height, rgb, color_profile, format, jpeg_options, cancellation,
                  png_options, TiffExportOptions{});
}

Result<std::vector<std::uint8_t>> QtRasterDecoder::encode(
    const std::uint32_t width, const std::uint32_t height, const std::vector<std::uint8_t> &rgb,
    const ColorProfileState &color_profile, const ExportFormat format,
    const JpegExportOptions &jpeg_options, const CancellationToken &cancellation,
    const PngExportOptions &png_options, const TiffExportOptions &tiff_options) const
{
    return encode(width, height, rgb, color_profile, format, jpeg_options, cancellation,
                  png_options, tiff_options, ExportMetadataSnapshot{});
}

Result<std::vector<std::uint8_t>> QtRasterDecoder::encode(
    const std::uint32_t width, const std::uint32_t height, const std::vector<std::uint8_t> &rgb,
    const ColorProfileState &color_profile, const ExportFormat format,
    const JpegExportOptions &jpeg_options, const CancellationToken &cancellation,
    const PngExportOptions &png_options, const TiffExportOptions &tiff_options,
    const ExportMetadataSnapshot &metadata) const
{
    return encode_export_pixels(width, height, color_profile,
                                ExportSampleView{std::span<const std::uint8_t>{rgb}}, format,
                                jpeg_options, cancellation, png_options, tiff_options, metadata);
}

} // namespace ravo
