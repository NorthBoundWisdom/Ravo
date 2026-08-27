#include "tiff_encoder.h"
#include "export_metadata_encoder.h"

#include <algorithm>
#include <array>
#include <cmath>
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

enum class TiffSourceKind
{
    kRgb8,
    kRgb16,
    kRgbFloat,
};

struct TiffSampleSource
{
    TiffSourceKind kind = TiffSourceKind::kRgb8;
    std::span<const std::uint8_t> rgb8;
    std::span<const std::uint16_t> rgb16;
    std::span<const float> rgb_float;
};

[[nodiscard]] bool channels_differ_uint8(const int red, const int green, const int blue) noexcept
{
    return std::abs(red - green) > 2 || std::abs(red - blue) > 2 || std::abs(green - blue) > 2;
}

[[nodiscard]] bool channels_differ_uint16(const int red, const int green, const int blue) noexcept
{
    return std::abs(red - green) > 165 || std::abs(red - blue) > 165 ||
           std::abs(green - blue) > 165;
}

[[nodiscard]] bool channels_differ_float(const float red, const float green,
                                         const float blue) noexcept
{
    const auto ratio = [](const float left, const float right) noexcept
    { return std::fabs(std::fmax(left, 0.001F) / std::fmax(right, 0.001F)) > 1.01F; };
    return ratio(red, green) || ratio(red, blue) || ratio(green, blue);
}

[[nodiscard]] Result<bool> use_grayscale(const std::uint32_t width, const std::uint32_t height,
                                         const TiffSampleSource &source,
                                         const TiffExportOptions &options,
                                         const CancellationToken &cancellation)
{
    if (!options.grayscale_if_neutral || width <= 4U || height <= 4U)
    {
        return false;
    }
    for (std::uint32_t row = 1U; row + 1U < height; ++row)
    {
        if (cancellation.is_cancellation_requested())
        {
            return tiff_cancellation_error(cancellation);
        }
        for (std::uint32_t column = 1U; column + 1U < width; ++column)
        {
            const std::size_t offset = (static_cast<std::size_t>(row) * width + column) * 3U;
            switch (source.kind)
            {
            case TiffSourceKind::kRgb8:
                if (channels_differ_uint8(source.rgb8[offset], source.rgb8[offset + 1U],
                                          source.rgb8[offset + 2U]))
                {
                    return false;
                }
                break;
            case TiffSourceKind::kRgb16:
                if (channels_differ_uint16(source.rgb16[offset], source.rgb16[offset + 1U],
                                           source.rgb16[offset + 2U]))
                {
                    return false;
                }
                break;
            case TiffSourceKind::kRgbFloat:
                if (channels_differ_float(source.rgb_float[offset], source.rgb_float[offset + 1U],
                                          source.rgb_float[offset + 2U]))
                {
                    return false;
                }
                break;
            }
        }
    }
    return true;
}

[[nodiscard]] std::size_t source_bytes_per_pixel(const TiffSourceKind kind) noexcept
{
    switch (kind)
    {
    case TiffSourceKind::kRgb8:
        return 3U;
    case TiffSourceKind::kRgb16:
        return 6U;
    case TiffSourceKind::kRgbFloat:
        return 12U;
    }
    return 0U;
}

[[nodiscard]] bool source_matches_options(const TiffSourceKind kind,
                                          const TiffSampleType sample_type) noexcept
{
    switch (sample_type)
    {
    case TiffSampleType::kUint8:
        return kind == TiffSourceKind::kRgb8;
    case TiffSampleType::kUint16:
        return kind == TiffSourceKind::kRgb16;
    case TiffSampleType::kFloat16:
    case TiffSampleType::kFloat32:
        return kind == TiffSourceKind::kRgbFloat;
    }
    return false;
}

