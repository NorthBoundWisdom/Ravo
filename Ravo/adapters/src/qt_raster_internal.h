#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <QByteArray>
#include <QColorSpace>
#include <QImage>
#include <QImageReader>
#include <QSize>

#include "ravo/adapters/qt_raster_decoder.h"

namespace ravo::qt_raster_internal
{

inline constexpr std::array<std::uint8_t, 12> kJpegIccSignature{'I', 'C', 'C', '_', 'P', 'R',
                                                                'O', 'F', 'I', 'L', 'E', 0};
inline constexpr std::array<std::uint8_t, 11> kJpegIccPrefix{'I', 'C', 'C', '_', 'P', 'R',
                                                             'O', 'F', 'I', 'L', 'E'};
inline constexpr std::array<std::uint8_t, 8> kPngSignature{0x89U, 'P',   'N',   'G',
                                                           0x0DU, 0x0AU, 0x1AU, 0x0AU};
inline constexpr std::array<std::uint8_t, 4> kQoiSignature{'q', 'o', 'i', 'f'};
inline constexpr std::array<std::uint8_t, 11> kRadianceSignature{'#', '?', 'R', 'A', 'D', 'I',
                                                                 'A', 'N', 'C', 'E', '\n'};
inline constexpr std::array<std::uint8_t, 7> kRgbeSignature{'#', '?', 'R', 'G', 'B', 'E', '\n'};
inline constexpr std::uint64_t kPngMaxEncodedBytes = 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kPngMaxDecodedBytes = 512ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kPngMaxIccBytes = 16U * 1024U * 1024U;
inline constexpr std::uint64_t kTiffMaxEncodedBytes = 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kTiffMaxDecodedBytes = 512ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kTiffMaxIccBytes = 16ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kTiffMaxIfdEntries = 1024ULL * 1024ULL;
inline constexpr std::uint64_t kTiffMaxStripCount = 1024ULL * 1024ULL;
inline constexpr std::size_t kCancellationCheckBytes = 64U * 1024U;
// QColorSpace's ICC LUT round-trip can differ by a few 16-bit code points even
// for the frozen matching encoding. Keep the bound below one eighth of an RGB8
// code so a redundant cICP declaration cannot change the published pixels.
inline constexpr std::uint16_t kPngProfileCompatibilityTolerance = 32U;

using ExportSampleView = std::variant<std::span<const std::uint8_t>, std::span<const std::uint16_t>,
                                      std::span<const float>>;

[[nodiscard]] Result<std::vector<std::uint8_t>>
encode_export_pixels(std::uint32_t width, std::uint32_t height,
                     const ColorProfileState &color_profile, const ExportSampleView &samples,
                     ExportFormat format, const JpegExportOptions &jpeg_options,
                     const CancellationToken &cancellation, const PngExportOptions &png_options,
                     const TiffExportOptions &tiff_options, const ExportMetadataSnapshot &metadata);

struct JpegContract
{
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint8_t components = 0;
    ColorProfileState color_profile;
};

struct JpegFileCandidate
{
    bool recognized = false;
    QByteArray bytes;
};

struct PngContract
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t bit_depth = 0;
    std::uint8_t color_type = 0;
    bool has_alpha = false;
    std::uint16_t orientation = 1;
    ColorProfileState color_profile;
};

struct PngFileCandidate
{
    bool recognized = false;
    QByteArray bytes;
};

struct TiffContract
{
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint16_t bit_depth = 0U;
    std::uint16_t orientation = 1U;
    std::optional<std::size_t> alpha_value_offset;
    ColorProfileState color_profile;
};

struct TiffFileCandidate
{
    bool recognized = false;
    QByteArray bytes;
};

struct QoiFileCandidate
{
    bool recognized = false;
};

struct RgbeFileCandidate
{
    bool recognized = false;
};

struct TiffField
{
    std::uint16_t tag = 0U;
    std::uint16_t type = 0U;
    std::uint64_t count = 0U;
    std::size_t payload_offset = 0U;
    std::uint64_t payload_size = 0U;
};

[[nodiscard]] QString qstring_from_utf8(std::string_view text);
[[nodiscard]] bool is_allowed_raster_format(const QByteArray &format);
[[nodiscard]] bool is_jpeg_payload(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] bool is_png_payload(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] bool is_tiff_payload(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] bool is_qoi_payload(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] bool is_rgbe_payload(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] bool starts_with(std::span<const std::uint8_t> bytes,
                               std::span<const std::uint8_t> prefix) noexcept;
