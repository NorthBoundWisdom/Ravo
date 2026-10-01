#pragma once

#include "ravo/engine/photo_merge.h"

namespace ravo::photo_merge_internal
{
using Matrix = std::array<double, 9>;
struct Point
{
    double x = 0;
    double y = 0;
};
struct Feature
{
    Point point;
    std::array<std::uint64_t, 4> descriptor{};
};
struct FeatureSet
{
    std::vector<Feature> features;
    double scale_x = 1;
    double scale_y = 1;
};
[[nodiscard]] Result<FeatureSet> find_features(const LinearWorkingBuffer &image,
                                               const CancellationToken &cancellation);
[[nodiscard]] Result<PhotoMergeAlignment> register_pair(const FeatureSet &source,
                                                        const FeatureSet &reference,
                                                        const CancellationToken &cancellation);
[[nodiscard]] Matrix multiply(const Matrix &a, const Matrix &b);
[[nodiscard]] Result<Matrix> inverse(const Matrix &a);
[[nodiscard]] Point project(const Matrix &matrix, Point point);
[[nodiscard]] bool sample(const LinearWorkingBuffer &image, Point point, std::array<float, 3> &rgb);
[[nodiscard]] Result<PhotoMergeImage> merge_hdr(std::span<const LinearWorkingBuffer> frames,
                                                const PhotoMergeOptions &options,
                                                std::vector<PhotoMergeAlignment> alignments,
                                                const CancellationToken &cancellation);
[[nodiscard]] Result<PhotoMergeImage> stitch_panorama(std::span<const LinearWorkingBuffer> frames,
                                                      const PhotoMergeOptions &options,
                                                      std::vector<PhotoMergeAlignment> alignments,
                                                      const CancellationToken &cancellation);
} // namespace ravo::photo_merge_internal
