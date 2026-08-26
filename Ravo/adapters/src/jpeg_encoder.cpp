#include "jpeg_encoder.h"

#include <algorithm>
#include <array>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include <jerror.h>
#include <jpeglib.h>

namespace ravo::detail
{
namespace
{

static_assert(static_cast<long>(kJpegMaxDimension) == JPEG_MAX_DIMENSION,
              "The JPEG dimension contract must track the pinned encoder");

inline constexpr std::size_t kDestinationChunkBytes = 64U * 1024U;
inline constexpr std::size_t kMaximumDestinationChunks =
    (kJpegMaxOutputBytes + kDestinationChunkBytes - 1U) / kDestinationChunkBytes;

[[nodiscard]] TaskError
jpeg_encode_error(const ErrorCode code, std::string message, const std::string_view reason,
                  std::map<std::string, std::string, std::less<>> context = {})
{
    context.emplace("format", "jpeg");
    context.emplace("reason", reason);
    return make_error(code, std::move(message), std::move(context));
}

struct JpegErrorManager
{
    jpeg_error_mgr manager{};
    std::jmp_buf jump{};
    std::array<char, JMSG_LENGTH_MAX> message{};
};

void jpeg_error_exit(j_common_ptr info)
{
    auto *const error = reinterpret_cast<JpegErrorManager *>(info->err);
    info->err->format_message(info, error->message.data());
    std::longjmp(error->jump, 1);
}

struct JpegDestination
{
    jpeg_destination_mgr manager{};
    std::array<JOCTET *, kMaximumDestinationChunks> chunks{};
    std::array<std::size_t, kMaximumDestinationChunks> capacities{};
    std::size_t chunk_count = 0U;
    std::size_t completed_bytes = 0U;
    std::size_t output_bytes = 0U;
    std::size_t maximum_bytes = 0U;
    bool output_too_large = false;
    bool allocation_failed = false;

