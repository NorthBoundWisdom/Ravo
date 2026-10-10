#include "ravo/desktop/studio_develop_presenter.h"
#include "studio_preset_paths.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <numbers>
#include <set>
#include <string_view>
#include <utility>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>
#include <QMetaObject>
#include <QMutexLocker>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include "ravo/recipe/develop.h"
#include "ravo/recipe/develop_mask.h"
#include "ravo/recipe/recipe.h"
#include "ravo/recipe/style.h"
#include "ravo/adapters/crs_xmp.h"
#include "ravo/adapters/text_file.h"
#include "studio_debug_info.h"
#include "studio_qt.h"

namespace ravo
{
namespace
{

QString preset_suffix_from_filename(const QFileInfo &info)
{
    const QString file_name = info.fileName();
    const QString style_suffix = QStringLiteral(".rstyle.json");
    const QString xmp_suffix = QStringLiteral(".xmp");
    if (file_name.endsWith(style_suffix, Qt::CaseInsensitive))
        return file_name.right(style_suffix.size());
    if (file_name.endsWith(xmp_suffix, Qt::CaseInsensitive))
        return file_name.right(xmp_suffix.size());
    return {};
}

bool is_managed_preset(const QVariantList &presets, const QString &directory,
                       const QFileInfo &candidate)
{
    if (directory.isEmpty() || !candidate.exists() || !candidate.isFile() || candidate.isSymLink())
        return false;
    const QString directory_path = QFileInfo(directory).canonicalFilePath();
    const QString parent_path = QFileInfo(candidate.absolutePath()).canonicalFilePath();
    if (directory_path.isEmpty() || parent_path != directory_path)
        return false;
    const QString candidate_path = canonical_or_absolute(candidate);
    return std::any_of(
        presets.cbegin(), presets.cend(),
        [&](const QVariant &entry)
        {
            const QFileInfo listed(entry.toMap().value(QStringLiteral("path")).toString());
            return !listed.isSymLink() && canonical_or_absolute(listed) == candidate_path;
        });
}

QString preset_name_validation_error(const QString &name)
{
    if (name.isEmpty())
        return QCoreApplication::translate("StudioPresenter", "Preset name must not be empty.");
    if (name != name.trimmed())
        return QCoreApplication::translate("StudioPresenter",
                                           "Preset name must not start or end with whitespace.");
    if (name.startsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char('.')) ||
        name == QLatin1String(".."))
        return QCoreApplication::translate("StudioPresenter",
                                           "Preset name must not start or end with a period.");
    if (name.toUtf8().size() > 200)
        return QCoreApplication::translate("StudioPresenter", "Preset name is too long.");
    static const QRegularExpression invalid_characters(QStringLiteral(R"([\x00-\x1f\\/:*?"<>|])"));
    if (name.contains(invalid_characters))
        return QCoreApplication::translate(
            "StudioPresenter",
            "Preset name contains characters that cannot be used in a file name.");
    const QString base_name = name.section(QLatin1Char('.'), 0, 0);
    static const QRegularExpression reserved_name(
        QStringLiteral(R"(^(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$)"),
        QRegularExpression::CaseInsensitiveOption);
    if (reserved_name.match(base_name).hasMatch())
        return QCoreApplication::translate("StudioPresenter",
                                           "Preset name is reserved by the operating system.");
    return {};
}

QString collect_modified_parameter_selection(const QVariantList &fields,
                                             const DevelopParams &baseline,
                                             const DevelopParams &current,
                                             std::vector<std::string> &selected_fields)
{
    if (fields.isEmpty())
        return QCoreApplication::translate("StudioPresenter",
                                           "Select at least one modified parameter.");
    if (fields.size() > static_cast<qsizetype>(develop_selectable_field_names().size()))
        return QCoreApplication::translate("StudioPresenter", "Parameter selection is invalid.");

    selected_fields.clear();
    selected_fields.reserve(static_cast<std::size_t>(fields.size()));
    std::set<std::string, std::less<>> selected_set;
    for (const auto &field : fields)
    {
        if (field.metaType().id() != QMetaType::QString || field.toString().isEmpty())
            return QCoreApplication::translate("StudioPresenter",
                                               "Parameter selection is invalid.");
        auto selected = utf8_from_qstring(field.toString());
        if (!is_develop_selectable_field(selected) || !selected_set.insert(selected).second)
            return QCoreApplication::translate("StudioPresenter",
                                               "Parameter selection is invalid.");
        selected_fields.push_back(std::move(selected));
    }

    std::set<std::string, std::less<>> current_fields;
    for (const auto &change : develop_modified_fields(baseline, current))
        current_fields.insert(change.field);
    if (!std::all_of(selected_fields.cbegin(), selected_fields.cend(),
                     [&current_fields](const std::string &field)
                     { return current_fields.contains(field); }))
        return QCoreApplication::translate("StudioPresenter",
                                           "The selected parameters are no longer modified.");
    std::sort(selected_fields.begin(), selected_fields.end());
    return {};
}

} // namespace

QString StudioDevelopPresenter::presets_directory() const
{
    if (catalog_path_.isEmpty())
        return {};
    return QDir(QFileInfo(catalog_path_).absolutePath()).filePath(QStringLiteral("Ravo Presets"));
}

