#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/color.h"
#include "ravo/services/image_artifact_verification.h"

namespace ravo
{
namespace
{

[[nodiscard]] std::vector<std::uint8_t> solid_rgb(const std::uint32_t width,
                                                  const std::uint32_t height, const std::uint8_t r,
                                                  const std::uint8_t g, const std::uint8_t b)
{
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) * height * 3U);
    for (std::size_t i = 0; i < rgb.size(); i += 3U)
    {
        rgb[i] = r;
        rgb[i + 1U] = g;
        rgb[i + 2U] = b;
    }
    return rgb;
}

[[nodiscard]] ColorProfileState builtin_srgb()
{
    ColorProfileState profile;
    profile.kind = ColorProfileKind::kBuiltin;
    profile.model = ColorModel::kRgb;
    profile.identifier = "srgb";
    return profile;
}

TEST(ImageArtifactVerification, VerifiesEncodedPngThroughSharedRasterOwner)
{
    QtRasterDecoder decoder;
    ColorProfileState profile = builtin_srgb();
    auto encoded =
        decoder.encode(4, 3, solid_rgb(4, 3, 10, 20, 30), profile, ExportFormat::kPng, {}, {}, {});
    ASSERT_TRUE(encoded) << encoded.error().message;

    ImageArtifactExpectation expected;
    expected.mime_type = "image/png";
    expected.width = 4;
    expected.height = 3;
    auto verified = verify_encoded_image_artifact(decoder, encoded.value(), expected, {});
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_EQ(verified.value().type, "ravo.image_artifact");
    EXPECT_EQ(verified.value().version, 1);
    EXPECT_EQ(verified.value().mime_type, "image/png");
    EXPECT_EQ(verified.value().width, 4U);
    EXPECT_EQ(verified.value().height, 3U);
    EXPECT_FALSE(verified.value().color_profile.empty());
    EXPECT_FALSE(verified.value().color_profile_fingerprint.empty());
    EXPECT_EQ(verified.value().byte_count, encoded.value().size());
    EXPECT_EQ(verified.value().content_sha256.size(), 64U);
}

TEST(ImageArtifactVerification, RejectsBudgetOverflowBeforeDecode)
{
    QtRasterDecoder decoder;
    std::vector<std::uint8_t> encoded(128, 0x89);
    ImageArtifactExpectation expected;
    expected.max_encoded_bytes = 64;
    auto verified = verify_encoded_image_artifact(decoder, encoded, expected, {});
    ASSERT_FALSE(verified);
    EXPECT_EQ(verified.error().context.at("reason"), "encoded_budget_exceeded");
}

TEST(ImageArtifactVerification, RejectsDimensionMismatch)
{
    QtRasterDecoder decoder;
    ColorProfileState profile = builtin_srgb();
    auto encoded =
        decoder.encode(2, 2, solid_rgb(2, 2, 1, 2, 3), profile, ExportFormat::kPng, {}, {}, {});
    ASSERT_TRUE(encoded) << encoded.error().message;
    ImageArtifactExpectation expected;
    expected.width = 9;
    expected.height = 2;
    auto verified = verify_encoded_image_artifact(decoder, encoded.value(), expected, {});
    ASSERT_FALSE(verified);
    EXPECT_EQ(verified.error().context.at("reason"), "width_mismatch");
}

TEST(ImageArtifactVerification, RejectsTruncatedPng)
{
    QtRasterDecoder decoder;
    ColorProfileState profile = builtin_srgb();
    auto encoded =
        decoder.encode(2, 2, solid_rgb(2, 2, 7, 8, 9), profile, ExportFormat::kPng, {}, {}, {});
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto truncated = encoded.value();
    truncated.resize(truncated.size() / 2U);
    ImageArtifactExpectation expected;
    auto verified = verify_encoded_image_artifact(decoder, truncated, expected, {});
    ASSERT_FALSE(verified);
    EXPECT_EQ(verified.error().context.at("reason"), "artifact_decode_failed");
}

TEST(ImageArtifactVerification, RejectsColorProfileMismatch)
{
    QtRasterDecoder decoder;
    ColorProfileState profile = builtin_srgb();
    auto encoded =
        decoder.encode(2, 2, solid_rgb(2, 2, 1, 2, 3), profile, ExportFormat::kPng, {}, {}, {});
    ASSERT_TRUE(encoded) << encoded.error().message;
    ImageArtifactExpectation expected;
    expected.color_profile = "display_p3";
    auto verified = verify_encoded_image_artifact(decoder, encoded.value(), expected, {});
    ASSERT_FALSE(verified);
    EXPECT_EQ(verified.error().context.at("reason"), "color_profile_mismatch");
}

TEST(ImageArtifactVerification, HonorsCancellation)
{
    QtRasterDecoder decoder;
    ColorProfileState profile = builtin_srgb();
    auto encoded =
        decoder.encode(2, 2, solid_rgb(2, 2, 1, 1, 1), profile, ExportFormat::kPng, {}, {}, {});
    ASSERT_TRUE(encoded) << encoded.error().message;
    CancellationSource source;
    ASSERT_TRUE(source.cancel("test"));
    ImageArtifactExpectation expected;
    auto verified =
        verify_encoded_image_artifact(decoder, encoded.value(), expected, source.token());
    ASSERT_FALSE(verified);
}

} // namespace
} // namespace ravo
