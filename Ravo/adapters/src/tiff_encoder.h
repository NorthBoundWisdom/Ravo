#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo::detail
{

inline constexpr std::size_t kTiffMaxSourceBytes = 512U * 1024U * 1024U;
inline constexpr std::size_t kTiffMaxOutputBytes = 512U * 1024U * 1024U;
inline constexpr std::size_t kTiffMaxIccBytes = 16U * 1024U * 1024U;

struct TiffEncodeConfiguration
{
    std::uint16_t bits_per_sample = 0U;
    std::uint16_t sample_format = 0U;
    std::uint16_t samples_per_pixel = 0U;
    std::uint16_t photometric = 0U;
    std::uint16_t planar_configuration = 0U;
    std::uint16_t orientation = 0U;
    std::uint16_t compression = 0U;
    std::uint16_t predictor = 0U;
    int compression_level = 0;
    float resolution_dpi = 0.0F;
    std::uint16_t resolution_unit = 0U;
    bool little_endian = false;
    bool tiled = false;
};

enum class TiffEncodeCheckpoint
{
    kConfigured,
    kMetadata,
    kScanline,
    kBeforeFinish,
};

enum class TiffEncodeInjectedFailure
{
    kNone,
    kEncoderFailure,
    kClientWriteFailure,
    kClientSeekFailure,
    kAllocationFailure,
    kClientCloseFailure,
    kFinalizeFailure,
    kMetadataTagFailure,
};

using TiffEncodeCheckpointCallback = TiffEncodeInjectedFailure (*)(
    void *context, TiffEncodeCheckpoint checkpoint, std::uint32_t progress,
    const TiffEncodeConfiguration &configuration) noexcept;

struct TiffEncodeCheckpointObserver
{
    // Synchronously borrowed by encode_tiff_*(); the caller owns context
    // and keeps it alive for the complete call.
    void *context = nullptr;
    TiffEncodeCheckpointCallback callback = nullptr;
};

struct TiffEncodeControl
{
    std::size_t max_output_bytes = kTiffMaxOutputBytes;
    TiffEncodeCheckpointObserver checkpoint_observer;
};

[[nodiscard]] Result<TiffEncodeConfiguration>
tiff_encode_configuration(const TiffExportOptions &options);

[[nodiscard]] Result<std::vector<std::uint8_t>>
encode_tiff_rgb8(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rgb,
                 std::span<const std::uint8_t> resolved_rgb_icc, const TiffExportOptions &options,
                 const CancellationToken &cancellation, TiffEncodeControl control = {});

[[nodiscard]] Result<std::vector<std::uint8_t>>
encode_tiff_rgb8(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rgb,
                 std::span<const std::uint8_t> resolved_rgb_icc, const TiffExportOptions &options,
                 const ExportMetadataSnapshot &metadata, const CancellationToken &cancellation,
                 TiffEncodeControl control = {});

[[nodiscard]] Result<std::vector<std::uint8_t>>
encode_tiff_rgb8(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rgb,
                 std::span<const std::uint8_t> resolved_rgb_icc, const TiffExportOptions &options,
                 const ExportMetadataSnapshot &metadata, bool builtin_srgb,
                 const CancellationToken &cancellation, TiffEncodeControl control = {});

[[nodiscard]] Result<std::vector<std::uint8_t>>
encode_tiff_rgb16(std::uint32_t width, std::uint32_t height, std::span<const std::uint16_t> rgb,
                  std::span<const std::uint8_t> resolved_rgb_icc, const TiffExportOptions &options,
                  const ExportMetadataSnapshot &metadata, const CancellationToken &cancellation,
                  TiffEncodeControl control = {});

[[nodiscard]] Result<std::vector<std::uint8_t>>
encode_tiff_rgb16(std::uint32_t width, std::uint32_t height, std::span<const std::uint16_t> rgb,
                  std::span<const std::uint8_t> resolved_rgb_icc, const TiffExportOptions &options,
                  const ExportMetadataSnapshot &metadata, bool builtin_srgb,
                  const CancellationToken &cancellation, TiffEncodeControl control = {});

[[nodiscard]] Result<std::vector<std::uint8_t>>
encode_tiff_rgb_float(std::uint32_t width, std::uint32_t height, std::span<const float> rgb,
                      std::span<const std::uint8_t> resolved_rgb_icc,
                      const TiffExportOptions &options, const ExportMetadataSnapshot &metadata,
                      const CancellationToken &cancellation, TiffEncodeControl control = {});

[[nodiscard]] Result<std::vector<std::uint8_t>>
encode_tiff_rgb_float(std::uint32_t width, std::uint32_t height, std::span<const float> rgb,
                      std::span<const std::uint8_t> resolved_rgb_icc,
                      const TiffExportOptions &options, const ExportMetadataSnapshot &metadata,
                      bool builtin_srgb, const CancellationToken &cancellation,
                      TiffEncodeControl control = {});

// Owned IEEE-754 binary16 conversion. Finite values that would become Inf/NaN
// fail structurally. Preserves signed zero and uses round-to-nearest-even.
[[nodiscard]] Result<std::uint16_t> float32_to_binary16(float value);

} // namespace ravo::detail