void StudioDevelopPresenter::savePreset(const QString &name, const QVariantList &fields)
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset || !engine_)
        return;
    const QString checked_name = name;
    const QString name_error = preset_name_validation_error(checked_name);
    if (!name_error.isEmpty())
    {
        emit errorOccurred(name_error);
        return;
    }
    if (static_cast<std::size_t>(checked_name.toUtf8().size()) > kRecipeStyleNameMaxBytes)
    {
        emit errorOccurred(
            QCoreApplication::translate("StudioPresenter", "Preset name is too long."));
        return;
    }
    std::vector<std::string> selected_fields;
    const QString selection_error = collect_modified_parameter_selection(
        fields, baseline_develop(), state_.develop_, selected_fields);
    if (!selection_error.isEmpty())
    {
        emit errorOccurred(selection_error);
        return;
    }

    const QString directory = presets_directory();
    if (directory.isEmpty())
    {
        emit errorOccurred(
            QCoreApplication::translate("StudioPresenter", "Open a library to save presets."));
        return;
    }
    if (!QDir().mkpath(directory))
    {
        emit errorOccurred(
            QCoreApplication::translate("StudioPresenter", "Preset folder could not be created."));
        return;
    }
    const bool duplicate_name =
        std::any_of(state_.develop_presets_.cbegin(), state_.develop_presets_.cend(),
                    [&checked_name](const QVariant &entry)
                    {
                        return entry.toMap()
                                   .value(QStringLiteral("name"))
                                   .toString()
                                   .compare(checked_name, Qt::CaseInsensitive) == 0;
                    });
    if (duplicate_name)
    {
        emit errorOccurred(QCoreApplication::translate("StudioPresenter",
                                                       "A preset with that name already exists."));
        return;
    }

    auto recipe = recipe_from_develop(
        {asset->id, asset->normalized_uri, asset->content_fingerprint}, state_.develop_);
    if (!recipe)
    {
        emit errorOccurred(qstring_from_utf8(recipe.error().message));
        return;
    }
    auto valid = engine_->validate(recipe.value());
    if (!valid)
    {
        emit errorOccurred(qstring_from_utf8(valid.error().message));
        return;
    }
    auto style = recipe_style_from_selected_fields(
        utf8_from_qstring(checked_name), {}, std::move(recipe).value(), std::move(selected_fields));
    if (!style)
    {
        emit errorOccurred(qstring_from_utf8(style.error().message));
        return;
    }
    auto serialized = serialize_recipe_style(style.value());
    if (!serialized)
    {
        emit errorOccurred(qstring_from_utf8(serialized.error().message));
        return;
    }
    const QString output = QDir(directory).filePath(checked_name + QStringLiteral(".rstyle.json"));
    auto written = write_utf8_text_file_atomically(utf8_from_qstring(output), serialized.value());
    if (!written)
    {
        emit errorOccurred(qstring_from_utf8(written.error().message));
        return;
    }
    reload_presets();
    emit statusOccurred(
        QCoreApplication::translate("StudioPresenter", "Preset “%1” saved.").arg(checked_name));
}

void StudioDevelopPresenter::reload_presets()
{
    QVariantList presets;
    const QString directory = presets_directory();
    if (!directory.isEmpty())
    {
        const QDir dir(directory);
        const auto entries = dir.entryInfoList(
            {QStringLiteral("*.xmp"), QStringLiteral("*.XMP"), QStringLiteral("*.rstyle.json")},
            QDir::Files, QDir::Name | QDir::IgnoreCase);
        for (const auto &entry : entries)
        {
            if (entry.isSymLink())
                continue;
            const QString name = preset_name_from_filename(entry);
            QString kind = QStringLiteral("style");
            const auto text = read_utf8_text_file(utf8_from_qstring(entry.absoluteFilePath()),
                                                  kRecipeStyleFileMaxBytes);
            if (text && is_crs_xmp_document(text.value()))
            {
                kind = QStringLiteral("crs");
            }
            else if (text)
            {
                auto style = parse_recipe_style_json(text.value());
                if (!style)
                    continue;
            }
            else
            {
                continue;
            }
            presets.push_back(QVariantMap{{QStringLiteral("name"), name},
                                          {QStringLiteral("path"), entry.absoluteFilePath()},
                                          {QStringLiteral("kind"), kind}});
        }
    }
    state_.develop_presets_ = std::move(presets);
    emit presetsChanged();
}

void StudioDevelopPresenter::importPresetFromPath(const QString &path)
{
    QString input_path = path.trimmed();
    if (input_path.startsWith(QStringLiteral("file:")))
        input_path = QUrl(input_path).toLocalFile();
    const QFileInfo source(input_path);
    if (!source.exists() || !source.isFile())
    {
        emit errorOccurred(
            QCoreApplication::translate("StudioPresenter", "Preset file was not found."));
        return;
    }
    const QString directory = presets_directory();
    if (directory.isEmpty())
    {
        emit errorOccurred(
            QCoreApplication::translate("StudioPresenter", "Open a library to import presets."));
        return;
    }
    if (!QDir().mkpath(directory))
    {
        emit errorOccurred(
            QCoreApplication::translate("StudioPresenter", "Preset folder could not be created."));
        return;
    }
    auto text = read_utf8_text_file(utf8_from_qstring(input_path), kRecipeStyleFileMaxBytes);
    if (!text)
    {
        emit errorOccurred(qstring_from_utf8(text.error().message));
        return;
    }
    QString stem = source.completeBaseName();
    QString suffix = QStringLiteral(".rstyle.json");
    if (is_crs_xmp_document(text.value()))
    {
        auto imported =
            import_crs_xmp({text.value(), {"preset", "ravo-preset://library", std::nullopt}});
        if (!imported)
        {
            emit errorOccurred(qstring_from_utf8(imported.error().message));
            return;
        }
        if (!imported.value().name.empty())
            stem = QString::fromStdString(imported.value().name);
        suffix = QStringLiteral(".xmp");
    }
    else
    {
        auto style = parse_recipe_style_json(text.value());
        if (!style)
        {
            emit errorOccurred(qstring_from_utf8(style.error().message));
            return;
        }
        stem = QString::fromStdString(style.value().name);
    }
    stem.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|])")), QStringLiteral("-"));
    if (stem.trimmed().isEmpty())
        stem = QStringLiteral("preset");
    QString destination = QDir(directory).filePath(stem + suffix);
    if (QFileInfo::exists(destination) &&
        QFileInfo(destination).canonicalFilePath() != source.canonicalFilePath())
    {
        int serial = 2;
        while (QFileInfo::exists(QDir(directory).filePath(stem + QStringLiteral("-") +
                                                          QString::number(serial) + suffix)))
            ++serial;
        destination =
            QDir(directory).filePath(stem + QStringLiteral("-") + QString::number(serial) + suffix);
    }
    if (QFileInfo(destination).canonicalFilePath() != source.canonicalFilePath())
    {
        QFile::remove(destination);
        if (!QFile::copy(input_path, destination))
        {
            emit errorOccurred(
                QCoreApplication::translate("StudioPresenter", "Preset could not be copied."));
            return;
        }
    }
    reload_presets();
    applyStyleFromPath(destination);
}

