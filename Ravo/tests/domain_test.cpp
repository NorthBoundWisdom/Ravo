#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
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

TEST(DomainTypesTest, PreviewContractInvalidatesAdaptiveDenoiseCaches)
{
    EXPECT_EQ(kPreviewContractVersion, 11);
    EXPECT_TRUE(make_preview_cache_key("asset", 640, 480, "fingerprint", "recipe")
                    .starts_with("v11_asset_640x480_fingerprint_recipe"));
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
    EXPECT_EQ(parse_export_metadata_mode("full").value(), ExportMetadataMode::kFull);
    EXPECT_EQ(parse_export_metadata_mode("no-location").value(), ExportMetadataMode::kNoLocation);
    EXPECT_EQ(parse_export_metadata_mode("none").value(), ExportMetadataMode::kNone);
    EXPECT_EQ(export_metadata_mode_name(ExportMetadataMode::kNoLocation), "no-location");
    auto bad_metadata = parse_export_metadata_mode("private-ish");
    ASSERT_FALSE(bad_metadata);
    EXPECT_EQ(bad_metadata.error().context.at("reason"), "invalid_export_metadata_mode");
}

TEST(ExportFormatTest, ExpandsOnlyBoundedPortableFilenameTemplateTokens)
{
    auto expanded = expand_export_filename_template("{stem}-{asset_id}-{sequence}{ext}", "portrait",
                                                    "ast_123", 7, ".jpg");
    ASSERT_TRUE(expanded) << expanded.error().message;
    EXPECT_EQ(expanded.value(), "portrait-ast_123-0007.jpg");
    auto appended =
        expand_export_filename_template("delivery-{sequence}", "portrait", "ast_123", 42, ".tif");
    ASSERT_TRUE(appended) << appended.error().message;
    EXPECT_EQ(appended.value(), "delivery-0042.tif");

    auto unknown =
        expand_export_filename_template("{camera}{ext}", "portrait", "ast_123", 1, ".png");
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().context.at("reason"), "unknown_export_filename_token");
    auto traversal =
        expand_export_filename_template("../{stem}{ext}", "portrait", "ast_123", 1, ".png");
    ASSERT_FALSE(traversal);
    EXPECT_EQ(traversal.error().context.at("reason"), "nonportable_export_filename");
    auto reserved = expand_export_filename_template("CON{ext}", "portrait", "ast_123", 1, ".png");
    ASSERT_FALSE(reserved);
    EXPECT_EQ(reserved.error().context.at("reason"), "reserved_export_filename");
    EXPECT_FALSE(expand_export_filename_template("{stem", "portrait", "ast_123", 1, ".png"));
    EXPECT_FALSE(expand_export_filename_template("{stem}{ext}", "bad/name", "ast_123", 1, ".png"));
    EXPECT_FALSE(expand_export_filename_template("{stem}{ext}", "portrait", "ast_123", 0, ".png"));
    auto too_large = expand_export_filename_template("{stem}{stem}{ext}", std::string(200, 'a'),
                                                     "ast_123", 1, ".png");
    ASSERT_FALSE(too_large);
    EXPECT_EQ(too_large.error().context.at("reason"), "invalid_expanded_export_filename");
}

TEST(ImportFilenameTemplateTest, ExpandsOnlyStableBoundedPortableTokens)
{
    auto expanded = expand_import_filename_template("job-{date}-{sequence}-{stem}{ext}", "portrait",
                                                    "20260901", 7U, ".CR3");
    ASSERT_TRUE(expanded) << expanded.error().message;
    EXPECT_EQ(expanded.value(), "job-20260901-0007-portrait.CR3");
    auto appended =
        expand_import_filename_template("job-{sequence}", "portrait", "20260901", 42U, ".jpg");
    ASSERT_TRUE(appended) << appended.error().message;
    EXPECT_EQ(appended.value(), "job-0042.jpg");

    auto unknown =
        expand_import_filename_template("{camera}{ext}", "portrait", "20260901", 1U, ".png");
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().context.at("reason"), "unknown_import_filename_token");
    auto traversal =
        expand_import_filename_template("../{stem}{ext}", "portrait", "20260901", 1U, ".png");
    ASSERT_FALSE(traversal);
    EXPECT_EQ(traversal.error().context.at("reason"), "nonportable_import_filename");
    auto reserved = expand_import_filename_template("CON{ext}", "portrait", "20260901", 1U, ".png");
    ASSERT_FALSE(reserved);
    EXPECT_EQ(reserved.error().context.at("reason"), "reserved_import_filename");
    EXPECT_FALSE(expand_import_filename_template("{stem", "portrait", "20260901", 1U, ".png"));
    EXPECT_FALSE(
        expand_import_filename_template("{stem}{ext}", "bad/name", "20260901", 1U, ".png"));
    EXPECT_FALSE(
        expand_import_filename_template("{stem}{ext}", "portrait", "2026/0901", 1U, ".png"));
    EXPECT_FALSE(
        expand_import_filename_template("{stem}{ext}", "portrait", "20260901", 0U, ".png"));
    auto too_large = expand_import_filename_template("{stem}{stem}{ext}", std::string(200, 'a'),
                                                     "20260901", 1U, ".png");
    ASSERT_FALSE(too_large);
    EXPECT_EQ(too_large.error().context.at("reason"), "invalid_expanded_import_filename");
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

