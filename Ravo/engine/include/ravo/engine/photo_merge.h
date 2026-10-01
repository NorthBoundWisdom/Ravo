#pragma once

#include <array>
#include <span>
#include <vector>

#include "ravo/engine/engine.h"

namespace ravo
{
enum class PhotoMergeKind
{
    kHdr,
    kPanorama
};

struct PhotoMergeOptions
{
    PhotoMergeKind kind = PhotoMergeKind::kHdr;
    bool auto_align = true;
    bool auto_crop = true;
    // Relative capture exposure, in stops; first entry need not be zero.
    std::vector<double> exposure_ev;
    double deghost_threshold = 0.2;
    std::uint64_t memory_budget_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
};

struct PhotoMergeAlignment
{
    std::array<double, 9> source_to_reference{1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::uint32_t inliers = 0;
    double residual_pixels = 0;
    std::size_t reference_frame = 0;
};

struct PhotoMergeImage
{
    // HDR is scene-linear radiance relative to the median capture exposure,
    // including values >1. Geometry remains anchored to frame 0.
    // Panorama remains in the same linear working profile as the inputs.
    LinearWorkingBuffer image;
    std::vector<PhotoMergeAlignment> alignments;
    std::uint64_t deghosted_pixels = 0;
    double origin_x = 0;
    double origin_y = 0;
    double exposure_normalization_ev = 0;
};

// Borrowed immutable frames live for this synchronous call only. Inputs must
// share the Engine linear-working colour profile. No I/O or task ownership.
[[nodiscard]] Result<PhotoMergeImage> merge_photos(std::span<const LinearWorkingBuffer> frames,
                                                   const PhotoMergeOptions &options,
                                                   const CancellationToken &cancellation);

// HDR's deterministic SDR delivery transform; hue-preserving luminance shoulder.
// The caller owns the scene-linear source and receives another owned buffer.
[[nodiscard]] Result<LinearWorkingBuffer>
tone_map_merged_hdr(const LinearWorkingBuffer &image, const CancellationToken &cancellation);
} // namespace ravo