void StudioDevelopPresenter::renamePreset(const QString &path, const QString &name)
{
    const QString input_path = local_file_path(path);
    const QFileInfo source(input_path);
    if (!source.exists() || !source.isFile())
    {
        emit errorOccurred(
            QCoreApplication::translate("StudioPresenter", "Preset file was not found."));
        return;
    }
    if (!is_managed_preset(state_.develop_presets_, presets_directory(), source))
    {
        emit errorOccurred(QCoreApplication::translate(
            "StudioPresenter", "Only presets imported into this library can be renamed."));
        return;
    }
    const QString name_error = preset_name_validation_error(name);
    if (!name_error.isEmpty())
    {
        emit errorOccurred(name_error);
        return;
    }
    const QString suffix = preset_suffix_from_filename(source);
    if (suffix.isEmpty())
    {
        emit errorOccurred(
            QCoreApplication::translate("StudioPresenter", "Preset file type is not supported."));
        return;
    }
    const QString destination = QDir(source.absolutePath()).filePath(name + suffix);
    if (QDir::cleanPath(destination) == QDir::cleanPath(source.absoluteFilePath()))
    {
        emit errorOccurred({});
        emit statusOccurred(
            QCoreApplication::translate("StudioPresenter", "Preset name is unchanged."));
        return;
    }
    const QFileInfo target(destination);
    if (target.exists() || target.isSymLink())
    {
        if (canonical_or_absolute(target) == canonical_or_absolute(source))
        {
            emit errorOccurred(QCoreApplication::translate(
                "StudioPresenter", "This filesystem cannot rename a preset by letter case only."));
        }
        else
        {
            emit errorOccurred(QCoreApplication::translate(
                "StudioPresenter", "A preset with that name already exists."));
        }
        return;
    }
    QFile file(source.absoluteFilePath());
    if (!file.rename(destination))
    {
        emit errorOccurred(
            QCoreApplication::translate("StudioPresenter", "Preset could not be renamed: %1")
                .arg(file.errorString()));
        return;
    }
    emit errorOccurred({});
    reload_presets();
    emit statusOccurred(
        QCoreApplication::translate("StudioPresenter", "Preset renamed to “%1”.").arg(name));
}

void StudioDevelopPresenter::deletePreset(const QString &path)
{
    const QString input_path = local_file_path(path);
    const QFileInfo source(input_path);
    if (!source.exists() || !source.isFile())
    {
        emit errorOccurred(
            QCoreApplication::translate("StudioPresenter", "Preset file was not found."));
        return;
    }
    if (!is_managed_preset(state_.develop_presets_, presets_directory(), source))
    {
        emit errorOccurred(QCoreApplication::translate(
            "StudioPresenter", "Only presets imported into this library can be deleted."));
        return;
    }
    QFile file(source.absoluteFilePath());
    if (!file.remove())
    {
        emit errorOccurred(
            QCoreApplication::translate("StudioPresenter", "Preset could not be deleted: %1")
                .arg(file.errorString()));
        return;
    }
    emit errorOccurred({});
    reload_presets();
    emit statusOccurred(QCoreApplication::translate("StudioPresenter", "Preset deleted."));
}

