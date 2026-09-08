#include <gtest/gtest.h>

#include <QByteArray>
#include <QColorSpace>

#include <array>
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

[[nodiscard]] ColorProfileState builtin_named(const std::string_view identifier)
{
    ColorProfileState profile;
    profile.kind = ColorProfileKind::kBuiltin;
    profile.model = ColorModel::kRgb;
    profile.identifier = std::string(identifier);
    return profile;
}

[[nodiscard]] ColorProfileState embedded_icc_from_qt(const QColorSpace &space)
{
    const QByteArray bytes = space.iccProfile();
    ColorProfileState profile;
    profile.kind = ColorProfileKind::kIcc;
    profile.model = ColorModel::kRgb;
    profile.identifier = "embedded_icc";
    profile.icc_bytes.assign(reinterpret_cast<const std::uint8_t *>(bytes.constData()),
                             reinterpret_cast<const std::uint8_t *>(bytes.constData()) +
                                 bytes.size());
    return profile;
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

TEST(ImageArtifactVerification, RejectsPngWhenJpegExpected)
{
    QtRasterDecoder decoder;
    ColorProfileState profile = builtin_srgb();
    auto encoded =
        decoder.encode(2, 2, solid_rgb(2, 2, 9, 8, 7), profile, ExportFormat::kPng, {}, {}, {});
    ASSERT_TRUE(encoded) << encoded.error().message;
    ImageArtifactExpectation expected;
    expected.mime_type = "image/jpeg";
    expected.width = 2;
    expected.height = 2;
    auto verified = verify_encoded_image_artifact(decoder, encoded.value(), expected, {});
    ASSERT_FALSE(verified);
    EXPECT_EQ(verified.error().context.at("reason"), "mime_type_mismatch");
    EXPECT_EQ(verified.error().context.at("actual"), "image/png");
    EXPECT_EQ(verified.error().context.at("expected"), "image/jpeg");
}

TEST(ImageArtifactVerification, RejectsJpegWhenPngExpected)
{
    QtRasterDecoder decoder;
    ColorProfileState profile = builtin_srgb();
    JpegExportOptions jpeg;
    jpeg.quality = 90;
    auto encoded =
        decoder.encode(2, 2, solid_rgb(2, 2, 3, 4, 5), profile, ExportFormat::kJpeg, jpeg, {}, {});
    ASSERT_TRUE(encoded) << encoded.error().message;
    ImageArtifactExpectation expected;
    expected.mime_type = "image/png";
    expected.width = 2;
    expected.height = 2;
    auto verified = verify_encoded_image_artifact(decoder, encoded.value(), expected, {});
    ASSERT_FALSE(verified);
    EXPECT_EQ(verified.error().context.at("reason"), "mime_type_mismatch");
    EXPECT_EQ(verified.error().context.at("actual"), "image/jpeg");
}

TEST(ImageArtifactVerification, ReportsActualMediaTypeFromContent)
{
    QtRasterDecoder decoder;
    ColorProfileState profile = builtin_srgb();
    auto encoded =
        decoder.encode(3, 2, solid_rgb(3, 2, 1, 2, 3), profile, ExportFormat::kPng, {}, {}, {});
    ASSERT_TRUE(encoded) << encoded.error().message;
    // Extension-style disguise must not affect content recognition.
    ImageArtifactExpectation expected;
    expected.mime_type = "image/png";
    auto verified = verify_encoded_image_artifact(decoder, encoded.value(), expected, {});
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_EQ(verified.value().mime_type, "image/png");
}

TEST(ImageArtifactVerification, AcceptsBuiltinSrgbLinearRec709AndDisplayP3RoundTrips)
{
    QtRasterDecoder decoder;
    struct Case
    {
        const char *identifier;
    };
    const std::array cases{Case{"srgb"}, Case{"linear_rec709"}, Case{"display_p3"}};
    for (const auto &test_case : cases)
    {
        ColorProfileState profile = builtin_named(test_case.identifier);
        auto encoded = decoder.encode(4, 2, solid_rgb(4, 2, 11, 22, 33), profile,
                                      ExportFormat::kPng, {}, {}, {});
        ASSERT_TRUE(encoded) << test_case.identifier << ": " << encoded.error().message;
        auto decoded = decoder.decode_memory(encoded.value(), 0U, {});
        ASSERT_TRUE(decoded) << test_case.identifier << ": " << decoded.error().message;
        ImageArtifactExpectation expected;
        expected.mime_type = "image/png";
        expected.width = 4;
        expected.height = 2;
        expected.color_profile = decoded.value().color_profile.identifier;
        const auto fingerprint = color_profile_fingerprint(decoded.value().color_profile);
        expected.color_profile_fingerprint = fingerprint;
        auto verified = verify_encoded_image_artifact(decoder, encoded.value(), expected, {});
        ASSERT_TRUE(verified) << test_case.identifier << ": " << verified.error().message;
        EXPECT_EQ(verified.value().color_profile, decoded.value().color_profile.identifier);
        EXPECT_EQ(verified.value().color_profile_fingerprint, fingerprint);
    }
}

TEST(ImageArtifactVerification, AcceptsEmbeddedSrgbIccIdentityWithoutConfusingBuiltinDescriptor)
{
    QtRasterDecoder decoder;
    ColorProfileState embedded = embedded_icc_from_qt(QColorSpace(QColorSpace::SRgb));
    ASSERT_FALSE(embedded.icc_bytes.empty());
    auto encoded =
        decoder.encode(2, 2, solid_rgb(2, 2, 4, 5, 6), embedded, ExportFormat::kPng, {}, {}, {});
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto decoded = decoder.decode_memory(encoded.value(), 0U, {});
    ASSERT_TRUE(decoded) << decoded.error().message;
    // Exact decoded identity may be embedded_icc or a recognized builtin depending on
    // accompanying cICP; bind through the color owner fingerprint either way.
    ImageArtifactExpectation expected;
    expected.mime_type = "image/png";
    expected.color_profile = decoded.value().color_profile.identifier;
    const auto fingerprint = color_profile_fingerprint(decoded.value().color_profile);
    expected.color_profile_fingerprint = fingerprint;
    auto verified = verify_encoded_image_artifact(decoder, encoded.value(), expected, {});
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_EQ(verified.value().color_profile_fingerprint, fingerprint);
    // Builtin srgb fingerprint must not silently accept a different decoded identity.
    ColorProfileState builtin = builtin_named("srgb");
    expected.color_profile = "srgb";
    expected.color_profile_fingerprint = color_profile_fingerprint(builtin);
    if (decoded.value().color_profile.identifier != "srgb" ||
        color_profile_fingerprint(decoded.value().color_profile) !=
            color_profile_fingerprint(builtin))
    {
        auto rejected = verify_encoded_image_artifact(decoder, encoded.value(), expected, {});
        ASSERT_FALSE(rejected);
        EXPECT_TRUE(rejected.error().context.at("reason") == "color_profile_mismatch" ||
                    rejected.error().context.at("reason") == "color_profile_fingerprint_mismatch");
    }
}

TEST(ImageArtifactVerification, RejectsSamePrimariesDifferentTransferExpectation)
{
    QtRasterDecoder decoder;
    ColorProfileState linear = builtin_named("linear_rec709");
    auto encoded =
        decoder.encode(2, 2, solid_rgb(2, 2, 7, 8, 9), linear, ExportFormat::kPng, {}, {}, {});
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto decoded = decoder.decode_memory(encoded.value(), 0U, {});
    ASSERT_TRUE(decoded) << decoded.error().message;
    // Encoded linear Rec.709 must not satisfy an sRGB TRC expectation, whether the
    // adapter reports linear_rec709 or an embedded ICC carrying that transfer.
    EXPECT_NE(decoded.value().color_profile.identifier, "srgb");
    EXPECT_NE(color_profile_fingerprint(decoded.value().color_profile),
              color_profile_fingerprint(builtin_named("srgb")));
    ImageArtifactExpectation expected;
    expected.mime_type = "image/png";
    expected.color_profile = "srgb";
    expected.color_profile_fingerprint = color_profile_fingerprint(builtin_named("srgb"));
    auto verified = verify_encoded_image_artifact(decoder, encoded.value(), expected, {});
    ASSERT_FALSE(verified);
    EXPECT_TRUE(verified.error().context.at("reason") == "color_profile_mismatch" ||
                verified.error().context.at("reason") == "color_profile_fingerprint_mismatch");
    EXPECT_NE(verified.error().context.at("actual"), "srgb");
}

TEST(ImageArtifactVerification, RejectsWrongColorFingerprintEvenWhenIdentifierMatches)
{
    QtRasterDecoder decoder;
    // Same public identifier ("embedded_icc") with different ICC payloads must yield
    // different fingerprints. Do not rely on builtin display_p3 vs srgb round-trips:
    // some platforms remapped those to identical fingerprints and broke ASSERT_NE.
    ColorProfileState encoded_profile = embedded_icc_from_qt(QColorSpace(QColorSpace::DisplayP3));
    ColorProfileState wrong_same_identifier = embedded_icc_from_qt(QColorSpace(QColorSpace::SRgb));
    ASSERT_FALSE(encoded_profile.icc_bytes.empty());
    ASSERT_FALSE(wrong_same_identifier.icc_bytes.empty());
    ASSERT_EQ(encoded_profile.identifier, wrong_same_identifier.identifier);
    ASSERT_EQ(encoded_profile.identifier, "embedded_icc");
    ASSERT_NE(color_profile_fingerprint(encoded_profile),
              color_profile_fingerprint(wrong_same_identifier));

    auto encoded = decoder.encode(2, 2, solid_rgb(2, 2, 1, 2, 3), encoded_profile,
                                  ExportFormat::kPng, {}, {}, {});
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto decoded = decoder.decode_memory(encoded.value(), 0U, {});
    ASSERT_TRUE(decoded) << decoded.error().message;

    ImageArtifactExpectation expected;
    expected.mime_type = "image/png";
    expected.color_profile = decoded.value().color_profile.identifier;
    const auto actual_fingerprint = color_profile_fingerprint(decoded.value().color_profile);
    if (decoded.value().color_profile.identifier == "embedded_icc")
    {
        // Identifier matches wrong_same_identifier; fingerprint must still differ.
        expected.color_profile_fingerprint = color_profile_fingerprint(wrong_same_identifier);
    }
    else
    {
        // Decoder remapped to a named builtin — keep that identifier but spoof a
        // fingerprint that cannot equal the decoded owner (matrix perturbation).
        ColorProfileState spoof;
        spoof.kind = decoded.value().color_profile.kind;
        spoof.model = decoded.value().color_profile.model;
        spoof.identifier = decoded.value().color_profile.identifier;
        spoof.icc_bytes = decoded.value().color_profile.icc_bytes;
        spoof.has_matrix = true;
        spoof.matrix_to_xyz_d50 = {0.5F, 0.0F, 0.0F, 0.0F, 0.5F, 0.0F, 0.0F, 0.0F, 0.5F};
        expected.color_profile_fingerprint = color_profile_fingerprint(spoof);
        if (expected.color_profile_fingerprint == actual_fingerprint)
        {
            spoof.matrix_to_xyz_d50[0] = 0.25F;
            expected.color_profile_fingerprint = color_profile_fingerprint(spoof);
        }
    }
    ASSERT_NE(expected.color_profile_fingerprint, actual_fingerprint)
        << "fixture must keep the same identifier with a different fingerprint";
    auto verified = verify_encoded_image_artifact(decoder, encoded.value(), expected, {});
    ASSERT_FALSE(verified);
    EXPECT_EQ(verified.error().context.at("reason"), "color_profile_fingerprint_mismatch");
}

} // namespace
} // namespace ravo
