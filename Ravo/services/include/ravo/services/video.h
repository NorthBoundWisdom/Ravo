#pragma once
#include "ravo/domain/types.h"
namespace ravo
{
struct VideoPreview
{
    DecodedRaster image;
    std::int64_t timestamp_us = 0;
};
[[nodiscard]] Result<std::string> video_source_path(const AssetRecord &asset,
                                                    const CancellationToken &cancellation = {});
[[nodiscard]] Result<DecodedRaster> render_video_frame(const VideoFrame &frame,
                                                       const CancellationToken &cancellation = {});
[[nodiscard]] Result<VideoPreview>
decode_video_preview(const VideoDecoder &decoder, std::string_view path, std::int64_t timestamp_us,
                     std::uint32_t max_edge, const CancellationToken &cancellation = {});
[[nodiscard]] Result<VideoPreview>
decode_video_asset_preview(const VideoDecoder &decoder, const AssetRecord &asset,
                           std::int64_t timestamp_us, std::uint32_t max_edge,
                           const CancellationToken &cancellation = {});
} // namespace ravo