void StudioDevelopPresenter::addRetouchRegion(const QVariantMap &values)
{
    const auto reject = [&](const QString &reason)
    {
        emit errorOccurred(
            QCoreApplication::translate("DevelopPanel", "Retouch region was rejected") +
            QStringLiteral(" [") + reason + QStringLiteral("]"));
    };
    if (state_.develop_.retouch.regions.size() >= kRetouchMaxRegions)
    {
        reject(QStringLiteral("region_limit"));
        return;
    }
    const auto number = [&](const char *name, const double minimum,
                            const double maximum) -> std::optional<double>
    {
        const auto found = values.constFind(QString::fromLatin1(name));
        if (found == values.cend())
            return std::nullopt;
        bool ok = false;
        const double value = found.value().toDouble(&ok);
        return ok && std::isfinite(value) && value >= minimum && value <= maximum ?
                   std::optional<double>{value} :
                   std::nullopt;
    };
    const QString mode_text = values.value(QStringLiteral("mode")).toString();
    RetouchMode mode = RetouchMode::kHeal;
    if (mode_text == QLatin1String("clone"))
        mode = RetouchMode::kClone;
    else if (mode_text == QLatin1String("heal"))
        mode = RetouchMode::kHeal;
    else if (mode_text == QLatin1String("blur"))
        mode = RetouchMode::kBlur;
    else if (mode_text == QLatin1String("fill"))
        mode = RetouchMode::kFill;
    else
    {
        reject(QStringLiteral("unsupported_mode"));
        return;
    }
    const auto center_x = number("centerX", 0.0, 1.0);
    const auto center_y = number("centerY", 0.0, 1.0);
    const auto radius = number("radius", kCanonicalMaskPositiveMin, 1.0);
    const auto feather = number("feather", 0.0, 1.0);
    const auto opacity = number("opacity", 0.0, 1.0);
    const auto source_x = number("sourceX", 0.0, 1.0);
    const auto source_y = number("sourceY", 0.0, 1.0);
    const auto blur_radius = number("blurRadius", kRetouchBlurRadiusMin, kRetouchBlurRadiusMax);
    const auto fill_r = number("fillR", 0.0, 1.0);
    const auto fill_g = number("fillG", 0.0, 1.0);
    const auto fill_b = number("fillB", 0.0, 1.0);
    const auto fill_brightness = number("fillBrightness", -1.0, 1.0);
    if (!center_x || !center_y || !radius || !feather || !opacity || !source_x || !source_y ||
        !blur_radius || !fill_r || !fill_g || !fill_b || !fill_brightness)
    {
        reject(QStringLiteral("invalid_numeric_field"));
        return;
    }
    const QString blur_text = values.value(QStringLiteral("blurType")).toString();
    const QString fill_text = values.value(QStringLiteral("fillMode")).toString();
    if (blur_text != QLatin1String("gaussian") && blur_text != QLatin1String("bilateral"))
    {
        reject(QStringLiteral("unsupported_blur_type"));
        return;
    }
    if (fill_text != QLatin1String("erase") && fill_text != QLatin1String("color"))
    {
        reject(QStringLiteral("unsupported_fill_mode"));
        return;
    }

    DevelopParams next = state_.develop_;
    std::size_t suffix = next.retouch.regions.size() + 1U;
    std::string mask_id;
    do
    {
        mask_id = "studio-retouch-" + std::to_string(suffix++);
    } while (std::any_of(next.masks.begin(), next.masks.end(),
                         [&mask_id](const Mask &mask) { return mask.id == mask_id; }));
    Mask mask{mask_id, kCanonicalMaskSchemaVersion, MaskKind::kCircle};
    mask.payload = CircleMask{*center_x, *center_y, *radius, *feather};
    next.masks.push_back(std::move(mask));
    RetouchRegion region;
    region.mask_id = mask_id;
    region.mode = mode;
    region.opacity = *opacity;
    region.source_x = *source_x;
    region.source_y = *source_y;
    region.blur_type = blur_text == QLatin1String("gaussian") ? RetouchBlurType::kGaussian :
                                                                RetouchBlurType::kBilateral;
    region.blur_radius = *blur_radius;
    region.fill_mode =
        fill_text == QLatin1String("erase") ? RetouchFillMode::kErase : RetouchFillMode::kColor;
    region.fill_color = {*fill_r, *fill_g, *fill_b};
    region.fill_brightness = *fill_brightness;
    next.retouch.regions.push_back(std::move(region));
    mutate_develop(std::move(next), DevelopEdit::Commit);
}

void StudioDevelopPresenter::removeRetouchRegion(const int index)
{
    if (index < 0 || static_cast<std::size_t>(index) >= state_.develop_.retouch.regions.size())
    {
        emit errorOccurred(
            QCoreApplication::translate("DevelopPanel", "Retouch region was rejected") +
            QStringLiteral(" [invalid_region_index]"));
        return;
    }
    DevelopParams next = state_.develop_;
    const std::string mask_id = next.retouch.regions[static_cast<std::size_t>(index)].mask_id;
    next.retouch.regions.erase(next.retouch.regions.begin() + index);
    const bool group_references_mask = std::any_of(
        next.masks.begin(), next.masks.end(),
        [&mask_id](const Mask &mask)
        {
            const auto *group = std::get_if<MaskGroup>(&mask.payload);
            return group != nullptr && std::any_of(group->children.begin(), group->children.end(),
                                                   [&mask_id](const MaskGroupChild &child)
                                                   { return child.mask_id == mask_id; });
        });
    if (mask_id.starts_with("studio-retouch-") &&
        std::none_of(next.retouch.regions.begin(), next.retouch.regions.end(),
                     [&mask_id](const RetouchRegion &region)
                     { return region.mask_id == mask_id; }) &&
        (!next.color_harmonizer_mask_id || *next.color_harmonizer_mask_id != mask_id) &&
        (!next.graduated_mask_id || *next.graduated_mask_id != mask_id) &&
        (!next.color_balance_rgb_mask_id || *next.color_balance_rgb_mask_id != mask_id) &&
        (!next.exposure_mask_id || *next.exposure_mask_id != mask_id) &&
        (!next.rgb_curve_mask_id || *next.rgb_curve_mask_id != mask_id) &&
        (!next.tone_curve_mask_id || *next.tone_curve_mask_id != mask_id) &&
        (!next.highlights_mask_id || *next.highlights_mask_id != mask_id) &&
        (!next.shadows_mask_id || *next.shadows_mask_id != mask_id) &&
        (!next.whites_mask_id || *next.whites_mask_id != mask_id) &&
        (!next.blacks_mask_id || *next.blacks_mask_id != mask_id) && !group_references_mask)
    {
        next.masks.erase(std::remove_if(next.masks.begin(), next.masks.end(),
                                        [&mask_id](const Mask &mask)
                                        { return mask.id == mask_id; }),
                         next.masks.end());
    }
    mutate_develop(std::move(next), DevelopEdit::Commit);
}

