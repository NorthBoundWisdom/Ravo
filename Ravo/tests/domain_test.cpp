#include <array>
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

TEST(DomainTypesTest, PreviewContractInvalidatesPreSliderCorrectionCaches)
{
    EXPECT_EQ(kPreviewContractVersion, 7);
    EXPECT_TRUE(make_preview_cache_key("asset", 640, 480, "fingerprint", "recipe")
                    .starts_with("v7_asset_640x480_fingerprint_recipe"));
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
        std::filesystem::temp_directory_path() / std::filesystem::path(u8"ravo-uri-unicode-café");
    std::filesystem::create_directories(directory);
    const auto file = directory / std::filesystem::path(u8"photo.arw");
    {
        std::ofstream output(file, std::ios::binary);
        output << "raw";
    }

    const auto utf8 = file.generic_u8string();
    const std::string path(reinterpret_cast<const char *>(utf8.data()), utf8.size());
    const auto normalized = normalize_local_input(path);
    ASSERT_TRUE(normalized) << normalized.error().message;
    const auto district = std::string_view(reinterpret_cast<const char *>(u8"café"));
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
    EXPECT_FALSE(parse_export_format("heif"));
}

TEST(ExportFormatTest, OwnsTypedJpegOptionsAndRejectsInvalidValues)
{
    const JpegExportOptions defaults;
    EXPECT_EQ(defaults.quality, 95);
    EXPECT_EQ(defaults.subsampling, JpegSubsampling::kAuto);
    EXPECT_TRUE(validate_jpeg_export_options(defaults));

    struct SubsamplingExpectation
    {
        JpegSubsampling value = JpegSubsampling::kAuto;
        std::string_view name;
        std::uint8_t wire_value = 0U;
    };
    constexpr std::array<SubsamplingExpectation, 5U> kExpectations{{
        {JpegSubsampling::kAuto, "auto", 0U},
        {JpegSubsampling::k444, "444", 1U},
        {JpegSubsampling::k440, "440", 2U},
        {JpegSubsampling::k422, "422", 3U},
        {JpegSubsampling::k420, "420", 4U},
    }};
    for (const SubsamplingExpectation &expectation : kExpectations)
    {
        EXPECT_EQ(static_cast<std::uint8_t>(expectation.value), expectation.wire_value);
        EXPECT_EQ(jpeg_subsampling_name(expectation.value), expectation.name);
        const auto parsed = parse_jpeg_subsampling(expectation.name);
        ASSERT_TRUE(parsed) << parsed.error().message;
        EXPECT_EQ(parsed.value(), expectation.value);
    }

    JpegExportOptions minimum{5, JpegSubsampling::k420};
    JpegExportOptions maximum{100, JpegSubsampling::k444};
    EXPECT_TRUE(validate_jpeg_export_options(minimum));
    EXPECT_TRUE(validate_jpeg_export_options(maximum));
    EXPECT_EQ(minimum, JpegExportOptions(5, JpegSubsampling::k420));

    JpegExportOptions below_minimum{4, JpegSubsampling::kAuto};
    const auto invalid_quality = validate_jpeg_export_options(below_minimum);
    ASSERT_FALSE(invalid_quality);
    EXPECT_EQ(invalid_quality.error().code, ErrorCode::kValidation);
    EXPECT_EQ(invalid_quality.error().context.at("format"), "jpeg");
    EXPECT_EQ(invalid_quality.error().context.at("reason"), "invalid_jpeg_quality");
    EXPECT_EQ(invalid_quality.error().context.at("quality"), "4");
    EXPECT_EQ(invalid_quality.error().context.at("minimum"), "5");
    EXPECT_EQ(invalid_quality.error().context.at("maximum"), "100");
    JpegExportOptions above_maximum{101, JpegSubsampling::kAuto};
    EXPECT_FALSE(validate_jpeg_export_options(above_maximum));

    JpegExportOptions invalid_subsampling{
        95, static_cast<JpegSubsampling>(static_cast<std::uint8_t>(5U))};
    const auto invalid_mode = validate_jpeg_export_options(invalid_subsampling);
    ASSERT_FALSE(invalid_mode);
    EXPECT_EQ(invalid_mode.error().code, ErrorCode::kValidation);
    EXPECT_EQ(invalid_mode.error().context.at("format"), "jpeg");
    EXPECT_EQ(invalid_mode.error().context.at("reason"), "invalid_jpeg_subsampling");
    EXPECT_EQ(invalid_mode.error().context.at("subsampling"), "5");
    EXPECT_EQ(jpeg_subsampling_name(invalid_subsampling.subsampling), "unknown");
    const auto noncanonical = parse_jpeg_subsampling("4:2:2");
    ASSERT_FALSE(noncanonical);
    EXPECT_EQ(noncanonical.error().code, ErrorCode::kValidation);
    EXPECT_EQ(noncanonical.error().context.at("format"), "jpeg");
    EXPECT_EQ(noncanonical.error().context.at("reason"), "invalid_jpeg_subsampling");
    EXPECT_EQ(noncanonical.error().context.at("subsampling"), "4:2:2");
}