[[nodiscard]] std::string utf8_text(const char8_t *text);

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

    first.review.picked = true;
    second.review.rejected = true;
    second.review.picked = false;
    third.review.picked = false;
    third.review.rejected = false;
    LibraryQuery only_picked;
    only_picked.cull_flag_filter = CullFlagFilter::kPicked;
    filtered = filter_and_sort_assets({first, second, third}, only_picked);
    ASSERT_EQ(filtered.size(), 1U);
    EXPECT_EQ(filtered[0].id, "ast_a");
    LibraryQuery unreviewed;
    unreviewed.cull_flag_filter = CullFlagFilter::kUnreviewed;
    filtered = filter_and_sort_assets({first, second, third}, unreviewed);
    ASSERT_EQ(filtered.size(), 1U);
    EXPECT_EQ(filtered[0].id, "ast_c");
    LibraryQuery only_pick_filter;
    only_pick_filter.pick_filter = PickFilter::kOnly;
    filtered = filter_and_sort_assets({first, second, third}, only_pick_filter);
    ASSERT_EQ(filtered.size(), 1U);
    EXPECT_EQ(filtered[0].id, "ast_a");

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

TEST(LibraryQueryTest, MatchesProductTextMediaEditCaptureAndNumericFields)
{
    AssetRecord photo;
    photo.id = "ast_match";
    photo.normalized_uri = "file:///photos/Trip/Sunrise.CR2";
    photo.media_type = "image/x-canon-cr2";
    photo.width = 6000U;
    photo.height = 4000U;
    photo.size_bytes = 42000000U;
    photo.created_unix_ms = 2000;
    photo.has_edits = true;
    photo.tags = {"Landscape", utf8_text(u8"旅行")};
    photo.metadata.title = "Golden hour";
    photo.capture.camera_make = "Canon";
    photo.capture.camera_model = "EOS R5";
    photo.capture.lens_make = "Canon";
    photo.capture.lens_model = "RF 35mm F1.8";
    photo.capture.iso = 400.0;
    photo.capture.aperture = 5.6;
    photo.capture.focal_length_mm = 35.0;
    photo.capture.shutter_s = 0.008;
    photo.capture.captured_unix_s = 1700000000;

    LibraryQuery query;
    query.text = "golden";
    query.media_types = {"image/x-canon-cr2"};
    query.edit_filter = EditFilter::kEdited;
    query.camera = "eos r5";
    query.iso = {399.0, 401.0};
    query.aperture = {5.5, 5.7};
    query.focal_length_mm = {34.0, 36.0};
    query.shutter_s = {0.007, 0.009};
    query.aspect_ratio = {1.49, 1.51};
    query.imported_after_unix_ms = 1999;
    query.imported_before_unix_ms = 2001;
    query.captured_after_unix_s = 1699999999;
    query.captured_before_unix_s = 1700000001;
    ASSERT_TRUE(validate_library_query(query));
    EXPECT_TRUE(asset_matches_query(photo, query));

    query.text = utf8_text(u8"旅行");
    EXPECT_TRUE(asset_matches_query(photo, query));
    query.text = "missing";
    EXPECT_FALSE(asset_matches_query(photo, query));
    query.text.clear();
    query.media_types = {"image/jpeg"};
    EXPECT_FALSE(asset_matches_query(photo, query));
    query.media_types = {photo.media_type};
    query.edit_filter = EditFilter::kUnedited;
    EXPECT_FALSE(asset_matches_query(photo, query));
    query.edit_filter = EditFilter::kEdited;
    query.iso = {401.0, std::nullopt};
    EXPECT_FALSE(asset_matches_query(photo, query));

    AssetRecord missing = photo;
    missing.capture.iso.reset();
    query.iso = {1.0, 1000.0};
    EXPECT_FALSE(asset_matches_query(missing, query));

    query = {};
    query.camera_make_equals = "Canon";
    query.camera_model_equals = "EOS R5";
    query.focal_length_mm_equals = 35.0;
    query.captured_local_date = "2023:11:14";
    ASSERT_TRUE(validate_library_query(query));
    EXPECT_FALSE(asset_matches_query(photo, query));
    photo.capture.captured_datetime =
        CaptureDateTime{"2023:11:14 10:00:00", std::nullopt, std::nullopt};
    EXPECT_TRUE(asset_matches_query(photo, query));
    query.camera_model_equals = "EOS R6";
    EXPECT_FALSE(asset_matches_query(photo, query));
    query.camera_model_equals.reset();
    auto half = validate_library_query(query);
    ASSERT_FALSE(half);
    EXPECT_EQ(half.error().context.at("reason"), "invalid_library_camera_facet");

    query = {};
    query.lens_make_equals = "Canon";
    query.lens_model_equals = "RF 35mm F1.8";
    ASSERT_TRUE(validate_library_query(query));
    EXPECT_TRUE(asset_matches_query(photo, query));
    query.lens_model_equals = "RF 50mm F1.8";
    EXPECT_FALSE(asset_matches_query(photo, query));
    query.lens_model_equals.reset();
    auto half_lens = validate_library_query(query);
    ASSERT_FALSE(half_lens);
    EXPECT_EQ(half_lens.error().context.at("reason"), "invalid_library_lens_name_facet");

    query = {};
    query.country_equals = "China";
    query.city_equals = "Shanghai";
    ASSERT_TRUE(validate_library_query(query));
    EXPECT_FALSE(asset_matches_query(photo, query));
    photo.metadata.country = "China";
    photo.metadata.province_state = "Shanghai";
    photo.metadata.city = "Shanghai";
    photo.metadata.sublocation = "Bund";
    EXPECT_TRUE(asset_matches_query(photo, query));
    query.sublocation_equals = "Bund";
    EXPECT_TRUE(asset_matches_query(photo, query));
    query.sublocation_equals = "Pudong";
    EXPECT_FALSE(asset_matches_query(photo, query));
    auto roundtrip = serialize_library_query_document(query);
    ASSERT_TRUE(roundtrip) << roundtrip.error().message;
    auto parsed_location = parse_library_query_document(roundtrip.value());
    ASSERT_TRUE(parsed_location) << parsed_location.error().message;
    EXPECT_EQ(parsed_location.value().country_equals, query.country_equals);
    EXPECT_EQ(parsed_location.value().sublocation_equals, query.sublocation_equals);
}