void StudioDevelopPresenter::setToneCurve(const QVariantList &points)
{
    setCurvePoints(QStringLiteral("tone"), 0, points);
}

void StudioDevelopPresenter::previewToneCurve(const QVariantList &points)
{
    previewCurvePoints(QStringLiteral("tone"), 0, points);
}

void StudioDevelopPresenter::setCurveFamily(const int family)
{
    const int next = family == 1 ? 1 : 0;
    if (state_.curve_family_ == next)
        return;
    state_.curve_family_ = next;
    state_.curve_channel_ = 0;
    emit editChanged();
}

void StudioDevelopPresenter::setCurveChannel(const int channel)
{
    const int max_channel = state_.curve_family_ == 0 ? 3 : 2;
    const int next = std::clamp(channel, 0, max_channel);
    if (state_.curve_channel_ == next)
        return;
    state_.curve_channel_ = next;
    emit editChanged();
}

void StudioDevelopPresenter::apply_curve_points(const QString &family, const int channel,
                                                const QVariantList &points, const DevelopEdit edit)
{
    DevelopParams next = edit_develop();
    const int family_index = family == QLatin1String("tone") ? 1 : 0;
    state_.curve_family_ = family_index;
    if (family_index == 0)
    {
        state_.curve_channel_ = std::clamp(channel, 0, 3);
        if (state_.curve_channel_ <= 0)
        {
            next.rgb_curve.mode = std::string(kRgbLevelsModeLinked);
            next.rgb_curve.channels[0] = tone_curve_from_variant(points);
        }
        else
        {
            next.rgb_curve.mode = std::string(kRgbLevelsModeIndependent);
            next.rgb_curve.channels[static_cast<std::size_t>(state_.curve_channel_ - 1)] =
                tone_curve_from_variant(points);
        }
    }
    else
    {
        state_.curve_channel_ = std::clamp(channel, 0, 2);
        if (state_.curve_channel_ == 1)
        {
            next.tone_curve_channel_mode = std::string(kToneCurveChannelModeIndependent);
            next.tone_curve_a = tone_curve_from_variant(points);
        }
        else if (state_.curve_channel_ == 2)
        {
            next.tone_curve_channel_mode = std::string(kToneCurveChannelModeIndependent);
            next.tone_curve_b = tone_curve_from_variant(points);
        }
        else
        {
            next.tone_curve = tone_curve_from_variant(points);
        }
    }
    mutate_scoped_develop(std::move(next), edit, true,
                          edit == DevelopEdit::Commit ?
                              std::optional<std::string>{"curve:" + utf8_from_qstring(family) +
                                                         ":" + std::to_string(channel)} :
                              std::nullopt);
}

void StudioDevelopPresenter::setCurvePoints(const QString &family, const int channel,
                                            const QVariantList &points)
{
    apply_curve_points(family, channel, points, DevelopEdit::Commit);
}

void StudioDevelopPresenter::previewCurvePoints(const QString &family, const int channel,
                                                const QVariantList &points)
{
    apply_curve_points(family, channel, points, DevelopEdit::Preview);
}

void StudioDevelopPresenter::sync_curve_ui_from_develop()
{
    if (!state_.develop_.rgb_curve.is_identity())
        state_.curve_family_ = 0;
    else if (!tone_curve_is_identity(state_.develop_.tone_curve) ||
             !tone_curve_is_identity(state_.develop_.tone_curve_a) ||
             !tone_curve_is_identity(state_.develop_.tone_curve_b))
        state_.curve_family_ = 1;
    else
        state_.curve_family_ = 0;
    state_.curve_channel_ = 0;
}

void StudioDevelopPresenter::previewDevelopNumber(const QString &name, const double value)
{
    DevelopParams next = edit_develop();
    const auto field = utf8_from_qstring(name);
    if (!localEditing())
        capture_instance_front_for_field(next, field);
    if (is_develop_mask_field(field))
    {
        auto applied = apply_develop_field_strict(next, field, value);
        if (!applied)
        {
            const auto reason = applied.error().context.find("reason");
            const auto reason_text = reason == applied.error().context.end() ?
                                         QStringLiteral("unknown") :
                                         qstring_from_utf8(reason->second);
            emit errorOccurred(
                QCoreApplication::translate("DevelopPanel", "Mask edit was rejected") +
                QStringLiteral(" [") + reason_text + QStringLiteral("]"));
            return;
        }
    }
    else if (!apply_develop_field(next, field, value))
    {
        return;
    }
    if (!localEditing())
        retarget_instance_edit_after_field(next, field);
    mutate_scoped_develop(std::move(next), DevelopEdit::Preview);
}

void StudioDevelopPresenter::setCropRect(const double x, const double y, const double width,
                                         const double height)
{
    DevelopParams next = state_.develop_;
    next.crop_x = x;
    next.crop_y = y;
    next.crop_width = width;
    next.crop_height = height;
    clamp_develop(next);
    constrain_geometry_crop(next);
    clamp_selected_crop(next);
    mutate_develop(std::move(next), DevelopEdit::Commit, true, std::string{"cropRect"});
}

void StudioDevelopPresenter::previewCropRect(const double x, const double y, const double width,
                                             const double height)
{
    DevelopParams next = state_.develop_;
    next.crop_x = x;
    next.crop_y = y;
    next.crop_width = width;
    next.crop_height = height;
    clamp_develop(next);
    constrain_geometry_crop(next);
    clamp_selected_crop(next);
    mutate_develop(std::move(next), DevelopEdit::Overlay);
}