[[nodiscard]] Result<void> validate_finite_float_source(const std::uint32_t width,
                                                        const std::uint32_t height,
                                                        const TiffSampleSource &source,
                                                        const CancellationToken &cancellation)
{
    if (source.kind != TiffSourceKind::kRgbFloat)
    {
        return {};
    }
    for (std::uint32_t row = 0U; row < height; ++row)
    {
        if (cancellation.is_cancellation_requested())
        {
            return tiff_cancellation_error(cancellation);
        }
        const std::size_t begin = static_cast<std::size_t>(row) * width * 3U;
        const std::size_t end = begin + static_cast<std::size_t>(width) * 3U;
        for (std::size_t index = begin; index < end; ++index)
        {
            if (!std::isfinite(source.rgb_float[index]))
            {
                return tiff_encode_error(
                    ErrorCode::kValidation, "TIFF float source contains NaN or infinity",
                    "non_finite_tiff_source", {{"sample_index", std::to_string(index)}});
            }
        }
    }
    return {};
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

Result<std::uint16_t> float32_to_binary16(const float value)
{
    static_assert(sizeof(float) == 4U, "TIFF binary16 conversion requires 32-bit float");
    static_assert(std::numeric_limits<float>::is_iec559,
                  "TIFF binary16 conversion requires IEC 559 binary32");

    if (!std::isfinite(value))
    {
        return tiff_encode_error(ErrorCode::kValidation,
                                 "TIFF float16 conversion requires a finite source",
                                 "non_finite_half_source");
    }

    std::uint32_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint16_t sign = static_cast<std::uint16_t>((bits >> 16U) & 0x8000U);
    const std::uint32_t abs_bits = bits & 0x7fffffffU;
    if (abs_bits == 0U)
    {
        return sign;
    }

    // 65520 is the RNE midpoint between max finite half (65504) and +inf.
    constexpr std::uint32_t kOverflowBits = 0x477ff000U;
    if (abs_bits >= kOverflowBits)
    {
        return tiff_encode_error(ErrorCode::kValidation,
                                 "TIFF float16 conversion overflowed a finite half",
                                 "half_overflow");
    }

    const int exp32 = static_cast<int>((abs_bits >> 23U) & 0xffU) - 127;
    const std::uint32_t frac32 = abs_bits & 0x7fffffU;
    if (exp32 >= -14)
    {
        std::uint32_t frac10 = frac32 >> 13U;
        const std::uint32_t remainder = frac32 & 0x1fffU;
        if (remainder > 0x1000U || (remainder == 0x1000U && (frac10 & 1U) != 0U))
        {
            ++frac10;
        }
        int exp16 = exp32 + 15;
        if (frac10 >= 0x400U)
        {
            frac10 = 0U;
            ++exp16;
        }
        if (exp16 >= 31)
        {
            return tiff_encode_error(ErrorCode::kValidation,
                                     "TIFF float16 conversion overflowed a finite half",
                                     "half_overflow");
        }
        return static_cast<std::uint16_t>(sign | (static_cast<std::uint16_t>(exp16) << 10U) |
                                          static_cast<std::uint16_t>(frac10));
    }
    if (exp32 < -25)
    {
        return sign;
    }
    if (exp32 == -25)
    {
        return frac32 == 0U ? sign : static_cast<std::uint16_t>(sign | 1U);
    }

    const std::uint32_t significand = 0x800000U | frac32;
    const int align = 23 - (exp32 + 24);
    std::uint32_t packed = significand >> align;
    const std::uint32_t mask = (1U << align) - 1U;
    const std::uint32_t remainder = significand & mask;
    const std::uint32_t halfway = 1U << (align - 1);
    if (remainder > halfway || (remainder == halfway && (packed & 1U) != 0U))
    {
        ++packed;
    }
    if (packed >= 0x400U)
    {
        return static_cast<std::uint16_t>(sign | (1U << 10U));
    }
    return static_cast<std::uint16_t>(sign | packed);
}

namespace
{

[[nodiscard]] Result<std::vector<std::uint8_t>>
encode_tiff(const std::uint32_t width, const std::uint32_t height, const TiffSampleSource &source,
            const std::span<const std::uint8_t> resolved_rgb_icc, const TiffExportOptions &options,
            const ExportMetadataSnapshot &metadata, const bool builtin_srgb,
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
    auto valid_metadata = validate_tiff_export_metadata(metadata, cancellation);
    if (!valid_metadata)
    {
        return valid_metadata.error();
    }
    auto prepared = prepare_export_metadata(metadata, width, height, builtin_srgb, cancellation);
    if (!prepared)
    {
        return prepared.error();
    }
    if (!source_matches_options(source.kind, options.sample_type))
    {
        if (source.kind == TiffSourceKind::kRgb8)
        {
            return tiff_encode_error(
                ErrorCode::kUnsupported,
                "TIFF high-precision output requires a high-precision source",
                "unsupported_tiff_high_precision_source",
                {{"sample_type", std::string(tiff_sample_type_name(options.sample_type))}});
        }
        return tiff_encode_error(
            ErrorCode::kValidation, "TIFF source samples do not match the requested sample type",
            "tiff_source_sample_mismatch",
            {{"sample_type", std::string(tiff_sample_type_name(options.sample_type))}});
    }
    if (width == 0U || height == 0U)
    {
        return tiff_encode_error(
            ErrorCode::kValidation, "TIFF dimensions must be nonzero", "invalid_tiff_dimensions",
            {{"height", std::to_string(height)}, {"width", std::to_string(width)}});
    }
    const std::size_t bytes_per_pixel = source_bytes_per_pixel(source.kind);
    if (bytes_per_pixel == 0U)
    {
        return tiff_encode_error(ErrorCode::kValidation, "TIFF source sample kind is invalid",
                                 "tiff_source_sample_mismatch");
    }
    const std::uint64_t pixel_count =
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    if (pixel_count > kTiffMaxSourceBytes / bytes_per_pixel)
    {
        return tiff_encode_error(ErrorCode::kValidation, "TIFF RGB source exceeds the safe bound",
                                 "tiff_source_too_large",
                                 {{"maximum_bytes", std::to_string(kTiffMaxSourceBytes)},
                                  {"pixel_count", std::to_string(pixel_count)}});
    }
    const std::size_t expected_samples = static_cast<std::size_t>(pixel_count * 3U);
    const std::size_t actual_samples =
        source.kind == TiffSourceKind::kRgb8  ? source.rgb8.size() :
        source.kind == TiffSourceKind::kRgb16 ? source.rgb16.size() :
                                                source.rgb_float.size();
    if (actual_samples != expected_samples)
    {
        return tiff_encode_error(
            ErrorCode::kValidation, "TIFF RGB source does not match its dimensions",
            "tiff_source_size_mismatch",
            {{"actual_bytes", std::to_string(actual_samples * (bytes_per_pixel / 3U))},
             {"expected_bytes", std::to_string(expected_samples * (bytes_per_pixel / 3U))}});
    }
    auto finite_source = validate_finite_float_source(width, height, source, cancellation);
    if (!finite_source)
    {
        return finite_source.error();
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

    auto grayscale_result = use_grayscale(width, height, source, options, cancellation);
    if (!grayscale_result)
    {
        return grayscale_result.error();
    }
    const bool grayscale = grayscale_result.value();
    if (grayscale)
    {
        configuration.value().samples_per_pixel = 1U;
        configuration.value().photometric = PHOTOMETRIC_MINISBLACK;
    }
    const std::size_t bytes_per_sample =
        static_cast<std::size_t>(configuration.value().bits_per_sample / 8U);
    if (bytes_per_sample == 0U ||
        static_cast<std::uint64_t>(width) >
            std::numeric_limits<std::size_t>::max() /
                (static_cast<std::uint64_t>(configuration.value().samples_per_pixel) *
                 bytes_per_sample))
    {
        return tiff_encode_error(ErrorCode::kValidation, "TIFF scanline exceeds the safe bound",
                                 "tiff_source_too_large");
    }
    const std::size_t row_bytes = static_cast<std::size_t>(width) *
                                  configuration.value().samples_per_pixel * bytes_per_sample;
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
    if (prepared.value().make)
    {
        set_metadata_ascii(TIFFTAG_MAKE, *prepared.value().make);
    }
    if (prepared.value().model)
    {
        set_metadata_ascii(TIFFTAG_MODEL, *prepared.value().model);
    }

    const auto set_metadata_bytes =
        [&](const std::uint32_t tag, const std::vector<std::uint8_t> &value)
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
        if (TIFFSetField(writer, tag, static_cast<std::uint32_t>(value.size()), value.data()) != 1)
        {
            fail_metadata_tag(tag, destination.detail[0] == '\0' ?
                                       std::string{} :
                                       std::string(destination.detail.data()));
        }
    };
    set_metadata_bytes(TIFFTAG_XMLPACKET, prepared.value().xmp_packet);
    if (prepared.value().iptc_iim)
    {
        set_metadata_bytes(TIFFTAG_RICHTIFFIPTC, *prepared.value().iptc_iim);
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

    const auto pack_row = [&](const std::uint32_t scanline) -> std::optional<TaskError>
    {
        const std::size_t pixel_offset = static_cast<std::size_t>(scanline) * width * 3U;
        if (options.sample_type == TiffSampleType::kUint8)
        {
            if (grayscale)
            {
                for (std::uint32_t column = 0U; column < width; ++column)
                {
                    row.get()[column] =
                        source.rgb8[pixel_offset + static_cast<std::size_t>(column) * 3U];
                }
            }
            else
            {
                std::memcpy(row.get(), source.rgb8.data() + pixel_offset, row_bytes);
            }
            return std::nullopt;
        }
        if (options.sample_type == TiffSampleType::kUint16)
        {
            auto *const out = reinterpret_cast<std::uint16_t *>(row.get());
            if (grayscale)
            {
                for (std::uint32_t column = 0U; column < width; ++column)
                {
                    out[column] =
                        source.rgb16[pixel_offset + static_cast<std::size_t>(column) * 3U];
                }
            }
            else
            {
                std::memcpy(out, source.rgb16.data() + pixel_offset, row_bytes);
            }
            return std::nullopt;
        }
        if (options.sample_type == TiffSampleType::kFloat32)
        {
            auto *const out = reinterpret_cast<float *>(row.get());
            if (grayscale)
            {
                for (std::uint32_t column = 0U; column < width; ++column)
                {
                    out[column] =
                        source.rgb_float[pixel_offset + static_cast<std::size_t>(column) * 3U];
                }
            }
            else
            {
                std::memcpy(out, source.rgb_float.data() + pixel_offset, row_bytes);
            }
            return std::nullopt;
        }
        auto *const out = reinterpret_cast<std::uint16_t *>(row.get());
        const std::uint16_t samples_per_pixel = configuration.value().samples_per_pixel;
        for (std::uint32_t column = 0U; column < width; ++column)
        {
            const std::size_t source_pixel = pixel_offset + static_cast<std::size_t>(column) * 3U;
            const std::size_t channels = grayscale ? 1U : 3U;
            for (std::size_t channel = 0U; channel < channels; ++channel)
            {
                auto converted = float32_to_binary16(source.rgb_float[source_pixel + channel]);
                if (!converted)
                {
                    return converted.error();
                }
                out[static_cast<std::size_t>(column) * samples_per_pixel + channel] =
                    converted.value();
            }
        }
        return std::nullopt;
    };

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
        if (auto pack_error = pack_row(scanline))
        {
            primary_error = std::move(pack_error).value();
            break;
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
    if (!primary_error)
    {
        if (cancellation.is_cancellation_requested())
        {
            primary_error = tiff_cancellation_error(cancellation);
        }
        else if (TIFFCreateEXIFDirectory(writer) != 0)
        {
            fail_metadata_tag(TIFFTAG_EXIFIFD, "tiff_create_exif_directory_failed");
        }
        else
        {
            const auto set_exif = [&](const std::uint32_t tag, const auto &...values) -> bool
            {
                if (primary_error)
                {
                    return false;
                }
                const TiffEncodeInjectedFailure injected = notify_checkpoint(
                    control, TiffEncodeCheckpoint::kMetadata, tag, configuration.value());
                if (cancellation.is_cancellation_requested())
                {
                    primary_error = tiff_cancellation_error(cancellation);
                    return false;
                }
                if (injected == TiffEncodeInjectedFailure::kMetadataTagFailure)
                {
                    fail_metadata_tag(tag, "injected_tiff_metadata_tag_failure");
                    return false;
                }
                if (auto injected_error = arm_injected_failure(destination, injected))
                {
                    primary_error = std::move(injected_error).value();
                    return false;
                }
                if (TIFFSetField(writer, tag, values...) != 1)
                {
                    fail_metadata_tag(tag, destination.detail[0] == '\0' ?
                                               std::string{} :
                                               std::string(destination.detail.data()));
                    return false;
                }
                return true;
            };
            const auto rational_as_float = [](const ExportUnsignedRational value) -> float
            {
                return static_cast<float>(static_cast<double>(value.numerator) /
                                          static_cast<double>(value.denominator));
            };
            if (prepared.value().shutter)
            {
                set_exif(EXIFTAG_EXPOSURETIME, rational_as_float(*prepared.value().shutter));
            }
            if (prepared.value().aperture)
            {
                set_exif(EXIFTAG_FNUMBER, rational_as_float(*prepared.value().aperture));
            }
            if (prepared.value().iso)
            {
                const std::uint16_t iso = *prepared.value().iso;
                set_exif(EXIFTAG_ISOSPEEDRATINGS, 1, &iso);
            }
            if (prepared.value().focal_length)
            {
                set_exif(EXIFTAG_FOCALLENGTH, rational_as_float(*prepared.value().focal_length));
            }
            set_exif(EXIFTAG_COLORSPACE, prepared.value().color_space);
            set_exif(EXIFTAG_PIXELXDIMENSION, prepared.value().pixel_width);
            set_exif(EXIFTAG_PIXELYDIMENSION, prepared.value().pixel_height);
            std::uint64_t exif_offset = 0U;
            if (!primary_error && TIFFWriteCustomDirectory(writer, &exif_offset) != 1)
            {
                fail_metadata_tag(TIFFTAG_EXIFIFD, "tiff_write_exif_directory_failed");
            }
            else if (!primary_error && TIFFSetDirectory(writer, 0) != 1)
            {
                fail_metadata_tag(TIFFTAG_EXIFIFD, "tiff_reload_main_directory_failed");
            }
            else if (!primary_error && TIFFSetField(writer, TIFFTAG_EXIFIFD, exif_offset) != 1)
            {
                fail_metadata_tag(TIFFTAG_EXIFIFD, "tiff_set_exififd_failed");
            }
        }
    }

    if (!primary_error && cancellation.is_cancellation_requested())
    {
        primary_error = tiff_cancellation_error(cancellation);
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

} // namespace

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
    TiffSampleSource source;
    source.kind = TiffSourceKind::kRgb8;
    source.rgb8 = rgb;
    return encode_tiff(width, height, source, resolved_rgb_icc, options, metadata, false,
                       cancellation, control);
}

Result<std::vector<std::uint8_t>>
encode_tiff_rgb16(const std::uint32_t width, const std::uint32_t height,
                  const std::span<const std::uint16_t> rgb,
                  const std::span<const std::uint8_t> resolved_rgb_icc,
                  const TiffExportOptions &options, const ExportMetadataSnapshot &metadata,
                  const CancellationToken &cancellation, const TiffEncodeControl control)
{
    TiffSampleSource source;
    source.kind = TiffSourceKind::kRgb16;
    source.rgb16 = rgb;
    return encode_tiff(width, height, source, resolved_rgb_icc, options, metadata, false,
                       cancellation, control);
}

Result<std::vector<std::uint8_t>>
encode_tiff_rgb_float(const std::uint32_t width, const std::uint32_t height,
                      const std::span<const float> rgb,
                      const std::span<const std::uint8_t> resolved_rgb_icc,
                      const TiffExportOptions &options, const ExportMetadataSnapshot &metadata,
                      const CancellationToken &cancellation, const TiffEncodeControl control)
{
    TiffSampleSource source;
    source.kind = TiffSourceKind::kRgbFloat;
    source.rgb_float = rgb;
    return encode_tiff(width, height, source, resolved_rgb_icc, options, metadata, false,
                       cancellation, control);
}

Result<std::vector<std::uint8_t>> encode_tiff_rgb8(
    const std::uint32_t width, const std::uint32_t height, const std::span<const std::uint8_t> rgb,
    const std::span<const std::uint8_t> resolved_rgb_icc, const TiffExportOptions &options,
    const ExportMetadataSnapshot &metadata, const bool builtin_srgb,
    const CancellationToken &cancellation, const TiffEncodeControl control)
{
    TiffSampleSource source;
    source.kind = TiffSourceKind::kRgb8;
    source.rgb8 = rgb;
    return encode_tiff(width, height, source, resolved_rgb_icc, options, metadata, builtin_srgb,
                       cancellation, control);
}

Result<std::vector<std::uint8_t>> encode_tiff_rgb16(
    const std::uint32_t width, const std::uint32_t height, const std::span<const std::uint16_t> rgb,
    const std::span<const std::uint8_t> resolved_rgb_icc, const TiffExportOptions &options,
    const ExportMetadataSnapshot &metadata, const bool builtin_srgb,
    const CancellationToken &cancellation, const TiffEncodeControl control)
{
    TiffSampleSource source;
    source.kind = TiffSourceKind::kRgb16;
    source.rgb16 = rgb;
    return encode_tiff(width, height, source, resolved_rgb_icc, options, metadata, builtin_srgb,
                       cancellation, control);
}

Result<std::vector<std::uint8_t>> encode_tiff_rgb_float(
    const std::uint32_t width, const std::uint32_t height, const std::span<const float> rgb,
    const std::span<const std::uint8_t> resolved_rgb_icc, const TiffExportOptions &options,
    const ExportMetadataSnapshot &metadata, const bool builtin_srgb,
    const CancellationToken &cancellation, const TiffEncodeControl control)
{
    TiffSampleSource source;
    source.kind = TiffSourceKind::kRgbFloat;
    source.rgb_float = rgb;
    return encode_tiff(width, height, source, resolved_rgb_icc, options, metadata, builtin_srgb,
                       cancellation, control);
}

} // namespace ravo::detail