TEST(LibraryQueryTest, SerializesNamedSetDocumentsWithoutCollectionIdentity)
{
    LibraryQuery query;
    query.rating_mode = RatingFilterMode::kExact;
    query.rating_value = 5;
    query.color_labels = {ColorLabel::kRed};
    query.tag = "beach";
    query.sort_field = AssetSortField::kRating;
    query.sort_direction = SortDirection::kAscending;
    auto serialized = serialize_library_query_document(query);
    ASSERT_TRUE(serialized) << serialized.error().message;
    auto parsed = parse_library_query_document(serialized.value());
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed.value(), query);

    query.collection_id = generate_library_set_id();
    EXPECT_FALSE(serialize_library_query_document(query));
    EXPECT_FALSE(parse_library_query_document(R"({"schema_version":1,"collection_id":"set_x"})"));
}

TEST(LibraryQueryTest, RejectsInvalidStateAndSortsCaptureOrSizeDeterministically)
{
    LibraryQuery query;
    query.rating_value = 6;
    auto invalid = validate_library_query(query);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().context.at("reason"), "invalid_library_rating_filter");

    query = {};
    query.color_labels = {ColorLabel::kRed, ColorLabel::kRed};
    invalid = validate_library_query(query);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().context.at("reason"), "invalid_library_color_filter");

    query = {};
    query.iso = {800.0, 100.0};
    invalid = validate_library_query(query);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().context.at("field"), "iso");

    query = {};
    query.media_types = {"image/jpeg", "image/jpeg"};
    EXPECT_FALSE(validate_library_query(query));
    query = {};
    query.text = std::string(513U, 'x');
    EXPECT_FALSE(validate_library_query(query));
    query = {};
    query.imported_after_unix_ms = 20;
    query.imported_before_unix_ms = 10;
    EXPECT_FALSE(validate_library_query(query));

    AssetRecord first;
    first.id = "first";
    first.size_bytes = 200U;
    first.capture.captured_unix_s = 20;
    AssetRecord second;
    second.id = "second";
    second.size_bytes = 100U;
    second.capture.captured_unix_s = 10;
    AssetRecord missing;
    missing.id = "missing";
    missing.size_bytes = 300U;

    query = {};
    query.sort_field = AssetSortField::kCaptureTime;
    query.sort_direction = SortDirection::kAscending;
    auto sorted = filter_and_sort_assets({first, missing, second}, query);
    ASSERT_EQ(sorted.size(), 3U);
    EXPECT_EQ(sorted[0].id, "second");
    EXPECT_EQ(sorted[1].id, "first");
    EXPECT_EQ(sorted[2].id, "missing");
    query.sort_direction = SortDirection::kDescending;
    sorted = filter_and_sort_assets({first, missing, second}, query);
    EXPECT_EQ(sorted[0].id, "first");
    EXPECT_EQ(sorted[1].id, "second");
    EXPECT_EQ(sorted[2].id, "missing");

    query.sort_field = AssetSortField::kFileSize;
    query.sort_direction = SortDirection::kAscending;
    sorted = filter_and_sort_assets({first, missing, second}, query);
    EXPECT_EQ(sorted[0].id, "second");
    EXPECT_EQ(sorted[2].id, "missing");
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