void StudioDevelopPresenter::setCropAspect(const QString &aspect)
{
    if (aspect == QLatin1String("locked"))
    {
        state_.locked_crop_ratio_ =
            state_.develop_.crop_width / std::max(state_.develop_.crop_height, 1e-6);
        state_.crop_aspect_ = QStringLiteral("locked");
        emit editChanged();
        return;
    }
    DevelopParams next = state_.develop_;
    if (!apply_crop_aspect(next, utf8_from_qstring(aspect)))
    {
        return;
    }
    state_.crop_aspect_ = aspect;
    state_.locked_crop_ratio_ = 0.0;
    fit_geometry_crop(next);
    clamp_selected_crop(next);
    if (!mutate_develop(std::move(next), DevelopEdit::Commit))
    {
        emit editChanged();
    }
}

void StudioDevelopPresenter::rotateLeft()
{
    DevelopParams next = state_.develop_;
    next.rotate_quarters = (next.rotate_quarters + 3) % 4;
    transform_crop_for_quarter_turns(next, 3);
    fit_geometry_crop(next);
    mutate_develop(std::move(next), DevelopEdit::Commit);
}

void StudioDevelopPresenter::rotateRight()
{
    DevelopParams next = state_.develop_;
    next.rotate_quarters = (next.rotate_quarters + 1) % 4;
    transform_crop_for_quarter_turns(next, 1);
    fit_geometry_crop(next);
    mutate_develop(std::move(next), DevelopEdit::Commit);
}

void StudioDevelopPresenter::flipHorizontal()
{
    DevelopParams next = state_.develop_;
    next.flip_horizontal = next.flip_horizontal == 0 ? 1 : 0;
    transform_crop_for_flip(next, true, false);
    fit_geometry_crop(next);
    mutate_develop(std::move(next), DevelopEdit::Commit);
}

void StudioDevelopPresenter::flipVertical()
{
    DevelopParams next = state_.develop_;
    next.flip_vertical = next.flip_vertical == 0 ? 1 : 0;
    transform_crop_for_flip(next, false, true);
    fit_geometry_crop(next);
    mutate_develop(std::move(next), DevelopEdit::Commit);
}

void StudioDevelopPresenter::setCropToolActive(const bool active)
{
    if (active && localEditing())
    {
        emit errorOccurred(QCoreApplication::translate(
            "DevelopPanel", "Finish mask editing before using global tools."));
        return;
    }
    if (state_.crop_tool_active_ == active)
    {
        return;
    }
    if (active)
    {
        static_cast<void>(clear_comparison());
    }
    state_.crop_tool_active_ = active;
    if (active)
    {
        if (state_.white_balance_pick_active_)
        {
            state_.white_balance_pick_active_ = false;
        }
        if (state_.mask_place_active_)
        {
            state_.mask_place_active_ = false;
        }
        if (state_.mask_parametric_assist_active_)
        {
            state_.mask_parametric_assist_active_ = false;
        }
        host_.zoom_mode(QStringLiteral("fit"));
        DevelopParams next = state_.develop_;
        // Geometry is rendered by the canonical Perspective owner while Crop
        // is stripped. The overlay therefore edits normalized coordinates in
        // the actual post-perspective frame without reproducing a homography
        // in QML.
        state_.crop_guide_ready_ = false;
        if (mutate_develop(std::move(next), DevelopEdit::Commit))
        {
            return;
        }
    }
    else
    {
        state_.crop_guide_ready_ = false;
        if (state_.develop_ != state_.saved_develop_)
        {
            DevelopParams pending = state_.develop_;
            if (mutate_develop(std::move(pending), DevelopEdit::Commit))
            {
                return;
            }
        }
    }
    emit editChanged();
    emit previewChanged();
    enqueue_preview();
}

void StudioDevelopPresenter::resetControl(const QString &name)
{
    DevelopParams next = edit_develop();
    const auto field = utf8_from_qstring(name);
    if (!localEditing())
        capture_instance_front_for_field(next, field);
    if (is_develop_mask_field(field))
    {
        auto reset = reset_develop_mask_field(next, field);
        if (!reset)
        {
            const auto reason = reset.error().context.find("reason");
            const auto reason_text = reason == reset.error().context.end() ?
                                         QStringLiteral("unknown") :
                                         qstring_from_utf8(reason->second);
            emit errorOccurred(
                QCoreApplication::translate("DevelopPanel", "Mask reset was rejected") +
                QStringLiteral(" [") + reason_text + QStringLiteral("]"));
            return;
        }
    }
    else if (!reset_develop_field(next, field))
    {
        return;
    }
    if (!localEditing())
        retarget_instance_edit_after_field(next, field);
    mutate_scoped_develop(std::move(next), DevelopEdit::Commit, true, field);
}

void StudioDevelopPresenter::resetSection(const QString &section)
{
    DevelopParams next = edit_develop();
    if (!reset_develop_section(next, utf8_from_qstring(section)))
    {
        return;
    }
    if (section == QLatin1String("geometry"))
    {
        state_.crop_aspect_ = QStringLiteral("free");
        state_.locked_crop_ratio_ = 0.0;
    }
    mutate_scoped_develop(std::move(next), DevelopEdit::Commit);
}

bool StudioDevelopPresenter::sectionModified(const QString &section) const
{
    return develop_section_modified(edit_develop(), utf8_from_qstring(section));
}

bool StudioDevelopPresenter::sectionEffectEnabled(const QString &section) const
{
    return develop_section_effect_enabled(edit_develop(), utf8_from_qstring(section));
}

