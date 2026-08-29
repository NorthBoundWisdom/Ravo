#include "png_encoder.h"
#include "export_metadata_encoder.h"

#include <algorithm>
#include <array>
#include <bit>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include <png.h>
#include <zlib.h>

namespace ravo::detail
{
namespace
{

static_assert(kPngMaxDimension == PNG_UINT_31_MAX, "The PNG dimension contract must track libpng");

inline constexpr std::size_t kDestinationGrowthBytes = 64U * 1024U;
inline constexpr std::array<png_byte, 5U> kCicpChunkName{'c', 'I', 'C', 'P', 0U};

[[nodiscard]] TaskError
png_encode_error(const ErrorCode code, std::string message, const std::string_view reason,
                 std::map<std::string, std::string, std::less<>> context = {})
{
    context.emplace("format", "png");
    context.emplace("reason", reason);
    return make_error(code, std::move(message), std::move(context));
}

[[nodiscard]] TaskError png_cancellation_error(const CancellationToken &cancellation)
{
    std::map<std::string, std::string, std::less<>> context;
    const std::string cancellation_reason = cancellation.reason();
    if (!cancellation_reason.empty())
    {
        context.emplace("cancellation_reason", cancellation_reason);
    }
    return png_encode_error(ErrorCode::kCancelled, "PNG encoding was cancelled",
                            "png_encode_cancelled", std::move(context));
}

enum class PngFailureKind
{
    kNone,
    kCancelled,
    kOutputTooLarge,
    kOutputAllocation,
    kInjected,
    kLibpng,
};

struct PngDestination
{
    std::uint8_t *bytes = nullptr;
    std::size_t size = 0U;
    std::size_t capacity = 0U;
    std::size_t maximum_bytes = 0U;

