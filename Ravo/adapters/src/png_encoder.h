#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo::detail
{

inline constexpr std::uint32_t kPngMaxDimension = 0x7FFFFFFFU;
inline constexpr std::size_t kPngMaxSourceBytes = 512U * 1024U * 1024U;
inline constexpr std::size_t kPngMaxOutputBytes = 512U * 1024U * 1024U;
inline constexpr std::size_t kPngMaxIccBytes = 16U * 1024U * 1024U;

struct PngEncodeConfiguration
{
    int bit_depth = 0;
    int color_type = 0;
    int interlace_type = 0;
    int compression_type = 0;
    int filter_method = 0;
    int compression_level = 0;
    int compression_mem_level = 0;
    int compression_strategy = 0;
    int compression_window_bits = 0;
    int compression_method = 0;
    std::size_t compression_buffer_size = 0U;
    int enabled_filters = 0;
};

struct PngEncodeColorMetadata
{
    // Both views are borrowed only for the synchronous encode_png_rgb8() call.
    std::span<const std::uint8_t> resolved_rgb_icc;
    bool has_cicp = false;
    std::array<std::uint8_t, 4U> cicp{};
};

enum class PngEncodeCheckpoint
{
    kConfigured,
    kScanline,
};

enum class PngEncodeInjectedFailure
{
    kNone,
    kEncoderFailure,
    kAllocationFailure,
};

using PngEncodeCheckpointCallback =
    PngEncodeInjectedFailure (*)(void *context, PngEncodeCheckpoint checkpoint,
                                 std::uint32_t progress, int configured_compression) noexcept;

struct PngEncodeCheckpointObserver
{
    // Synchronously borrowed by encode_png_rgb8(); the caller owns context
    // and keeps it alive for the complete call.
    void *context = nullptr;
    PngEncodeCheckpointCallback callback = nullptr;
};

struct PngEncodeControl
{
    std::size_t max_output_bytes = kPngMaxOutputBytes;
    PngEncodeCheckpointObserver checkpoint_observer;
};

[[nodiscard]] Result<PngEncodeConfiguration>
png_encode_configuration(const PngExportOptions &options);

[[nodiscard]] Result<std::vector<std::uint8_t>>
encode_png_rgb8(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rgb,
                const PngEncodeColorMetadata &color_metadata, const PngExportOptions &options,
                const CancellationToken &cancellation, PngEncodeControl control = {});

} // namespace ravo::detail