void StudioDevelopPresenter::setSectionEffectEnabled(const QString &section, const bool enabled)
{
    DevelopParams next = edit_develop();
    if (!set_develop_section_effect_enabled(next, utf8_from_qstring(section), enabled))
    {
        return;
    }
    mutate_scoped_develop(std::move(next), DevelopEdit::Commit);
}

void StudioDevelopPresenter::resetAllEdits()
{
    state_.crop_aspect_ = QStringLiteral("free");
    state_.locked_crop_ratio_ = 0.0;
    mutate_develop(baseline_develop(), DevelopEdit::Commit);
}

void StudioDevelopPresenter::copyParametersSelected(const QVariantList &fields)
{
    if (selected_asset_id_.isEmpty())
        return;
    std::vector<std::string> selected_fields;
    const QString selection_error = collect_modified_parameter_selection(
        fields, baseline_develop(), state_.develop_, selected_fields);
    if (!selection_error.isEmpty())
    {
        emit errorOccurred(selection_error);
        return;
    }
    state_.copied_parameters_ =
        CopiedDevelopParameters{state_.develop_, std::move(selected_fields)};
    emit copiedParametersChanged();
    emit statusOccurred(QCoreApplication::translate("StudioPresenter", "Parameters copied."));
}

void StudioDevelopPresenter::pasteParameters()
{
    if (selected_asset_id_.isEmpty() || !state_.copied_parameters_)
        return;
    DevelopParams next = state_.develop_;
    auto applied = apply_develop_selected_fields(next, state_.copied_parameters_->source,
                                                 state_.copied_parameters_->fields);
    if (!applied)
    {
        emit errorOccurred(qstring_from_utf8(applied.error().message));
        return;
    }
    if (mutate_develop(std::move(next), DevelopEdit::Commit))
        emit statusOccurred(QCoreApplication::translate("StudioPresenter", "Parameters pasted."));
}

void StudioDevelopPresenter::pasteParametersToSelection()
{
    if (catalog_path_.isEmpty() || !state_.copied_parameters_)
        return;
    if (busy_ || catalog_operation_active_ || import_work_active_)
        return;
    auto ids = host_.selected_assets();
    if (ids.size() < 2)
        return;
    host_.begin_catalog_operation();
    const auto cancellation = host_.catalog_operation_token();
    DevelopApplyRequest request;
    request.source = state_.copied_parameters_->source;
    request.fields = state_.copied_parameters_->fields;
    request.asset_ids = std::move(ids);
    if (observed_catalog_revision_ >= 0)
        request.expected_revision = observed_catalog_revision_;
    request.cancellation = cancellation;
    const auto selected = utf8_from_qstring(selected_asset_id_);
    const bool reload_selected = std::find(request.asset_ids.begin(), request.asset_ids.end(),
                                           selected) != request.asset_ids.end();
    emit errorOccurred({});
    host_.catalog_progress(
        QCoreApplication::translate("StudioPresenter", "Applying parameters to selection…"), 0,
        static_cast<int>(request.asset_ids.size()), true);
    executor_.post(
        [this, request = std::move(request), selected, reload_selected]() mutable
        {
            Result<DevelopApplyResult> applied =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (host_.worker_develop_service() != nullptr)
            {
                applied = host_.worker_develop_service()->apply_develop_selection(
                    request,
                    [this](const std::size_t completed, const std::size_t total,
                           const DevelopApplyItemResult *)
                    {
                        QMetaObject::invokeMethod(
                            this,
                            [this, completed, total]()
                            {
                                if (!catalog_operation_active_)
                                    return;
                                host_.catalog_progress(
                                    QCoreApplication::translate(
                                        "StudioPresenter", "Applying parameters to selection…"),
                                    static_cast<int>(completed), static_cast<int>(total), true);
                            },
                            Qt::QueuedConnection);
                    });
            }
            QMetaObject::invokeMethod(
                this,
                [this, applied = std::move(applied), selected, reload_selected]() mutable
                {
                    host_.catalog_progress({}, 0, 0, false);
                    if (!applied)
                    {
                        emit errorOccurred(qstring_from_utf8(applied.error().message));
                        emit statusOccurred(QCoreApplication::translate(
                            "StudioPresenter", "Applying parameters to selection failed."));
                        return;
                    }
                    host_.observed_revision(applied.value().revision);
                    const auto total = applied.value().items.size();
                    if (applied.value().failed > 0 || applied.value().skipped > 0)
                    {
                        emit statusOccurred(QCoreApplication::translate(
                                                "StudioPresenter",
                                                "Applied parameters to %1 of %2 selected photos.")
                                                .arg(applied.value().applied)
                                                .arg(total));
                        for (const auto &item : applied.value().items)
                        {
                            if (item.status == DevelopApplyItemStatus::kFailed && item.error)
                            {
                                emit errorOccurred(qstring_from_utf8(item.error->message));
                                break;
                            }
                        }
                    }
                    else
                    {
                        emit statusOccurred(QCoreApplication::translate(
                            "StudioPresenter", "Parameters applied to the selection."));
                    }
                    if (reload_selected && utf8_from_qstring(selected_asset_id_) == selected)
                        load_develop_for_selection();
                    host_.reload_library();
                },
                Qt::QueuedConnection);
        });
}

void StudioDevelopPresenter::applyDevelopNumbers(const QVariantMap &fields, const DevelopEdit edit)
{
    if (fields.isEmpty())
    {
        return;
    }
    DevelopParams next = edit_develop();
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it)
    {
        if (it.key().trimmed().isEmpty())
        {
            return;
        }
        bool ok = false;
        const double value = it.value().toDouble(&ok);
        if (!ok || !std::isfinite(value))
        {
            return;
        }
        if (!apply_develop_field(next, utf8_from_qstring(it.key()), value))
        {
            return;
        }
    }
    std::optional<std::string> history_coalesce_key;
    if (edit == DevelopEdit::Commit && fields.size() == 1)
    {
        history_coalesce_key = utf8_from_qstring(fields.cbegin().key());
    }
    mutate_scoped_develop(std::move(next), edit, true, std::move(history_coalesce_key));
}

