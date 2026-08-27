#include "ravo/desktop/export_option_conversion.h"

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
    QT_TRANSLATE_NOOP("StudioExport", "TIFF resolution must be between 72 and 9600 DPI")};

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
        auto keys = require_keys(
            input, {kStudioExportOptionQuality, kStudioExportOptionJpegSubsampling}, "jpeg");
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
        auto keys = require_keys(
            input, {kStudioExportOptionPngBitDepth, kStudioExportOptionPngCompression}, "png");
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
        auto keys = require_keys(
            input,
            {kStudioExportOptionTiffSampleType, kStudioExportOptionTiffCompression,
             kStudioExportOptionTiffCompressionLevel, kStudioExportOptionTiffGrayscaleIfNeutral,
             kStudioExportOptionTiffResolutionDpi},
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
    auto selection = studio_export_options_from_presentation(format_name, options);
    if (!selection)
    {
        return selection.error();
    }
    auto normalized = normalize_studio_export_path(path, selection.value().format);
    if (!normalized)
    {
        return normalized.error();
    }
    ExportRequest request;
    request.asset_id = std::move(asset_id);
    request.output_path = utf8(normalized.value());
    request.format = selection.value().format;
    request.jpeg_options = selection.value().jpeg_options;
    request.png_options = selection.value().png_options;
    request.tiff_options = selection.value().tiff_options;
    return request;
}

} // namespace ravo