TEST(ExportMetadataDomainTest, CanonicalizesAndRejectsInvalidTags)
{
    auto sorted = canonicalize_export_tags({"zeta", "  alpha  ", "mu"});
    ASSERT_TRUE(sorted) << sorted.error().message;
    ASSERT_EQ(sorted.value().size(), 3U);
    EXPECT_EQ(sorted.value()[0], "alpha");
    EXPECT_EQ(sorted.value()[1], "mu");
    EXPECT_EQ(sorted.value()[2], "zeta");

    auto unicode = canonicalize_export_tags({utf8_text(u8"摄影"), "archive", utf8_text(u8"café")});
    ASSERT_TRUE(unicode) << unicode.error().message;
    ASSERT_EQ(unicode.value().size(), 3U);
    EXPECT_EQ(unicode.value()[0], "archive");
    EXPECT_EQ(unicode.value()[1], utf8_text(u8"café"));
    EXPECT_EQ(unicode.value()[2], utf8_text(u8"摄影"));

    auto duplicate = canonicalize_export_tags({"alpha", "alpha"});
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().context.at("reason"), "duplicate_export_tag");
}

TEST(ExportMetadataDomainTest, ApproximatesPositiveRationalsDeterministically)
{
    const auto exact = export_positive_rational(50.0);
    ASSERT_TRUE(exact) << exact.error().message;
    EXPECT_EQ(exact.value(), (ExportUnsignedRational{50U, 1U}));
    EXPECT_EQ(export_rational_xmp_text(exact.value()), "50/1");

    const auto aperture = export_positive_rational(2.8);
    ASSERT_TRUE(aperture) << aperture.error().message;
    EXPECT_EQ(aperture.value(), (ExportUnsignedRational{14U, 5U}));
    EXPECT_EQ(export_rational_xmp_text(aperture.value()), "14/5");

    const auto shutter = export_positive_rational(0.008);
    ASSERT_TRUE(shutter) << shutter.error().message;
    EXPECT_EQ(shutter.value(), (ExportUnsignedRational{1U, 125U}));
    EXPECT_EQ(export_rational_xmp_text(shutter.value()), "1/125");

    const auto third = export_positive_rational(1.0 / 3.0);
    ASSERT_TRUE(third) << third.error().message;
    EXPECT_EQ(third.value(), (ExportUnsignedRational{1U, 3U}));

    const auto again = export_positive_rational(2.8);
    ASSERT_TRUE(again);
    EXPECT_EQ(again.value(), aperture.value());

    EXPECT_FALSE(export_positive_rational(0.0));
    EXPECT_FALSE(export_positive_rational(-1.5));
    EXPECT_FALSE(export_positive_rational(std::numeric_limits<double>::infinity()));
    EXPECT_FALSE(export_positive_rational(std::numeric_limits<double>::quiet_NaN()));
    auto overflow = export_positive_rational(
        static_cast<double>(std::numeric_limits<std::uint32_t>::max()) + 1.0);
    ASSERT_FALSE(overflow);
    EXPECT_EQ(overflow.error().context.at("reason"), "export_rational_overflow");
}

TEST(ExportMetadataDomainTest, RejectsInvalidIsoAndAcceptsExactShortIntegers)
{
    const auto iso = export_photographic_sensitivity(100.0);
    ASSERT_TRUE(iso) << iso.error().message;
    EXPECT_EQ(iso.value(), 100U);
    const auto max_iso = export_photographic_sensitivity(65535.0);
    ASSERT_TRUE(max_iso) << max_iso.error().message;
    EXPECT_EQ(max_iso.value(), 65535U);

    EXPECT_EQ(export_photographic_sensitivity(0.0).error().context.at("reason"),
              "invalid_export_capture_number");
    EXPECT_EQ(export_photographic_sensitivity(-50.0).error().context.at("reason"),
              "invalid_export_capture_number");
    EXPECT_EQ(export_photographic_sensitivity(100.5).error().context.at("reason"),
              "invalid_export_iso_fractional");
    EXPECT_EQ(export_photographic_sensitivity(65536.0).error().context.at("reason"),
              "invalid_export_iso_range");
}

