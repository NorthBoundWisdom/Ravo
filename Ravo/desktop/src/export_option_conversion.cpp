#include "ravo/desktop/export_option_conversion.h"

#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QFileInfo>
#include <QMetaType>
#include <QUrl>
#include <QVariant>

#include "studio_qt.h"

namespace ravo
{
namespace
{

[[nodiscard]] QVariantMap choice(const QString &id, const QString &label)
{
    QVariantMap item;
    item.insert(QStringLiteral("id"), id);
    item.insert(QStringLiteral("label"), label);
    return item;
}

[[nodiscard]] std::string utf8(const QString &text)
{
    return utf8_from_qstring(text);
}

[[nodiscard]] TaskError conversion_error(const char *message,
                                         std::map<std::string, std::string, std::less<>> context)
{
    return make_error(ErrorCode::kValidation, message, std::move(context));
}

[[maybe_unused]] const char *const kStudioExportDomainTranslationSources[] = {
    QT_TRANSLATE_NOOP("StudioExport", "Unknown JPEG subsampling mode"),
    QT_TRANSLATE_NOOP("StudioExport", "JPEG quality must be between 5 and 100"),
    QT_TRANSLATE_NOOP("StudioExport", "Unknown PNG bit depth"),
    QT_TRANSLATE_NOOP("StudioExport", "PNG compression must be between 0 and 9"),
    QT_TRANSLATE_NOOP("StudioExport", "Unknown TIFF sample type"),
    QT_TRANSLATE_NOOP("StudioExport", "Unknown TIFF compression mode"),
    QT_TRANSLATE_NOOP("StudioExport", "TIFF compression level must be between 1 and 9"),
    QT_TRANSLATE_NOOP("StudioExport", "TIFF resolution must be between 72 and 9600 DPI"),
    QT_TRANSLATE_NOOP("StudioExport", "Long edge must be between 0 and 65535")};

[[nodiscard]] Result<int> exact_integer(const QVariant &value, const std::string_view key,
                                        const std::string_view format,
                                        const std::string_view reason)
{
    const auto fail = [&](const char *detail)
    {
        return Result<int>{conversion_error(detail, {{"field", std::string(key)},
                                                     {"format", std::string(format)},
                                                     {"reason", std::string(reason)}})};
    };
    switch (value.metaType().id())
    {
    case QMetaType::Bool:
        return fail(QT_TRANSLATE_NOOP("StudioExport", "Export option must be an integer"));
    case QMetaType::Int:
        return value.toInt();
    case QMetaType::UInt:
    {
        const auto number = value.toUInt();
        if (number > static_cast<unsigned int>(std::numeric_limits<int>::max()))
        {
            return fail(
                QT_TRANSLATE_NOOP("StudioExport", "Export option is outside the integer range"));
        }
        return static_cast<int>(number);
    }
    case QMetaType::LongLong:
    {
        const auto number = value.toLongLong();
        if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max())
        {
            return fail(
                QT_TRANSLATE_NOOP("StudioExport", "Export option is outside the integer range"));
        }
        return static_cast<int>(number);
    }
    case QMetaType::ULongLong:
    {
        const auto number = value.toULongLong();
        if (number > static_cast<unsigned long long>(std::numeric_limits<int>::max()))
        {
            return fail(
                QT_TRANSLATE_NOOP("StudioExport", "Export option is outside the integer range"));
        }
        return static_cast<int>(number);
    }
    case QMetaType::Double:
    case QMetaType::Float:
    {
        const double number = value.toDouble();
        if (!std::isfinite(number) || std::floor(number) != number)
        {
            return fail(QT_TRANSLATE_NOOP("StudioExport", "Export option must be a whole number"));
        }
        if (number < static_cast<double>(std::numeric_limits<int>::min()) ||
            number > static_cast<double>(std::numeric_limits<int>::max()))
        {
            return fail(
                QT_TRANSLATE_NOOP("StudioExport", "Export option is outside the integer range"));
        }
        return static_cast<int>(number);
    }
    default:
        return fail(
            QT_TRANSLATE_NOOP("StudioExport", "Export option has an unsupported value type"));
    }
}

[[nodiscard]] Result<double> exact_double(const QVariant &value, const std::string_view key,
                                          const std::string_view format,
                                          const std::string_view reason)
{
    const auto fail = [&](const char *detail)
    {
        return Result<double>{conversion_error(detail, {{"field", std::string(key)},
                                                        {"format", std::string(format)},
                                                        {"reason", std::string(reason)}})};
    };
    switch (value.metaType().id())
    {
    case QMetaType::Bool:
        return fail(QT_TRANSLATE_NOOP("StudioExport", "Export option must be a number"));
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Double:
    case QMetaType::Float:
    {
        const double number = value.toDouble();
        if (!std::isfinite(number))
            return fail(QT_TRANSLATE_NOOP("StudioExport", "Export option must be a finite number"));
        return number;
    }
    default:
        return fail(
            QT_TRANSLATE_NOOP("StudioExport", "Export option has an unsupported value type"));
    }
}

[[nodiscard]] Result<bool> exact_bool(const QVariant &value, const std::string_view key)
{
    if (value.metaType().id() != QMetaType::Bool)
    {
        return conversion_error(
            QT_TRANSLATE_NOOP("StudioExport", "Export option must be a boolean"),
            {{"field", std::string(key)},
             {"format", "tiff"},
             {"reason", "studio_export_invalid_option_type"}});
    }
    return value.toBool();
}

[[nodiscard]] Result<std::string> exact_string(const QVariant &value, const std::string_view key,
                                               const std::string_view format)
{
    if (value.metaType().id() != QMetaType::QString)
    {
        return conversion_error(
            QT_TRANSLATE_NOOP("StudioExport", "Export option must be a canonical name"),
            {{"field", std::string(key)},
             {"format", std::string(format)},
             {"reason", "studio_export_invalid_option_type"}});
    }
    return utf8(value.toString());
}

[[nodiscard]] Result<void> require_keys(const QVariantMap &options,
                                        const std::set<std::string_view> &allowed,
                                        const std::string_view format)
{
    std::set<std::string> seen;
    for (auto it = options.constBegin(); it != options.constEnd(); ++it)
    {
        const auto key = utf8(it.key());
        if (allowed.find(std::string_view(key)) == allowed.end())
        {
            return conversion_error(
                QT_TRANSLATE_NOOP("StudioExport",
                                  "Export option is not valid for the selected format"),
                {{"field", key},
                 {"format", std::string(format)},
                 {"reason", "studio_export_unknown_option"}});
        }
        seen.insert(key);
    }
    for (const auto key : allowed)
    {
        if (seen.find(std::string(key)) == seen.end())
        {
            return conversion_error(
                QT_TRANSLATE_NOOP("StudioExport",
                                  "Export option is required for the selected format"),
                {{"field", std::string(key)},
                 {"format", std::string(format)},
                 {"reason", "studio_export_missing_option"}});
        }
    }
    return {};
}

[[nodiscard]] QString local_path(const QString &path)
{
    QString output = path.trimmed();
    if (output.startsWith(QStringLiteral("file:")))
    {
        output = QUrl(output).toLocalFile();
    }
    return output;
}

} // namespace