TEST(ExportFormatTest, OwnsTypedPngOptionsAndRejectsInvalidValues)
{
    const PngExportOptions defaults;
    EXPECT_EQ(defaults.bit_depth, PngBitDepth::k8);
    EXPECT_EQ(defaults.compression, 5);
    EXPECT_TRUE(validate_png_export_options(defaults));

    struct BitDepthExpectation
    {
        PngBitDepth value = PngBitDepth::k8;
        std::string_view name;
        std::uint8_t wire_value = 0U;
    };
    constexpr std::array<BitDepthExpectation, 2U> kExpectations{{
        {PngBitDepth::k8, "8", 8U},
        {PngBitDepth::k16, "16", 16U},
    }};
    for (const BitDepthExpectation &expectation : kExpectations)
    {
        EXPECT_EQ(static_cast<std::uint8_t>(expectation.value), expectation.wire_value);
        EXPECT_EQ(png_bit_depth_name(expectation.value), expectation.name);
        const auto parsed = parse_png_bit_depth(expectation.name);
        ASSERT_TRUE(parsed) << parsed.error().message;
        EXPECT_EQ(parsed.value(), expectation.value);
    }

    EXPECT_TRUE(validate_png_export_options({PngBitDepth::k8, 0}));
    EXPECT_TRUE(validate_png_export_options({PngBitDepth::k16, 9}));

    const PngExportOptions invalid_depth{static_cast<PngBitDepth>(255U), 5};
    const auto invalid_depth_result = validate_png_export_options(invalid_depth);
    ASSERT_FALSE(invalid_depth_result);
    EXPECT_EQ(invalid_depth_result.error().code, ErrorCode::kValidation);
    EXPECT_EQ(invalid_depth_result.error().context.at("format"), "png");
    EXPECT_EQ(invalid_depth_result.error().context.at("reason"), "invalid_png_bit_depth");
    EXPECT_EQ(invalid_depth_result.error().context.at("bit_depth"), "255");
    EXPECT_EQ(png_bit_depth_name(invalid_depth.bit_depth), "unknown");

    for (const int compression : {-1, 10})
    {
        const auto invalid_compression =
            validate_png_export_options({PngBitDepth::k8, compression});
        ASSERT_FALSE(invalid_compression);
        EXPECT_EQ(invalid_compression.error().code, ErrorCode::kValidation);
        EXPECT_EQ(invalid_compression.error().context.at("format"), "png");
        EXPECT_EQ(invalid_compression.error().context.at("reason"), "invalid_png_compression");
        EXPECT_EQ(invalid_compression.error().context.at("compression"),
                  std::to_string(compression));
        EXPECT_EQ(invalid_compression.error().context.at("minimum"), "0");
        EXPECT_EQ(invalid_compression.error().context.at("maximum"), "9");
    }

    const auto noncanonical = parse_png_bit_depth("8-bit");
    ASSERT_FALSE(noncanonical);
    EXPECT_EQ(noncanonical.error().context.at("format"), "png");
    EXPECT_EQ(noncanonical.error().context.at("reason"), "invalid_png_bit_depth");
    EXPECT_EQ(noncanonical.error().context.at("bit_depth"), "8-bit");
}