    ~PngDestination()
    {
        std::free(bytes);
    }
};

struct PngEncoderState
{
    png_structp writer = nullptr;
    png_infop info = nullptr;
    std::jmp_buf jump{};
    PngDestination destination;
    PngFailureKind failure = PngFailureKind::kNone;
    std::array<char, 256U> detail{};
};

void destroy_writer(PngEncoderState &state) noexcept
{
    if (state.writer != nullptr)
    {
        png_destroy_write_struct(&state.writer, state.info != nullptr ? &state.info : nullptr);
        state.info = nullptr;
    }
}

void set_detail(PngEncoderState &state, const char *const message) noexcept
{
    if (message == nullptr)
    {
        return;
    }
    const std::size_t length =
        std::min(std::strlen(message), static_cast<std::size_t>(state.detail.size() - 1U));
    std::memcpy(state.detail.data(), message, length);
    state.detail[length] = '\0';
}

[[noreturn]] void png_error_callback(png_structp writer, png_const_charp message) noexcept
{
    auto *const state = static_cast<PngEncoderState *>(png_get_error_ptr(writer));
    if (state == nullptr)
    {
        std::abort();
    }
    if (state->failure == PngFailureKind::kNone)
    {
        state->failure = PngFailureKind::kLibpng;
    }
    set_detail(*state, message);
    std::longjmp(state->jump, 1);
}

void png_warning_callback(png_structp, png_const_charp) noexcept
{
}

[[noreturn]] void fail_png(PngEncoderState &state, const PngFailureKind failure,
                           const char *const detail) noexcept
{
    state.failure = failure;
    png_error(state.writer, detail);
    std::abort();
}

void png_write_callback(png_structp writer, png_bytep data, const png_size_t length) noexcept
{
    auto *const state = static_cast<PngEncoderState *>(png_get_io_ptr(writer));
    if (state == nullptr)
    {
        png_error(writer, "missing PNG output state");
        return;
    }
    const std::size_t byte_count = static_cast<std::size_t>(length);
    if (byte_count > state->destination.maximum_bytes - state->destination.size)
    {
        fail_png(*state, PngFailureKind::kOutputTooLarge, "PNG output bound exceeded");
    }
    const std::size_t required = state->destination.size + byte_count;
    if (required > state->destination.capacity)
    {
        std::size_t capacity = state->destination.capacity;
        if (capacity == 0U)
        {
            capacity = std::min(kDestinationGrowthBytes, state->destination.maximum_bytes);
        }
        while (capacity < required)
        {
            const std::size_t growth =
                std::min(capacity, state->destination.maximum_bytes - capacity);
            if (growth == 0U)
            {
                fail_png(*state, PngFailureKind::kOutputTooLarge, "PNG output bound exceeded");
            }
            capacity += growth;
        }
        void *const resized = std::realloc(state->destination.bytes, capacity);
        if (resized == nullptr)
        {
            fail_png(*state, PngFailureKind::kOutputAllocation,
                     "unable to allocate PNG output storage");
        }
        state->destination.bytes = static_cast<std::uint8_t *>(resized);
        state->destination.capacity = capacity;
    }
    if (byte_count != 0U)
    {
        std::memcpy(state->destination.bytes + state->destination.size, data, byte_count);
        state->destination.size += byte_count;
    }
}

void png_flush_callback(png_structp) noexcept
{
}

[[nodiscard]] PngEncodeInjectedFailure notify_checkpoint(const PngEncodeControl &control,
                                                         const PngEncodeCheckpoint checkpoint,
                                                         const std::uint32_t progress,
                                                         const int compression) noexcept
{
    if (control.checkpoint_observer.callback == nullptr)
    {
        return PngEncodeInjectedFailure::kNone;
    }
    return control.checkpoint_observer.callback(control.checkpoint_observer.context, checkpoint,
                                                progress, compression);
}

[[nodiscard]] TaskError png_failure(const PngEncoderState &state,
                                    const CancellationToken &cancellation)
{
    switch (state.failure)
    {
    case PngFailureKind::kCancelled:
        return png_cancellation_error(cancellation);
    case PngFailureKind::kOutputTooLarge:
        return png_encode_error(
            ErrorCode::kValidation, "PNG output exceeds the safe bound", "png_output_too_large",
            {{"maximum_bytes", std::to_string(state.destination.maximum_bytes)}});
    case PngFailureKind::kOutputAllocation:
        return png_encode_error(ErrorCode::kIo, "Unable to allocate PNG output storage",
                                "png_output_allocation_failed");
    case PngFailureKind::kInjected:
        return png_encode_error(ErrorCode::kIo, "Unable to encode PNG image", "png_encoder_failure",
                                {{"detail", "injected_png_failure"}});
    case PngFailureKind::kNone:
    case PngFailureKind::kLibpng:
        break;
    }
    std::map<std::string, std::string, std::less<>> context;
    if (state.detail[0] != '\0')
    {
        context.emplace("detail", state.detail.data());
    }
    return png_encode_error(ErrorCode::kIo, "Unable to encode PNG image", "png_encoder_failure",
                            std::move(context));
}

[[nodiscard]] Result<std::vector<std::uint8_t>> copy_encoded_output(const PngDestination &source)
{
    if (source.size == 0U)
    {
        return std::vector<std::uint8_t>{};
    }
    try
    {
        return std::vector<std::uint8_t>(source.bytes, source.bytes + source.size);
    }
    catch (const std::bad_alloc &)
    {
        return png_encode_error(ErrorCode::kIo, "Unable to publish encoded PNG bytes",
                                "png_output_allocation_failed");
    }
}

[[nodiscard]] Result<std::vector<std::uint8_t>>
encode_png_samples(const std::uint32_t width, const std::uint32_t height,
                   const std::span<const std::uint8_t> samples, const std::size_t bytes_per_sample,
                   const bool swap_sixteen_bit, const PngEncodeColorMetadata &color_metadata,
                   const PngEncodeConfiguration &configuration,
                   const PreparedExportMetadata *const prepared,
                   const CancellationToken &cancellation, const PngEncodeControl control)
{
    if (bytes_per_sample != 1U && bytes_per_sample != 2U)
    {
        return png_encode_error(ErrorCode::kValidation, "PNG sample width is invalid",
                                "invalid_png_sample_width");
    }
    if (width == 0U || height == 0U || width > kPngMaxDimension || height > kPngMaxDimension)
    {
        return png_encode_error(ErrorCode::kValidation, "PNG dimensions exceed the encoder limit",
                                "invalid_png_dimensions",
                                {{"height", std::to_string(height)},
                                 {"maximum_dimension", std::to_string(kPngMaxDimension)},
                                 {"width", std::to_string(width)}});
    }
    const std::uint64_t pixel_count =
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    const std::uint64_t bytes_per_pixel = 3U * static_cast<std::uint64_t>(bytes_per_sample);
    if (pixel_count > std::numeric_limits<std::uint64_t>::max() / bytes_per_pixel)
    {
        return png_encode_error(
            ErrorCode::kValidation, "PNG RGB source exceeds the safe bound", "png_source_too_large",
            {{"maximum_bytes", std::to_string(kPngMaxSourceBytes)}, {"size_bytes", "overflow"}});
    }
    const std::uint64_t source_bytes = pixel_count * bytes_per_pixel;
    if (source_bytes > kPngMaxSourceBytes)
    {
        return png_encode_error(ErrorCode::kValidation, "PNG RGB source exceeds the safe bound",
                                "png_source_too_large",
                                {{"maximum_bytes", std::to_string(kPngMaxSourceBytes)},
                                 {"size_bytes", std::to_string(source_bytes)}});
    }
    if (samples.size() != source_bytes)
    {
        return png_encode_error(ErrorCode::kValidation,
                                "PNG RGB source does not match its dimensions",
                                "png_source_size_mismatch",
                                {{"actual_bytes", std::to_string(samples.size())},
                                 {"expected_bytes", std::to_string(source_bytes)}});
    }
    if (color_metadata.resolved_rgb_icc.empty())
    {
        return png_encode_error(ErrorCode::kValidation, "PNG output requires a resolved RGB ICC",
                                "missing_png_output_icc");
    }
    if (color_metadata.resolved_rgb_icc.size() > kPngMaxIccBytes)
    {
        return png_encode_error(
            ErrorCode::kValidation, "PNG output ICC exceeds the safe bound",
            "oversized_png_output_icc",
            {{"maximum_bytes", std::to_string(kPngMaxIccBytes)},
             {"size_bytes", std::to_string(color_metadata.resolved_rgb_icc.size())}});
    }
    if (color_metadata.has_cicp && (color_metadata.cicp[0] == 0U || color_metadata.cicp[1] == 0U ||
                                    color_metadata.cicp[2] != 0U || color_metadata.cicp[3] != 1U))
    {
        return png_encode_error(ErrorCode::kValidation, "PNG cICP declaration is invalid",
                                "invalid_png_cicp");
    }
    if (control.max_output_bytes == 0U || control.max_output_bytes > kPngMaxOutputBytes)
    {
        return png_encode_error(ErrorCode::kValidation, "PNG output bound is invalid",
                                "invalid_png_output_bound");
    }

    std::unique_ptr<PngEncoderState> state;
    try
    {
        state = std::make_unique<PngEncoderState>();
    }
    catch (const std::bad_alloc &)
    {
        return png_encode_error(ErrorCode::kIo, "Unable to allocate PNG encoder state",
                                "png_encoder_allocation_failed");
    }
    state->destination.maximum_bytes = control.max_output_bytes;
    std::vector<png_byte> exif_bytes;
    std::string xmp_bytes;
    if (prepared != nullptr)
    {
        try
        {
            exif_bytes.assign(prepared->exif_tiff_profile.begin(),
                              prepared->exif_tiff_profile.end());
            xmp_bytes.assign(prepared->xmp_packet.begin(), prepared->xmp_packet.end());
        }
        catch (const std::bad_alloc &)
        {
            return png_encode_error(ErrorCode::kIo, "Unable to allocate PNG metadata state",
                                    "png_encoder_allocation_failed");
        }
    }
    state->writer = png_create_write_struct(PNG_LIBPNG_VER_STRING, state.get(), png_error_callback,
                                            png_warning_callback);
    if (state->writer == nullptr)
    {
        return png_encode_error(ErrorCode::kIo, "Unable to allocate PNG encoder state",
                                "png_encoder_allocation_failed");
    }

    // All C++ objects with non-trivial destruction are constructed before this
    // point. Code reachable after setjmp uses only POD locals; libpng longjmp
    // therefore cannot bypass a C++ object lifetime.
    if (setjmp(state->jump) != 0)
    {
        destroy_writer(*state);
        return png_failure(*state, cancellation);
    }

    state->info = png_create_info_struct(state->writer);
    if (state->info == nullptr)
    {
        fail_png(*state, PngFailureKind::kOutputAllocation,
                 "unable to allocate PNG metadata state");
    }
    png_set_write_fn(state->writer, state.get(), png_write_callback, png_flush_callback);
    png_set_user_limits(state->writer, kPngMaxDimension, kPngMaxDimension);
    png_set_compression_level(state->writer, configuration.compression_level);
    png_set_compression_mem_level(state->writer, configuration.compression_mem_level);
    png_set_compression_strategy(state->writer, configuration.compression_strategy);
    png_set_compression_window_bits(state->writer, configuration.compression_window_bits);
    png_set_compression_method(state->writer, configuration.compression_method);
    png_set_compression_buffer_size(state->writer, configuration.compression_buffer_size);
    png_set_filter(state->writer, configuration.filter_method, configuration.enabled_filters);
    const PngEncodeInjectedFailure configured_failure = notify_checkpoint(
        control, PngEncodeCheckpoint::kConfigured, 0U, configuration.compression_level);
    if (cancellation.is_cancellation_requested())
    {
        fail_png(*state, PngFailureKind::kCancelled, "PNG encoding cancelled");
    }
    if (configured_failure != PngEncodeInjectedFailure::kNone)
    {
        fail_png(*state,
                 configured_failure == PngEncodeInjectedFailure::kAllocationFailure ?
                     PngFailureKind::kOutputAllocation :
                     PngFailureKind::kInjected,
                 "injected PNG encoder failure");
    }

    png_set_IHDR(state->writer, state->info, width, height, configuration.bit_depth,
                 configuration.color_type, configuration.interlace_type,
                 configuration.compression_type, configuration.filter_method);
    png_set_iCCP(state->writer, state->info, "icc", PNG_COMPRESSION_TYPE_BASE,
                 color_metadata.resolved_rgb_icc.data(),
                 static_cast<png_uint_32>(color_metadata.resolved_rgb_icc.size()));
    if (prepared != nullptr)
    {
        if (cancellation.is_cancellation_requested())
        {
            fail_png(*state, PngFailureKind::kCancelled, "PNG encoding cancelled");
        }
        png_set_eXIf_1(state->writer, state->info, static_cast<png_uint_32>(exif_bytes.size()),
                       exif_bytes.data());
        if (cancellation.is_cancellation_requested())
        {
            fail_png(*state, PngFailureKind::kCancelled, "PNG encoding cancelled");
        }
        png_text xmp_text{};
        xmp_text.compression = PNG_ITXT_COMPRESSION_NONE;
        xmp_text.key = const_cast<png_charp>(kPngXmpItxtKeyword.data());
        xmp_text.text = xmp_bytes.data();
        xmp_text.text_length = xmp_bytes.size();
        xmp_text.itxt_length = xmp_bytes.size();
        xmp_text.lang = const_cast<png_charp>("");
        xmp_text.lang_key = const_cast<png_charp>("");
        png_set_text(state->writer, state->info, &xmp_text, 1);
    }
    png_write_info(state->writer, state->info);
    if (color_metadata.has_cicp)
    {
        png_write_chunk(state->writer, kCicpChunkName.data(), color_metadata.cicp.data(),
                        color_metadata.cicp.size());
    }
    if (swap_sixteen_bit)
    {
        png_set_swap(state->writer);
    }

    const std::size_t stride = static_cast<std::size_t>(width) * 3U * bytes_per_sample;
    for (std::uint32_t row = 0U; row < height; ++row)
    {
        const PngEncodeInjectedFailure row_failure = notify_checkpoint(
            control, PngEncodeCheckpoint::kScanline, row, configuration.compression_level);
        if (cancellation.is_cancellation_requested())
        {
            fail_png(*state, PngFailureKind::kCancelled, "PNG encoding cancelled");
        }
        if (row_failure != PngEncodeInjectedFailure::kNone)
        {
            fail_png(*state,
                     row_failure == PngEncodeInjectedFailure::kAllocationFailure ?
                         PngFailureKind::kOutputAllocation :
                         PngFailureKind::kInjected,
                     "injected PNG encoder failure");
        }
        png_write_row(state->writer, const_cast<png_bytep>(samples.data() +
                                                           static_cast<std::size_t>(row) * stride));
    }
    const PngEncodeInjectedFailure finish_failure = notify_checkpoint(
        control, PngEncodeCheckpoint::kScanline, height, configuration.compression_level);
    if (cancellation.is_cancellation_requested())
    {
        fail_png(*state, PngFailureKind::kCancelled, "PNG encoding cancelled");
    }
    if (finish_failure != PngEncodeInjectedFailure::kNone)
    {
        fail_png(*state,
                 finish_failure == PngEncodeInjectedFailure::kAllocationFailure ?
                     PngFailureKind::kOutputAllocation :
                     PngFailureKind::kInjected,
                 "injected PNG encoder failure");
    }
    png_write_end(state->writer, state->info);
    destroy_writer(*state);
    return copy_encoded_output(state->destination);
}

} // namespace

Result<PngEncodeConfiguration> png_encode_configuration(const PngExportOptions &options)
{
    auto valid = validate_png_export_options(options);
    if (!valid)
    {
        return valid.error();
    }
    PngEncodeConfiguration configuration;
    configuration.bit_depth = static_cast<int>(options.bit_depth);
    configuration.color_type = PNG_COLOR_TYPE_RGB;
    configuration.interlace_type = PNG_INTERLACE_NONE;
    configuration.compression_type = PNG_COMPRESSION_TYPE_DEFAULT;
    configuration.filter_method = PNG_FILTER_TYPE_DEFAULT;
    configuration.compression_level = options.compression;
    configuration.compression_mem_level = 8;
    configuration.compression_strategy = Z_DEFAULT_STRATEGY;
    configuration.compression_window_bits = 15;
    configuration.compression_method = 8;
    configuration.compression_buffer_size = 8192U;
    configuration.enabled_filters = PNG_ALL_FILTERS;
    return configuration;
}

Result<std::vector<std::uint8_t>>
encode_png_rgb8(const std::uint32_t width, const std::uint32_t height,
                const std::span<const std::uint8_t> rgb,
                const PngEncodeColorMetadata &color_metadata, const PngExportOptions &options,
                const CancellationToken &cancellation, const PngEncodeControl control)
{
    if (cancellation.is_cancellation_requested())
    {
        return png_cancellation_error(cancellation);
    }
    auto configuration = png_encode_configuration(options);
    if (!configuration)
    {
        return configuration.error();
    }
    if (options.bit_depth == PngBitDepth::k16)
    {
        return png_encode_error(ErrorCode::kUnsupported,
                                "PNG 16-bit output requires a 16-bit rendered source",
                                "unsupported_png_16bit_source",
                                {{"source_pixel_format", "rgb8"}, {"requested_bit_depth", "16"}});
    }
    return encode_png_samples(width, height, rgb, 1U, false, color_metadata, configuration.value(),
                              nullptr, cancellation, control);
}

Result<std::vector<std::uint8_t>>
encode_png_rgb16(const std::uint32_t width, const std::uint32_t height,
                 const std::span<const std::uint16_t> rgb,
                 const PngEncodeColorMetadata &color_metadata, const PngExportOptions &options,
                 const CancellationToken &cancellation, const PngEncodeControl control)
{
    if (cancellation.is_cancellation_requested())
    {
        return png_cancellation_error(cancellation);
    }
    auto configuration = png_encode_configuration(options);
    if (!configuration)
    {
        return configuration.error();
    }
    if (options.bit_depth != PngBitDepth::k16)
    {
        return png_encode_error(
            ErrorCode::kValidation, "PNG 16-bit encoding requires a 16-bit request",
            "png_16bit_source_requires_16bit_depth",
            {{"requested_bit_depth", std::string(png_bit_depth_name(options.bit_depth))},
             {"source_pixel_format", "rgb16"}});
    }
    if (rgb.size() > std::numeric_limits<std::size_t>::max() / sizeof(std::uint16_t))
    {
        return png_encode_error(
            ErrorCode::kValidation, "PNG RGB source exceeds the safe bound", "png_source_too_large",
            {{"maximum_bytes", std::to_string(kPngMaxSourceBytes)}, {"size_bytes", "overflow"}});
    }
    static_assert(std::endian::native == std::endian::little ||
                      std::endian::native == std::endian::big,
                  "PNG RGB16 encoding requires a uniform host byte order");
    const auto samples = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(rgb.data()), rgb.size() * sizeof(std::uint16_t));
    constexpr bool kSwapSixteenBit = std::endian::native == std::endian::little;
    return encode_png_samples(width, height, samples, 2U, kSwapSixteenBit, color_metadata,
                              configuration.value(), nullptr, cancellation, control);
}

