#include <algorithm>
#include <cmath>
#include <random>
#include <gtest/gtest.h>

#include "ravo/engine/photo_merge.h"

namespace ravo
{
namespace
{
LinearWorkingBuffer scene(std::uint32_t width = 640, std::uint32_t height = 256)
{
    LinearWorkingBuffer image;
    image.width = width;
    image.height = height;
    image.color_profile.kind = ColorProfileKind::kBuiltin;
    image.color_profile.identifier = "linear-rec709";
    image.rgb.resize(std::size_t(width) * height * 3);
    std::mt19937 random(123456);
    std::vector<float> tiles((width / 5 + 1) * (height / 5 + 1));
    for (auto &v : tiles)
        v = .08F + .7F * float(random() % 1000) / 1000;
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const float base = tiles[(y / 5) * (width / 5 + 1) + x / 5];
            const auto p = (std::size_t(y) * width + x) * 3;
            image.rgb[p] = base;
            image.rgb[p + 1] = base * .8F;
            image.rgb[p + 2] = base * .65F;
        }
    return image;
}
LinearWorkingBuffer section(const LinearWorkingBuffer &image, std::uint32_t x, std::uint32_t y,
                            std::uint32_t width, std::uint32_t height, float gain = 1)
{
    LinearWorkingBuffer out;
    out.width = width;
    out.height = height;
    out.color_profile = image.color_profile;
    out.rgb.resize(std::size_t(width) * height * 3);
    for (std::uint32_t row = 0; row < height; ++row)
        for (std::uint32_t col = 0; col < width; ++col)
            for (unsigned c = 0; c < 3; ++c)
                out.rgb[(std::size_t(row) * width + col) * 3 + c] = std::min(
                    1.F, image.rgb[(std::size_t(row + y) * image.width + col + x) * 3 + c] * gain);
    return out;
}
} // namespace

TEST(PhotoMerge, HdrRecoversHighlightsAndPreservesChromaticity)
{
    auto image = scene(64, 48);
    for (std::size_t i = 0; i < image.rgb.size(); i += 3)
    {
        image.rgb[i] = 1.5F;
        image.rgb[i + 1] = 1.2F;
        image.rgb[i + 2] = .6F;
    }
    std::vector<LinearWorkingBuffer> frames{section(image, 0, 0, 64, 48),
                                            section(image, 0, 0, 64, 48, .25F),
                                            section(image, 0, 0, 64, 48, 4.F)};
    const auto original = frames[1].rgb;
    PhotoMergeOptions options;
    options.auto_align = false;
    options.exposure_ev = {0, -2, 2};
    auto result = merge_photos(frames, options, {});
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_NEAR(result.value().image.rgb[0], 1.5, 1e-5);
    EXPECT_NEAR(result.value().image.rgb[1], 1.2, 1e-5);
    EXPECT_NEAR(result.value().image.rgb[2], .6, 1e-5);
    EXPECT_EQ(frames[1].rgb, original);
    auto display = tone_map_merged_hdr(result.value().image, {});
    ASSERT_TRUE(display);
    EXPECT_LT(display.value().rgb[0], 1);
    EXPECT_NEAR(display.value().rgb[0] / display.value().rgb[1], 1.25, 1e-5);
}

TEST(PhotoMerge, HdrRadianceDoesNotDependOnBracketOrder)
{
    const auto image = scene(64, 48);
    const auto dark = section(image, 0, 0, 64, 48, .25F);
    const auto middle = section(image, 0, 0, 64, 48);
    const auto bright = section(image, 0, 0, 64, 48, 4.F);
    PhotoMergeOptions options;
    options.auto_align = false;
    options.exposure_ev = {-2, 0, 2};
    auto forward =
        merge_photos(std::vector<LinearWorkingBuffer>{dark, middle, bright}, options, {});
    ASSERT_TRUE(forward);
    options.exposure_ev = {2, 0, -2};
    auto reverse =
        merge_photos(std::vector<LinearWorkingBuffer>{bright, middle, dark}, options, {});
    ASSERT_TRUE(reverse);
    ASSERT_EQ(forward.value().image.rgb.size(), reverse.value().image.rgb.size());
    for (std::size_t i = 0; i < forward.value().image.rgb.size(); ++i)
        EXPECT_NEAR(forward.value().image.rgb[i], reverse.value().image.rgb[i], 1e-5);
}