[[nodiscard]] std::span<const std::uint8_t> byte_span(const QByteArray &bytes) noexcept;
[[nodiscard]] std::uint32_t read_u32_be(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] std::string png_chunk_name(std::span<const std::uint8_t> type);
[[nodiscard]] TaskError jpeg_error(ErrorCode code, std::string message, std::string_view source,
                                   std::string_view reason,
                                   std::map<std::string, std::string, std::less<>> context = {});
[[nodiscard]] TaskError png_error(ErrorCode code, std::string message, std::string_view source,
                                  std::string_view reason,
                                  std::map<std::string, std::string, std::less<>> context = {});
[[nodiscard]] TaskError tiff_error(ErrorCode code, std::string message, std::string_view source,
                                   std::string_view reason,
                                   std::map<std::string, std::string, std::less<>> context = {});
[[nodiscard]] TaskError qoi_unsupported_error(std::string_view source);
[[nodiscard]] TaskError rgbe_unsupported_error(std::string_view source);
[[nodiscard]] std::uint16_t read_u16(std::span<const std::uint8_t> bytes, std::size_t offset,
                                     bool little_endian) noexcept;
[[nodiscard]] std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset,
                                     bool little_endian) noexcept;
[[nodiscard]] std::uint64_t read_u64(std::span<const std::uint8_t> bytes, std::size_t offset,
                                     bool little_endian) noexcept;
[[nodiscard]] Result<JpegFileCandidate>
read_jpeg_file_candidate(std::string_view path, const CancellationToken *cancellation = nullptr);
[[nodiscard]] Result<PngFileCandidate>
read_png_file_candidate(std::string_view path, const CancellationToken *cancellation = nullptr);
[[nodiscard]] Result<TiffFileCandidate>
read_tiff_file_candidate(std::string_view path, const CancellationToken *cancellation = nullptr);
[[nodiscard]] Result<QoiFileCandidate>
read_qoi_file_candidate(std::string_view path, const CancellationToken *cancellation = nullptr);
[[nodiscard]] Result<RgbeFileCandidate>
read_rgbe_file_candidate(std::string_view path, const CancellationToken *cancellation = nullptr);
[[nodiscard]] std::string media_type_for_format(const QByteArray &format);
[[nodiscard]] Result<void> prepare_raster_reader(QImageReader &reader, std::string_view path);
[[nodiscard]] QSize transformed_reader_size(const QImageReader &reader);
[[nodiscard]] QImage apply_display_rotation(QImage image, int rotate_quarters);
[[nodiscard]] QSize apply_display_rotation_to_size(QSize size, int rotate_quarters);
[[nodiscard]] QImage apply_png_orientation(QImage image, std::uint16_t orientation);
void apply_scaled_decode_size(QImageReader &reader, std::uint32_t max_edge);
[[nodiscard]] ColorProfileState color_profile_for_image(const QImage &image);
[[nodiscard]] Result<TiffContract>
parse_tiff_contract(std::span<const std::uint8_t> bytes, std::string_view source,
                    const CancellationToken *cancellation = nullptr);
[[nodiscard]] Result<std::uint16_t> png_exif_orientation(std::span<const std::uint8_t> payload,
                                                         std::string_view source,
                                                         const CancellationToken *cancellation);
[[nodiscard]] std::uint8_t png_channel_to_u8(std::uint16_t value) noexcept;
[[nodiscard]] Result<DecodedRaster> decode_raster(QImage image, std::uint32_t max_edge,
                                                  const CancellationToken &cancellation,
                                                  std::string_view context,
                                                  std::optional<ColorProfileState> color_profile,
                                                  QSize source_size);
[[nodiscard]] Result<RasterInfo> probe_jpeg_bytes(const QByteArray &bytes, std::string_view source);
[[nodiscard]] Result<DecodedRaster> decode_jpeg_bytes(const QByteArray &bytes,
                                                      std::uint32_t max_edge,
                                                      const CancellationToken &cancellation,
                                                      std::string_view source, int rotate_quarters);
[[nodiscard]] Result<RasterInfo> probe_png_bytes(const QByteArray &bytes, std::string_view source);
[[nodiscard]] Result<DecodedRaster> decode_png_bytes(const QByteArray &bytes,
                                                     std::uint32_t max_edge,
                                                     const CancellationToken &cancellation,
                                                     std::string_view source, int rotate_quarters);
[[nodiscard]] Result<RasterInfo> probe_tiff_bytes(const QByteArray &bytes, std::string_view source);
[[nodiscard]] Result<DecodedRaster> decode_tiff_bytes(const QByteArray &bytes,
                                                      std::uint32_t max_edge,
                                                      const CancellationToken &cancellation,
                                                      std::string_view source, int rotate_quarters);

} // namespace ravo::qt_raster_internal