Result<std::vector<std::uint8_t>>
encode_png_rgb8(const std::uint32_t width, const std::uint32_t height,
                const std::span<const std::uint8_t> rgb,
                const PngEncodeColorMetadata &color_metadata, const PngExportOptions &options,
                const ExportMetadataSnapshot &metadata, const bool builtin_srgb,
                const CancellationToken &cancellation, const PngEncodeControl control)
{
    if (cancellation.is_cancellation_requested())
    {
        return png_cancellation_error(cancellation);
    }
    if (!metadata.embed_metadata)
    {
        auto valid = validate_export_metadata(metadata, cancellation);
        if (!valid)
            return valid.error();
        return encode_png_rgb8(width, height, rgb, color_metadata, options, cancellation, control);
    }
    auto prepared = prepare_export_metadata(metadata, width, height, builtin_srgb, cancellation);
    if (!prepared)
    {
        return prepared.error();
    }
    auto configuration = png_encode_configuration(options);
    if (!configuration)
    {
        return configuration.error();
    }
    if (options.bit_depth == PngBitDepth::k16)
    {
        return png_encode_error(ErrorCode::kUnsupported,
                                "PNG 16-bit output requires a 16-bit rendered source",
                                "unsupported_png_16bit_source",
                                {{"source_pixel_format", "rgb8"}, {"requested_bit_depth", "16"}});
    }
    return encode_png_samples(width, height, rgb, 1U, false, color_metadata, configuration.value(),
                              &prepared.value(), cancellation, control);
}

