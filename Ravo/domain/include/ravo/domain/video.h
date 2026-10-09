#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/json.h"

namespace ravo
{
struct VideoInfo
{
    std::string container;
    std::string codec;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::optional<std::int64_t> duration_us;
    std::int32_t frame_rate_num = 0;
    std::int32_t frame_rate_den = 1;
    int rotation = 0; // Clockwise quarter turns, applied once to coded pixels.
    bool has_audio = false;
    std::string audio_codec;
    std::string transfer = "bt709";
    std::string primaries = "bt709";
    double peak_nits = 1000.;
    std::string peak_source = "nominal_1000_nits";
    std::optional<std::int64_t> captured_unix_s;
    std::string dolby_vision = "none";
    // Stable nonfatal diagnostics; original audio and metadata bytes are preserved.
    std::vector<std::string> warnings;
    // Optional only for metadata written before matrix identity was persisted.
    std::optional<std::string> matrix{};
    std::optional<bool> full_range{};
    [[nodiscard]] bool operator==(const VideoInfo &) const noexcept = default;
};

enum class VideoPixelFormat
{
    kYuv420p,
    kNv12,
    kP010,
    kYuv420p10,
    kRgba,
    kBgra,
    kYuv422p,
    kP016
};
// Planes are borrowed only for convert_frame's synchronous call. The caller
// owns and keeps their mapped storage alive until the call returns.
struct VideoImageView
{
    VideoPixelFormat format = VideoPixelFormat::kYuv420p;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::span<const std::uint8_t> planes[3];
    int strides[3]{};
    bool full_range = false;
    std::string matrix = "bt709";
};
struct VideoFrame
{
    VideoInfo info;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int64_t timestamp_us = 0;
    // Nonlinear RGB in the source transfer/primaries, before SDR presentation.
    std::vector<float> rgb;
};
class VideoDecoder
{
public:
    virtual ~VideoDecoder() = default;
    [[nodiscard]] virtual Result<VideoInfo>
    probe(std::string_view path, const CancellationToken &cancellation = {}) const = 0;
    [[nodiscard]] virtual Result<VideoFrame>
    frame(std::string_view path, std::int64_t timestamp_us, std::uint32_t max_edge,
          const CancellationToken &cancellation = {}) const = 0;
    [[nodiscard]] virtual Result<VideoFrame>
    convert_frame(const VideoImageView &image, const VideoInfo &info, std::uint32_t max_edge,
                  const CancellationToken &cancellation = {}) const = 0;
};
[[nodiscard]] bool is_video_path(std::string_view path);
[[nodiscard]] bool is_video_media_type(std::string_view media_type) noexcept;
[[nodiscard]] JsonValue video_info_json(const VideoInfo &info);
[[nodiscard]] Result<VideoInfo> parse_video_info(const JsonValue &value);
} // namespace ravo