TEST(PhotoMerge, RegistersExposureShiftAndCropsToIntersection)
{
    const auto image = scene();
    std::vector<LinearWorkingBuffer> frames{section(image, 0, 0, 360, 240),
                                            section(image, 8, 4, 360, 240, .5F)};
    PhotoMergeOptions options;
    options.exposure_ev = {0, -1};
    options.deghost_threshold = 0;
    auto result = merge_photos(frames, options, {});
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_GE(result.value().alignments[1].inliers, 12);
    EXPECT_NEAR(result.value().alignments[1].source_to_reference[2], 8, .5);
    EXPECT_NEAR(result.value().alignments[1].source_to_reference[5], 4, .5);
    EXPECT_GE(result.value().image.width, 350);
    EXPECT_LE(result.value().image.width, 353);
    EXPECT_GE(result.value().image.height, 234);
    EXPECT_LE(result.value().image.height, 236);
}

TEST(PhotoMerge, RecoversMildProjectiveHandheldRegistration)
{
    const auto original = scene();
    auto reference = section(original, 0, 0, 400, 240);
    auto changed = reference;
    for (std::uint32_t y = 0; y < changed.height; ++y)
        for (std::uint32_t x = 0; x < changed.width; ++x)
        {
            const double u = (x + 7 + .01 * y) / (1 + .00002 * x);
            const double v = (y + 4 - .01 * x) / (1 + .00002 * x);
            const auto ix = static_cast<std::uint32_t>(u), iy = static_cast<std::uint32_t>(v);
            const float dx = static_cast<float>(u - ix), dy = static_cast<float>(v - iy);
            for (std::size_t c = 0; c < 3; ++c)
            {
                const auto p = (std::size_t(iy) * original.width + ix) * 3 + c;
                changed.rgb[(std::size_t(y) * changed.width + x) * 3 + c] =
                    .5F * ((1 - dy) * ((1 - dx) * original.rgb[p] + dx * original.rgb[p + 3]) +
                           dy * ((1 - dx) * original.rgb[p + original.width * 3] +
                                 dx * original.rgb[p + original.width * 3 + 3]));
            }
        }
    PhotoMergeOptions options;
    options.exposure_ev = {0, -1};
    auto result = merge_photos(std::vector<LinearWorkingBuffer>{reference, changed}, options, {});
    ASSERT_TRUE(result) << result.error().message;
    const auto &m = result.value().alignments[1].source_to_reference;
    const double x = 350, y = 210, z = m[6] * x + m[7] * y + m[8];
    EXPECT_NEAR((m[0] * x + m[1] * y + m[2]) / z, (x + 7 + .01 * y) / (1 + .00002 * x), 1.5);
    EXPECT_NEAR((m[3] * x + m[4] * y + m[5]) / z, (y + 4 - .01 * x) / (1 + .00002 * x), 1.5);
}

TEST(PhotoMerge, PanoramaMatchesTrueCanvasAndCompensatesExposure)
{
    const auto image = scene();
    std::vector<LinearWorkingBuffer> frames{section(image, 0, 0, 360, 256),
                                            section(image, 200, 0, 360, 256, .8F)};
    PhotoMergeOptions options;
    options.kind = PhotoMergeKind::kPanorama;
    auto result = merge_photos(frames, options, {});
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_NEAR(result.value().alignments[1].source_to_reference[2], 200, .5);
    EXPECT_GE(result.value().image.width, 555);
    EXPECT_LE(result.value().image.width, 562);
    EXPECT_GE(result.value().image.height, 253);
    double total = 0;
    // Interior patches are compared after a possible 1-pixel conservative crop.
    for (std::uint32_t y = 20; y < 200; ++y)
        for (std::uint32_t x = 20; x < 500; ++x)
        {
            const auto i = (std::size_t(y) * result.value().image.width + x) * 3;
            const auto source_x = x + static_cast<std::uint32_t>(result.value().origin_x);
            const auto source_y = y + static_cast<std::uint32_t>(result.value().origin_y);
            total += std::abs(result.value().image.rgb[i] -
                              image.rgb[(std::size_t(source_y) * image.width + source_x) * 3]);
        }
    EXPECT_LT(total / (180 * 480), .03);
}