TEST(ExportMetadataDomainTest, ValidatesSnapshotFieldsBoundsAndOmitsIptcWhenAbsent)
{
    ExportMetadataSnapshot metadata;
    EXPECT_TRUE(validate_export_metadata(metadata));
    EXPECT_TRUE(export_iptc_should_omit(metadata));

    metadata.writable.title = "";
    EXPECT_TRUE(validate_export_metadata(metadata));
    EXPECT_FALSE(export_iptc_should_omit(metadata));

    metadata.writable.title.reset();
    metadata.tags = {"beta", "alpha"};
    auto unsorted = validate_export_metadata(metadata);
    ASSERT_FALSE(unsorted);
    EXPECT_EQ(unsorted.error().context.at("reason"), "invalid_export_tag_order");

    metadata.tags = canonicalize_export_tags({"beta", "alpha"}).value();
    EXPECT_TRUE(validate_export_metadata(metadata));
    EXPECT_FALSE(export_iptc_should_omit(metadata));

    metadata.writable.description = std::string("\xE2\x28\xA1", 3U);
    auto bad_utf8 = validate_export_metadata(metadata);
    ASSERT_FALSE(bad_utf8);
    EXPECT_EQ(bad_utf8.error().context.at("reason"), "invalid_export_metadata");
    EXPECT_EQ(bad_utf8.error().context.at("field"), "description");

    metadata = {};
    metadata.capture.iso = 0.0;
    EXPECT_EQ(validate_export_metadata(metadata).error().context.at("reason"),
              "invalid_export_capture_number");
    metadata.capture.iso = 80.0;
    metadata.capture.aperture = 2.8;
    metadata.capture.focal_length_mm = 50.0;
    metadata.capture.shutter_s = 0.008;
    metadata.capture.camera_make = "RavoCam";
    EXPECT_TRUE(validate_export_metadata(metadata));

    ColorProfileState srgb;
    srgb.kind = ColorProfileKind::kBuiltin;
    srgb.identifier = "srgb";
    EXPECT_TRUE(export_color_space_is_srgb(srgb));
    srgb.identifier = "display_p3";
    EXPECT_FALSE(export_color_space_is_srgb(srgb));

    metadata = {};
    metadata.destination_document_name.assign(kExportDocumentNameMaxBytes + 1U, 'p');
    EXPECT_TRUE(validate_export_metadata(metadata));
    EXPECT_EQ(validate_tiff_export_metadata(metadata).error().context.at("reason"),
              "invalid_tiff_document_name");
    EXPECT_EQ(validate_tiff_export_document_name(metadata.destination_document_name)
                  .error()
                  .context.at("reason"),
              "invalid_tiff_document_name");
}

TEST(ExportMetadataDomainTest, DisabledSnapshotRejectsRetainedPayload)
{
    ExportMetadataSnapshot stripped;
    stripped.embed_metadata = false;
    EXPECT_TRUE(validate_export_metadata(stripped));
    stripped.writable.title = "leak";
    auto rejected = validate_export_metadata(stripped);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "disabled_export_metadata_has_payload");
}

TEST(ExportMetadataDomainTest, EstimatesPacketsAgainstJpegSafeBounds)
{
    ExportMetadataSnapshot metadata;
    metadata.writable.title = "Title";
    metadata.writable.description = "Description";
    metadata.writable.creator = "Creator";
    metadata.writable.copyright = "Copyright";
    metadata.tags = {"alpha"};
    const auto sizes = estimate_export_metadata_packets(metadata);
    ASSERT_TRUE(sizes) << sizes.error().message;
    EXPECT_GT(sizes.value().exif_tiff_profile_bytes, 0U);
    EXPECT_GT(sizes.value().xmp_packet_bytes, 0U);
    EXPECT_GT(sizes.value().iptc_iim_bytes, 0U);
    EXPECT_LE(sizes.value().exif_tiff_profile_bytes, kExportExifTiffProfileMaxBytes);
    EXPECT_LE(sizes.value().xmp_packet_bytes, kExportXmpPacketMaxBytes);
    EXPECT_LE(sizes.value().iptc_iim_bytes, kExportIptcIimMaxBytes);

    ExportMetadataSnapshot oversized;
    oversized.writable.title = std::string(kMetadataFieldMaxLength, '&');
    oversized.writable.description = std::string(kMetadataFieldMaxLength, '&');
    oversized.writable.creator = std::string(kMetadataFieldMaxLength, '&');
    oversized.writable.copyright = std::string(kMetadataFieldMaxLength, '&');
    oversized.tags.assign(32U, std::string(kTagMaxLength, '&'));
    for (std::size_t index = 0U; index < oversized.tags.size(); ++index)
    {
        oversized.tags[index].back() = static_cast<char>('A' + static_cast<int>(index));
    }
    auto packets = estimate_export_metadata_packets(oversized);
    ASSERT_FALSE(packets);
    EXPECT_TRUE(packets.error().context.at("reason") == "export_xmp_packet_too_large" ||
                packets.error().context.at("reason") == "export_iptc_packet_too_large");
}

