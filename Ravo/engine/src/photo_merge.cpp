#include "ravo/engine/photo_merge.h"
#include "photo_registration.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace ravo
{
Result<PhotoMergeImage> merge_photos(const std::span<const LinearWorkingBuffer> frames,
                                     const PhotoMergeOptions &options,
                                     const CancellationToken &cancellation)
{
    using namespace photo_merge_internal;
    if (auto active = cancellation.check(); !active)
        return active.error();
    if (frames.size() < 2 || frames.size() > 16 || options.memory_budget_bytes < 1024 * 1024 ||
        options.memory_budget_bytes > 4ULL * 1024 * 1024 * 1024 ||
        !std::isfinite(options.deghost_threshold) || options.deghost_threshold < 0 ||
        options.deghost_threshold > 1)
        return make_error(ErrorCode::kInvalidArgument, "Invalid bounded photo merge options",
                          {{"reason", "invalid_merge_options"}});
    if (options.kind != PhotoMergeKind::kHdr && options.kind != PhotoMergeKind::kPanorama)
        return make_error(ErrorCode::kInvalidArgument, "Unknown photo merge kind");
    if (options.kind == PhotoMergeKind::kPanorama && !options.auto_align)
        return make_error(ErrorCode::kInvalidArgument, "Panorama requires automatic alignment");
    std::uint64_t owned_bytes = 0;
    for (const auto &frame : frames)
    {
        const std::uint64_t pixels = std::uint64_t(frame.width) * frame.height;
        if (!pixels || frame.width > 50000 || frame.height > 50000 || pixels > 80000000 ||
            frame.rgb.size() != pixels * 3 || frame.color_profile != frames.front().color_profile)
            return make_error(ErrorCode::kValidation, "Invalid photo merge image or colour profile",
                              {{"reason", "merge_input_mismatch"}});
        owned_bytes += pixels * 12;
        for (std::size_t i = 0; i < frame.rgb.size(); ++i)
        {
            if (i % 65536 == 0)
                if (auto active = cancellation.check(); !active)
                    return active.error();
            if (!std::isfinite(frame.rgb[i]))
                return make_error(ErrorCode::kValidation, "Non-finite photo merge input",
                                  {{"reason", "merge_nonfinite_input"}});
        }
        if (options.kind == PhotoMergeKind::kHdr &&
            (frame.width != frames.front().width || frame.height != frames.front().height))
            return make_error(ErrorCode::kValidation, "HDR exposures must have equal dimensions",
                              {{"reason", "hdr_dimensions_mismatch"}});
    }
    // Includes proxy/descriptor scratch and result/warp/encode headroom. Output
    // canvas has a separate gate after registration, before allocating it.
    if (owned_bytes + 64ULL * 1024 * 1024 > options.memory_budget_bytes)
        return make_error(ErrorCode::kValidation, "Photo merge memory budget exceeded",
                          {{"reason", "merge_memory_budget_exceeded"}});
    if (options.kind == PhotoMergeKind::kHdr)
    {
        if (options.exposure_ev.size() != frames.size())
            return make_error(ErrorCode::kInvalidArgument,
                              "One capture exposure is required per HDR frame",
                              {{"reason", "hdr_exposure_required"}});
        for (const auto ev : options.exposure_ev)
            if (!std::isfinite(ev) || std::abs(ev) > 24 ||
                std::abs(ev - options.exposure_ev.front()) > 24)
                return make_error(ErrorCode::kInvalidArgument,
                                  "HDR exposure stops are out of range");
        const auto [low, high] =
            std::minmax_element(options.exposure_ev.begin(), options.exposure_ev.end());
        if (*high - *low < 0.1)
            return make_error(ErrorCode::kInvalidArgument,
                              "HDR requires different capture exposures",
                              {{"reason", "hdr_exposures_identical"}});
    }
    try
    {
        std::vector<PhotoMergeAlignment> alignments(frames.size());
        if (options.auto_align)
        {
            std::vector<FeatureSet> features;
            for (const auto &frame : frames)
            {
                auto found = find_features(frame, cancellation);
                if (!found)
                    return found.error();
                features.push_back(std::move(found).value());
            }
            std::vector<bool> connected(frames.size(), false);
            connected[0] = true;
            std::vector<bool> attempted(frames.size() * frames.size(), false);
            std::vector<std::optional<PhotoMergeAlignment>> matches(frames.size() * frames.size());
            for (std::size_t count = 1; count < frames.size(); ++count)
            {
                std::size_t source = frames.size(), reference = 0;
                PhotoMergeAlignment best;
                for (std::size_t i = 1; i < frames.size(); ++i)
                    if (!connected[i])
                    {
                        for (std::size_t j = 0; j < frames.size(); ++j)
                            if (connected[j])
                            {
                                // HDR uses one reference; panorama builds an overlap tree
                                // so the user need not order a connected selection.
                                if (options.kind == PhotoMergeKind::kHdr && j != 0)
                                    continue;
                                const auto edge = i * frames.size() + j;
                                if (!attempted[edge])
                                {
                                    auto match =
                                        register_pair(features[i], features[j], cancellation);
                                    attempted[edge] = true;
                                    if (!match)
                                    {
                                        if (match.error().code == ErrorCode::kCancelled)
                                            return match.error();
                                    }
                                    else
                                        matches[edge] = match.value();
                                }
                                if (matches[edge] && matches[edge]->inliers > best.inliers)
                                {
                                    source = i;
                                    reference = j;
                                    best = *matches[edge];
                                }
                            }
                    }
                if (source == frames.size())
                    return make_error(ErrorCode::kValidation,
                                      "Selection has no reliable connected alignment",
                                      {{"reason", "merge_alignment_failed"}});
                best.source_to_reference =
                    multiply(alignments[reference].source_to_reference, best.source_to_reference);
                alignments[source] = best;
                connected[source] = true;
                alignments[source].reference_frame = reference;
            }
        }
        if (options.kind == PhotoMergeKind::kHdr)
        {
            for (const auto &alignment : alignments)
            {
                const auto &m = alignment.source_to_reference;
                const double width = frames.front().width, height = frames.front().height;
                const std::array<Point, 4> corners{
                    {{0, 0}, {width - 1, 0}, {width - 1, height - 1}, {0, height - 1}}};
                for (const auto corner : corners)
                {
                    const auto p = project(m, corner);
                    if (!std::isfinite(p.x) || !std::isfinite(p.y) ||
                        std::abs(p.x - corner.x) > width * .2 ||
                        std::abs(p.y - corner.y) > height * .2)
                        return make_error(ErrorCode::kValidation,
                                          "HDR registration has excessive drift",
                                          {{"reason", "hdr_unreliable_transform"}});
                }
            }
            return merge_hdr(frames, options, std::move(alignments), cancellation);
        }
        return stitch_panorama(frames, options, std::move(alignments), cancellation);
    }
    catch (const std::bad_alloc &)
    {
        return make_error(ErrorCode::kIo, "Unable to allocate photo merge buffers",
                          {{"reason", "merge_allocation_failed"}});
    }
}

Result<LinearWorkingBuffer> tone_map_merged_hdr(const LinearWorkingBuffer &image,
                                                const CancellationToken &cancellation)
{
    if (!image.width || !image.height ||
        image.rgb.size() != std::uint64_t(image.width) * image.height * 3)
        return make_error(ErrorCode::kValidation, "Invalid HDR tone-map buffer");
    try
    {
        LinearWorkingBuffer output = image;
        for (std::size_t i = 0; i < output.rgb.size(); i += 3)
        {
            if (i % 65535 == 0)
                if (auto active = cancellation.check(); !active)
                    return active.error();
            const float y = std::max(0.F, .2126F * image.rgb[i] + .7152F * image.rgb[i + 1] +
                                              .0722F * image.rgb[i + 2]);
            for (unsigned c = 0; c < 3; ++c)
            {
                if (!std::isfinite(image.rgb[i + c]))
                    return make_error(ErrorCode::kValidation, "Non-finite HDR radiance");
                output.rgb[i + c] = std::max(0.F, image.rgb[i + c]) / (1 + y);
            }
        }
        return output;
    }
    catch (const std::bad_alloc &)
    {
        return make_error(ErrorCode::kIo, "Unable to allocate HDR delivery buffer");
    }
}
} // namespace ravo
