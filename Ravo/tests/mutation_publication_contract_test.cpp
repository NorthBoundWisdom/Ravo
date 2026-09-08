#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "catalog_internal.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/color.h"
#include "ravo/services/artifact_publication.h"
#include "ravo/services/image_artifact_verification.h"

namespace ravo
{
namespace
{

class ContractTempDirectory
{
public:
    ContractTempDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("ravo-mutation-contract-" + generate_catalog_id()))
    {
        std::filesystem::create_directories(path_);
    }

    ~ContractTempDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] ColorProfileState builtin_srgb()
{
    ColorProfileState profile;
    profile.kind = ColorProfileKind::kBuiltin;
    profile.model = ColorModel::kRgb;
    profile.identifier = "srgb";
    return profile;
}

[[nodiscard]] std::vector<std::uint8_t> solid_rgb(const std::uint32_t width,
                                                  const std::uint32_t height, const std::uint8_t r,
                                                  const std::uint8_t g, const std::uint8_t b)
{
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(width) * height * 3U);
    for (std::size_t i = 0; i < bytes.size(); i += 3)
    {
        bytes[i] = r;
        bytes[i + 1] = g;
        bytes[i + 2] = b;
    }
    return bytes;
}

TEST(MutationPublicationContract, AtomicPublicationDoesNotReplaceExistingDestination)
{
    ContractTempDirectory temp;
    const auto destination = temp.path() / "artifact.bin";
    {
        std::ofstream out(destination, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out);
        const std::string prior = "prior-bytes";
        out.write(prior.data(), static_cast<std::streamsize>(prior.size()));
    }
    const std::vector<std::uint8_t> payload{'n', 'e', 'w'};
    const auto published =
        publish_bytes_artifact_no_replace(destination.string(), payload, CancellationToken{});
    ASSERT_FALSE(published);
    EXPECT_EQ(published.error().code, ErrorCode::kConflict);
    std::ifstream in(destination, std::ios::binary);
    ASSERT_TRUE(in);
    const std::string kept{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    EXPECT_EQ(kept, "prior-bytes");
}

TEST(MutationPublicationContract, CancelBeforePublishLeavesNoDestination)
{
    ContractTempDirectory temp;
    const auto destination = temp.path() / "cancelled.bin";
    CancellationSource cancel;
    ASSERT_TRUE(cancel.cancel("cancel-before-publish"));
    const std::vector<std::uint8_t> payload(4096, 0x5A);
    const auto published =
        publish_bytes_artifact_no_replace(destination.string(), payload, cancel.token());
    ASSERT_FALSE(published);
    EXPECT_EQ(published.error().code, ErrorCode::kCancelled);
    EXPECT_FALSE(std::filesystem::exists(destination));
}

TEST(MutationPublicationContract, ArtifactVerificationReusesSharedRasterOwner)
{
    QtRasterDecoder decoder;
    auto encoded = decoder.encode(2, 2, solid_rgb(2, 2, 1, 2, 3), builtin_srgb(),
                                  ExportFormat::kPng, {}, {}, {});
    ASSERT_TRUE(encoded) << encoded.error().message;
    ImageArtifactExpectation expected;
    expected.mime_type = "image/png";
    expected.width = 2;
    expected.height = 2;
    auto verified = verify_encoded_image_artifact(decoder, encoded.value(), expected, {});
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_EQ(verified.value().content_sha256.size(), 64U);
}

TEST(MutationPublicationContract, SourceIdentityStableForIdenticalPayload)
{
    QtRasterDecoder decoder;
    auto first = decoder.encode(2, 2, solid_rgb(2, 2, 9, 8, 7), builtin_srgb(), ExportFormat::kPng,
                                {}, {}, {});
    auto second = decoder.encode(2, 2, solid_rgb(2, 2, 9, 8, 7), builtin_srgb(), ExportFormat::kPng,
                                 {}, {}, {});
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ImageArtifactExpectation expected;
    expected.mime_type = "image/png";
    auto a = verify_encoded_image_artifact(decoder, first.value(), expected, {});
    auto b = verify_encoded_image_artifact(decoder, second.value(), expected, {});
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_EQ(a.value().content_sha256, b.value().content_sha256);
}

} // namespace
} // namespace ravo