void StudioDevelopPresenter::previewDevelopNumbers(const QVariantMap &fields)
{
    applyDevelopNumbers(fields, DevelopEdit::Preview);
}

void StudioDevelopPresenter::setDevelopNumbers(const QVariantMap &fields)
{
    applyDevelopNumbers(fields, DevelopEdit::Commit);
}

void StudioDevelopPresenter::undoEdit()
{
    if (state_.undo_stack_.empty())
    {
        return;
    }
    state_.redo_stack_.push_back(state_.develop_);
    const auto previous = state_.undo_stack_.back();
    state_.undo_stack_.pop_back();
    mutate_develop(previous, DevelopEdit::Revert);
}

void StudioDevelopPresenter::redoEdit()
{
    if (state_.redo_stack_.empty())
    {
        return;
    }
    state_.undo_stack_.push_back(state_.develop_);
    const auto next = state_.redo_stack_.back();
    state_.redo_stack_.pop_back();
    mutate_develop(next, DevelopEdit::Revert);
}

void StudioDevelopPresenter::toggleBeforeAfter()
{
    static_cast<void>(clear_comparison());
    state_.before_after_ = !state_.before_after_;
    emit editChanged();
    enqueue_preview();
}

void StudioDevelopPresenter::toggleComparison()
{
    if (state_.comparison_active_)
    {
        if (clear_comparison())
        {
            emit editChanged();
            emit previewChanged();
        }
        return;
    }
    if (selected_asset_id_.isEmpty() || browse_mode_ != QLatin1String("develop"))
    {
        return;
    }
    if (state_.crop_tool_active_)
    {
        setCropToolActive(false);
    }
    if (state_.white_balance_pick_active_)
    {
        setWhiteBalancePickActive(false);
    }
    if (state_.mask_place_active_)
    {
        setMaskPlaceActive(false);
    }
    if (state_.mask_parametric_assist_active_)
    {
        setMaskParametricAssistActive(false);
    }
    if (state_.mask_overlay_visible_)
    {
        setMaskOverlay(state_.mask_overlay_target_, false);
    }

    state_.comparison_active_ = true;
    state_.comparison_before_requested_ = true;
    if (state_.before_after_)
    {
        if (!inspect_.previewLoading() && !inspect_.displayedDevelop().has_value() &&
            host_.adopt_preview_as_comparison())
            state_.comparison_before_requested_ = false;
        state_.before_after_ = false;
        emit editChanged();
        enqueue_preview();
        return;
    }
    emit editChanged();
    request_comparison_before();
}

bool StudioDevelopPresenter::working_source_size(double &width, double &height) const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset || !asset->width || !asset->height || *asset->width == 0 || *asset->height == 0)
    {
        return false;
    }
    width = static_cast<double>(*asset->width);
    height = static_cast<double>(*asset->height);
    const auto turns = ((state_.develop_.rotate_quarters % 4) + 4) % 4;
    if (turns == 1 || turns == 3)
    {
        std::swap(width, height);
    }
    return true;
}

void StudioDevelopPresenter::clamp_selected_crop(DevelopParams &params) const
{
    double width = 0.0;
    double height = 0.0;
    if (!working_source_size(width, height))
    {
        return;
    }
    clamp_develop_crop_min_extent(params, width, height);
}

double StudioDevelopPresenter::selected_source_aspect() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (asset && asset->width && asset->height && *asset->height > 0)
    {
        return static_cast<double>(*asset->width) / static_cast<double>(*asset->height);
    }
    return 1.5;
}

double StudioDevelopPresenter::selected_working_aspect() const
{
    if (state_.crop_tool_active_)
    {
        const QImage preview_image = inspect_.previewImage();
        if (!preview_image.isNull() && preview_image.height() > 0)
        {
            if (!inspect_.cropPreviewLayout().isEmpty())
                return (preview_image.width() *
                        inspect_.cropPreviewLayout().value("width").toDouble()) /
                       std::max(1.0, preview_image.height() *
                                         inspect_.cropPreviewLayout().value("height").toDouble());
            return static_cast<double>(preview_image.width()) /
                   static_cast<double>(preview_image.height());
        }
    }
    const auto asset = assets_.assetById(selected_asset_id_);
    if (asset && asset->width && asset->height && *asset->height > 0)
    {
        return working_image_aspect(state_.develop_.rotate_quarters, selected_source_aspect());
    }
    const QImage preview_image = inspect_.previewImage();
    if (!preview_image.isNull() && preview_image.height() > 0)
    {
        return static_cast<double>(preview_image.width()) /
               static_cast<double>(preview_image.height());
    }
    return working_image_aspect(state_.develop_.rotate_quarters, selected_source_aspect());
}

void StudioDevelopPresenter::constrain_geometry_crop(DevelopParams &params) const
{
    const double rotation = params.straighten_degrees;
    params.straighten_degrees = 0.0;
    constrain_crop_to_straighten(params, selected_working_aspect());
    params.straighten_degrees = rotation;
}

void StudioDevelopPresenter::fit_geometry_crop(DevelopParams &params) const
{
    const double rotation = params.straighten_degrees;
    params.straighten_degrees = 0.0;
    fit_crop_to_straighten(params, selected_working_aspect());
    params.straighten_degrees = rotation;
}

} // namespace ravo