QVariantList studio_export_format_choices()
{
    return {
        choice(QStringLiteral("jpeg"), QCoreApplication::translate("ExportOptionsDialog", "JPEG")),
        choice(QStringLiteral("png"), QCoreApplication::translate("ExportOptionsDialog", "PNG")),
        choice(QStringLiteral("tiff"), QCoreApplication::translate("ExportOptionsDialog", "TIFF")),
        choice(QStringLiteral("original"),
               QCoreApplication::translate("ExportOptionsDialog", "Original copy"))};
}

QVariantList studio_jpeg_subsampling_choices()
{
    return {
        choice(QStringLiteral("auto"),
               QCoreApplication::translate("ExportOptionsDialog", "Automatic")),
        choice(QStringLiteral("444"), QCoreApplication::translate("ExportOptionsDialog", "4:4:4")),
        choice(QStringLiteral("440"), QCoreApplication::translate("ExportOptionsDialog", "4:4:0")),
        choice(QStringLiteral("422"), QCoreApplication::translate("ExportOptionsDialog", "4:2:2")),
        choice(QStringLiteral("420"), QCoreApplication::translate("ExportOptionsDialog", "4:2:0"))};
}

QVariantList studio_png_bit_depth_choices()
{
    return {
        choice(QStringLiteral("8"), QCoreApplication::translate("ExportOptionsDialog", "8-bit")),
        choice(QStringLiteral("16"), QCoreApplication::translate("ExportOptionsDialog", "16-bit"))};
}

QVariantList studio_tiff_sample_type_choices()
{
    return {choice(QStringLiteral("uint8"),
                   QCoreApplication::translate("ExportOptionsDialog", "Unsigned 8-bit")),
            choice(QStringLiteral("uint16"),
                   QCoreApplication::translate("ExportOptionsDialog", "Unsigned 16-bit")),
            choice(QStringLiteral("float16"),
                   QCoreApplication::translate("ExportOptionsDialog", "16-bit float")),
            choice(QStringLiteral("float32"),
                   QCoreApplication::translate("ExportOptionsDialog", "32-bit float"))};
}

QVariantList studio_tiff_compression_choices()
{
    return {
        choice(QStringLiteral("none"), QCoreApplication::translate("ExportOptionsDialog", "None")),
        choice(QStringLiteral("deflate"),
               QCoreApplication::translate("ExportOptionsDialog", "Deflate")),
        choice(QStringLiteral("deflate_predictor"),
               QCoreApplication::translate("ExportOptionsDialog", "Deflate + predictor"))};
}

QVariantList studio_export_metadata_mode_choices()
{
    return {choice(QStringLiteral("full"),
                   QCoreApplication::translate("ExportOptionsDialog", "Full metadata")),
            choice(QStringLiteral("no-location"),
                   QCoreApplication::translate("ExportOptionsDialog", "Without location")),
            choice(QStringLiteral("none"),
                   QCoreApplication::translate("ExportOptionsDialog", "No public metadata"))};
}

