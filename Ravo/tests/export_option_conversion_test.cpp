#include <limits>
#include <string>
#include <string_view>

#include <QVariant>
#include <gtest/gtest.h>

#include "ravo/desktop/export_option_conversion.h"
#include "ravo/domain/types.h"

namespace ravo
{
namespace
{

[[nodiscard]] QVariantMap jpeg_options(const int quality, const QString &subsampling,
                                       const QString &metadata = QStringLiteral("full"),
                                       const int max_edge = 0)
{
    QVariantMap options;
    options.insert(QStringLiteral("quality"), quality);
    options.insert(QStringLiteral("jpegSubsampling"), subsampling);
    options.insert(QStringLiteral("metadataMode"), metadata);
    options.insert(QStringLiteral("maxEdge"), max_edge);
    options.insert(QStringLiteral("maxWidth"), 0);
    options.insert(QStringLiteral("maxHeight"), 0);
    options.insert(QStringLiteral("outputSharpenEnabled"), false);
    options.insert(QStringLiteral("outputSharpenAmount"), 0.5);
    options.insert(QStringLiteral("outputSharpenRadius"), 0.5);
    options.insert(QStringLiteral("outputSharpenThreshold"), 0.0);
    options.insert(QStringLiteral("watermarkEnabled"), false);
    options.insert(QStringLiteral("watermarkText"), QStringLiteral("RAVO"));
    options.insert(QStringLiteral("watermarkOpacity"), 0.5);
    options.insert(QStringLiteral("watermarkScale"), 8.0);
    options.insert(QStringLiteral("watermarkAlignment"), QStringLiteral("bottom_right"));
    return options;
}

[[nodiscard]] QVariantMap png_options(const QString &bit_depth, const int compression,
                                      const QString &metadata = QStringLiteral("full"),
                                      const int max_edge = 0)
{
    QVariantMap options;
    options.insert(QStringLiteral("pngBitDepth"), bit_depth);
    options.insert(QStringLiteral("pngCompression"), compression);
    options.insert(QStringLiteral("metadataMode"), metadata);
    options.insert(QStringLiteral("maxEdge"), max_edge);
    options.insert(QStringLiteral("maxWidth"), 0);
    options.insert(QStringLiteral("maxHeight"), 0);
    options.insert(QStringLiteral("outputSharpenEnabled"), false);
    options.insert(QStringLiteral("outputSharpenAmount"), 0.5);
    options.insert(QStringLiteral("outputSharpenRadius"), 0.5);
    options.insert(QStringLiteral("outputSharpenThreshold"), 0.0);
    options.insert(QStringLiteral("watermarkEnabled"), false);
    options.insert(QStringLiteral("watermarkText"), QStringLiteral("RAVO"));
    options.insert(QStringLiteral("watermarkOpacity"), 0.5);
    options.insert(QStringLiteral("watermarkScale"), 8.0);
    options.insert(QStringLiteral("watermarkAlignment"), QStringLiteral("bottom_right"));
    return options;
}

[[nodiscard]] QVariantMap tiff_options(const QString &sample, const QString &compression,
                                       const int level, const bool grayscale, const int dpi,
                                       const QString &metadata = QStringLiteral("full"),
                                       const int max_edge = 0)
{
    QVariantMap options;
    options.insert(QStringLiteral("tiffSampleType"), sample);
    options.insert(QStringLiteral("tiffCompression"), compression);
    options.insert(QStringLiteral("tiffCompressionLevel"), level);
    options.insert(QStringLiteral("tiffGrayscaleIfNeutral"), grayscale);
    options.insert(QStringLiteral("tiffResolutionDpi"), dpi);
    options.insert(QStringLiteral("metadataMode"), metadata);
    options.insert(QStringLiteral("maxEdge"), max_edge);
    options.insert(QStringLiteral("maxWidth"), 0);
    options.insert(QStringLiteral("maxHeight"), 0);
    options.insert(QStringLiteral("outputSharpenEnabled"), false);
    options.insert(QStringLiteral("outputSharpenAmount"), 0.5);
    options.insert(QStringLiteral("outputSharpenRadius"), 0.5);
    options.insert(QStringLiteral("outputSharpenThreshold"), 0.0);
    options.insert(QStringLiteral("watermarkEnabled"), false);
    options.insert(QStringLiteral("watermarkText"), QStringLiteral("RAVO"));
    options.insert(QStringLiteral("watermarkOpacity"), 0.5);
    options.insert(QStringLiteral("watermarkScale"), 8.0);
    options.insert(QStringLiteral("watermarkAlignment"), QStringLiteral("bottom_right"));
    return options;
}

void expect_reason(const TaskError &error, const std::string_view reason)
{
    const auto found = error.context.find("reason");
    ASSERT_NE(found, error.context.end());
    EXPECT_EQ(found->second, reason);
}

TEST(ExportOptionConversion, DefaultsMatchDomainDefaults)
{
    const auto defaults = studio_export_default_options();
    EXPECT_EQ(defaults.value(QStringLiteral("format")).toString(), QStringLiteral("jpeg"));
    auto jpeg = studio_export_options_from_presentation(
        QStringLiteral("jpeg"),
        jpeg_options(defaults.value(QStringLiteral("quality")).toInt(),
                     defaults.value(QStringLiteral("jpegSubsampling")).toString()));
    ASSERT_TRUE(jpeg) << jpeg.error().message;
    EXPECT_EQ(jpeg.value().format, ExportFormat::kJpeg);
    EXPECT_EQ(jpeg.value().jpeg_options.quality, kDefaultJpegQuality);
    EXPECT_EQ(jpeg.value().jpeg_options.subsampling, JpegSubsampling::kAuto);
    EXPECT_EQ(jpeg.value().metadata_mode, ExportMetadataMode::kFull);
    EXPECT_EQ(jpeg.value().max_edge, 0U);
    EXPECT_EQ(defaults.value(QStringLiteral("maxEdge")).toInt(), 0);
    EXPECT_EQ(defaults.value(QStringLiteral("maxWidth")).toInt(), 0);
    EXPECT_EQ(defaults.value(QStringLiteral("maxHeight")).toInt(), 0);
    EXPECT_FALSE(defaults.value(QStringLiteral("outputSharpenEnabled")).toBool());
    EXPECT_FALSE(defaults.value(QStringLiteral("watermarkEnabled")).toBool());
    EXPECT_EQ(defaults.value(QStringLiteral("watermarkText")).toString(), QStringLiteral("RAVO"));
    EXPECT_FALSE(jpeg.value().watermark.enabled);
    EXPECT_EQ(jpeg.value().max_width, 0U);
    EXPECT_EQ(jpeg.value().max_height, 0U);
    EXPECT_FALSE(jpeg.value().output_sharpen.enabled);

    auto png = studio_export_options_from_presentation(
        QStringLiteral("png"),
        png_options(defaults.value(QStringLiteral("pngBitDepth")).toString(),
                    defaults.value(QStringLiteral("pngCompression")).toInt()));
    ASSERT_TRUE(png) << png.error().message;
    EXPECT_EQ(png.value().png_options.bit_depth, PngBitDepth::k8);
    EXPECT_EQ(png.value().png_options.compression, kDefaultPngCompression);

    auto tiff = studio_export_options_from_presentation(
        QStringLiteral("tiff"),
        tiff_options(defaults.value(QStringLiteral("tiffSampleType")).toString(),
                     defaults.value(QStringLiteral("tiffCompression")).toString(),
                     defaults.value(QStringLiteral("tiffCompressionLevel")).toInt(),
                     defaults.value(QStringLiteral("tiffGrayscaleIfNeutral")).toBool(),
                     defaults.value(QStringLiteral("tiffResolutionDpi")).toInt()));
    ASSERT_TRUE(tiff) << tiff.error().message;
    EXPECT_EQ(tiff.value().tiff_options.sample_type, TiffSampleType::kUint8);
    EXPECT_EQ(tiff.value().tiff_options.compression, TiffCompression::kDeflatePredictor);
    EXPECT_EQ(tiff.value().tiff_options.compression_level, kDefaultTiffCompressionLevel);
    EXPECT_FALSE(tiff.value().tiff_options.grayscale_if_neutral);
    EXPECT_EQ(tiff.value().tiff_options.resolution_dpi, kDefaultTiffResolutionDpi);
    EXPECT_EQ(defaults.value(QStringLiteral("metadataMode")).toString(), QStringLiteral("full"));

    auto original = studio_export_options_from_presentation(QStringLiteral("original"), {});
    ASSERT_TRUE(original) << original.error().message;
    EXPECT_EQ(original.value().format, ExportFormat::kOriginalCopy);
}

TEST(ExportOptionConversion, MetadataPrivacyModesMapAndRejectUnknownValues)
{
    auto no_location = studio_export_options_from_presentation(
        QStringLiteral("jpeg"),
        jpeg_options(95, QStringLiteral("auto"), QStringLiteral("no-location")));
    ASSERT_TRUE(no_location) << no_location.error().message;
    EXPECT_EQ(no_location.value().metadata_mode, ExportMetadataMode::kNoLocation);
    auto none = studio_export_options_from_presentation(
        QStringLiteral("png"), png_options(QStringLiteral("8"), 5, QStringLiteral("none")));
    ASSERT_TRUE(none) << none.error().message;
    EXPECT_EQ(none.value().metadata_mode, ExportMetadataMode::kNone);
    auto unknown = studio_export_options_from_presentation(
        QStringLiteral("tiff"), tiff_options(QStringLiteral("uint8"), QStringLiteral("deflate"), 6,
                                             false, 300, QStringLiteral("private-ish")));
    ASSERT_FALSE(unknown);
    expect_reason(unknown.error(), "invalid_export_metadata_mode");
}

TEST(ExportOptionConversion, ExplicitCanonicalValuesMapToTypedOptions)
{
    struct JpegCase
    {
        int quality = 0;
        const char *name = nullptr;
        JpegSubsampling expected = JpegSubsampling::kAuto;
    };
    for (const auto &test : {
             JpegCase{5, "auto", JpegSubsampling::kAuto},
             JpegCase{85, "444", JpegSubsampling::k444},
             JpegCase{90, "440", JpegSubsampling::k440},
             JpegCase{95, "422", JpegSubsampling::k422},
             JpegCase{100, "420", JpegSubsampling::k420},
         })
    {
        auto result = studio_export_options_from_presentation(
            QStringLiteral("jpeg"), jpeg_options(test.quality, QString::fromLatin1(test.name)));
        ASSERT_TRUE(result) << test.name << " " << result.error().message;
        EXPECT_EQ(result.value().jpeg_options.quality, test.quality);
        EXPECT_EQ(result.value().jpeg_options.subsampling, test.expected);
    }

    for (const auto &[name, expected] : {std::pair{"8", PngBitDepth::k8}, {"16", PngBitDepth::k16}})
    {
        auto result = studio_export_options_from_presentation(
            QStringLiteral("png"), png_options(QString::fromLatin1(name), 0));
        ASSERT_TRUE(result) << name;
        EXPECT_EQ(result.value().png_options.bit_depth, expected);
        EXPECT_EQ(result.value().png_options.compression, 0);
    }
    auto png_max = studio_export_options_from_presentation(QStringLiteral("png"),
                                                           png_options(QStringLiteral("8"), 9));
    ASSERT_TRUE(png_max);
    EXPECT_EQ(png_max.value().png_options.compression, 9);

    struct TiffCase
    {
        const char *sample = nullptr;
        const char *compression = nullptr;
        int level = 0;
        bool grayscale = false;
        int dpi = 0;
        TiffSampleType expected_sample = TiffSampleType::kUint8;
        TiffCompression expected_compression = TiffCompression::kNone;
    };
    for (const auto &test : {
             TiffCase{"uint8", "none", 1, false, 72, TiffSampleType::kUint8,
                      TiffCompression::kNone},
             TiffCase{"uint16", "deflate", 9, true, 300, TiffSampleType::kUint16,
                      TiffCompression::kDeflate},
             TiffCase{"float16", "deflate_predictor", 6, false, 9600, TiffSampleType::kFloat16,
                      TiffCompression::kDeflatePredictor},
             TiffCase{"float32", "deflate", 3, true, 240, TiffSampleType::kFloat32,
                      TiffCompression::kDeflate},
         })
    {
        auto result = studio_export_options_from_presentation(
            QStringLiteral("tiff"),
            tiff_options(QString::fromLatin1(test.sample), QString::fromLatin1(test.compression),
                         test.level, test.grayscale, test.dpi));
        ASSERT_TRUE(result) << test.sample << " " << result.error().message;
        EXPECT_EQ(result.value().tiff_options.sample_type, test.expected_sample);
        EXPECT_EQ(result.value().tiff_options.compression, test.expected_compression);
        EXPECT_EQ(result.value().tiff_options.compression_level, test.level);
        EXPECT_EQ(result.value().tiff_options.grayscale_if_neutral, test.grayscale);
        EXPECT_EQ(result.value().tiff_options.resolution_dpi, test.dpi);
    }
}

TEST(ExportOptionConversion, RejectsUnknownMissingAndNonApplicableKeys)
{
    auto unknown_format = studio_export_options_from_presentation(
        QStringLiteral("jpg"), jpeg_options(95, QStringLiteral("auto")));
    ASSERT_FALSE(unknown_format);
    expect_reason(unknown_format.error(), "studio_export_invalid_format");

    auto padded_format = studio_export_options_from_presentation(
        QStringLiteral(" jpeg "), jpeg_options(95, QStringLiteral("auto")));
    ASSERT_FALSE(padded_format);
    expect_reason(padded_format.error(), "studio_export_invalid_format");

    auto extra_jpeg = jpeg_options(95, QStringLiteral("auto"));
    extra_jpeg.insert(QStringLiteral("pngBitDepth"), QStringLiteral("8"));
    auto unknown = studio_export_options_from_presentation(QStringLiteral("jpeg"), extra_jpeg);
    ASSERT_FALSE(unknown);
    expect_reason(unknown.error(), "studio_export_unknown_option");

    auto missing = jpeg_options(95, QStringLiteral("auto"));
    missing.remove(QStringLiteral("quality"));
    auto missing_result = studio_export_options_from_presentation(QStringLiteral("jpeg"), missing);
    ASSERT_FALSE(missing_result);
    expect_reason(missing_result.error(), "studio_export_missing_option");

    auto original_with_options = studio_export_options_from_presentation(
        QStringLiteral("original"), jpeg_options(95, QStringLiteral("auto")));
    ASSERT_FALSE(original_with_options);
    expect_reason(original_with_options.error(), "studio_export_unknown_option");

    auto png_with_jpeg = png_options(QStringLiteral("8"), 5);
    png_with_jpeg.insert(QStringLiteral("quality"), 95);
    auto png = studio_export_options_from_presentation(QStringLiteral("png"), png_with_jpeg);
    ASSERT_FALSE(png);
    expect_reason(png.error(), "studio_export_unknown_option");
}

TEST(ExportOptionConversion, RejectsTypeMismatchFractionalOverflowAndBounds)
{
    auto bool_quality = jpeg_options(95, QStringLiteral("auto"));
    bool_quality.insert(QStringLiteral("quality"), true);
    auto bool_result =
        studio_export_options_from_presentation(QStringLiteral("jpeg"), bool_quality);
    ASSERT_FALSE(bool_result);
    expect_reason(bool_result.error(), "studio_export_invalid_option_type");

    auto fractional = jpeg_options(95, QStringLiteral("auto"));
    fractional.insert(QStringLiteral("quality"), 95.5);
    auto fractional_result =
        studio_export_options_from_presentation(QStringLiteral("jpeg"), fractional);
    ASSERT_FALSE(fractional_result);
    expect_reason(fractional_result.error(), "studio_export_invalid_option_type");

    auto overflow = jpeg_options(95, QStringLiteral("auto"));
    overflow.insert(
        QStringLiteral("quality"),
        QVariant::fromValue(static_cast<qlonglong>(std::numeric_limits<int>::max()) + 1));
    auto overflow_result =
        studio_export_options_from_presentation(QStringLiteral("jpeg"), overflow);
    ASSERT_FALSE(overflow_result);
    expect_reason(overflow_result.error(), "studio_export_invalid_option_type");

    auto string_quality = jpeg_options(95, QStringLiteral("auto"));
    string_quality.insert(QStringLiteral("quality"), QStringLiteral("95"));
    auto string_result =
        studio_export_options_from_presentation(QStringLiteral("jpeg"), string_quality);
    ASSERT_FALSE(string_result);
    expect_reason(string_result.error(), "studio_export_invalid_option_type");

    auto int_subsampling = jpeg_options(95, QStringLiteral("auto"));
    int_subsampling.insert(QStringLiteral("jpegSubsampling"), 444);
    auto int_enum =
        studio_export_options_from_presentation(QStringLiteral("jpeg"), int_subsampling);
    ASSERT_FALSE(int_enum);
    expect_reason(int_enum.error(), "studio_export_invalid_option_type");

    auto low = studio_export_options_from_presentation(QStringLiteral("jpeg"),
                                                       jpeg_options(4, QStringLiteral("auto")));
    ASSERT_FALSE(low);
    const auto low_reason = low.error().context.find("reason");
    ASSERT_NE(low_reason, low.error().context.end());
    EXPECT_EQ(low_reason->second, "invalid_jpeg_quality");

    auto high_dpi = studio_export_options_from_presentation(
        QStringLiteral("tiff"),
        tiff_options(QStringLiteral("uint8"), QStringLiteral("deflate"), 6, false, 9601));
    ASSERT_FALSE(high_dpi);
    const auto dpi_reason = high_dpi.error().context.find("reason");
    ASSERT_NE(dpi_reason, high_dpi.error().context.end());
    EXPECT_EQ(dpi_reason->second, "invalid_tiff_resolution");

    auto int_grayscale =
        tiff_options(QStringLiteral("uint8"), QStringLiteral("none"), 6, false, 300);
    int_grayscale.insert(QStringLiteral("tiffGrayscaleIfNeutral"), 1);
    auto grayscale_type =
        studio_export_options_from_presentation(QStringLiteral("tiff"), int_grayscale);
    ASSERT_FALSE(grayscale_type);
    expect_reason(grayscale_type.error(), "studio_export_invalid_option_type");
}

TEST(ExportOptionConversion, InputMapIsNotMutated)
{
    const auto original = jpeg_options(85, QStringLiteral("422"));
    QVariantMap input = original;
    auto result = studio_export_options_from_presentation(QStringLiteral("jpeg"), input);
    ASSERT_TRUE(result);
    EXPECT_EQ(input, original);
}

TEST(ExportOptionConversion, PathNormalizationFollowsExplicitFormat)
{
    auto jpeg_plain = normalize_studio_export_path(QStringLiteral("/tmp/out"), ExportFormat::kJpeg);
    ASSERT_TRUE(jpeg_plain);
    EXPECT_EQ(jpeg_plain.value(), QStringLiteral("/tmp/out.jpg"));

    auto png_plain = normalize_studio_export_path(QStringLiteral("/tmp/out"), ExportFormat::kPng);
    ASSERT_TRUE(png_plain);
    EXPECT_EQ(png_plain.value(), QStringLiteral("/tmp/out.png"));

    auto tiff_plain = normalize_studio_export_path(QStringLiteral("/tmp/out"), ExportFormat::kTiff);
    ASSERT_TRUE(tiff_plain);
    EXPECT_EQ(tiff_plain.value(), QStringLiteral("/tmp/out.tif"));

    for (const auto &path : {QStringLiteral("/tmp/photo.jpg"), QStringLiteral("/tmp/photo.jpeg"),
                             QStringLiteral("/tmp/photo.JPG"), QStringLiteral("/tmp/photo.JPEG")})
    {
        auto accepted = normalize_studio_export_path(path, ExportFormat::kJpeg);
        ASSERT_TRUE(accepted) << path.toStdString();
        EXPECT_EQ(accepted.value(), path);
    }
    for (const auto &path : {QStringLiteral("/tmp/photo.tif"), QStringLiteral("/tmp/photo.tiff"),
                             QStringLiteral("/tmp/photo.TIF"), QStringLiteral("/tmp/photo.TIFF")})
    {
        auto accepted = normalize_studio_export_path(path, ExportFormat::kTiff);
        ASSERT_TRUE(accepted) << path.toStdString();
        EXPECT_EQ(accepted.value(), path);
    }
    auto png = normalize_studio_export_path(QStringLiteral("/tmp/photo.PNG"), ExportFormat::kPng);
    ASSERT_TRUE(png);
    EXPECT_EQ(png.value(), QStringLiteral("/tmp/photo.PNG"));

    auto original = normalize_studio_export_path(QStringLiteral("/tmp/photo.raw.bak"),
                                                 ExportFormat::kOriginalCopy);
    ASSERT_TRUE(original);
    EXPECT_EQ(original.value(), QStringLiteral("/tmp/photo.raw.bak"));

    auto empty = normalize_studio_export_path(QStringLiteral("   "), ExportFormat::kPng);
    ASSERT_FALSE(empty);
    expect_reason(empty.error(), "studio_export_empty_path");

    auto mismatch =
        normalize_studio_export_path(QStringLiteral("/tmp/photo.png"), ExportFormat::kJpeg);
    ASSERT_FALSE(mismatch);
    expect_reason(mismatch.error(), "studio_export_extension_mismatch");

    auto file_url =
        normalize_studio_export_path(QStringLiteral("file:///tmp/export"), ExportFormat::kPng);
    ASSERT_TRUE(file_url);
    EXPECT_EQ(file_url.value(), QStringLiteral("/tmp/export.png"));
}

TEST(ExportOptionConversion, RequestHelperCopiesTypedSnapshot)
{
    auto request =
        make_studio_export_request("asset-1", QStringLiteral("/tmp/export"), QStringLiteral("jpeg"),
                                   jpeg_options(80, QStringLiteral("420")));
    ASSERT_TRUE(request) << request.error().message;
    EXPECT_EQ(request.value().asset_id, "asset-1");
    EXPECT_EQ(request.value().output_path, "/tmp/export.jpg");
    EXPECT_EQ(request.value().format, ExportFormat::kJpeg);
    EXPECT_EQ(request.value().jpeg_options.quality, 80);
    EXPECT_EQ(request.value().jpeg_options.subsampling, JpegSubsampling::k420);
    EXPECT_EQ(request.value().max_edge, 0U);
    EXPECT_FALSE(request.value().cancellation.is_cancellation_requested());

    auto batch_options =
        make_studio_export_options(QStringLiteral("png"), png_options(QStringLiteral("16"), 7));
    ASSERT_TRUE(batch_options) << batch_options.error().message;
    EXPECT_EQ(batch_options.value().format, ExportFormat::kPng);
    EXPECT_EQ(batch_options.value().png_options.bit_depth, PngBitDepth::k16);
    EXPECT_EQ(batch_options.value().png_options.compression, 7);
    EXPECT_EQ(batch_options.value().max_edge, 0U);

    auto sized = make_studio_export_options(
        QStringLiteral("jpeg"),
        jpeg_options(90, QStringLiteral("auto"), QStringLiteral("full"), 2048));
    ASSERT_TRUE(sized) << sized.error().message;
    EXPECT_EQ(sized.value().max_edge, 2048U);

    auto oversized = studio_export_options_from_presentation(
        QStringLiteral("png"), png_options(QStringLiteral("8"), 5, QStringLiteral("full"), 65536));
    ASSERT_FALSE(oversized);
    expect_reason(oversized.error(), "studio_export_invalid_max_edge");

    auto original_resize = studio_export_options_from_presentation(
        QStringLiteral("original"), QVariantMap{{QStringLiteral("maxEdge"), 1024}});
    ASSERT_FALSE(original_resize);
    expect_reason(original_resize.error(), "studio_export_unknown_option");
}

TEST(ExportOptionConversion, AcceptsBoxAndOutputSharpen)
{
    auto options = jpeg_options(90, QStringLiteral("auto"));
    options.insert(QStringLiteral("maxWidth"), 1280);
    options.insert(QStringLiteral("maxHeight"), 720);
    options.insert(QStringLiteral("outputSharpenEnabled"), true);
    options.insert(QStringLiteral("outputSharpenAmount"), 0.8);
    options.insert(QStringLiteral("outputSharpenRadius"), 0.7);
    options.insert(QStringLiteral("outputSharpenThreshold"), 1.5);
    auto parsed = studio_export_options_from_presentation(QStringLiteral("jpeg"), options);
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed.value().max_width, 1280U);
    EXPECT_EQ(parsed.value().max_height, 720U);
    EXPECT_TRUE(parsed.value().output_sharpen.enabled);
    EXPECT_DOUBLE_EQ(parsed.value().output_sharpen.amount, 0.8);
    EXPECT_DOUBLE_EQ(parsed.value().output_sharpen.radius, 0.7);
    EXPECT_DOUBLE_EQ(parsed.value().output_sharpen.threshold, 1.5);
}

TEST(ExportOptionConversion, AcceptsDeliveryWatermark)
{
    auto options = jpeg_options(90, QStringLiteral("auto"));
    options.insert(QStringLiteral("watermarkEnabled"), true);
    options.insert(QStringLiteral("watermarkText"), QStringLiteral("HELLO"));
    options.insert(QStringLiteral("watermarkOpacity"), 0.75);
    options.insert(QStringLiteral("watermarkScale"), 12.0);
    options.insert(QStringLiteral("watermarkAlignment"), QStringLiteral("top_left"));
    auto parsed = studio_export_options_from_presentation(QStringLiteral("jpeg"), options);
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_TRUE(parsed.value().watermark.enabled);
    EXPECT_EQ(parsed.value().watermark.text, "HELLO");
    EXPECT_DOUBLE_EQ(parsed.value().watermark.opacity, 0.75);
    EXPECT_DOUBLE_EQ(parsed.value().watermark.scale_percent, 12.0);
    EXPECT_EQ(parsed.value().watermark.alignment, "top_left");
}

TEST(ExportOptionConversion, PresentationCatalogExposesCanonicalIds)
{
    const auto formats = studio_export_format_choices();
    ASSERT_EQ(formats.size(), 4);
    EXPECT_EQ(formats.at(0).toMap().value(QStringLiteral("id")).toString(), QStringLiteral("jpeg"));
    EXPECT_EQ(formats.at(1).toMap().value(QStringLiteral("id")).toString(), QStringLiteral("png"));
    EXPECT_EQ(formats.at(2).toMap().value(QStringLiteral("id")).toString(), QStringLiteral("tiff"));
    EXPECT_EQ(formats.at(3).toMap().value(QStringLiteral("id")).toString(),
              QStringLiteral("original"));

    const auto bounds = studio_export_option_bounds();
    EXPECT_EQ(bounds.value(QStringLiteral("jpegQualityMin")).toInt(), kJpegQualityMin);
    EXPECT_EQ(bounds.value(QStringLiteral("jpegQualityMax")).toInt(), kJpegQualityMax);
    EXPECT_EQ(bounds.value(QStringLiteral("tiffResolutionDpiMin")).toInt(), kTiffResolutionDpiMin);
    EXPECT_EQ(bounds.value(QStringLiteral("tiffResolutionDpiMax")).toInt(), kTiffResolutionDpiMax);
    EXPECT_EQ(bounds.value(QStringLiteral("maxEdgeMin")).toInt(),
              static_cast<int>(kExportMaxEdgeMin));
    EXPECT_EQ(bounds.value(QStringLiteral("maxEdgeMax")).toInt(),
              static_cast<int>(kExportMaxEdgeMax));
}

} // namespace
} // namespace ravo
