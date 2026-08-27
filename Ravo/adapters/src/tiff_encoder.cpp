#include "tiff_encoder.h"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <tiffio.h>

namespace ravo::detail
{
namespace
{

inline constexpr std::size_t kDestinationGrowthBytes = 64U * 1024U;

[[nodiscard]] TaskError
tiff_encode_error(const ErrorCode code, std::string message, const std::string_view reason,
                  std::map<std::string, std::string, std::less<>> context = {})
{
    context.emplace("format", "tiff");
    context.emplace("reason", reason);
    return make_error(code, std::move(message), std::move(context));
}

[[nodiscard]] TaskError tiff_cancellation_error(const CancellationToken &cancellation)
{
    std::map<std::string, std::string, std::less<>> context;
    const std::string cancellation_reason = cancellation.reason();
    if (!cancellation_reason.empty())
    {
        context.emplace("cancellation_reason", cancellation_reason);
    }
    return tiff_encode_error(ErrorCode::kCancelled, "TIFF encoding was cancelled",
                             "tiff_encode_cancelled", std::move(context));
}

enum class TiffClientFailure
{
    kNone,
    kOutputTooLarge,
    kAllocation,
    kRead,
    kWrite,
    kSeek,
    kClose,
    kEncoder,
};

struct TiffDestination
{
    std::uint8_t *bytes = nullptr;
    std::size_t size = 0U;
    std::size_t capacity = 0U;
    std::size_t position = 0U;
    std::size_t maximum_bytes = 0U;
    TiffClientFailure failure = TiffClientFailure::kNone;
    std::array<char, 256U> detail{};
    bool close_called = false;
    bool fail_next_write = false;
    bool fail_next_seek = false;
    bool fail_next_allocation = false;
    bool fail_close = false;
    bool fail_finalize = false;

