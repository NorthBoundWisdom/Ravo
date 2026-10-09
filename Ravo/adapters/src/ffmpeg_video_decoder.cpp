#include "ravo/adapters/ffmpeg_video_decoder.h"
#include "ravo/domain/uri.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <new>
#include <mutex>
#include <cstdarg>
#include <cstring>
#include <utility>
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/parseutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace ravo
{
namespace
{
constexpr std::uint64_t maximum_pixels = 32U * 1024U * 1024U;
TaskError video_error(std::string reason, std::string message, int error = 0)
{
    char detail[AV_ERROR_MAX_STRING_SIZE]{};
    if (error < 0)
        av_strerror(error, detail, sizeof(detail));
    const auto code = reason == "video_allocation_failed" ? ErrorCode::kInternal :
                      error == AVERROR_INVALIDDATA        ? ErrorCode::kValidation :
                      error < 0                           ? ErrorCode::kIo :
                                                            ErrorCode::kUnsupported;
    return make_error(code, std::move(message),
                      {{"reason", std::move(reason)}, {"detail", detail}});
}
struct Input
{
    AVFormatContext *context = nullptr;
    CancellationToken cancellation;
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    int video = -1;
    VideoInfo info;
    unsigned diagnostics = 0;
    bool supported_audio = false;
    AVCodecContext *decoder = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *decoded = nullptr;
    bool draining = false;
    bool prefetched = false;
    std::size_t decode_attempts = 0;
    ~Input()
    {
        av_frame_free(&decoded);
        av_packet_free(&packet);
        avcodec_free_context(&decoder);
        avformat_close_input(&context);
    }
    static int interrupted(void *opaque)
    {
        auto &self = *static_cast<Input *>(opaque);
        return self.cancellation.is_cancellation_requested() ||
               std::chrono::steady_clock::now() >= self.deadline;
    }
    Result<void> active() const
    {
        if (auto result = cancellation.check(); !result)
            return result.error();
        if (std::chrono::steady_clock::now() >= deadline)
            return make_error(ErrorCode::kIo, "Video decode timed out",
                              {{"reason", "video_timeout"}});
        return {};
    }
    Result<void> open_decoder()
    {
        if (decoder)
            return {};
        const auto *codec = avcodec_find_decoder(context->streams[video]->codecpar->codec_id);
        if (!codec)
            return video_error("video_decoder_missing", "Video decoder is not packaged");
        decoder = avcodec_alloc_context3(codec);
        if (!decoder)
            return video_error("video_allocation_failed", "Cannot allocate video decoder");
        decoder->thread_count = 2;
        decoder->max_pixels = maximum_pixels;
        if (avcodec_parameters_to_context(decoder, context->streams[video]->codecpar) < 0 ||
            avcodec_open2(decoder, codec, nullptr) < 0)
            return video_error("video_decoder_open_failed", "Cannot initialize video decoder");
        packet = av_packet_alloc();
        decoded = av_frame_alloc();
        if (!packet || !decoded)
            return video_error("video_allocation_failed", "Cannot allocate video packet/frame");
        return {};
    }
    // The returned frame is borrowed until the next call or Input destruction.
    Result<AVFrame *> next_frame()
    {
        if (prefetched)
        {
            prefetched = false;
            return decoded;
        }
        av_frame_unref(decoded);
        while (++decode_attempts <= 100000)
        {
            if (auto checked = active(); !checked)
                return checked.error();
            const int received = avcodec_receive_frame(decoder, decoded);
            if (received == 0)
            {
                if (auto checked = active(); !checked)
                    return checked.error();
                return decoded;
            }
            if (received == AVERROR_EOF || draining)
                break;
            if (received != AVERROR(EAGAIN))
                return video_error("video_decode_failed", "Corrupt video frame", received);
            int read = 0;
            do
            {
                av_packet_unref(packet);
                read = av_read_frame(context, packet);
                if (auto checked = active(); !checked)
                    return checked.error();
            } while (read >= 0 && packet->stream_index != video);
            if (read < 0 && read != AVERROR_EOF)
                return video_error("video_read_failed", "Cannot read video packet", read);
            draining = read == AVERROR_EOF;
            const int sent = avcodec_send_packet(decoder, draining ? nullptr : packet);
            if (sent < 0)
                return video_error("video_decode_failed", "Cannot decode video packet", sent);
        }
        return video_error("video_frame_unavailable",
                           "No decodable video frame at the requested time");
    }
};
thread_local Input *diagnostic_input = nullptr;
struct DiagnosticScope
{
    Input *previous = diagnostic_input;
    explicit DiagnosticScope(Input &input)
    {
        diagnostic_input = &input;
    }
    ~DiagnosticScope()
    {
        diagnostic_input = previous;
    }
};
void diagnostic_log(void *context, int level, const char *format, va_list arguments)
{
    auto *input = diagnostic_input;
    // Route only this adapter's demuxer warnings. Qt/player/other threads and
    // every error keep the default callback, including malformed channel data.
    if (input && context == input->context && level == AV_LOG_WARNING)
    {
        if (std::strcmp(format, "Unknown cover type: 0x%x.\n") == 0)
        {
            input->diagnostics |= 1U;
            return;
        }
        if (std::strcmp(format, "got %d channel descriptions when number of channels is %d\n") ==
                0 ||
            std::strcmp(format, "capping channel descriptions to the number of channels\n") == 0)
        {
            input->diagnostics |= 2U;
            return;
        }
        if (input->supported_audio &&
            std::string_view(format).starts_with("Could not find codec parameters for stream %d"))
        {
            va_list copy;
            va_copy(copy, arguments);
            const int stream = va_arg(copy, int);
            va_end(copy);
            if (stream >= 0 && static_cast<unsigned>(stream) < input->context->nb_streams)
            {
                const auto *parameters = input->context->streams[stream]->codecpar;
                if (parameters->codec_type == AVMEDIA_TYPE_AUDIO &&
                    parameters->codec_id == AV_CODEC_ID_NONE)
                {
                    input->diagnostics |= 4U;
                    return;
                }
            }
        }
    }
    av_log_default_callback(context, level, format, arguments);
}
bool supported_audio_codec(AVCodecID id)
{
    return id == AV_CODEC_ID_AAC || id == AV_CODEC_ID_PCM_S16LE || id == AV_CODEC_ID_PCM_S24LE ||
           id == AV_CODEC_ID_PCM_S16BE || id == AV_CODEC_ID_PCM_S24BE ||
           id == AV_CODEC_ID_PCM_F32LE;
}
Result<std::string> matrix_name(AVColorSpace matrix)
{
    switch (matrix)
    {
    case AVCOL_SPC_UNSPECIFIED:
    case AVCOL_SPC_BT709:
        return std::string{"bt709"};
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        return std::string{"bt601"};
    case AVCOL_SPC_BT2020_NCL:
        return std::string{"bt2020"};
    default:
        return video_error("video_matrix_unsupported", "Video colour matrix is unsupported");
    }
}
Result<int> matrix_coefficients(std::string_view matrix)
{
    if (matrix == "bt601")
        return SWS_CS_ITU601;
    if (matrix == "bt709")
        return SWS_CS_ITU709;
    if (matrix == "bt2020")
        return SWS_CS_BT2020;
    auto error = video_error("video_matrix_unsupported", "Video colour matrix is unsupported");
    error.context.emplace("matrix", matrix);
    return error;
}
Result<std::unique_ptr<Input>> open_input(std::string_view path,
                                          const CancellationToken &cancellation)
{
    initialize_ffmpeg_diagnostics();
    if (auto result = cancellation.check(); !result)
        return result.error();
    if (std::string_view(av_version_info()) != "7.1.5" ||
        avcodec_version() != LIBAVCODEC_VERSION_INT ||
        avformat_version() != LIBAVFORMAT_VERSION_INT || avutil_version() != LIBAVUTIL_VERSION_INT)
    {
        auto error = video_error("video_runtime_version_mismatch",
                                 "FFmpeg runtime does not match pinned 7.1.5 headers");
        error.context.emplace("runtime", av_version_info());
        error.context.emplace("avcodec", std::to_string(avcodec_version()) + "/" +
                                             std::to_string(LIBAVCODEC_VERSION_INT));
        error.context.emplace("avformat", std::to_string(avformat_version()) + "/" +
                                              std::to_string(LIBAVFORMAT_VERSION_INT));
        error.context.emplace("avutil", std::to_string(avutil_version()) + "/" +
                                            std::to_string(LIBAVUTIL_VERSION_INT));
        return error;
    }
    auto location = normalize_local_input(path);
    if (!location)
        return location.error();
    std::error_code fs_error;
    if (!std::filesystem::is_regular_file(
            std::filesystem::path(
                std::u8string(location.value().path.begin(), location.value().path.end())),
            fs_error) ||
        fs_error)
        return make_error(ErrorCode::kNotFound, "Video source is missing",
                          {{"reason", "video_source_missing"}});
    auto input = std::make_unique<Input>();
    input->cancellation = cancellation;
    input->context = avformat_alloc_context();
    if (!input->context)
        return video_error("video_allocation_failed", "Cannot allocate video reader");
    input->context->interrupt_callback = {Input::interrupted, input.get()};
    input->context->probesize = 8 * 1024 * 1024;
    input->context->max_streams = 16;
    input->context->max_index_size = 8 * 1024 * 1024;
    input->context->max_analyze_duration = 5 * AV_TIME_BASE;
    DiagnosticScope diagnostics(*input);
    AVDictionary *options = nullptr;
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    const int opened =
        avformat_open_input(&input->context, location.value().path.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (auto active = input->active(); !active)
        return active.error();
    if (opened < 0)
        return video_error("video_open_failed", "Cannot read video container", opened);
    if (std::string_view(input->context->iformat->name).find("mov") == std::string_view::npos)
        return video_error("video_container_unsupported",
                           "Only MOV, MP4 and M4V video containers are supported");
    int audio = -1;
    bool has_audio = false;
    for (unsigned index = 0; index < input->context->nb_streams; ++index)
    {
        auto *stream = input->context->streams[index];
        const auto *parameters = stream->codecpar;
        if (parameters->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            has_audio = true;
            if (supported_audio_codec(parameters->codec_id))
            {
                input->supported_audio = true;
                if (audio < 0 || (stream->disposition & AV_DISPOSITION_DEFAULT))
                    audio = static_cast<int>(index);
            }
            else
            {
                input->diagnostics |= 4U;
                stream->discard = AVDISCARD_ALL;
            }
        }
        else if (parameters->codec_type != AVMEDIA_TYPE_VIDEO ||
                 (stream->disposition & AV_DISPOSITION_ATTACHED_PIC))
            stream->discard = AVDISCARD_ALL;
    }
    if (has_audio && audio < 0)
        return video_error("video_audio_codec_unsupported",
                           "Video has no supported AAC or PCM audio track");
    const int inspected = avformat_find_stream_info(input->context, nullptr);
    if (auto active = input->active(); !active)
        return active.error();
    if (inspected < 0)
        return video_error("video_probe_failed", "Cannot read video streams", inspected);
    input->video = av_find_best_stream(input->context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (input->video < 0)
        return video_error("video_stream_missing", "Container has no video stream");
    auto *stream = input->context->streams[input->video];
    auto *parameters = stream->codecpar;
    if ((stream->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
        (parameters->codec_id != AV_CODEC_ID_H264 && parameters->codec_id != AV_CODEC_ID_HEVC &&
         parameters->codec_id != AV_CODEC_ID_PRORES))
    {
        auto error =
            video_error("video_codec_unsupported", "Video codec is not H.264, HEVC or ProRes");
        error.context.emplace("codec", avcodec_get_name(parameters->codec_id));
        error.context.emplace("attached_picture",
                              (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) ? "true" :
                                                                                    "false");
        error.context.emplace("stream_index", std::to_string(input->video));
        return error;
    }
    if (!avcodec_find_decoder(parameters->codec_id))
        return video_error("video_decoder_missing", "Video decoder is not packaged");
    if (parameters->width <= 0 || parameters->height <= 0 || parameters->width > 16384 ||
        parameters->height > 16384 ||
        static_cast<std::uint64_t>(parameters->width) *
                static_cast<std::uint64_t>(parameters->height) >
            maximum_pixels)
        return video_error("video_dimensions_exceeded", "Video dimensions exceed the decode bound");
    auto &info = input->info;
    if (stream->sample_aspect_ratio.num > 0 && stream->sample_aspect_ratio.den > 0 &&
        stream->sample_aspect_ratio.num != stream->sample_aspect_ratio.den)
        return video_error("video_aspect_unsupported", "Non-square video pixels are unsupported");
    auto ext = std::filesystem::path(std::u8string(path.begin(), path.end())).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    info.container = ext == ".mov" ? "mov" : "mp4";
    info.codec = avcodec_get_name(parameters->codec_id);
    info.width = static_cast<std::uint32_t>(parameters->width);
    info.height = static_cast<std::uint32_t>(parameters->height);
    if (stream->duration != AV_NOPTS_VALUE && stream->duration >= 0)
        info.duration_us = av_rescale_q(stream->duration, stream->time_base, AV_TIME_BASE_Q);
    else if (input->context->duration != AV_NOPTS_VALUE && input->context->duration >= 0)
        info.duration_us = input->context->duration;
    if (stream->avg_frame_rate.num >= 0 && stream->avg_frame_rate.den > 0)
    {
        info.frame_rate_num = stream->avg_frame_rate.num;
        info.frame_rate_den = stream->avg_frame_rate.den;
    }
    const auto *matrix = av_packet_side_data_get(
        parameters->coded_side_data, parameters->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if (matrix && matrix->size >= 9 * sizeof(std::int32_t))
    {
        const auto *values = reinterpret_cast<const std::int32_t *>(matrix->data);
        const double determinant =
            static_cast<double>(values[0]) * values[4] - static_cast<double>(values[1]) * values[3];
        if (determinant <= 0)
            return video_error("video_transform_unsupported",
                               "Mirrored or degenerate video transforms are unsupported");
        const double angle =
            -av_display_rotation_get(reinterpret_cast<const std::int32_t *>(matrix->data));
        if (!std::isfinite(angle) || std::abs(angle / 90. - std::round(angle / 90.)) > 0.001)
            return video_error("video_transform_unsupported",
                               "Video rotation is not a quarter turn");
        info.rotation = (static_cast<int>(std::lround(angle)) % 360 + 360) % 360;
    }
    const auto *dovi = av_packet_side_data_get(
        parameters->coded_side_data, parameters->nb_coded_side_data, AV_PKT_DATA_DOVI_CONF);
    if (dovi && dovi->size >= sizeof(AVDOVIDecoderConfigurationRecord))
    {
        const auto *configuration =
            reinterpret_cast<const AVDOVIDecoderConfigurationRecord *>(dovi->data);
        if (configuration->dv_profile != 8 || (configuration->dv_bl_signal_compatibility_id != 1 &&
                                               configuration->dv_bl_signal_compatibility_id != 4))
            return video_error("video_dolby_vision_unsupported",
                               "Dolby Vision profile has no supported PQ/HLG base layer");
        info.dolby_vision = "compatible_base_layer_only";
    }
    if (parameters->codec_id == AV_CODEC_ID_PRORES)
    {
        // ProRes can carry colour tags only in its coded frame header. FFmpeg's
        // stream probe need not copy these per-frame values to codec parameters.
        auto ready = input->open_decoder();
        if (!ready)
            return ready.error();
        auto first = input->next_frame();
        if (!first)
            return first.error();
        if (first.value()->colorspace != AVCOL_SPC_UNSPECIFIED)
            parameters->color_space = first.value()->colorspace;
        if (first.value()->color_trc != AVCOL_TRC_UNSPECIFIED)
            parameters->color_trc = first.value()->color_trc;
        if (first.value()->color_primaries != AVCOL_PRI_UNSPECIFIED)
            parameters->color_primaries = first.value()->color_primaries;
        if (first.value()->color_range != AVCOL_RANGE_UNSPECIFIED)
            parameters->color_range = first.value()->color_range;
        input->prefetched = true;
    }
    if (parameters->color_range != AVCOL_RANGE_UNSPECIFIED ||
        parameters->codec_id == AV_CODEC_ID_PRORES)
        info.full_range = parameters->color_range == AVCOL_RANGE_JPEG;
    const auto transfer = parameters->color_trc;
    const auto colour_error = [&](std::string reason, std::string message)
    {
        auto error = video_error(std::move(reason), std::move(message));
        const auto name = [](const char *value) { return value ? std::string(value) : "unknown"; };
        error.context.emplace("matrix", name(av_color_space_name(parameters->color_space)));
        error.context.emplace("transfer", name(av_color_transfer_name(parameters->color_trc)));
        error.context.emplace("primaries",
                              name(av_color_primaries_name(parameters->color_primaries)));
        return error;
    };
    auto colour_matrix = matrix_name(parameters->color_space);
    if (!colour_matrix)
        return colour_error("video_matrix_unsupported", "Video colour matrix is unsupported");
    info.matrix = std::move(colour_matrix).value();
    if (transfer == AVCOL_TRC_SMPTE2084)
        info.transfer = "pq";
    else if (transfer == AVCOL_TRC_ARIB_STD_B67)
        info.transfer = "hlg";
    else if (transfer == AVCOL_TRC_IEC61966_2_1)
        info.transfer = "srgb";
    else if (transfer != AVCOL_TRC_BT709 && transfer != AVCOL_TRC_SMPTE170M &&
             transfer != AVCOL_TRC_UNSPECIFIED)
        return colour_error("video_transfer_unsupported", "Video transfer function is unsupported");
    if (parameters->color_primaries == AVCOL_PRI_BT2020)
        info.primaries = "bt2020";
    else if (parameters->color_primaries == AVCOL_PRI_SMPTE432)
        info.primaries = "p3_d65";
    else if (parameters->color_primaries != AVCOL_PRI_BT709 &&
             parameters->color_primaries != AVCOL_PRI_UNSPECIFIED)
        return colour_error("video_primaries_unsupported", "Video primaries are unsupported");
    if ((info.transfer == "pq" || info.transfer == "hlg") &&
        (parameters->color_primaries != AVCOL_PRI_BT2020 ||
         parameters->color_space != AVCOL_SPC_BT2020_NCL))
        return colour_error(
            "video_hdr_metadata_unsupported",
            "HDR video requires explicit BT.2020 primaries and non-constant luminance matrix");
    const auto *mastering =
        av_packet_side_data_get(parameters->coded_side_data, parameters->nb_coded_side_data,
                                AV_PKT_DATA_MASTERING_DISPLAY_METADATA);
    if (mastering && mastering->size >= sizeof(AVMasteringDisplayMetadata))
    {
        const auto *metadata =
            reinterpret_cast<const AVMasteringDisplayMetadata *>(mastering->data);
        const double peak = av_q2d(metadata->max_luminance);
        if (metadata->has_luminance && std::isfinite(peak) && peak > 100 && peak <= 10000)
        {
            info.peak_nits = peak;
            info.peak_source = "mastering_display_metadata";
        }
    }
    info.has_audio = audio >= 0;
    if (audio >= 0)
    {
        const auto id = input->context->streams[audio]->codecpar->codec_id;
        info.audio_codec = avcodec_get_name(id);
    }
    if (auto *date = av_dict_get(input->context->metadata, "creation_time", nullptr, 0))
    {
        std::int64_t time = 0;
        if (av_parse_time(&time, date->value, 0) >= 0)
            info.captured_unix_s = time / AV_TIME_BASE;
    }
    for (auto [flag, code] : {std::pair{1U, "unknown_cover_ignored"},
                              std::pair{2U, "extra_channel_descriptions_capped"},
                              std::pair{4U, "unsupported_auxiliary_audio"}})
        if (input->diagnostics & flag)
            info.warnings.emplace_back(code);
    return input;
}
Result<VideoFrame> convert_image(const std::uint8_t *const *planes, const int *strides, int width,
                                 int height, AVPixelFormat format, bool full_range, int colourspace,
                                 const VideoInfo &info, std::uint32_t max_edge,
                                 const CancellationToken &cancellation)
{
    if (auto active = cancellation.check(); !active)
        return active.error();
    // YUVJ has the same plane layout as YUV, with full range encoded in the
    // deprecated pixel-format name. Supply that range explicitly to swscale.
    switch (format)
    {
    case AV_PIX_FMT_YUVJ420P:
        format = AV_PIX_FMT_YUV420P;
        full_range = true;
        break;
    case AV_PIX_FMT_YUVJ422P:
        format = AV_PIX_FMT_YUV422P;
        full_range = true;
        break;
    case AV_PIX_FMT_YUVJ444P:
        format = AV_PIX_FMT_YUV444P;
        full_range = true;
        break;
    case AV_PIX_FMT_YUVJ440P:
        format = AV_PIX_FMT_YUV440P;
        full_range = true;
        break;
    default:
        break;
    }
    if (width < 1 || height < 1 ||
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) > maximum_pixels ||
        max_edge > 4096)
        return video_error("video_dimensions_exceeded", "Video frame exceeds the conversion bound");
    const auto edge =
        max_edge == 0 ? static_cast<std::uint32_t>(std::max(width, height)) : max_edge;
    if (edge > 4096)
        return video_error("video_dimensions_exceeded",
                           "Full-size video frame exceeds the conversion bound");
    const double scale = std::min(1., static_cast<double>(edge) / std::max(width, height));
    const int out_w = std::max(1, static_cast<int>(std::lround(width * scale)));
    const int out_h = std::max(1, static_cast<int>(std::lround(height * scale)));
    std::unique_ptr<SwsContext, decltype(&sws_freeContext)> scaler(
        sws_getContext(width, height, format, out_w, out_h, AV_PIX_FMT_RGB48LE, SWS_BILINEAR,
                       nullptr, nullptr, nullptr),
        sws_freeContext);
    if (!scaler)
        return video_error("video_pixel_format_unsupported", "Cannot convert video pixel format");
    const auto *coefficients = sws_getCoefficients(colourspace);
    if (sws_setColorspaceDetails(scaler.get(), coefficients, full_range, coefficients, 1, 0,
                                 1 << 16, 1 << 16) < 0)
        return video_error("video_matrix_unsupported", "Cannot apply video colour matrix");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(out_w) *
                                    static_cast<std::size_t>(out_h) * 6U);
    std::uint8_t *output[4]{bytes.data(), nullptr, nullptr, nullptr};
    int output_strides[4]{out_w * 6, 0, 0, 0};
    if (sws_scale(scaler.get(), planes, strides, 0, height, output, output_strides) != out_h)
        return video_error("video_scale_failed", "Video frame conversion failed");
    if (auto active = cancellation.check(); !active)
        return active.error();
    VideoFrame frame;
    frame.info = info;
    const bool swapped = info.rotation == 90 || info.rotation == 270;
    frame.width = static_cast<std::uint32_t>(swapped ? out_h : out_w);
    frame.height = static_cast<std::uint32_t>(swapped ? out_w : out_h);
    frame.rgb.resize(static_cast<std::size_t>(frame.width) * frame.height * 3U);
    for (int y = 0; y < out_h; ++y)
        for (int x = 0; x < out_w; ++x)
        {
            int dx = x, dy = y;
            if (info.rotation == 90)
            {
                dx = out_h - y - 1;
                dy = x;
            }
            else if (info.rotation == 180)
            {
                dx = out_w - x - 1;
                dy = out_h - y - 1;
            }
            else if (info.rotation == 270)
            {
                dx = y;
                dy = out_w - x - 1;
            }
            for (std::size_t c = 0; c < 3; ++c)
            {
                const auto source = (static_cast<std::size_t>(y) * static_cast<std::size_t>(out_w) +
                                     static_cast<std::size_t>(x)) *
                                        6U +
                                    c * 2U;
                const auto value = static_cast<unsigned>(bytes[source]) |
                                   (static_cast<unsigned>(bytes[source + 1]) << 8U);
                frame.rgb[(static_cast<std::size_t>(dy) * frame.width +
                           static_cast<std::size_t>(dx)) *
                              3U +
                          c] = static_cast<float>(value) / 65535.F;
            }
        }
    return frame;
}
} // namespace

void initialize_ffmpeg_diagnostics()
{
    static std::once_flag initialized;
    std::call_once(initialized, [] { av_log_set_callback(diagnostic_log); });
}

Result<VideoInfo> FfmpegVideoDecoder::probe(std::string_view path,
                                            const CancellationToken &cancellation) const
try
{
    auto input = open_input(path, cancellation);
    if (!input)
        return input.error();
    return input.value()->info;
}
catch (const std::bad_alloc &)
{
    return video_error("video_allocation_failed", "Video probe exhausted memory");
}
Result<VideoFrame> FfmpegVideoDecoder::frame(std::string_view path, std::int64_t timestamp_us,
                                             std::uint32_t max_edge,
                                             const CancellationToken &cancellation) const
try
{
    auto opened = open_input(path, cancellation);
    if (!opened)
        return opened.error();
    auto &input = *opened.value();
    if (timestamp_us < 0 || (input.info.duration_us && timestamp_us >= *input.info.duration_us))
        return make_error(ErrorCode::kInvalidArgument, "Requested video time is outside the media",
                          {{"reason", "video_time_out_of_range"}});
    auto *stream = input.context->streams[input.video];
    if (auto ready = input.open_decoder(); !ready)
        return ready.error();
    const auto origin = stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
    const auto target = origin + av_rescale_q(timestamp_us, AV_TIME_BASE_Q, stream->time_base);
    if (timestamp_us > 0)
    {
        if (av_seek_frame(input.context, input.video, target, AVSEEK_FLAG_BACKWARD) < 0)
            return video_error("video_seek_failed", "Cannot seek video");
        avcodec_flush_buffers(input.decoder);
        input.prefetched = false;
        input.draining = false;
    }
    for (std::size_t attempts = 0; attempts < 100000; ++attempts)
    {
        if (auto active = input.active(); !active)
            return active.error();
        auto next = input.next_frame();
        if (!next)
            return next.error();
        const auto *decoded = next.value();
        {
            const auto pts = decoded->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE)
                return video_error("video_timestamp_unavailable",
                                   "Decoded video frame has no presentation timestamp");
            if (pts != AV_NOPTS_VALUE && pts < target)
            {
                continue;
            }
            auto coefficients = matrix_coefficients(*input.info.matrix);
            if (!coefficients)
                return coefficients.error();
            if (decoded->colorspace != AVCOL_SPC_UNSPECIFIED)
            {
                auto frame_matrix = matrix_name(decoded->colorspace);
                if (!frame_matrix)
                    return frame_matrix.error();
                if (frame_matrix.value() != *input.info.matrix)
                    return video_error("video_frame_colour_mismatch",
                                       "Video frame matrix changed since probing");
            }
            auto result =
                convert_image(decoded->data, decoded->linesize, decoded->width, decoded->height,
                              static_cast<AVPixelFormat>(decoded->format),
                              decoded->color_range == AVCOL_RANGE_UNSPECIFIED ?
                                  input.info.full_range.value_or(false) :
                                  decoded->color_range == AVCOL_RANGE_JPEG,
                              coefficients.value(), input.info, max_edge, cancellation);
            if (!result)
                return result.error();
            if (auto checked = input.active(); !checked)
                return checked.error();
            result.value().timestamp_us = std::max<std::int64_t>(
                0, av_rescale_q(pts - origin, stream->time_base, AV_TIME_BASE_Q));
            return result;
        }
    }
    return video_error("video_frame_unavailable", "No decodable video frame at the requested time");
}
catch (const std::bad_alloc &)
{
    return video_error("video_allocation_failed", "Video decode exhausted memory");
}
Result<VideoFrame> FfmpegVideoDecoder::convert_frame(const VideoImageView &image,
                                                     const VideoInfo &info, std::uint32_t max_edge,
                                                     const CancellationToken &cancellation) const
try
{
    auto coefficients = matrix_coefficients(image.matrix);
    if (!coefficients)
        return coefficients.error();
    const bool rgb =
        image.format == VideoPixelFormat::kRgba || image.format == VideoPixelFormat::kBgra;
    if (!rgb && info.matrix && *info.matrix != image.matrix)
        return video_error("video_frame_colour_mismatch",
                           "Video frame matrix differs from source metadata");
    AVPixelFormat format = AV_PIX_FMT_NONE;
    int count = 1;
    switch (image.format)
    {
    case VideoPixelFormat::kYuv420p:
        format = AV_PIX_FMT_YUV420P;
        count = 3;
        break;
    case VideoPixelFormat::kYuv422p:
        format = AV_PIX_FMT_YUV422P;
        count = 3;
        break;
    case VideoPixelFormat::kNv12:
        format = AV_PIX_FMT_NV12;
        count = 2;
        break;
    case VideoPixelFormat::kP010:
        format = AV_PIX_FMT_P010LE;
        count = 2;
        break;
    case VideoPixelFormat::kP016:
        format = AV_PIX_FMT_P016LE;
        count = 2;
        break;
    case VideoPixelFormat::kYuv420p10:
        format = AV_PIX_FMT_YUV420P10LE;
        count = 3;
        break;
    case VideoPixelFormat::kRgba:
        format = AV_PIX_FMT_RGBA;
        break;
    case VideoPixelFormat::kBgra:
        format = AV_PIX_FMT_BGRA;
        break;
    }
    const std::uint8_t *planes[4]{};
    int strides[4]{};
    for (int p = 0; p < count; ++p)
    {
        const auto rows = p == 0 || image.format == VideoPixelFormat::kYuv422p ?
                              image.height :
                              (image.height + 1U) / 2U;
        const auto samples =
            p == 0 ? image.width :
            (image.format == VideoPixelFormat::kNv12 || image.format == VideoPixelFormat::kP010 ||
             image.format == VideoPixelFormat::kP016) ?
                     ((image.width + 1U) / 2U) * 2U :
                     (image.width + 1U) / 2U;
        const auto bytes =
            image.format == VideoPixelFormat::kRgba || image.format == VideoPixelFormat::kBgra ?
                4U :
            image.format == VideoPixelFormat::kP010 || image.format == VideoPixelFormat::kP016 ||
                    image.format == VideoPixelFormat::kYuv420p10 ?
                2U :
                1U;
        if (image.strides[p] < 1 ||
            static_cast<std::uint64_t>(image.strides[p]) <
                static_cast<std::uint64_t>(samples) * bytes ||
            image.planes[p].size() < static_cast<std::uint64_t>(image.strides[p]) * rows)
            return video_error("video_plane_invalid", "Video frame plane exceeds its storage");
        planes[p] = image.planes[p].data();
        strides[p] = image.strides[p];
    }
    return convert_image(planes, strides, static_cast<int>(image.width),
                         static_cast<int>(image.height), format, image.full_range,
                         coefficients.value(), info, max_edge, cancellation);
}
catch (const std::bad_alloc &)
{
    return video_error("video_allocation_failed", "Video conversion exhausted memory");
}
} // namespace ravo