TEST(ExportMetadataDomainTest, EnforcesXmlAndIptcTextContractsBeforeEncoding)
{
    ExportMetadataSnapshot metadata;
    metadata.writable.title = std::string(kExportIptcTitleMaxBytes, 't');
    EXPECT_TRUE(validate_export_metadata(metadata));
    metadata.writable.title->push_back('x');
    auto oversized = validate_export_metadata(metadata);
    ASSERT_FALSE(oversized);
    EXPECT_EQ(oversized.error().context.at("reason"), "export_iptc_dataset_too_large");
    EXPECT_EQ(oversized.error().context.at("field"), "title");

    metadata = {};
    metadata.writable.creator = std::string(kExportIptcCreatorMaxBytes + 1U, 'c');
    EXPECT_EQ(validate_export_metadata(metadata).error().context.at("reason"),
              "export_iptc_dataset_too_large");
    metadata = {};
    metadata.writable.copyright = std::string(kExportIptcCopyrightMaxBytes + 1U, 'c');
    EXPECT_EQ(validate_export_metadata(metadata).error().context.at("reason"),
              "export_iptc_dataset_too_large");
    metadata = {};
    metadata.writable.description = std::string(kExportIptcDescriptionMaxBytes + 1U, 'd');
    EXPECT_EQ(validate_export_metadata(metadata).error().context.at("reason"),
              "export_iptc_dataset_too_large");

    metadata = {};
    metadata.writable.title = "line\nbreak";
    EXPECT_EQ(validate_export_metadata(metadata).error().context.at("reason"),
              "invalid_export_iptc_text");
    metadata.writable.title.reset();
    metadata.writable.description = "line\r\nbreak";
    EXPECT_TRUE(validate_export_metadata(metadata));

    metadata = {};
    metadata.writable.title = std::string(1U, '\x01');
    const auto control = validate_export_metadata(metadata);
    ASSERT_FALSE(control);
    EXPECT_EQ(control.error().context.at("detail"), "invalid_xml_character");
    metadata.writable.title = std::string("\xEF\xBF\xBE", 3U); // U+FFFE
    const auto noncharacter = validate_export_metadata(metadata);
    ASSERT_FALSE(noncharacter);
    EXPECT_EQ(noncharacter.error().context.at("detail"), "invalid_xml_character");

    metadata = {};
    metadata.tags = {std::string(kExportIptcKeywordMaxBytes, 'k')};
    EXPECT_TRUE(validate_export_metadata(metadata));
    metadata.tags.front().push_back('x');
    EXPECT_EQ(validate_export_metadata(metadata).error().context.at("reason"),
              "export_iptc_dataset_too_large");

    std::vector<std::string> too_many(kExportTagMaxCount + 1U, "tag");
    const auto count = canonicalize_export_tags(too_many);
    ASSERT_FALSE(count);
    EXPECT_EQ(count.error().context.at("reason"), "export_tag_count_too_large");
}