    ~TiffDestination()
    {
        std::free(bytes);
    }
};

void set_failure(TiffDestination &destination, const TiffClientFailure failure) noexcept
{
    if (destination.failure == TiffClientFailure::kNone)
    {
        destination.failure = failure;
    }
}

void set_detail(TiffDestination &destination, const char *const module, const char *const format,
                va_list arguments) noexcept
{
    if (destination.detail[0] != '\0')
    {
        return;
    }
    std::size_t offset = 0U;
    if (module != nullptr && module[0] != '\0')
    {
        const int written =
            std::snprintf(destination.detail.data(), destination.detail.size(), "%s: ", module);
        if (written > 0)
        {
            offset = std::min(static_cast<std::size_t>(written), destination.detail.size() - 1U);
        }
    }
    if (format != nullptr && offset < destination.detail.size() - 1U)
    {
        (void)std::vsnprintf(destination.detail.data() + offset, destination.detail.size() - offset,
                             format, arguments);
    }
}

int tiff_error_handler(TIFF *, void *const context, const char *const module,
                       const char *const format, va_list arguments) noexcept
{
    auto *const destination = static_cast<TiffDestination *>(context);
    if (destination != nullptr)
    {
        set_detail(*destination, module, format, arguments);
    }
    return 1;
}

int tiff_warning_handler(TIFF *, void *, const char *, const char *, va_list) noexcept
{
    return 1;
}

[[nodiscard]] bool ensure_capacity(TiffDestination &destination,
                                   const std::size_t required) noexcept
{
    if (required > destination.maximum_bytes)
    {
        set_failure(destination, TiffClientFailure::kOutputTooLarge);
        return false;
    }
    if (required <= destination.capacity)
    {
        return true;
    }
    if (destination.fail_next_allocation)
    {
        destination.fail_next_allocation = false;
        set_failure(destination, TiffClientFailure::kAllocation);
        return false;
    }
    std::size_t capacity = destination.capacity;
    if (capacity == 0U)
    {
        capacity = std::min(kDestinationGrowthBytes, destination.maximum_bytes);
    }
    while (capacity < required)
    {
        const std::size_t growth = std::min(capacity, destination.maximum_bytes - capacity);
        if (growth == 0U)
        {
            set_failure(destination, TiffClientFailure::kOutputTooLarge);
            return false;
        }
        capacity += growth;
    }
    void *const resized = std::realloc(destination.bytes, capacity);
    if (resized == nullptr)
    {
        set_failure(destination, TiffClientFailure::kAllocation);
        return false;
    }
    destination.bytes = static_cast<std::uint8_t *>(resized);
    destination.capacity = capacity;
    return true;
}

tmsize_t tiff_client_read(const thandle_t handle, void *const output,
                          const tmsize_t byte_count) noexcept
{
    auto &destination = *static_cast<TiffDestination *>(handle);
    if (byte_count < 0 || (byte_count > 0 && output == nullptr))
    {
        set_failure(destination, TiffClientFailure::kRead);
        return static_cast<tmsize_t>(-1);
    }
    if (byte_count == 0 || destination.position >= destination.size)
    {
        return 0;
    }
    const std::size_t requested = static_cast<std::size_t>(byte_count);
    const std::size_t available = destination.size - destination.position;
    const std::size_t copied = std::min(requested, available);
    std::memcpy(output, destination.bytes + destination.position, copied);
    destination.position += copied;
    return static_cast<tmsize_t>(copied);
}

tmsize_t tiff_client_write(const thandle_t handle, void *const input,
                           const tmsize_t byte_count) noexcept
{
    auto &destination = *static_cast<TiffDestination *>(handle);
    if (byte_count < 0 || (byte_count > 0 && input == nullptr))
    {
        set_failure(destination, TiffClientFailure::kWrite);
        return static_cast<tmsize_t>(-1);
    }
    const std::size_t count = static_cast<std::size_t>(byte_count);
    if (destination.fail_next_write)
    {
        destination.fail_next_write = false;
        set_failure(destination, TiffClientFailure::kWrite);
        return static_cast<tmsize_t>(-1);
    }
    if (destination.fail_finalize)
    {
        destination.fail_finalize = false;
        set_failure(destination, TiffClientFailure::kEncoder);
        return static_cast<tmsize_t>(-1);
    }
    if (destination.position > destination.maximum_bytes ||
        count > destination.maximum_bytes - destination.position)
    {
        set_failure(destination, TiffClientFailure::kOutputTooLarge);
        return static_cast<tmsize_t>(-1);
    }
    const std::size_t required = destination.position + count;
    if (!ensure_capacity(destination, required))
    {
        return static_cast<tmsize_t>(-1);
    }
    if (destination.position > destination.size)
    {
        std::memset(destination.bytes + destination.size, 0,
                    destination.position - destination.size);
    }
    if (count != 0U)
    {
        std::memcpy(destination.bytes + destination.position, input, count);
    }
    destination.position = required;
    destination.size = std::max(destination.size, destination.position);
    return byte_count;
}

toff_t tiff_client_seek(const thandle_t handle, const toff_t offset, const int origin) noexcept
{
    auto &destination = *static_cast<TiffDestination *>(handle);
    if (destination.fail_next_seek)
    {
        destination.fail_next_seek = false;
        set_failure(destination, TiffClientFailure::kSeek);
        return static_cast<toff_t>(-1);
    }
    std::size_t base = 0U;
    switch (origin)
    {
    case SEEK_SET:
        break;
    case SEEK_CUR:
        base = destination.position;
        break;
    case SEEK_END:
        base = destination.size;
        break;
    default:
        set_failure(destination, TiffClientFailure::kSeek);
        return static_cast<toff_t>(-1);
    }
    if (offset > static_cast<toff_t>(destination.maximum_bytes) ||
        base > destination.maximum_bytes - static_cast<std::size_t>(offset))
    {
        set_failure(destination, TiffClientFailure::kSeek);
        return static_cast<toff_t>(-1);
    }
    destination.position = base + static_cast<std::size_t>(offset);
    return static_cast<toff_t>(destination.position);
}

int tiff_client_close(const thandle_t handle) noexcept
{
    auto &destination = *static_cast<TiffDestination *>(handle);
    destination.close_called = true;
    if (destination.fail_close)
    {
        destination.fail_close = false;
        set_failure(destination, TiffClientFailure::kClose);
        return -1;
    }
    return 0;
}

toff_t tiff_client_size(const thandle_t handle) noexcept
{
    const auto &destination = *static_cast<TiffDestination *>(handle);
    return static_cast<toff_t>(destination.size);
}

int tiff_client_map(thandle_t, void **const base, toff_t *const size) noexcept
{
    if (base != nullptr)
    {
        *base = nullptr;
    }
    if (size != nullptr)
    {
        *size = 0U;
    }
    return 0;
}

void tiff_client_unmap(thandle_t, void *, toff_t) noexcept
{
}

[[nodiscard]] TaskError tiff_client_error(const TiffDestination &destination)
{
    std::map<std::string, std::string, std::less<>> context;
    if (destination.detail[0] != '\0')
    {
        context.emplace("detail", destination.detail.data());
    }
    switch (destination.failure)
    {
    case TiffClientFailure::kOutputTooLarge:
        context.emplace("maximum_bytes", std::to_string(destination.maximum_bytes));
        return tiff_encode_error(ErrorCode::kValidation, "TIFF output exceeds the safe bound",
                                 "tiff_output_too_large", std::move(context));
    case TiffClientFailure::kAllocation:
        return tiff_encode_error(ErrorCode::kIo, "Unable to allocate TIFF output storage",
                                 "tiff_output_allocation_failed", std::move(context));
    case TiffClientFailure::kRead:
        return tiff_encode_error(ErrorCode::kIo, "Unable to read TIFF client storage",
                                 "tiff_client_read_failed", std::move(context));
    case TiffClientFailure::kWrite:
        return tiff_encode_error(ErrorCode::kIo, "Unable to write TIFF client storage",
                                 "tiff_client_write_failed", std::move(context));
    case TiffClientFailure::kSeek:
        return tiff_encode_error(ErrorCode::kIo, "Unable to seek TIFF client storage",
                                 "tiff_client_seek_failed", std::move(context));
    case TiffClientFailure::kClose:
        return tiff_encode_error(ErrorCode::kIo, "Unable to close TIFF client storage",
                                 "tiff_client_close_failed", std::move(context));
    case TiffClientFailure::kNone:
    case TiffClientFailure::kEncoder:
        return tiff_encode_error(ErrorCode::kIo, "Unable to encode TIFF image",
                                 "tiff_encoder_failure", std::move(context));
    }
    return tiff_encode_error(ErrorCode::kIo, "Unable to encode TIFF image", "tiff_encoder_failure",
                             std::move(context));
}

[[nodiscard]] TiffEncodeInjectedFailure
notify_checkpoint(const TiffEncodeControl &control, const TiffEncodeCheckpoint checkpoint,
                  const std::uint32_t progress,
                  const TiffEncodeConfiguration &configuration) noexcept
{
    if (control.checkpoint_observer.callback == nullptr)
    {
        return TiffEncodeInjectedFailure::kNone;
    }
    return control.checkpoint_observer.callback(control.checkpoint_observer.context, checkpoint,
                                                progress, configuration);
}

[[nodiscard]] TaskError injected_failure_error(const TiffEncodeInjectedFailure failure)
{
    switch (failure)
    {
    case TiffEncodeInjectedFailure::kClientWriteFailure:
        return tiff_encode_error(ErrorCode::kIo, "Unable to write TIFF client storage",
                                 "tiff_client_write_failed",
                                 {{"detail", "injected_tiff_client_write_failure"}});
    case TiffEncodeInjectedFailure::kClientSeekFailure:
        return tiff_encode_error(ErrorCode::kIo, "Unable to seek TIFF client storage",
                                 "tiff_client_seek_failed",
                                 {{"detail", "injected_tiff_client_seek_failure"}});
    case TiffEncodeInjectedFailure::kAllocationFailure:
        return tiff_encode_error(ErrorCode::kIo, "Unable to allocate TIFF output storage",
                                 "tiff_output_allocation_failed",
                                 {{"detail", "injected_tiff_allocation_failure"}});
    case TiffEncodeInjectedFailure::kClientCloseFailure:
        return tiff_encode_error(ErrorCode::kIo, "Unable to close TIFF client storage",
                                 "tiff_client_close_failed",
                                 {{"detail", "injected_tiff_client_close_failure"}});
    case TiffEncodeInjectedFailure::kFinalizeFailure:
        return tiff_encode_error(ErrorCode::kIo, "Unable to finalize TIFF output",
                                 "tiff_encoder_failure",
                                 {{"detail", "injected_tiff_finalize_failure"}});
    case TiffEncodeInjectedFailure::kMetadataTagFailure:
        return tiff_encode_error(ErrorCode::kIo, "Unable to write TIFF metadata tag",
                                 "tiff_metadata_tag_failed",
                                 {{"detail", "injected_tiff_metadata_tag_failure"}});
    case TiffEncodeInjectedFailure::kNone:
    case TiffEncodeInjectedFailure::kEncoderFailure:
        return tiff_encode_error(ErrorCode::kIo, "Unable to encode TIFF image",
                                 "tiff_encoder_failure",
                                 {{"detail", "injected_tiff_encoder_failure"}});
    }
    return tiff_encode_error(ErrorCode::kIo, "Unable to encode TIFF image", "tiff_encoder_failure");
}

[[nodiscard]] std::optional<TaskError> arm_injected_failure(TiffDestination &destination,
                                                            const TiffEncodeInjectedFailure failure)
{
    switch (failure)
    {
    case TiffEncodeInjectedFailure::kNone:
        return std::nullopt;
    case TiffEncodeInjectedFailure::kEncoderFailure:
        return injected_failure_error(failure);
    case TiffEncodeInjectedFailure::kClientWriteFailure:
        destination.fail_next_write = true;
        return std::nullopt;
    case TiffEncodeInjectedFailure::kClientSeekFailure:
        destination.fail_next_seek = true;
        return std::nullopt;
    case TiffEncodeInjectedFailure::kAllocationFailure:
        destination.capacity = destination.size;
        destination.fail_next_allocation = true;
        return std::nullopt;
    case TiffEncodeInjectedFailure::kClientCloseFailure:
        destination.fail_close = true;
        return std::nullopt;
    case TiffEncodeInjectedFailure::kFinalizeFailure:
        destination.fail_finalize = true;
        return std::nullopt;
    case TiffEncodeInjectedFailure::kMetadataTagFailure:
        return injected_failure_error(failure);
    }
    return injected_failure_error(failure);
}

[[nodiscard]] bool use_grayscale(const std::uint32_t width, const std::uint32_t height,
                                 const std::span<const std::uint8_t> rgb,
                                 const TiffExportOptions &options) noexcept
{
    if (!options.grayscale_if_neutral || width <= 4U || height <= 4U)
    {
        return false;
    }
    for (std::uint32_t row = 1U; row + 1U < height; ++row)
    {
        for (std::uint32_t column = 1U; column + 1U < width; ++column)
        {
            const std::size_t offset = (static_cast<std::size_t>(row) * width + column) * 3U;
            const int red = rgb[offset];
            const int green = rgb[offset + 1U];
            const int blue = rgb[offset + 2U];
            if (std::abs(red - green) > 2 || std::abs(red - blue) > 2 || std::abs(green - blue) > 2)
            {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] Result<std::vector<std::uint8_t>>
copy_encoded_output(const TiffDestination &destination)
{
    if (destination.size == 0U)
    {
        return tiff_encode_error(ErrorCode::kIo, "TIFF encoder produced no bytes",
                                 "tiff_encoder_failure");
    }
    try
    {
        return std::vector<std::uint8_t>(destination.bytes, destination.bytes + destination.size);
    }
    catch (const std::bad_alloc &)
    {
        return tiff_encode_error(ErrorCode::kIo, "Unable to publish encoded TIFF bytes",
                                 "tiff_output_allocation_failed");
    }
}

} // namespace

Result<TiffEncodeConfiguration> tiff_encode_configuration(const TiffExportOptions &options)
{
    auto valid = validate_tiff_export_options(options);
    if (!valid)
    {
        return valid.error();
    }

    TiffEncodeConfiguration result;
    result.bits_per_sample = 8U;
    result.sample_format = SAMPLEFORMAT_UINT;
    switch (options.sample_type)
    {
    case TiffSampleType::kUint8:
        break;
    case TiffSampleType::kUint16:
        result.bits_per_sample = 16U;
        break;
    case TiffSampleType::kFloat16:
        result.bits_per_sample = 16U;
        result.sample_format = SAMPLEFORMAT_IEEEFP;
        break;
    case TiffSampleType::kFloat32:
        result.bits_per_sample = 32U;
        result.sample_format = SAMPLEFORMAT_IEEEFP;
        break;
    }
    result.samples_per_pixel = 3U;
    result.photometric = PHOTOMETRIC_RGB;
    result.planar_configuration = PLANARCONFIG_CONTIG;
    result.orientation = ORIENTATION_TOPLEFT;
    result.compression = COMPRESSION_NONE;
    result.predictor = PREDICTOR_NONE;
    if (options.compression != TiffCompression::kNone)
    {
        result.compression = COMPRESSION_ADOBE_DEFLATE;
    }
    if (options.compression == TiffCompression::kDeflatePredictor)
    {
        result.predictor = result.sample_format == SAMPLEFORMAT_IEEEFP ? PREDICTOR_FLOATINGPOINT :
                                                                         PREDICTOR_HORIZONTAL;
    }
    result.compression_level = options.compression_level;
    result.resolution_dpi = static_cast<float>(options.resolution_dpi);
    result.resolution_unit = RESUNIT_INCH;
    result.little_endian = true;
    result.tiled = false;
    return result;
}

Result<std::vector<std::uint8_t>> encode_tiff_rgb8(
    const std::uint32_t width, const std::uint32_t height, const std::span<const std::uint8_t> rgb,
    const std::span<const std::uint8_t> resolved_rgb_icc, const TiffExportOptions &options,
    const CancellationToken &cancellation, const TiffEncodeControl control)
{
    return encode_tiff_rgb8(width, height, rgb, resolved_rgb_icc, options, ExportMetadataSnapshot{},
                            cancellation, control);
}

Result<std::vector<std::uint8_t>>
encode_tiff_rgb8(const std::uint32_t width, const std::uint32_t height,
                 const std::span<const std::uint8_t> rgb,
                 const std::span<const std::uint8_t> resolved_rgb_icc,
                 const TiffExportOptions &options, const ExportMetadataSnapshot &metadata,
                 const CancellationToken &cancellation, const TiffEncodeControl control)
{
    if (cancellation.is_cancellation_requested())
    {
        return tiff_cancellation_error(cancellation);
    }
    auto configuration = tiff_encode_configuration(options);
    if (!configuration)
    {
        return configuration.error();
    }
    auto valid_metadata = validate_tiff_export_metadata(metadata);
    if (!valid_metadata)
    {
        return valid_metadata.error();
    }
    if (options.sample_type != TiffSampleType::kUint8)
    {
        return tiff_encode_error(
            ErrorCode::kUnsupported, "TIFF high-precision output requires a high-precision source",
            "unsupported_tiff_high_precision_source",
            {{"sample_type", std::string(tiff_sample_type_name(options.sample_type))}});
    }
    if (width == 0U || height == 0U)
    {
        return tiff_encode_error(
            ErrorCode::kValidation, "TIFF dimensions must be nonzero", "invalid_tiff_dimensions",
            {{"height", std::to_string(height)}, {"width", std::to_string(width)}});
    }
    const std::uint64_t pixel_count =
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    if (pixel_count > kTiffMaxSourceBytes / 3U)
    {
        return tiff_encode_error(ErrorCode::kValidation, "TIFF RGB source exceeds the safe bound",
                                 "tiff_source_too_large",
                                 {{"maximum_bytes", std::to_string(kTiffMaxSourceBytes)},
                                  {"pixel_count", std::to_string(pixel_count)}});
    }
    const std::size_t source_bytes = static_cast<std::size_t>(pixel_count * 3U);
    if (rgb.size() != source_bytes)
    {
        return tiff_encode_error(ErrorCode::kValidation,
                                 "TIFF RGB source does not match its dimensions",
                                 "tiff_source_size_mismatch",
                                 {{"actual_bytes", std::to_string(rgb.size())},
                                  {"expected_bytes", std::to_string(source_bytes)}});
    }
    if (resolved_rgb_icc.empty())
    {
        return tiff_encode_error(ErrorCode::kValidation, "TIFF output ICC is missing",
                                 "missing_tiff_output_icc");
    }
    if (resolved_rgb_icc.size() > kTiffMaxIccBytes)
    {
        return tiff_encode_error(ErrorCode::kValidation, "TIFF output ICC exceeds the safe bound",
                                 "oversized_tiff_output_icc",
                                 {{"maximum_bytes", std::to_string(kTiffMaxIccBytes)},
                                  {"size_bytes", std::to_string(resolved_rgb_icc.size())}});
    }
    if (control.max_output_bytes == 0U || control.max_output_bytes > kTiffMaxOutputBytes)
    {
        return tiff_encode_error(ErrorCode::kValidation, "TIFF output bound is invalid",
                                 "invalid_tiff_output_bound",
                                 {{"maximum_bytes", std::to_string(kTiffMaxOutputBytes)},
                                  {"requested_bytes", std::to_string(control.max_output_bytes)}});
    }

    const bool grayscale = use_grayscale(width, height, rgb, options);
    if (grayscale)
    {
        configuration.value().samples_per_pixel = 1U;
        configuration.value().photometric = PHOTOMETRIC_MINISBLACK;
    }
    const std::size_t row_bytes =
        static_cast<std::size_t>(width) * configuration.value().samples_per_pixel;
    auto row = std::unique_ptr<std::uint8_t, decltype(&std::free)>(
        static_cast<std::uint8_t *>(std::malloc(row_bytes)), &std::free);
    if (!row)
    {
        return tiff_encode_error(ErrorCode::kIo, "Unable to allocate TIFF scanline storage",
                                 "tiff_output_allocation_failed");
    }

    TiffDestination destination;
    destination.maximum_bytes = control.max_output_bytes;
    TIFFOpenOptions *const open_options = TIFFOpenOptionsAlloc();
    if (open_options == nullptr)
    {
        return tiff_encode_error(ErrorCode::kIo, "Unable to allocate TIFF open options",
                                 "tiff_output_allocation_failed");
    }
    TIFFOpenOptionsSetMaxSingleMemAlloc(open_options, static_cast<tmsize_t>(kTiffMaxOutputBytes));
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(open_options,
                                           static_cast<tmsize_t>(kTiffMaxOutputBytes));
    TIFFOpenOptionsSetErrorHandlerExtR(open_options, tiff_error_handler, &destination);
    TIFFOpenOptionsSetWarningHandlerExtR(open_options, tiff_warning_handler, &destination);
    TIFF *writer = TIFFClientOpenExt(
        "ravo-memory", "wl", &destination, tiff_client_read, tiff_client_write, tiff_client_seek,
        tiff_client_close, tiff_client_size, tiff_client_map, tiff_client_unmap, open_options);
    TIFFOpenOptionsFree(open_options);
    if (writer == nullptr)
    {
        if (destination.failure == TiffClientFailure::kNone)
        {
            destination.failure = TiffClientFailure::kEncoder;
        }
        return tiff_client_error(destination);
    }

    std::optional<TaskError> primary_error;
    const auto fail_encoder = [&]()
    {
        if (!primary_error)
        {
            if (destination.failure == TiffClientFailure::kNone)
            {
                destination.failure = TiffClientFailure::kEncoder;
            }
            primary_error = tiff_client_error(destination);
        }
    };
    const auto set_field = [&](const std::uint32_t tag, auto... values)
    {
        if (!primary_error && TIFFSetField(writer, tag, values...) != 1)
        {
            fail_encoder();
        }
    };
    const auto fail_metadata_tag = [&](const std::uint32_t tag, std::string detail = {})
    {
        if (!primary_error)
        {
            std::map<std::string, std::string, std::less<>> context{{"tag", std::to_string(tag)}};
            if (!detail.empty())
            {
                context.emplace("detail", std::move(detail));
            }
            primary_error = tiff_encode_error(ErrorCode::kIo, "Unable to write TIFF metadata tag",
                                              "tiff_metadata_tag_failed", std::move(context));
        }
    };
    const auto set_metadata_ascii = [&](const std::uint32_t tag, const std::string &value)
    {
        if (primary_error)
        {
            return;
        }
        const TiffEncodeInjectedFailure injected =
            notify_checkpoint(control, TiffEncodeCheckpoint::kMetadata, tag, configuration.value());
        if (cancellation.is_cancellation_requested())
        {
            primary_error = tiff_cancellation_error(cancellation);
            return;
        }
        if (injected == TiffEncodeInjectedFailure::kMetadataTagFailure)
        {
            fail_metadata_tag(tag, "injected_tiff_metadata_tag_failure");
            return;
        }
        if (auto injected_error = arm_injected_failure(destination, injected))
        {
            primary_error = std::move(injected_error).value();
            return;
        }
        if (TIFFSetField(writer, tag, value.c_str()) != 1)
        {
            fail_metadata_tag(tag, destination.detail[0] == '\0' ?
                                       std::string{} :
                                       std::string(destination.detail.data()));
        }
    };

    set_field(TIFFTAG_IMAGEWIDTH, width);
    set_field(TIFFTAG_IMAGELENGTH, height);
    set_field(TIFFTAG_COMPRESSION, configuration.value().compression);
    if (options.compression != TiffCompression::kNone)
    {
        set_field(TIFFTAG_PREDICTOR, configuration.value().predictor);
        set_field(TIFFTAG_ZIPQUALITY,
                  static_cast<std::uint16_t>(configuration.value().compression_level));
    }
    set_field(TIFFTAG_SAMPLESPERPIXEL, configuration.value().samples_per_pixel);
    set_field(TIFFTAG_BITSPERSAMPLE, configuration.value().bits_per_sample);
    set_field(TIFFTAG_SAMPLEFORMAT, configuration.value().sample_format);
    set_field(TIFFTAG_PHOTOMETRIC, configuration.value().photometric);
    set_field(TIFFTAG_PLANARCONFIG, configuration.value().planar_configuration);
    set_field(TIFFTAG_ORIENTATION, configuration.value().orientation);
    set_field(TIFFTAG_XRESOLUTION, configuration.value().resolution_dpi);
    set_field(TIFFTAG_YRESOLUTION, configuration.value().resolution_dpi);
    set_field(TIFFTAG_RESOLUTIONUNIT, configuration.value().resolution_unit);
    set_field(TIFFTAG_ICCPROFILE, static_cast<std::uint32_t>(resolved_rgb_icc.size()),
              const_cast<std::uint8_t *>(resolved_rgb_icc.data()));
    if (!primary_error)
    {
        const std::uint32_t rows_per_strip = TIFFDefaultStripSize(writer, 0U);
        if (rows_per_strip == 0U)
        {
            fail_encoder();
        }
        else
        {
            set_field(TIFFTAG_ROWSPERSTRIP, rows_per_strip);
        }
    }

    if (!metadata.destination_document_name.empty())
    {
        set_metadata_ascii(TIFFTAG_DOCUMENTNAME, metadata.destination_document_name);
    }
    if (metadata.writable.description)
    {
        set_metadata_ascii(TIFFTAG_IMAGEDESCRIPTION, *metadata.writable.description);
    }
    if (metadata.writable.creator)
    {
        set_metadata_ascii(TIFFTAG_ARTIST, *metadata.writable.creator);
    }
    if (metadata.writable.copyright)
    {
        set_metadata_ascii(TIFFTAG_COPYRIGHT, *metadata.writable.copyright);
    }

    if (!primary_error)
    {
        const TiffEncodeInjectedFailure injected = notify_checkpoint(
            control, TiffEncodeCheckpoint::kConfigured, 0U, configuration.value());
        if (cancellation.is_cancellation_requested())
        {
            primary_error = tiff_cancellation_error(cancellation);
        }
        else if (auto injected_error = arm_injected_failure(destination, injected))
        {
            primary_error = std::move(injected_error).value();
        }
    }

    for (std::uint32_t scanline = 0U; scanline < height && !primary_error; ++scanline)
    {
        const TiffEncodeInjectedFailure injected = notify_checkpoint(
            control, TiffEncodeCheckpoint::kScanline, scanline, configuration.value());
        if (cancellation.is_cancellation_requested())
        {
            primary_error = tiff_cancellation_error(cancellation);
            break;
        }
        if (auto injected_error = arm_injected_failure(destination, injected))
        {
            primary_error = std::move(injected_error).value();
            break;
        }
        const std::size_t source_offset = static_cast<std::size_t>(scanline) * width * 3U;
        if (grayscale)
        {
            for (std::uint32_t column = 0U; column < width; ++column)
            {
                row.get()[column] = rgb[source_offset + static_cast<std::size_t>(column) * 3U];
            }
        }
        else
        {
            std::memcpy(row.get(), rgb.data() + source_offset, row_bytes);
        }
        if (TIFFWriteScanline(writer, row.get(), scanline, 0U) < 0)
        {
            fail_encoder();
        }
    }

    if (!primary_error)
    {
        const TiffEncodeInjectedFailure injected = notify_checkpoint(
            control, TiffEncodeCheckpoint::kBeforeFinish, height, configuration.value());
        if (cancellation.is_cancellation_requested())
        {
            primary_error = tiff_cancellation_error(cancellation);
        }
        else if (auto injected_error = arm_injected_failure(destination, injected))
        {
            primary_error = std::move(injected_error).value();
        }
    }
    if (!primary_error && TIFFWriteDirectory(writer) != 1)
    {
        fail_encoder();
    }

    TIFFClose(writer);
    writer = nullptr;
    if (!primary_error &&
        (destination.failure != TiffClientFailure::kNone || !destination.close_called))
    {
        if (!destination.close_called && destination.failure == TiffClientFailure::kNone)
        {
            destination.failure = TiffClientFailure::kClose;
        }
        primary_error = tiff_client_error(destination);
    }
    if (primary_error)
    {
        return std::move(primary_error).value();
    }
    return copy_encoded_output(destination);
}

} // namespace ravo::detail
