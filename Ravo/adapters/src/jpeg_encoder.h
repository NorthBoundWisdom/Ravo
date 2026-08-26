#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo::detail
{

inline constexpr std::size_t kJpegMaxSourceBytes = 512U * 1024U * 1024U;
inline constexpr std::size_t kJpegMaxOutputBytes = 512U * 1024U * 1024U;
inline constexpr std::size_t kJpegIccSegmentBytes = 65519U;
inline constexpr std::size_t kJpegMaxIccBytes = 255U * kJpegIccSegmentBytes;
// legacy dimension() advertised 65535, but its jpeg_start_compress() owner
// rejects anything above the pinned libjpeg JPEG_MAX_DIMENSION.
inline constexpr std::uint32_t kJpegMaxDimension = 65500U;

enum class JpegDctMethod
{
    kIntegerSlow,
    kIntegerFast,
    kFloat,
};

struct JpegEncodeConfiguration
{
    int quality = 0;
    int smoothing_factor = 0;
    JpegDctMethod dct_method = JpegDctMethod::kIntegerSlow;
    bool optimize_coding = false;
    std::uint8_t y_horizontal = 0U;
    std::uint8_t y_vertical = 0U;
    std::uint8_t cb_horizontal = 0U;
    std::uint8_t cb_vertical = 0U;
    std::uint8_t cr_horizontal = 0U;
    std::uint8_t cr_vertical = 0U;
};

enum class JpegEncodeCheckpoint
{
    kBeforeStart,
    kScanline,
};

using JpegEncodeCheckpointCallback = void (*)(void *context, JpegEncodeCheckpoint checkpoint,
                                              std::uint32_t progress) noexcept;

struct JpegEncodeCheckpointObserver
{
    // Synchronously borrowed by encode_jpeg_rgb8(); the caller owns context
    // and keeps it alive for the complete call.
    void *context = nullptr;
    JpegEncodeCheckpointCallback callback = nullptr;
};

struct JpegEncodeControl
{
    std::size_t max_output_bytes = kJpegMaxOutputBytes;
    JpegEncodeCheckpointObserver checkpoint_observer;
};

[[nodiscard]] Result<JpegEncodeConfiguration> jpeg_encode_configuration(int quality);

[[nodiscard]] Result<std::vector<std::uint8_t>>
encode_jpeg_rgb8(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rgb,
                 std::span<const std::uint8_t> resolved_rgb_icc, int quality,
                 const CancellationToken &cancellation, JpegEncodeControl control = {});

} // namespace ravo::detail