TEST(ExportMetadataDomainTest, CancelsDuringSnapshotValidation)
{
    CancellationSource source;
    ASSERT_TRUE(source.cancel("export-metadata-test"));
    ExportMetadataSnapshot metadata;
    metadata.writable.title = "Title";
    const auto cancelled = validate_export_metadata(metadata, source.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
}

TEST(CaptureDateTimeTest, AcceptsGregorianBoundariesAndRejectsImpossibleDates)
{
    const auto accept = [](const std::string &local)
    {
        CaptureDateTime value;
        value.local_exif = local;
        return validate_capture_datetime(value);
    };
    EXPECT_TRUE(accept("0001:01:01 00:00:00"));
    EXPECT_TRUE(accept("9999:12:31 23:59:59"));
    EXPECT_TRUE(accept("2000:02:29 12:00:00"));
    EXPECT_TRUE(accept("2024:02:29 00:00:00"));
    EXPECT_FALSE(accept("2000:02:30 00:00:00"));
    EXPECT_FALSE(accept("1900:02:29 00:00:00"));
    EXPECT_FALSE(accept("2023:02:29 00:00:00"));
    EXPECT_FALSE(accept("2024:13:01 00:00:00"));
    EXPECT_FALSE(accept("2024:00:01 00:00:00"));
    EXPECT_FALSE(accept("2024:01:00 00:00:00"));
    EXPECT_FALSE(accept("2024:01:32 00:00:00"));
    EXPECT_FALSE(accept("2024:04:31 00:00:00"));
    EXPECT_FALSE(accept("2024:01:01 24:00:00"));
    EXPECT_FALSE(accept("2024:01:01 00:60:00"));
    EXPECT_FALSE(accept("2024:01:01 00:00:60"));
    EXPECT_FALSE(accept("0000:01:01 00:00:00"));
    EXPECT_FALSE(accept("2024-01-01 00:00:00"));
    EXPECT_FALSE(accept("2024:01:01T00:00:00"));
    EXPECT_FALSE(accept("2024:01:01 00:00:00Z"));
    EXPECT_FALSE(accept("2024:01:01 00:00"));
    EXPECT_EQ(accept("2024:01:01 00:00:60").error().context.at("reason"),
              "invalid_capture_datetime");
}

TEST(CaptureDateTimeTest, PreservesSubsecondsAndExactOffsetBounds)
{
    CaptureDateTime value;
    value.local_exif = "2007:09:11 13:53:33";
    value.subsecond_digits = "18";
    EXPECT_TRUE(validate_capture_datetime(value));
    EXPECT_EQ(format_capture_datetime_iso(value), "2007-09-11T13:53:33.18");

    value.utc_offset_minutes = 0;
    EXPECT_TRUE(validate_capture_datetime(value));
    EXPECT_EQ(format_capture_datetime_iso(value), "2007-09-11T13:53:33.18+00:00");
    EXPECT_EQ(format_capture_utc_offset(0), "+00:00");
    EXPECT_EQ(format_capture_utc_offset(120), "+02:00");
    EXPECT_EQ(format_capture_utc_offset(-840), "-14:00");
    EXPECT_EQ(format_capture_utc_offset(840), "+14:00");

    value.utc_offset_minutes = 840;
    EXPECT_TRUE(validate_capture_datetime(value));
    value.utc_offset_minutes = -840;
    EXPECT_TRUE(validate_capture_datetime(value));
    value.utc_offset_minutes = 841;
    EXPECT_FALSE(validate_capture_datetime(value));
    value.utc_offset_minutes = -841;
    EXPECT_FALSE(validate_capture_datetime(value));

    value.utc_offset_minutes.reset();
    value.subsecond_digits = "123456789";
    EXPECT_TRUE(validate_capture_datetime(value));
    value.subsecond_digits = "";
    EXPECT_FALSE(validate_capture_datetime(value));
    value.subsecond_digits = "1234567890";
    EXPECT_FALSE(validate_capture_datetime(value));
    value.subsecond_digits = "1a";
    EXPECT_FALSE(validate_capture_datetime(value));
    value.subsecond_digits = "18 ";
    EXPECT_FALSE(validate_capture_datetime(value));
}

TEST(CaptureLocationTest, RequiresCompleteAltitudeAndPreservesZeroReference)
{
    CaptureLocation location;
    location.latitude_e6 = 1000000;
    location.longitude_e6 = 2000000;
    EXPECT_TRUE(validate_capture_location(location));

    CaptureAltitude below;
    below.magnitude_mm = 123456;
    below.reference = CaptureAltitudeReference::kBelowSeaLevel;
    location.altitude = below;
    EXPECT_TRUE(validate_capture_location(location));

    CaptureAltitude above;
    above.magnitude_mm = 123456;
    above.reference = CaptureAltitudeReference::kAboveSeaLevel;
    location.altitude = above;
    EXPECT_TRUE(validate_capture_location(location));

    CaptureAltitude zero_below;
    zero_below.magnitude_mm = 0;
    zero_below.reference = CaptureAltitudeReference::kBelowSeaLevel;
    location.altitude = zero_below;
    EXPECT_TRUE(validate_capture_location(location));
    CaptureAltitude zero_above;
    zero_above.magnitude_mm = 0;
    zero_above.reference = CaptureAltitudeReference::kAboveSeaLevel;
    location.altitude = zero_above;
    EXPECT_TRUE(validate_capture_location(location));
    EXPECT_NE(zero_below, zero_above);

    location.altitude->magnitude_mm = 12000000;
    location.altitude->reference = CaptureAltitudeReference::kBelowSeaLevel;
    EXPECT_TRUE(validate_capture_location(location));
    location.altitude->magnitude_mm = 12000001;
    EXPECT_FALSE(validate_capture_location(location));
    location.altitude->magnitude_mm = 100000000;
    location.altitude->reference = CaptureAltitudeReference::kAboveSeaLevel;
    EXPECT_TRUE(validate_capture_location(location));
    location.altitude->magnitude_mm = 100000001;
    EXPECT_FALSE(validate_capture_location(location));

    location.altitude.reset();
    location.latitude_e6 = 90000001;
    EXPECT_FALSE(validate_capture_location(location));
}

TEST(CaptureLocationTest, FormatsScaledDecimalsAndExactDmsRationalsWithoutFloatingPoint)
{
    EXPECT_EQ(format_scaled_decimal(49253239, 6), "49.253239");
    EXPECT_EQ(format_scaled_decimal(3050766, 6), "3.050766");
    EXPECT_EQ(format_scaled_decimal(-3050766, 6), "-3.050766");
    EXPECT_EQ(format_scaled_decimal(0, 6), "0");
    EXPECT_EQ(format_scaled_decimal(49000000, 6), "49");
    EXPECT_EQ(format_scaled_decimal(123456, 3), "123.456");
    EXPECT_EQ(format_scaled_decimal(-12000, 3), "-12");

    const auto dms = capture_microdegrees_to_dms(49253239);
    EXPECT_EQ(dms[0].numerator, 49U);
    EXPECT_EQ(dms[0].denominator, 1U);
    EXPECT_EQ(dms[1].numerator, 15U);
    EXPECT_EQ(dms[1].denominator, 1U);
    EXPECT_EQ(dms[2].numerator, 29151U);
    EXPECT_EQ(dms[2].denominator, 2500U);

    const auto altitude = capture_altitude_mm_to_rational(123456);
    EXPECT_EQ(altitude.numerator, 15432U);
    EXPECT_EQ(altitude.denominator, 125U);

    EXPECT_EQ(format_gps_xmp_coordinate(49253239, 'N', 'S'), "49,15.19434N");
    EXPECT_EQ(format_gps_xmp_coordinate(-3050766, 'E', 'W'), "3,3.04596W");
    EXPECT_EQ(format_gps_xmp_coordinate(0, 'N', 'S'), "0,0N");
    EXPECT_EQ(format_gps_xmp_coordinate(90000000, 'N', 'S'), "90,0N");
}

TEST(CaptureMetadataTest, EqualityIncludesNewFieldsAndRejectsPartialStateOnExport)
{
    CaptureMetadata left;
    CaptureMetadata right;
    EXPECT_EQ(left, right);
    EXPECT_FALSE(capture_metadata_has_values(left));
    left.captured_datetime = CaptureDateTime{"2007:09:11 13:53:33", "18", {}};
    EXPECT_NE(left, right);
    EXPECT_TRUE(capture_metadata_has_values(left));
    EXPECT_TRUE(validate_capture_metadata(left));

    ExportMetadataSnapshot snapshot;
    snapshot.capture = left;
    EXPECT_TRUE(validate_export_metadata(snapshot));
    auto sizes = estimate_export_metadata_packets(snapshot);
    ASSERT_TRUE(sizes) << sizes.error().message;
    EXPECT_GT(sizes.value().exif_tiff_profile_bytes, 80U);

    snapshot.capture.location = CaptureLocation{
        49253239, 3050766, CaptureAltitude{123456U, CaptureAltitudeReference::kAboveSeaLevel}};
    EXPECT_TRUE(validate_export_metadata(snapshot));
    snapshot.capture.location->latitude_e6 = 90000001;
    EXPECT_FALSE(validate_export_metadata(snapshot));
}

TEST(DomainExportFitTest, BoxFitNeverEnlargesAndPreservesAspect)
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    fit_within_box(100, 50, 1600, 1200, width, height);
    EXPECT_EQ(width, 100U);
    EXPECT_EQ(height, 50U);

    fit_within_box(4000, 2000, 800, 600, width, height);
    EXPECT_EQ(width, 800U);
    EXPECT_EQ(height, 400U);

    fit_within_box(2000, 4000, 800, 600, width, height);
    EXPECT_EQ(width, 300U);
    EXPECT_EQ(height, 600U);
}