    ~JpegDestination()
    {
        for (std::size_t index = 0U; index < chunk_count; ++index)
        {
            std::free(chunks[index]);
        }
    }
};

[[nodiscard]] JpegDestination *jpeg_destination(j_compress_ptr info) noexcept
{
    return reinterpret_cast<JpegDestination *>(info->dest);
}

void allocate_destination_chunk(j_compress_ptr info, JpegDestination &destination)
{
    if (destination.completed_bytes >= destination.maximum_bytes ||
        destination.chunk_count >= destination.chunks.size())
    {
        destination.output_too_large = true;
        ERREXIT(info, JERR_FILE_WRITE);
    }
    const std::size_t capacity =
        std::min(kDestinationChunkBytes, destination.maximum_bytes - destination.completed_bytes);
    auto *const chunk = static_cast<JOCTET *>(std::malloc(capacity));
    if (chunk == nullptr)
    {
        destination.allocation_failed = true;
        ERREXIT1(info, JERR_OUT_OF_MEMORY, static_cast<int>(capacity));
    }
    const std::size_t index = destination.chunk_count++;
    destination.chunks[index] = chunk;
    destination.capacities[index] = capacity;
    destination.manager.next_output_byte = chunk;
    destination.manager.free_in_buffer = capacity;
}

void initialize_destination(j_compress_ptr info)
{
    auto &destination = *jpeg_destination(info);
    destination.chunk_count = 0U;
    destination.completed_bytes = 0U;
    destination.output_bytes = 0U;
    allocate_destination_chunk(info, destination);
}

boolean empty_destination_buffer(j_compress_ptr info)
{
    auto &destination = *jpeg_destination(info);
    destination.completed_bytes += destination.capacities[destination.chunk_count - 1U];
    allocate_destination_chunk(info, destination);
    return TRUE;
}

void terminate_destination(j_compress_ptr info)
{
    auto &destination = *jpeg_destination(info);
    const std::size_t capacity = destination.capacities[destination.chunk_count - 1U];
    destination.output_bytes = destination.completed_bytes + capacity - info->dest->free_in_buffer;
}

struct JpegEncoderState
{
    jpeg_compress_struct compressor{};
    JpegErrorManager error;
    JpegDestination destination;
    bool compressor_created = false;
};

void destroy_compressor(JpegEncoderState &state) noexcept
{
    if (state.compressor_created)
    {
        jpeg_destroy_compress(&state.compressor);
        state.compressor_created = false;
    }
}

[[nodiscard]] TaskError libjpeg_failure(const JpegEncoderState &state)
{
    if (state.destination.output_too_large)
    {
        return jpeg_encode_error(
            ErrorCode::kValidation, "JPEG output exceeds the safe bound", "jpeg_output_too_large",
            {{"maximum_bytes", std::to_string(state.destination.maximum_bytes)}});
    }
    if (state.destination.allocation_failed)
    {
        return jpeg_encode_error(ErrorCode::kIo, "Unable to allocate JPEG output storage",
                                 "jpeg_output_allocation_failed");
    }
    std::map<std::string, std::string, std::less<>> context;
    if (state.error.message[0] != '\0')
    {
        context.emplace("detail", state.error.message.data());
    }
    return jpeg_encode_error(ErrorCode::kIo, "Unable to encode JPEG image", "jpeg_encoder_failure",
                             std::move(context));
}

[[nodiscard]] Result<std::vector<std::uint8_t>> copy_encoded_output(const JpegDestination &source)
{
    try
    {
        std::vector<std::uint8_t> output(source.output_bytes);
        std::size_t offset = 0U;
        for (std::size_t index = 0U; index < source.chunk_count && offset < output.size(); ++index)
        {
            const std::size_t available =
                std::min(source.capacities[index], output.size() - offset);
            std::memcpy(output.data() + offset, source.chunks[index], available);
            offset += available;
        }
        if (offset != output.size())
        {
            return jpeg_encode_error(ErrorCode::kInternal, "JPEG output accounting is invalid",
                                     "jpeg_output_accounting_error");
        }
        return output;
    }
    catch (const std::bad_alloc &)
    {
        return jpeg_encode_error(ErrorCode::kIo, "Unable to publish encoded JPEG bytes",
                                 "jpeg_output_allocation_failed");
    }
}

void notify_checkpoint(const JpegEncodeControl &control, const JpegEncodeCheckpoint checkpoint,
                       const std::uint32_t progress) noexcept
{
    if (control.checkpoint_observer.callback != nullptr)
    {
        control.checkpoint_observer.callback(control.checkpoint_observer.context, checkpoint,
                                             progress);
    }
}

} // namespace

Result<JpegEncodeConfiguration> jpeg_encode_configuration(const JpegExportOptions &options)
{
    auto valid = validate_jpeg_export_options(options);
    if (!valid)
    {
        return valid.error();
    }

    JpegEncodeConfiguration result;
    result.quality = options.quality;
    result.optimize_coding = true;
    result.y_horizontal = 2U;
    result.y_vertical = 2U;
    result.cb_horizontal = 1U;
    result.cb_vertical = 1U;
    result.cr_horizontal = 1U;
    result.cr_vertical = 1U;
    if (options.quality > 90)
    {
        result.y_vertical = 1U;
    }
    if (options.quality > 92)
    {
        result.y_horizontal = 1U;
    }
    if (options.quality > 95)
    {
        result.dct_method = JpegDctMethod::kFloat;
    }
    if (options.quality < 50)
    {
        result.dct_method = JpegDctMethod::kIntegerFast;
    }
    if (options.quality < 80)
    {
        result.smoothing_factor = 20;
    }
    if (options.quality < 60)
    {
        result.smoothing_factor = 40;
    }
    if (options.quality < 40)
    {
        result.smoothing_factor = 60;
    }
    switch (options.subsampling)
    {
    case JpegSubsampling::kAuto:
        break;
    case JpegSubsampling::k444:
        result.y_horizontal = 1U;
        result.y_vertical = 1U;
        break;
    case JpegSubsampling::k440:
        result.y_horizontal = 1U;
        result.y_vertical = 2U;
        break;
    case JpegSubsampling::k422:
        result.y_horizontal = 2U;
        result.y_vertical = 1U;
        break;
    case JpegSubsampling::k420:
        result.y_horizontal = 2U;
        result.y_vertical = 2U;
        break;
    }
    return result;
}

Result<std::vector<std::uint8_t>> encode_jpeg_rgb8(
    const std::uint32_t width, const std::uint32_t height, const std::span<const std::uint8_t> rgb,
    const std::span<const std::uint8_t> resolved_rgb_icc, const JpegExportOptions &options,
    const CancellationToken &cancellation, const JpegEncodeControl control)
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    if (width == 0U || height == 0U || width > kJpegMaxDimension || height > kJpegMaxDimension)
    {
        return jpeg_encode_error(ErrorCode::kValidation, "JPEG dimensions exceed the encoder limit",
                                 "invalid_jpeg_dimensions",
                                 {{"height", std::to_string(height)},
                                  {"maximum_dimension", std::to_string(kJpegMaxDimension)},
                                  {"width", std::to_string(width)}});
    }
    const std::uint64_t source_bytes =
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 3U;
    if (source_bytes > kJpegMaxSourceBytes)
    {
        return jpeg_encode_error(ErrorCode::kValidation, "JPEG RGB source exceeds the safe bound",
                                 "jpeg_source_too_large",
                                 {{"maximum_bytes", std::to_string(kJpegMaxSourceBytes)},
                                  {"size_bytes", std::to_string(source_bytes)}});
    }
    if (rgb.size() != source_bytes)
    {
        return jpeg_encode_error(ErrorCode::kValidation,
                                 "JPEG RGB source does not match its dimensions",
                                 "jpeg_source_size_mismatch",
                                 {{"actual_bytes", std::to_string(rgb.size())},
                                  {"expected_bytes", std::to_string(source_bytes)}});
    }
    if (resolved_rgb_icc.empty())
    {
        return jpeg_encode_error(ErrorCode::kValidation, "JPEG output requires a resolved RGB ICC",
                                 "missing_jpeg_output_icc");
    }
    if (resolved_rgb_icc.size() > kJpegMaxIccBytes)
    {
        return jpeg_encode_error(ErrorCode::kValidation,
                                 "JPEG output ICC exceeds the APP2 segment limit",
                                 "oversized_jpeg_output_icc",
                                 {{"maximum_bytes", std::to_string(kJpegMaxIccBytes)},
                                  {"size_bytes", std::to_string(resolved_rgb_icc.size())}});
    }
    if (control.max_output_bytes == 0U || control.max_output_bytes > kJpegMaxOutputBytes)
    {
        return jpeg_encode_error(ErrorCode::kValidation, "JPEG output bound is invalid",
                                 "invalid_jpeg_output_bound");
    }
    auto configuration = jpeg_encode_configuration(options);
    if (!configuration)
    {
        return configuration.error();
    }

