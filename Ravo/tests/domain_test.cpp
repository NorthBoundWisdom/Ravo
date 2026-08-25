#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

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
    const auto directory =
        std::filesystem::temp_directory_path() / std::filesystem::path(u8"ravo-uri-unicode-安吉");
    std::filesystem::create_directories(directory);
    const auto file = directory / std::filesystem::path(u8"照片.arw");
    {
        std::ofstream output(file, std::ios::binary);
        output << "raw";
    }

    const auto utf8 = file.generic_u8string();
    const std::string path(reinterpret_cast<const char *>(utf8.data()), utf8.size());
    const auto normalized = normalize_local_input(path);
    ASSERT_TRUE(normalized) << normalized.error().message;
    const auto district = std::string_view(reinterpret_cast<const char *>(u8"安吉"));
    EXPECT_NE(normalized.value().uri.find(district), std::string::npos);
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

TEST(ReviewStateTest, ValidatesRatingAndParsesColorLabels)
{
    EXPECT_TRUE(validate_rating(0));
    EXPECT_TRUE(validate_rating(5));
    EXPECT_FALSE(validate_rating(-1));
    EXPECT_FALSE(validate_rating(6));
    auto red = parse_color_label("red");
    ASSERT_TRUE(red);
    EXPECT_EQ(red.value(), ColorLabel::kRed);
    EXPECT_EQ(color_label_name(ColorLabel::kPurple), "purple");
    EXPECT_FALSE(parse_color_label("orange"));
}

TEST(ExportFormatTest, ParsesNamesAndRejectsUnknownValues)
{
    auto jpeg = parse_export_format("jpg");
    ASSERT_TRUE(jpeg);
    EXPECT_EQ(jpeg.value(), ExportFormat::kJpeg);
    EXPECT_EQ(export_format_name(ExportFormat::kOriginalCopy), "original");
    EXPECT_EQ(export_format_extension(ExportFormat::kPng), ".png");
    EXPECT_TRUE(validate_jpeg_quality(90));
    EXPECT_FALSE(validate_jpeg_quality(0));
    EXPECT_FALSE(validate_jpeg_quality(101));
    EXPECT_FALSE(parse_export_format("heif"));
}

TEST(ReviewStateTest, FiltersAndSortsLibraryQuery)
{
    AssetRecord first;
    first.id = "ast_a";
    first.normalized_uri = "file:///photos/b.jpg";
    first.created_unix_ms = 10;
    first.review.rating = 5;
    first.review.color_label = ColorLabel::kBlue;
    AssetRecord second;
    second.id = "ast_b";
    second.normalized_uri = "file:///photos/a.jpg";
    second.created_unix_ms = 20;
    second.review.rating = 2;
    second.review.rejected = true;
    AssetRecord third;
    third.id = "ast_c";
    third.normalized_uri = "file:///photos/c.jpg";
    third.created_unix_ms = 30;
    third.review.rating = 5;
    third.review.color_label = ColorLabel::kRed;

    LibraryQuery min_rating;
    min_rating.rating_mode = RatingFilterMode::kMinimum;
    min_rating.rating_value = 5;
    auto filtered = filter_and_sort_assets({first, second, third}, min_rating);
    ASSERT_EQ(filtered.size(), 2U);
    EXPECT_EQ(filtered[0].id, "ast_c");
    EXPECT_EQ(filtered[1].id, "ast_a");

    LibraryQuery exclude_rejected;
    exclude_rejected.reject_filter = RejectFilter::kExclude;
    exclude_rejected.sort_field = AssetSortField::kDisplayName;
    exclude_rejected.sort_direction = SortDirection::kAscending;
    filtered = filter_and_sort_assets({first, second, third}, exclude_rejected);
    ASSERT_EQ(filtered.size(), 2U);
    EXPECT_EQ(filtered[0].id, "ast_a");
    EXPECT_EQ(filtered[1].id, "ast_c");

    LibraryQuery colors;
    colors.color_labels = {ColorLabel::kBlue};
    filtered = filter_and_sort_assets({first, second, third}, colors);
    ASSERT_EQ(filtered.size(), 1U);
    EXPECT_EQ(filtered[0].id, "ast_a");

    LibraryQuery folder;
    folder.folder_uri = "file:///photos";
    filtered = filter_and_sort_assets({first, second, third}, folder);
    EXPECT_EQ(filtered.size(), 3U);
}

