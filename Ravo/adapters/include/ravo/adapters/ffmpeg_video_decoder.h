#pragma once
#include "ravo/domain/video.h"
namespace ravo
{
// Process startup owner. In Studio call after Qt initializes its multimedia
// backend, before launching media work. Does not change FFmpeg's log level.
void initialize_ffmpeg_diagnostics();
class FfmpegVideoDecoder final : public VideoDecoder
{
public:
    [[nodiscard]] Result<VideoInfo>
    probe(std::string_view path, const CancellationToken &cancellation = {}) const override;
    [[nodiscard]] Result<VideoFrame>
    frame(std::string_view path, std::int64_t timestamp_us, std::uint32_t max_edge,
          const CancellationToken &cancellation = {}) const override;
    [[nodiscard]] Result<VideoFrame>
    convert_frame(const VideoImageView &image, const VideoInfo &info, std::uint32_t max_edge,
                  const CancellationToken &cancellation = {}) const override;
};
} // namespace ravo