Result<std::vector<std::uint8_t>>
encode_png_rgb16(const std::uint32_t width, const std::uint32_t height,
                 const std::span<const std::uint16_t> rgb,
                 const PngEncodeColorMetadata &color_metadata, const PngExportOptions &options,
                 const ExportMetadataSnapshot &metadata, const bool builtin_srgb,
                 const CancellationToken &cancellation, const PngEncodeControl control)
{
    if (cancellation.is_cancellation_requested())
    {
        return png_cancellation_error(cancellation);
    }
    if (!metadata.embed_metadata)
    {
        auto valid = validate_export_metadata(metadata, cancellation);
        if (!valid)
            return valid.error();
        return encode_png_rgb16(width, height, rgb, color_metadata, options, cancellation,
                                control);
    }
    auto prepared = prepare_export_metadata(metadata, width, height, builtin_srgb, cancellation);
    if (!prepared)
    {
        return prepared.error();
    }
    auto configuration = png_encode_configuration(options);
    if (!configuration)
    {
        return configuration.error();
    }
    if (options.bit_depth != PngBitDepth::k16)
    {
        return png_encode_error(
            ErrorCode::kValidation, "PNG 16-bit encoding requires a 16-bit request",
            "png_16bit_source_requires_16bit_depth",
            {{"requested_bit_depth", std::string(png_bit_depth_name(options.bit_depth))},
             {"source_pixel_format", "rgb16"}});
    }
    if (rgb.size() > std::numeric_limits<std::size_t>::max() / sizeof(std::uint16_t))
    {
        return png_encode_error(
            ErrorCode::kValidation, "PNG RGB source exceeds the safe bound", "png_source_too_large",
            {{"maximum_bytes", std::to_string(kPngMaxSourceBytes)}, {"size_bytes", "overflow"}});
    }
    static_assert(std::endian::native == std::endian::little ||
                      std::endian::native == std::endian::big,
                  "PNG RGB16 encoding requires a uniform host byte order");
    const auto samples = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t *>(rgb.data()), rgb.size() * sizeof(std::uint16_t));
    constexpr bool kSwapSixteenBit = std::endian::native == std::endian::little;
    return encode_png_samples(width, height, samples, 2U, kSwapSixteenBit, color_metadata,
                              configuration.value(), &prepared.value(), cancellation, control);
}

} // namespace ravo::detail