    notify_checkpoint(control, JpegEncodeCheckpoint::kBeforeStart, 0U);
    active = cancellation.check();
    if (!active)
    {
        return active.error();
    }

    std::unique_ptr<JpegEncoderState> state;
    try
    {
        state = std::make_unique<JpegEncoderState>();
    }
    catch (const std::bad_alloc &)
    {
        return jpeg_encode_error(ErrorCode::kIo, "Unable to allocate JPEG encoder state",
                                 "jpeg_encoder_allocation_failed");
    }
    state->destination.maximum_bytes = control.max_output_bytes;
    state->compressor.err = jpeg_std_error(&state->error.manager);
    state->error.manager.error_exit = jpeg_error_exit;
    if (setjmp(state->error.jump) != 0)
    {
        destroy_compressor(*state);
        return libjpeg_failure(*state);
    }

    state->compressor_created = true;
    jpeg_create_compress(&state->compressor);
    state->destination.manager.init_destination = initialize_destination;
    state->destination.manager.empty_output_buffer = empty_destination_buffer;
    state->destination.manager.term_destination = terminate_destination;
    state->compressor.dest = &state->destination.manager;
    state->compressor.image_width = width;
    state->compressor.image_height = height;
    state->compressor.input_components = 3;
    state->compressor.in_color_space = JCS_RGB;
    jpeg_set_defaults(&state->compressor);
    jpeg_set_quality(&state->compressor, configuration.value().quality, TRUE);
    state->compressor.comp_info[0].h_samp_factor = configuration.value().y_horizontal;
    state->compressor.comp_info[0].v_samp_factor = configuration.value().y_vertical;
    state->compressor.comp_info[1].h_samp_factor = configuration.value().cb_horizontal;
    state->compressor.comp_info[1].v_samp_factor = configuration.value().cb_vertical;
    state->compressor.comp_info[2].h_samp_factor = configuration.value().cr_horizontal;
    state->compressor.comp_info[2].v_samp_factor = configuration.value().cr_vertical;
    state->compressor.optimize_coding = configuration.value().optimize_coding ? TRUE : FALSE;
    state->compressor.smoothing_factor = configuration.value().smoothing_factor;
    switch (configuration.value().dct_method)
    {
    case JpegDctMethod::kIntegerSlow:
        state->compressor.dct_method = JDCT_ISLOW;
        break;
    case JpegDctMethod::kIntegerFast:
        state->compressor.dct_method = JDCT_IFAST;
        break;
    case JpegDctMethod::kFloat:
        state->compressor.dct_method = JDCT_FLOAT;
        break;
    }
    state->compressor.density_unit = 1U;
    state->compressor.X_density = 300U;
    state->compressor.Y_density = 300U;

    jpeg_start_compress(&state->compressor, TRUE);
    jpeg_write_icc_profile(&state->compressor, resolved_rgb_icc.data(),
                           static_cast<unsigned int>(resolved_rgb_icc.size()));
    const std::size_t stride = static_cast<std::size_t>(width) * 3U;
    while (state->compressor.next_scanline < state->compressor.image_height)
    {
        const std::uint32_t scanline = state->compressor.next_scanline;
        notify_checkpoint(control, JpegEncodeCheckpoint::kScanline, scanline);
        {
            auto scanline_active = cancellation.check();
            if (!scanline_active)
            {
                destroy_compressor(*state);
                return scanline_active.error();
            }
        }
        const std::size_t offset = static_cast<std::size_t>(scanline) * stride;
        JSAMPROW row = const_cast<JSAMPROW>(rgb.data() + offset);
        if (jpeg_write_scanlines(&state->compressor, &row, 1U) != 1U)
        {
            destroy_compressor(*state);
            return jpeg_encode_error(ErrorCode::kIo, "JPEG scanline write made no progress",
                                     "jpeg_scanline_write_failed",
                                     {{"scanline", std::to_string(scanline)}});
        }
    }
    notify_checkpoint(control, JpegEncodeCheckpoint::kScanline, height);
    {
        auto finish_active = cancellation.check();
        if (!finish_active)
        {
            destroy_compressor(*state);
            return finish_active.error();
        }
    }
    jpeg_finish_compress(&state->compressor);
    destroy_compressor(*state);
    return copy_encoded_output(state->destination);
}

} // namespace ravo::detail
