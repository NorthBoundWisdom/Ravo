#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "ravo/domain/types.h"
#include "ravo/domain/uri.h"

namespace ravo
{
namespace
{

TEST(DomainTypesTest, FitsTheLongEdgeAndKeepsSmallImagesUnchanged)
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    fit_within_max_edge(100, 50, 1600, width, height);
    EXPECT_EQ(width, 100U);
    EXPECT_EQ(height, 50U);
    fit_within_max_edge(4000, 2000, 1000, width, height);
    EXPECT_EQ(width, 1000U);
    EXPECT_EQ(height, 500U);
}

TEST(DomainUriTest, NormalizesAbsoluteAndRelativePathsToTheSameFileUri)
{
    const auto directory = std::filesystem::temp_directory_path() / "ravo-uri-test";
    std::filesystem::create_directories(directory);
    const auto file = directory / "photo.png";
    {
        std::ofstream output(file, std::ios::binary);
        output << "png";
    }

    const auto absolute = normalize_local_input(file.string());
    ASSERT_TRUE(absolute) << absolute.error().message;
    EXPECT_TRUE(absolute.value().uri.starts_with("file://"));
    EXPECT_FALSE(absolute.value().path.empty());

    const auto current = std::filesystem::current_path();
    std::filesystem::current_path(directory);
    const auto relative = normalize_local_input("photo.png");
    std::filesystem::current_path(current);
    ASSERT_TRUE(relative) << relative.error().message;
    EXPECT_EQ(relative.value().uri, absolute.value().uri);

    const auto identity = read_file_identity(absolute.value().path);
    ASSERT_TRUE(identity) << identity.error().message;
    EXPECT_GT(identity.value().size_bytes, 0U);
    std::filesystem::remove_all(directory);
}

TEST(DomainUriTest, RoundTripsNonAsciiPathsThroughFileUris)
{
    const auto directory = std::filesystem::temp_directory_path() / "ravo-uri-unicode-安吉";
    std::filesystem::create_directories(directory);
    const auto file = directory / "照片.arw";
    {
        std::ofstream output(file, std::ios::binary);
        output << "raw";
    }

    const auto utf8 = file.generic_u8string();
    const std::string path(reinterpret_cast<const char *>(utf8.data()), utf8.size());
    const auto normalized = normalize_local_input(path);
    ASSERT_TRUE(normalized) << normalized.error().message;
    EXPECT_NE(normalized.value().uri.find("安吉"), std::string::npos);
    EXPECT_EQ(normalized.value().uri.find('%'), std::string::npos);

    const auto round_trip = normalize_local_input(normalized.value().uri);
    ASSERT_TRUE(round_trip) << round_trip.error().message;
    EXPECT_EQ(round_trip.value().path, normalized.value().path);
    const auto identity = read_file_identity(round_trip.value().path);
    ASSERT_TRUE(identity) << identity.error().message;
    EXPECT_EQ(identity.value().size_bytes, 3U);
    std::filesystem::remove_all(directory);
}

TEST(DomainUriTest, EncodesSpacesButKeepsUtf8FileNames)
{
    const auto directory = std::filesystem::temp_directory_path() / "ravo uri space";
    std::filesystem::create_directories(directory);
    const auto file = directory / "a photo.png";
    {
        std::ofstream output(file, std::ios::binary);
        output << "png";
    }
    const auto utf8 = file.generic_u8string();
    const std::string path(reinterpret_cast<const char *>(utf8.data()), utf8.size());
    const auto normalized = normalize_local_input(path);
    ASSERT_TRUE(normalized) << normalized.error().message;
    EXPECT_NE(normalized.value().uri.find("%20"), std::string::npos);
    const auto round_trip = normalize_local_input(normalized.value().uri);
    ASSERT_TRUE(round_trip) << round_trip.error().message;
    EXPECT_EQ(round_trip.value().path, normalized.value().path);
    std::filesystem::remove_all(directory);
}

} // namespace
} // namespace ravo