TEST(PhotoMerge, ConnectsUnorderedThreeFramePanorama)
{
    const auto image = scene(900, 256);
    std::vector<LinearWorkingBuffer> frames{section(image, 0, 0, 360, 256),
                                            section(image, 500, 0, 360, 256),
                                            section(image, 250, 0, 360, 256)};
    PhotoMergeOptions options;
    options.kind = PhotoMergeKind::kPanorama;
    auto result = merge_photos(frames, options, {});
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_GE(result.value().image.width, 853);
    EXPECT_EQ(result.value().alignments[1].reference_frame, 2);
}

TEST(PhotoMerge, RejectsDisconnectedFeaturelessAndMalformedSelections)
{
    std::vector<LinearWorkingBuffer> frames{scene(64, 48), scene(64, 48)};
    for (auto &frame : frames)
        std::fill(frame.rgb.begin(), frame.rgb.end(), .5F);
    PhotoMergeOptions options;
    options.kind = PhotoMergeKind::kPanorama;
    auto disconnected = merge_photos(frames, options, {});
    ASSERT_FALSE(disconnected);
    EXPECT_EQ(disconnected.error().context.at("reason"), "merge_alignment_failed");
    options.kind = PhotoMergeKind::kHdr;
    options.auto_align = false;
    auto no_exposure = merge_photos(frames, options, {});
    ASSERT_FALSE(no_exposure);
    options.exposure_ev = {0, 0};
    EXPECT_FALSE(merge_photos(frames, options, {}));
    options.exposure_ev = {0, 1};
    frames[1].rgb.pop_back();
    EXPECT_FALSE(merge_photos(frames, options, {}));
}

TEST(PhotoMerge, DeghostingRejectsMovingPixelContribution)
{
    auto image = scene(64, 48);
    std::fill(image.rgb.begin(), image.rgb.end(), .3F);
    std::vector<LinearWorkingBuffer> frames{image, section(image, 0, 0, 64, 48, 2.F)};
    for (std::size_t y = 16; y < 32; ++y)
        for (std::size_t x = 16; x < 32; ++x)
            for (std::size_t c = 0; c < 3; ++c)
                frames[1].rgb[(y * 64 + x) * 3 + c] = .08F;
    PhotoMergeOptions options;
    options.auto_align = false;
    options.exposure_ev = {0, 1};
    auto result = merge_photos(frames, options, {});
    ASSERT_TRUE(result);
    EXPECT_GT(result.value().deghosted_pixels, 0);
    const auto pixel = (20 * 64 + 20) * 3;
    EXPECT_NEAR(result.value().image.rgb[pixel], .3 * std::exp2(.5), 1e-5);
}

TEST(PhotoMerge, CancellationMemoryAndNonfiniteFailExplicitly)
{
    std::vector<LinearWorkingBuffer> frames{scene(64, 48), scene(64, 48)};
    PhotoMergeOptions options;
    options.auto_align = false;
    options.exposure_ev = {0, 1};
    CancellationSource cancel;
    static_cast<void>(cancel.cancel("test"));
    auto result = merge_photos(frames, options, cancel.token());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::kCancelled);
    options.memory_budget_bytes = 64ULL * 1024 * 1024;
    auto bound = merge_photos(frames, options, {});
    ASSERT_FALSE(bound);
    EXPECT_EQ(bound.error().context.at("reason"), "merge_memory_budget_exceeded");
    options.memory_budget_bytes = 2ULL * 1024 * 1024 * 1024;
    frames[1].rgb[0] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(merge_photos(frames, options, {}));
}
} // namespace ravo