QVariantList studio_export_watermark_alignment_choices()
{
    const auto choice = [](const QString &id, const QString &label)
    { return QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("label"), label}}; };
    return {
        choice(QStringLiteral("top_left"),
               QCoreApplication::translate("ExportOptionsDialog", "Top left")),
        choice(QStringLiteral("top_center"),
               QCoreApplication::translate("ExportOptionsDialog", "Top center")),
        choice(QStringLiteral("top_right"),
               QCoreApplication::translate("ExportOptionsDialog", "Top right")),
        choice(QStringLiteral("center_left"),
               QCoreApplication::translate("ExportOptionsDialog", "Center left")),
        choice(QStringLiteral("center"),
               QCoreApplication::translate("ExportOptionsDialog", "Center")),
        choice(QStringLiteral("center_right"),
               QCoreApplication::translate("ExportOptionsDialog", "Center right")),
        choice(QStringLiteral("bottom_left"),
               QCoreApplication::translate("ExportOptionsDialog", "Bottom left")),
        choice(QStringLiteral("bottom_center"),
               QCoreApplication::translate("ExportOptionsDialog", "Bottom center")),
        choice(QStringLiteral("bottom_right"),
               QCoreApplication::translate("ExportOptionsDialog", "Bottom right")),
    };
}