TEST(DomainExportFitTest, TighterOfLongEdgeAndBoxWins)
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    fit_export_output_size(4000, 2000, 1000, 800, 600, width, height);
    EXPECT_EQ(width, 800U);
    EXPECT_EQ(height, 400U);

    fit_export_output_size(4000, 2000, 600, 800, 600, width, height);
    EXPECT_EQ(width, 600U);
    EXPECT_EQ(height, 300U);
}

TEST(DomainExportPresetTest, RoundTripAndFailClosed)
{
    ExportPreset preset;
    preset.options.format = ExportFormat::kJpeg;
    preset.options.max_edge = 1600;
    preset.options.max_width = 1200;
    preset.options.max_height = 800;
    preset.options.output_sharpen.enabled = true;
    preset.options.output_sharpen.amount = 0.75;
    preset.options.output_sharpen.radius = 0.8;
    preset.options.output_sharpen.threshold = 2.0;
    preset.options.jpeg_options.quality = 90;
    auto serialized = serialize_export_preset(preset);
    ASSERT_TRUE(serialized) << serialized.error().message;
    auto parsed = parse_export_preset_json(serialized.value());
    ASSERT_TRUE(parsed) << parsed.error().message;
    auto applied = apply_export_preset(parsed.value());
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(applied.value(), preset.options);

    EXPECT_FALSE(parse_export_preset_json("{"));
    EXPECT_FALSE(parse_export_preset_json(R"({"schema":"other","schema_version":1,"options":{}})"));
    EXPECT_FALSE(parse_export_preset_json(
        R"({"schema":"ravo.export_preset","schema_version":99,"options":{}})"));
}

TEST(DomainExportOptionsTest, OriginalCopyRejectsResizeAndSharpen)
{
    ExportOptions options;
    options.format = ExportFormat::kOriginalCopy;
    options.max_edge = 100;
    auto valid = validate_export_options(options);
    ASSERT_FALSE(valid);
    EXPECT_EQ(valid.error().context.at("reason"), "original_copy_resize_not_applicable");

    options.max_edge = 0;
    options.output_sharpen.enabled = true;
    valid = validate_export_options(options);
    ASSERT_FALSE(valid);
    EXPECT_EQ(valid.error().context.at("reason"), "original_copy_resize_not_applicable");
}

} // namespace
} // namespace ravo