TEST(ExportFormatTest, OwnsTypedTiffOptionsAndRejectsInvalidValues)
{
    const TiffExportOptions defaults;
    EXPECT_EQ(defaults.sample_type, TiffSampleType::kUint8);
    EXPECT_EQ(defaults.compression, TiffCompression::kDeflatePredictor);
    EXPECT_EQ(defaults.compression_level, 6);
    EXPECT_FALSE(defaults.grayscale_if_neutral);
    EXPECT_TRUE(validate_tiff_export_options(defaults));

    struct SampleTypeExpectation
    {
        TiffSampleType value = TiffSampleType::kUint8;
        std::string_view name;
        std::uint8_t wire_value = 0U;
    };
    constexpr std::array<SampleTypeExpectation, 4U> kSampleTypes{{
        {TiffSampleType::kUint8, "uint8", 0U},
        {TiffSampleType::kUint16, "uint16", 1U},
        {TiffSampleType::kFloat16, "float16", 2U},
        {TiffSampleType::kFloat32, "float32", 3U},
    }};
    for (const SampleTypeExpectation &expectation : kSampleTypes)
    {
        EXPECT_EQ(static_cast<std::uint8_t>(expectation.value), expectation.wire_value);
        EXPECT_EQ(tiff_sample_type_name(expectation.value), expectation.name);
        const auto parsed = parse_tiff_sample_type(expectation.name);
        ASSERT_TRUE(parsed) << parsed.error().message;
        EXPECT_EQ(parsed.value(), expectation.value);
        TiffExportOptions options;
        options.sample_type = expectation.value;
        EXPECT_TRUE(validate_tiff_export_options(options));
    }

    struct CompressionExpectation
    {
        TiffCompression value = TiffCompression::kNone;
        std::string_view name;
        std::uint8_t wire_value = 0U;
    };
    constexpr std::array<CompressionExpectation, 3U> kCompressions{{
        {TiffCompression::kNone, "none", 0U},
        {TiffCompression::kDeflate, "deflate", 1U},
        {TiffCompression::kDeflatePredictor, "deflate_predictor", 2U},
    }};
    for (const CompressionExpectation &expectation : kCompressions)
    {
        EXPECT_EQ(static_cast<std::uint8_t>(expectation.value), expectation.wire_value);
        EXPECT_EQ(tiff_compression_name(expectation.value), expectation.name);
        const auto parsed = parse_tiff_compression(expectation.name);
        ASSERT_TRUE(parsed) << parsed.error().message;
        EXPECT_EQ(parsed.value(), expectation.value);
        TiffExportOptions options;
        options.compression = expectation.value;
        EXPECT_TRUE(validate_tiff_export_options(options));
    }

    TiffExportOptions minimum;
    minimum.compression_level = 1;
    EXPECT_TRUE(validate_tiff_export_options(minimum));
    TiffExportOptions maximum;
    maximum.compression_level = 9;
    maximum.grayscale_if_neutral = true;
    EXPECT_TRUE(validate_tiff_export_options(maximum));

    TiffExportOptions invalid_sample;
    invalid_sample.sample_type = static_cast<TiffSampleType>(255U);
    const auto invalid_sample_result = validate_tiff_export_options(invalid_sample);
    ASSERT_FALSE(invalid_sample_result);
    EXPECT_EQ(invalid_sample_result.error().code, ErrorCode::kValidation);
    EXPECT_EQ(invalid_sample_result.error().context.at("format"), "tiff");
    EXPECT_EQ(invalid_sample_result.error().context.at("reason"), "invalid_tiff_sample_type");
    EXPECT_EQ(invalid_sample_result.error().context.at("sample_type"), "255");
    EXPECT_EQ(tiff_sample_type_name(invalid_sample.sample_type), "unknown");

    TiffExportOptions invalid_compression;
    invalid_compression.compression = static_cast<TiffCompression>(255U);
    const auto invalid_compression_result = validate_tiff_export_options(invalid_compression);
    ASSERT_FALSE(invalid_compression_result);
    EXPECT_EQ(invalid_compression_result.error().code, ErrorCode::kValidation);
    EXPECT_EQ(invalid_compression_result.error().context.at("format"), "tiff");
    EXPECT_EQ(invalid_compression_result.error().context.at("reason"), "invalid_tiff_compression");
    EXPECT_EQ(invalid_compression_result.error().context.at("compression"), "255");
    EXPECT_EQ(tiff_compression_name(invalid_compression.compression), "unknown");

    for (const int level : {0, 10})
    {
        TiffExportOptions invalid_level;
        invalid_level.compression_level = level;
        const auto result = validate_tiff_export_options(invalid_level);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, ErrorCode::kValidation);
        EXPECT_EQ(result.error().context.at("format"), "tiff");
        EXPECT_EQ(result.error().context.at("reason"), "invalid_tiff_compression_level");
        EXPECT_EQ(result.error().context.at("compression_level"), std::to_string(level));
        EXPECT_EQ(result.error().context.at("minimum"), "1");
        EXPECT_EQ(result.error().context.at("maximum"), "9");
    }

    const auto noncanonical_sample = parse_tiff_sample_type("8");
    ASSERT_FALSE(noncanonical_sample);
    EXPECT_EQ(noncanonical_sample.error().context.at("format"), "tiff");
    EXPECT_EQ(noncanonical_sample.error().context.at("reason"), "invalid_tiff_sample_type");
    EXPECT_EQ(noncanonical_sample.error().context.at("sample_type"), "8");
    const auto noncanonical_compression = parse_tiff_compression("predictor");
    ASSERT_FALSE(noncanonical_compression);
    EXPECT_EQ(noncanonical_compression.error().context.at("format"), "tiff");
    EXPECT_EQ(noncanonical_compression.error().context.at("reason"), "invalid_tiff_compression");
    EXPECT_EQ(noncanonical_compression.error().context.at("compression"), "predictor");
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
    EXPECT_EQ(uri_display_name(utf8_text(u8"file:///Users/me/photos/résumé.jpg")),
              utf8_text(u8"résumé.jpg"));
    EXPECT_EQ(uri_parent(utf8_text(u8"file:///Users/me/photos/résumé.jpg")),
              utf8_text(u8"file:///Users/me/photos"));
    EXPECT_EQ(uri_display_name(utf8_text(u8"file:///Users/me/photos")), utf8_text(u8"photos"));
}

TEST(LibraryFolderTest, BuildsTreeFromUtf8FolderNames)
{
    AssetRecord photo;
    photo.id = "ast_cjk";
    photo.normalized_uri = utf8_text(u8"file:///Users/me/photos/résumé/a.jpg");
    const auto folders = library_folders({photo});
    bool saw_test = false;
    for (const auto &folder : folders)
    {
        if (folder.display_name == utf8_text(u8"résumé"))
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
    auto parsed = parse_tag_list("  landscape, archive, landscape ");
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_EQ(parsed.value().size(), 2U);
    EXPECT_EQ(parsed.value().front(), "landscape");
    EXPECT_EQ(parsed.value().back(), "archive");
    auto empty = normalize_tag_name("   ");
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code, ErrorCode::kValidation);

    AssetRecord tagged;
    tagged.id = "ast_tag";
    tagged.tags = {"landscape"};
    LibraryQuery query;
    query.tag = "landscape";
    EXPECT_TRUE(asset_matches_query(tagged, query));
    query.tag = "archive";
    EXPECT_FALSE(asset_matches_query(tagged, query));
}

} // namespace
} // namespace ravo
