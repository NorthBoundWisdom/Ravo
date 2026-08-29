#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <QCoreApplication>
#include <QStandardPaths>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include "ravo/domain/types.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/develop.h"

namespace ravo
{

[[nodiscard]] inline QString qstring_from_utf8(const std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

[[nodiscard]] inline std::string utf8_from_qstring(const QString &text)
{
    const QByteArray bytes = text.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

[[nodiscard]] inline QVariantList tone_curve_to_variant(const std::vector<ToneCurvePoint> &points)
{
    std::vector<ToneCurvePoint> display = points;
    if (tone_curve_is_identity(display))
    {
        display = {{0.0, 0.0}, {1.0, 1.0}};
    }
    QVariantList list;
    list.reserve(static_cast<qsizetype>(display.size()));
    for (const auto &point : display)
    {
        QVariantMap item;
        item.insert(QStringLiteral("x"), point.x);
        item.insert(QStringLiteral("y"), point.y);
        list.push_back(item);
    }
    return list;
}

[[nodiscard]] inline std::vector<ToneCurvePoint> tone_curve_from_variant(const QVariantList &list)
{
    std::vector<ToneCurvePoint> points;
    points.reserve(static_cast<std::size_t>(std::max<qsizetype>(0, list.size())));
    for (const auto &item : list)
    {
        const auto map = item.toMap();
        points.push_back(
            {map.value(QStringLiteral("x")).toDouble(), map.value(QStringLiteral("y")).toDouble()});
    }
    clamp_tone_curve(points);
    return points;
}

[[nodiscard]] inline QVariantList tone_curve_sample_list(const std::vector<ToneCurvePoint> &points)
{
    constexpr int kSamples = 65;
    QVariantList samples;
    samples.reserve(kSamples);
    for (int index = 0; index < kSamples; ++index)
    {
        const double x = static_cast<double>(index) / static_cast<double>(kSamples - 1);
        samples.push_back(evaluate_tone_curve(points, x));
    }
    return samples;
}

[[nodiscard]] inline std::string preview_root_for(const std::string &database_path)
{
    return database_path + ".preview";
}

[[nodiscard]] inline QString pictures_directory()
{
    QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (pictures.isEmpty())
    {
        pictures = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    }
    return pictures;
}

[[nodiscard]] inline QUrl url_from_dialog_path(const QString &path)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty())
    {
        return {};
    }
    if (trimmed.startsWith(QStringLiteral("file:")))
    {
        return QUrl(trimmed);
    }
    return QUrl::fromLocalFile(trimmed);
}

[[nodiscard]] inline QString describe_import(const std::vector<ImportItemResult> &results)
{
    int imported = 0;
    int duplicate = 0;
    int unsupported = 0;
    int failed = 0;
    for (const auto &item : results)
    {
        switch (item.status)
        {
        case ImportItemStatus::kImported:
            ++imported;
            break;
        case ImportItemStatus::kDuplicate:
            ++duplicate;
            break;
        case ImportItemStatus::kUnsupported:
            ++unsupported;
            break;
        case ImportItemStatus::kFailed:
            ++failed;
            break;
        }
    }
    return QCoreApplication::translate("StudioPresenter",
                                       "Imported %1, duplicate %2, unsupported %3, failed %4")
        .arg(imported)
        .arg(duplicate)
        .arg(unsupported)
        .arg(failed);
}

[[nodiscard]] inline QString rating_mode_name(const RatingFilterMode mode)
{
    switch (mode)
    {
    case RatingFilterMode::kMinimum:
        return QStringLiteral("min");
    case RatingFilterMode::kExact:
        return QStringLiteral("exact");
    case RatingFilterMode::kAny:
        break;
    }
    return QStringLiteral("any");
}

[[nodiscard]] inline QString reject_filter_name(const RejectFilter filter)
{
    switch (filter)
    {
    case RejectFilter::kExclude:
        return QStringLiteral("exclude");
    case RejectFilter::kOnly:
        return QStringLiteral("only");
    case RejectFilter::kInclude:
        break;
    }
    return QStringLiteral("include");
}

[[nodiscard]] inline QString sort_field_name(const AssetSortField field)
{
    switch (field)
    {
    case AssetSortField::kDisplayName:
        return QStringLiteral("name");
    case AssetSortField::kRating:
        return QStringLiteral("rating");
    case AssetSortField::kCaptureTime:
        return QStringLiteral("captured");
    case AssetSortField::kFileSize:
        return QStringLiteral("size");
    case AssetSortField::kImportTime:
        break;
    }
    return QStringLiteral("imported");
}

} // namespace ravo