QVariantList studio_export_output_profile_choices()
{
    QVariantList choices;
    for (const auto name : std::array<std::string_view, 11>{
             "srgb", "adobe_rgb", "linear_rec709", "linear_rec2020", "rec709", "prophoto_rgb",
             "pq_rec2020", "hlg_rec2020", "pq_p3", "hlg_p3", "display_p3"})
    {
        QVariantMap item;
        item.insert(QStringLiteral("id"),
                    QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
        item.insert(QStringLiteral("label"),
                    QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
        choices.push_back(item);
    }
    return choices;
}

QVariantList studio_export_rendering_intent_choices()
{
    QVariantList choices;
    for (const auto name : std::array<std::string_view, 4>{"perceptual", "relative_colorimetric",
                                                           "saturation", "absolute_colorimetric"})
    {
        QVariantMap item;
        item.insert(QStringLiteral("id"),
                    QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
        item.insert(QStringLiteral("label"),
                    QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
        choices.push_back(item);
    }
    return choices;
}

QVariantMap studio_export_default_options()
{
    QVariantMap options;
    options.insert(QStringLiteral("format"), QStringLiteral("jpeg"));
    options.insert(QString::fromUtf8(kStudioExportOptionQuality.data(),
                                     static_cast<qsizetype>(kStudioExportOptionQuality.size())),
                   kDefaultJpegQuality);
    options.insert(
        QString::fromUtf8(kStudioExportOptionJpegSubsampling.data(),
                          static_cast<qsizetype>(kStudioExportOptionJpegSubsampling.size())),
        QStringLiteral("auto"));
    options.insert(QString::fromUtf8(kStudioExportOptionPngBitDepth.data(),
                                     static_cast<qsizetype>(kStudioExportOptionPngBitDepth.size())),
                   QStringLiteral("8"));
    options.insert(
        QString::fromUtf8(kStudioExportOptionPngCompression.data(),
                          static_cast<qsizetype>(kStudioExportOptionPngCompression.size())),
        kDefaultPngCompression);
    options.insert(
        QString::fromUtf8(kStudioExportOptionTiffSampleType.data(),
                          static_cast<qsizetype>(kStudioExportOptionTiffSampleType.size())),
        QStringLiteral("uint8"));
    options.insert(
        QString::fromUtf8(kStudioExportOptionTiffCompression.data(),
                          static_cast<qsizetype>(kStudioExportOptionTiffCompression.size())),
        QStringLiteral("deflate_predictor"));
    options.insert(
        QString::fromUtf8(kStudioExportOptionTiffCompressionLevel.data(),
                          static_cast<qsizetype>(kStudioExportOptionTiffCompressionLevel.size())),
        kDefaultTiffCompressionLevel);
    options.insert(
        QString::fromUtf8(kStudioExportOptionTiffGrayscaleIfNeutral.data(),
                          static_cast<qsizetype>(kStudioExportOptionTiffGrayscaleIfNeutral.size())),
        false);
    options.insert(
        QString::fromUtf8(kStudioExportOptionTiffResolutionDpi.data(),
                          static_cast<qsizetype>(kStudioExportOptionTiffResolutionDpi.size())),
        kDefaultTiffResolutionDpi);
    options.insert(
        QString::fromUtf8(kStudioExportOptionMetadataMode.data(),
                          static_cast<qsizetype>(kStudioExportOptionMetadataMode.size())),
        QStringLiteral("full"));
    options.insert(QString::fromUtf8(kStudioExportOptionMaxEdge.data(),
                                     static_cast<qsizetype>(kStudioExportOptionMaxEdge.size())),
                   0);
    options.insert(QString::fromUtf8(kStudioExportOptionMaxWidth.data(),
                                     static_cast<qsizetype>(kStudioExportOptionMaxWidth.size())),
                   0);
    options.insert(QString::fromUtf8(kStudioExportOptionMaxHeight.data(),
                                     static_cast<qsizetype>(kStudioExportOptionMaxHeight.size())),
                   0);
    options.insert(
        QString::fromUtf8(kStudioExportOptionOutputSharpenEnabled.data(),
                          static_cast<qsizetype>(kStudioExportOptionOutputSharpenEnabled.size())),
        false);
    options.insert(
        QString::fromUtf8(kStudioExportOptionOutputSharpenAmount.data(),
                          static_cast<qsizetype>(kStudioExportOptionOutputSharpenAmount.size())),
        0.5);
    options.insert(
        QString::fromUtf8(kStudioExportOptionOutputSharpenRadius.data(),
                          static_cast<qsizetype>(kStudioExportOptionOutputSharpenRadius.size())),
        0.5);
    options.insert(
        QString::fromUtf8(kStudioExportOptionOutputSharpenThreshold.data(),
                          static_cast<qsizetype>(kStudioExportOptionOutputSharpenThreshold.size())),
        0.0);
    options.insert(
        QString::fromUtf8(kStudioExportOptionWatermarkEnabled.data(),
                          static_cast<qsizetype>(kStudioExportOptionWatermarkEnabled.size())),
        false);
    options.insert(
        QString::fromUtf8(kStudioExportOptionWatermarkText.data(),
                          static_cast<qsizetype>(kStudioExportOptionWatermarkText.size())),
        QStringLiteral("RAVO"));
    options.insert(
        QString::fromUtf8(kStudioExportOptionWatermarkOpacity.data(),
                          static_cast<qsizetype>(kStudioExportOptionWatermarkOpacity.size())),
        0.5);
    options.insert(
        QString::fromUtf8(kStudioExportOptionWatermarkScale.data(),
                          static_cast<qsizetype>(kStudioExportOptionWatermarkScale.size())),
        8.0);
    options.insert(
        QString::fromUtf8(kStudioExportOptionWatermarkAlignment.data(),
                          static_cast<qsizetype>(kStudioExportOptionWatermarkAlignment.size())),
        QStringLiteral("bottom_right"));
    options.insert(
        QString::fromUtf8(kStudioExportOptionOutputColorEnabled.data(),
                          static_cast<qsizetype>(kStudioExportOptionOutputColorEnabled.size())),
        false);
    options.insert(
        QString::fromUtf8(kStudioExportOptionOutputProfile.data(),
                          static_cast<qsizetype>(kStudioExportOptionOutputProfile.size())),
        QStringLiteral("srgb"));
    options.insert(
        QString::fromUtf8(kStudioExportOptionRenderingIntent.data(),
                          static_cast<qsizetype>(kStudioExportOptionRenderingIntent.size())),
        QStringLiteral("perceptual"));
    options.insert(
        QString::fromUtf8(kStudioExportOptionFrameEnabled.data(),
                          static_cast<qsizetype>(kStudioExportOptionFrameEnabled.size())),
        false);
    options.insert(QString::fromUtf8(kStudioExportOptionFrameSize.data(),
                                     static_cast<qsizetype>(kStudioExportOptionFrameSize.size())),
                   0.1);
    return options;
}

QVariantMap studio_export_option_bounds()
{
    QVariantMap bounds;
    bounds.insert(QStringLiteral("jpegQualityMin"), kJpegQualityMin);
    bounds.insert(QStringLiteral("jpegQualityMax"), kJpegQualityMax);
    bounds.insert(QStringLiteral("pngCompressionMin"), kPngCompressionMin);
    bounds.insert(QStringLiteral("pngCompressionMax"), kPngCompressionMax);
    bounds.insert(QStringLiteral("tiffCompressionLevelMin"), kTiffCompressionLevelMin);
    bounds.insert(QStringLiteral("tiffCompressionLevelMax"), kTiffCompressionLevelMax);
    bounds.insert(QStringLiteral("tiffResolutionDpiMin"), kTiffResolutionDpiMin);
    bounds.insert(QStringLiteral("tiffResolutionDpiMax"), kTiffResolutionDpiMax);
    bounds.insert(QStringLiteral("maxEdgeMin"), static_cast<int>(kExportMaxEdgeMin));
    bounds.insert(QStringLiteral("maxEdgeMax"), static_cast<int>(kExportMaxEdgeMax));
    bounds.insert(QStringLiteral("maxWidthMin"), static_cast<int>(kExportBoxLimitMin));
    bounds.insert(QStringLiteral("maxWidthMax"), static_cast<int>(kExportBoxLimitMax));
    bounds.insert(QStringLiteral("maxHeightMin"), static_cast<int>(kExportBoxLimitMin));
    bounds.insert(QStringLiteral("maxHeightMax"), static_cast<int>(kExportBoxLimitMax));
    bounds.insert(QStringLiteral("outputSharpenAmountMin"), kExportOutputSharpenAmountMin);
    bounds.insert(QStringLiteral("outputSharpenAmountMax"), kExportOutputSharpenAmountMax);
    bounds.insert(QStringLiteral("outputSharpenRadiusMin"), kExportOutputSharpenRadiusMin);
    bounds.insert(QStringLiteral("outputSharpenRadiusMax"), kExportOutputSharpenRadiusMax);
    bounds.insert(QStringLiteral("outputSharpenThresholdMin"), kExportOutputSharpenThresholdMin);
    bounds.insert(QStringLiteral("outputSharpenThresholdMax"), kExportOutputSharpenThresholdMax);
    bounds.insert(QStringLiteral("watermarkOpacityMin"), kExportWatermarkOpacityMin);
    bounds.insert(QStringLiteral("watermarkOpacityMax"), kExportWatermarkOpacityMax);
    bounds.insert(QStringLiteral("watermarkScaleMin"), kExportWatermarkScaleMin);
    bounds.insert(QStringLiteral("watermarkScaleMax"), kExportWatermarkScaleMax);
    bounds.insert(QStringLiteral("frameSizeMin"), 0.0);
    bounds.insert(QStringLiteral("frameSizeMax"), 0.5);
    return bounds;
}

Result<StudioExportSelection> studio_export_options_from_presentation(const QString &format_name,
                                                                      const QVariantMap &options)
{
    const QVariantMap input = options;
    const auto format_text = utf8(format_name);
    ExportFormat format = ExportFormat::kPng;
    if (format_text == "jpeg")
    {
        format = ExportFormat::kJpeg;
    }
    else if (format_text == "png")
    {
        format = ExportFormat::kPng;
    }
    else if (format_text == "tiff")
    {
        format = ExportFormat::kTiff;
    }
    else if (format_text == "original")
    {
        format = ExportFormat::kOriginalCopy;
    }
    else
    {
        return conversion_error(
            QT_TRANSLATE_NOOP("StudioExport", "Unknown export format"),
            {{"format", format_text}, {"reason", "studio_export_invalid_format"}});
    }

    StudioExportSelection selection;
    selection.format = format;
    switch (selection.format)
    {
    case ExportFormat::kJpeg:
    {
        auto keys = require_keys(input,
                                 {kStudioExportOptionQuality,
                                  kStudioExportOptionJpegSubsampling,
                                  kStudioExportOptionMetadataMode,
                                  kStudioExportOptionMaxEdge,
                                  kStudioExportOptionMaxWidth,
                                  kStudioExportOptionMaxHeight,
                                  kStudioExportOptionOutputSharpenEnabled,
                                  kStudioExportOptionOutputSharpenAmount,
                                  kStudioExportOptionOutputSharpenRadius,
                                  kStudioExportOptionOutputSharpenThreshold,
                                  kStudioExportOptionWatermarkEnabled,
                                  kStudioExportOptionWatermarkText,
                                  kStudioExportOptionWatermarkOpacity,
                                  kStudioExportOptionWatermarkScale,
                                  kStudioExportOptionWatermarkAlignment,
                                  kStudioExportOptionOutputColorEnabled,
                                  kStudioExportOptionOutputProfile,
                                  kStudioExportOptionRenderingIntent,
                                  kStudioExportOptionFrameEnabled,
                                  kStudioExportOptionFrameSize},
                                 "jpeg");
        if (!keys)
        {
            return keys.error();
        }
        auto quality =
            exact_integer(input.value(QStringLiteral("quality")), kStudioExportOptionQuality,
                          "jpeg", "studio_export_invalid_option_type");
        if (!quality)
        {
            return quality.error();
        }
        selection.jpeg_options.quality = quality.value();
        auto subsampling = exact_string(input.value(QStringLiteral("jpegSubsampling")),
                                        kStudioExportOptionJpegSubsampling, "jpeg");
        if (!subsampling)
        {
            return subsampling.error();
        }
        auto parsed = parse_jpeg_subsampling(subsampling.value());
        if (!parsed)
        {
            return parsed.error();
        }
        selection.jpeg_options.subsampling = parsed.value();
        auto valid = validate_jpeg_export_options(selection.jpeg_options);
        if (!valid)
        {
            return valid.error();
        }
        break;
    }
    case ExportFormat::kPng:
    {
        auto keys = require_keys(input,
                                 {kStudioExportOptionPngBitDepth,
                                  kStudioExportOptionPngCompression,
                                  kStudioExportOptionMetadataMode,
                                  kStudioExportOptionMaxEdge,
                                  kStudioExportOptionMaxWidth,
                                  kStudioExportOptionMaxHeight,
                                  kStudioExportOptionOutputSharpenEnabled,
                                  kStudioExportOptionOutputSharpenAmount,
                                  kStudioExportOptionOutputSharpenRadius,
                                  kStudioExportOptionOutputSharpenThreshold,
                                  kStudioExportOptionWatermarkEnabled,
                                  kStudioExportOptionWatermarkText,
                                  kStudioExportOptionWatermarkOpacity,
                                  kStudioExportOptionWatermarkScale,
                                  kStudioExportOptionWatermarkAlignment,
                                  kStudioExportOptionOutputColorEnabled,
                                  kStudioExportOptionOutputProfile,
                                  kStudioExportOptionRenderingIntent,
                                  kStudioExportOptionFrameEnabled,
                                  kStudioExportOptionFrameSize},
                                 "png");
        if (!keys)
        {
            return keys.error();
        }
        auto bit_depth = exact_string(input.value(QStringLiteral("pngBitDepth")),
                                      kStudioExportOptionPngBitDepth, "png");
        if (!bit_depth)
        {
            return bit_depth.error();
        }
        auto parsed_depth = parse_png_bit_depth(bit_depth.value());
        if (!parsed_depth)
        {
            return parsed_depth.error();
        }
        selection.png_options.bit_depth = parsed_depth.value();
        auto compression = exact_integer(input.value(QStringLiteral("pngCompression")),
                                         kStudioExportOptionPngCompression, "png",
                                         "studio_export_invalid_option_type");
        if (!compression)
        {
            return compression.error();
        }
        selection.png_options.compression = compression.value();
        auto valid = validate_png_export_options(selection.png_options);
        if (!valid)
        {
            return valid.error();
        }
        break;
    }
    case ExportFormat::kTiff:
    {
        auto keys = require_keys(input,
                                 {kStudioExportOptionTiffSampleType,
                                  kStudioExportOptionTiffCompression,
                                  kStudioExportOptionTiffCompressionLevel,
                                  kStudioExportOptionTiffGrayscaleIfNeutral,
                                  kStudioExportOptionTiffResolutionDpi,
                                  kStudioExportOptionMetadataMode,
                                  kStudioExportOptionMaxEdge,
                                  kStudioExportOptionMaxWidth,
                                  kStudioExportOptionMaxHeight,
                                  kStudioExportOptionOutputSharpenEnabled,
                                  kStudioExportOptionOutputSharpenAmount,
                                  kStudioExportOptionOutputSharpenRadius,
                                  kStudioExportOptionOutputSharpenThreshold,
                                  kStudioExportOptionWatermarkEnabled,
                                  kStudioExportOptionWatermarkText,
                                  kStudioExportOptionWatermarkOpacity,
                                  kStudioExportOptionWatermarkScale,
                                  kStudioExportOptionWatermarkAlignment,
                                  kStudioExportOptionOutputColorEnabled,
                                  kStudioExportOptionOutputProfile,
                                  kStudioExportOptionRenderingIntent,
                                  kStudioExportOptionFrameEnabled,
                                  kStudioExportOptionFrameSize},
                                 "tiff");
        if (!keys)
        {
            return keys.error();
        }
        auto sample_type = exact_string(input.value(QStringLiteral("tiffSampleType")),
                                        kStudioExportOptionTiffSampleType, "tiff");
        if (!sample_type)
        {
            return sample_type.error();
        }
        auto parsed_sample = parse_tiff_sample_type(sample_type.value());
        if (!parsed_sample)
        {
            return parsed_sample.error();
        }
        selection.tiff_options.sample_type = parsed_sample.value();
        auto compression = exact_string(input.value(QStringLiteral("tiffCompression")),
                                        kStudioExportOptionTiffCompression, "tiff");
        if (!compression)
        {
            return compression.error();
        }
        auto parsed_compression = parse_tiff_compression(compression.value());
        if (!parsed_compression)
        {
            return parsed_compression.error();
        }
        selection.tiff_options.compression = parsed_compression.value();
        auto level = exact_integer(input.value(QStringLiteral("tiffCompressionLevel")),
                                   kStudioExportOptionTiffCompressionLevel, "tiff",
                                   "studio_export_invalid_option_type");
        if (!level)
        {
            return level.error();
        }
        selection.tiff_options.compression_level = level.value();
        auto grayscale = exact_bool(input.value(QStringLiteral("tiffGrayscaleIfNeutral")),
                                    kStudioExportOptionTiffGrayscaleIfNeutral);
        if (!grayscale)
        {
            return grayscale.error();
        }
        selection.tiff_options.grayscale_if_neutral = grayscale.value();
        auto dpi = exact_integer(input.value(QStringLiteral("tiffResolutionDpi")),
                                 kStudioExportOptionTiffResolutionDpi, "tiff",
                                 "studio_export_invalid_option_type");
        if (!dpi)
        {
            return dpi.error();
        }
        selection.tiff_options.resolution_dpi = dpi.value();
        auto valid = validate_tiff_export_options(selection.tiff_options);
        if (!valid)
        {
            return valid.error();
        }
        break;
    }
    case ExportFormat::kOriginalCopy:
    {
        auto keys = require_keys(input, {}, "original");
        if (!keys)
        {
            return keys.error();
        }
        break;
    }
    }
    if (selection.format != ExportFormat::kOriginalCopy)
    {
        auto mode =
            exact_string(input.value(QStringLiteral("metadataMode")),
                         kStudioExportOptionMetadataMode, export_format_name(selection.format));
        if (!mode)
            return mode.error();
        auto parsed = parse_export_metadata_mode(mode.value());
        if (!parsed)
            return parsed.error();
        selection.metadata_mode = parsed.value();
        auto max_edge = exact_integer(
            input.value(QStringLiteral("maxEdge")), kStudioExportOptionMaxEdge,
            export_format_name(selection.format), "studio_export_invalid_option_type");
        if (!max_edge)
            return max_edge.error();
        if (max_edge.value() < static_cast<int>(kExportMaxEdgeMin) ||
            max_edge.value() > static_cast<int>(kExportMaxEdgeMax))
        {
            return conversion_error(
                QT_TRANSLATE_NOOP("StudioExport", "Long edge must be between 0 and 65535"),
                {{"field", std::string(kStudioExportOptionMaxEdge)},
                 {"format", std::string(export_format_name(selection.format))},
                 {"reason", "studio_export_invalid_max_edge"}});
        }
        selection.max_edge = static_cast<std::uint32_t>(max_edge.value());
        auto max_width = exact_integer(
            input.value(QStringLiteral("maxWidth")), kStudioExportOptionMaxWidth,
            export_format_name(selection.format), "studio_export_invalid_option_type");
        if (!max_width)
            return max_width.error();
        if (max_width.value() < static_cast<int>(kExportBoxLimitMin) ||
            max_width.value() > static_cast<int>(kExportBoxLimitMax))
        {
            return conversion_error(
                QT_TRANSLATE_NOOP("StudioExport", "Max width must be between 0 and 65535"),
                {{"field", std::string(kStudioExportOptionMaxWidth)},
                 {"format", std::string(export_format_name(selection.format))},
                 {"reason", "studio_export_invalid_max_width"}});
        }
        selection.max_width = static_cast<std::uint32_t>(max_width.value());
        auto max_height = exact_integer(
            input.value(QStringLiteral("maxHeight")), kStudioExportOptionMaxHeight,
            export_format_name(selection.format), "studio_export_invalid_option_type");
        if (!max_height)
            return max_height.error();
        if (max_height.value() < static_cast<int>(kExportBoxLimitMin) ||
            max_height.value() > static_cast<int>(kExportBoxLimitMax))
        {
            return conversion_error(
                QT_TRANSLATE_NOOP("StudioExport", "Max height must be between 0 and 65535"),
                {{"field", std::string(kStudioExportOptionMaxHeight)},
                 {"format", std::string(export_format_name(selection.format))},
                 {"reason", "studio_export_invalid_max_height"}});
        }
        selection.max_height = static_cast<std::uint32_t>(max_height.value());

        const QVariant enabled_value = input.value(QStringLiteral("outputSharpenEnabled"));
        if (!enabled_value.isValid() || enabled_value.userType() != QMetaType::Bool)
        {
            return conversion_error(
                QT_TRANSLATE_NOOP("StudioExport", "Output sharpen enabled must be a boolean"),
                {{"field", std::string(kStudioExportOptionOutputSharpenEnabled)},
                 {"format", std::string(export_format_name(selection.format))},
                 {"reason", "studio_export_invalid_option_type"}});
        }
        selection.output_sharpen.enabled = enabled_value.toBool();
        auto amount =
            exact_double(input.value(QStringLiteral("outputSharpenAmount")),
                         kStudioExportOptionOutputSharpenAmount,
                         export_format_name(selection.format), "studio_export_invalid_option_type");
        auto radius =
            exact_double(input.value(QStringLiteral("outputSharpenRadius")),
                         kStudioExportOptionOutputSharpenRadius,
                         export_format_name(selection.format), "studio_export_invalid_option_type");
        auto threshold =
            exact_double(input.value(QStringLiteral("outputSharpenThreshold")),
                         kStudioExportOptionOutputSharpenThreshold,
                         export_format_name(selection.format), "studio_export_invalid_option_type");
        if (!amount || !radius || !threshold)
            return !amount ? amount.error() : !radius ? radius.error() : threshold.error();
        selection.output_sharpen.amount = amount.value();
        selection.output_sharpen.radius = radius.value();
        selection.output_sharpen.threshold = threshold.value();
        auto sharpen_valid = validate_export_output_sharpen_options(selection.output_sharpen);
        if (!sharpen_valid)
            return sharpen_valid.error();

        const QVariant watermark_enabled_value = input.value(QStringLiteral("watermarkEnabled"));
        if (!watermark_enabled_value.isValid() ||
            watermark_enabled_value.userType() != QMetaType::Bool)
        {
            return conversion_error(
                QT_TRANSLATE_NOOP("StudioExport", "Watermark enabled must be a boolean"),
                {{"field", std::string(kStudioExportOptionWatermarkEnabled)},
                 {"format", std::string(export_format_name(selection.format))},
                 {"reason", "studio_export_invalid_option_type"}});
        }
        selection.watermark.enabled = watermark_enabled_value.toBool();
        auto watermark_text =
            exact_string(input.value(QStringLiteral("watermarkText")),
                         kStudioExportOptionWatermarkText, export_format_name(selection.format));
        if (!watermark_text)
            return watermark_text.error();
        selection.watermark.text = watermark_text.value();
        auto watermark_opacity = exact_double(
            input.value(QStringLiteral("watermarkOpacity")), kStudioExportOptionWatermarkOpacity,
            export_format_name(selection.format), "studio_export_invalid_option_type");
        if (!watermark_opacity)
            return watermark_opacity.error();
        selection.watermark.opacity = watermark_opacity.value();
        auto watermark_scale = exact_double(
            input.value(QStringLiteral("watermarkScale")), kStudioExportOptionWatermarkScale,
            export_format_name(selection.format), "studio_export_invalid_option_type");
        if (!watermark_scale)
            return watermark_scale.error();
        selection.watermark.scale_percent = watermark_scale.value();
        auto watermark_alignment = exact_string(input.value(QStringLiteral("watermarkAlignment")),
                                                kStudioExportOptionWatermarkAlignment,
                                                export_format_name(selection.format));
        if (!watermark_alignment)
            return watermark_alignment.error();
        selection.watermark.alignment = watermark_alignment.value();
        auto watermark_valid = validate_export_watermark_options(selection.watermark);
        if (!watermark_valid)
            return watermark_valid.error();

        const QVariant color_enabled_value = input.value(QStringLiteral("outputColorEnabled"));
        if (!color_enabled_value.isValid() || color_enabled_value.userType() != QMetaType::Bool)
        {
            return conversion_error(
                QT_TRANSLATE_NOOP("StudioExport", "Output colour enabled must be a boolean"),
                {{"field", std::string(kStudioExportOptionOutputColorEnabled)},
                 {"format", std::string(export_format_name(selection.format))},
                 {"reason", "studio_export_invalid_option_type"}});
        }
        selection.output_color.enabled = color_enabled_value.toBool();
        auto output_profile =
            exact_string(input.value(QStringLiteral("outputProfile")),
                         kStudioExportOptionOutputProfile, export_format_name(selection.format));
        if (!output_profile)
            return output_profile.error();
        selection.output_color.output_profile = output_profile.value();
        auto rendering_intent =
            exact_string(input.value(QStringLiteral("renderingIntent")),
                         kStudioExportOptionRenderingIntent, export_format_name(selection.format));
        if (!rendering_intent)
            return rendering_intent.error();
        selection.output_color.rendering_intent = rendering_intent.value();
        auto color_valid = validate_export_color_options(selection.output_color);
        if (!color_valid)
            return color_valid.error();

        const QVariant frame_enabled_value = input.value(QStringLiteral("frameEnabled"));
        if (!frame_enabled_value.isValid() || frame_enabled_value.userType() != QMetaType::Bool)
        {
            return conversion_error(
                QT_TRANSLATE_NOOP("StudioExport", "Frame enabled must be a boolean"),
                {{"field", std::string(kStudioExportOptionFrameEnabled)},
                 {"format", std::string(export_format_name(selection.format))},
                 {"reason", "studio_export_invalid_option_type"}});
        }
        selection.frame.enabled = frame_enabled_value.toBool();
        auto frame_size =
            exact_double(input.value(QStringLiteral("frameSize")), kStudioExportOptionFrameSize,
                         export_format_name(selection.format), "studio_export_invalid_option_type");
        if (!frame_size)
            return frame_size.error();
        selection.frame.size = frame_size.value();
        auto frame_valid = validate_export_frame_options(selection.frame);
        if (!frame_valid)
            return frame_valid.error();
    }
    if (input != options)
    {
        return conversion_error(
            QT_TRANSLATE_NOOP("StudioExport", "Export options map was mutated during conversion"),
            {{"reason", "studio_export_options_mutated"}});
    }
    return selection;
}

Result<QString> normalize_studio_export_path(const QString &path, const ExportFormat format)
{
    const QString output = local_path(path);
    if (output.isEmpty())
    {
        return conversion_error(QT_TRANSLATE_NOOP("StudioExport", "Export path must not be empty"),
                                {{"reason", "studio_export_empty_path"}});
    }
    if (format == ExportFormat::kOriginalCopy)
    {
        return output;
    }

    const QFileInfo info(output);
    const QString suffix = info.suffix().toLower();
    const auto mismatch = [&]()
    {
        return Result<QString>{conversion_error(
            QT_TRANSLATE_NOOP("StudioExport",
                              "Export path suffix does not match the selected format"),
            {{"format", std::string(export_format_name(format))},
             {"path", utf8(output)},
             {"reason", "studio_export_extension_mismatch"},
             {"suffix", utf8(suffix)}})};
    };
    if (suffix.isEmpty())
    {
        return output +
               QString::fromUtf8(export_format_extension(format).data(),
                                 static_cast<qsizetype>(export_format_extension(format).size()));
    }
    switch (format)
    {
    case ExportFormat::kJpeg:
        if (suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg"))
        {
            return output;
        }
        return mismatch();
    case ExportFormat::kPng:
        if (suffix == QLatin1String("png"))
        {
            return output;
        }
        return mismatch();
    case ExportFormat::kTiff:
        if (suffix == QLatin1String("tif") || suffix == QLatin1String("tiff"))
        {
            return output;
        }
        return mismatch();
    case ExportFormat::kOriginalCopy:
        break;
    }
    return mismatch();
}

Result<ExportRequest> make_studio_export_request(std::string asset_id, const QString &path,
                                                 const QString &format_name,
                                                 const QVariantMap &options)
{
    auto export_options = make_studio_export_options(format_name, options);
    if (!export_options)
        return export_options.error();
    auto normalized = normalize_studio_export_path(path, export_options.value().format);
    if (!normalized)
    {
        return normalized.error();
    }
    ExportRequest request;
    static_cast<ExportOptions &>(request) = std::move(export_options).value();
    request.asset_id = std::move(asset_id);
    request.output_path = utf8(normalized.value());
    return request;
}

Result<ExportOptions> make_studio_export_options(const QString &format_name,
                                                 const QVariantMap &options)
{
    auto selection = studio_export_options_from_presentation(format_name, options);
    if (!selection)
        return selection.error();
    ExportOptions result;
    result.format = selection.value().format;
    result.jpeg_options = selection.value().jpeg_options;
    result.png_options = selection.value().png_options;
    result.tiff_options = selection.value().tiff_options;
    result.metadata_mode = selection.value().metadata_mode;
    result.max_edge = selection.value().max_edge;
    result.max_width = selection.value().max_width;
    result.max_height = selection.value().max_height;
    result.output_sharpen = selection.value().output_sharpen;
    result.output_color = selection.value().output_color;
    result.frame = selection.value().frame;
    result.watermark = selection.value().watermark;
    return result;
}

} // namespace ravo