[[nodiscard]] std::string utf8_text(const char8_t *text)
{
    const auto *bytes = reinterpret_cast<const char *>(text);
    return {bytes};
}

TEST(DomainUriTest, DisplayNameKeepsUtf8FolderNames)
{
    EXPECT_EQ(uri_display_name(utf8_text(u8"file:///Users/me/照片/测试.jpg")),
              utf8_text(u8"测试.jpg"));
    EXPECT_EQ(uri_parent(utf8_text(u8"file:///Users/me/照片/测试.jpg")),
              utf8_text(u8"file:///Users/me/照片"));
    EXPECT_EQ(uri_display_name(utf8_text(u8"file:///Users/me/照片")), utf8_text(u8"照片"));
}

TEST(LibraryFolderTest, BuildsTreeFromUtf8FolderNames)
{
    AssetRecord photo;
    photo.id = "ast_cjk";
    photo.normalized_uri = utf8_text(u8"file:///Users/me/照片/测试/a.jpg");
    const auto folders = library_folders({photo});
    bool saw_test = false;
    for (const auto &folder : folders)
    {
        if (folder.display_name == utf8_text(u8"测试"))
        {
            saw_test = true;
            EXPECT_EQ(folder.asset_count, 1);
        }
    }
    EXPECT_TRUE(saw_test);
}

TEST(LibraryFolderTest, BuildsTreeFromImportedAssetUris)
{
    AssetRecord trip;
    trip.id = "ast_trip";
    trip.normalized_uri = "file:///Users/me/Photos/2024/Trip/a.jpg";
    AssetRecord home;
    home.id = "ast_home";
    home.normalized_uri = "file:///Users/me/Photos/2024/Home/b.jpg";
    const auto folders = library_folders({trip, home});
    ASSERT_GE(folders.size(), 3U);
    EXPECT_TRUE(folders.front().uri.empty());
    EXPECT_EQ(folders.front().display_name, "All Photographs");
    EXPECT_EQ(folders.front().asset_count, 2);

    bool saw_trip = false;
    bool saw_home = false;
    for (const auto &folder : folders)
    {
        if (folder.uri.ends_with("/Trip"))
        {
            saw_trip = true;
            EXPECT_EQ(folder.asset_count, 1);
            EXPECT_EQ(folder.display_name, "Trip");
        }
        if (folder.uri.ends_with("/Home"))
        {
            saw_home = true;
            EXPECT_EQ(folder.asset_count, 1);
        }
    }
    EXPECT_TRUE(saw_trip);
    EXPECT_TRUE(saw_home);

    LibraryQuery query;
    query.folder_uri = "file:///Users/me/Photos/2024/Trip";
    auto filtered = filter_and_sort_assets({trip, home}, query);
    ASSERT_EQ(filtered.size(), 1U);
    EXPECT_EQ(filtered.front().id, "ast_trip");
}

TEST(DomainTypesTest, NormalizesUnicodeTagsAndFiltersAssets)
{
    auto parsed = parse_tag_list("  风景, archive, 风景 ");
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_EQ(parsed.value().size(), 2U);
    EXPECT_EQ(parsed.value().front(), "风景");
    EXPECT_EQ(parsed.value().back(), "archive");
    auto empty = normalize_tag_name("   ");
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code, ErrorCode::kValidation);

    AssetRecord tagged;
    tagged.id = "ast_tag";
    tagged.tags = {"风景"};
    LibraryQuery query;
    query.tag = "风景";
    EXPECT_TRUE(asset_matches_query(tagged, query));
    query.tag = "archive";
    EXPECT_FALSE(asset_matches_query(tagged, query));
}

} // namespace
} // namespace ravo
