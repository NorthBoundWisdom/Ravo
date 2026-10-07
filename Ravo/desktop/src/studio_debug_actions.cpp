#include "studio_import_workspace.h"
#include "ravo/desktop/studio_presenter.h"

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

#include "studio_preset_paths.h"
namespace ravo
{
QString StudioPresenter::selectedPhotoDebugInfo() const
{
    if (importPageOpen())
    {
        const QString path = importContextPath();
        if (path.isEmpty())
            return {};
        const auto index = import_workspace_->candidates.index(import_context_row_, 0);
        return QStringLiteral(
                   "ravo.debug.import-photo 1\npath=%1\nuri=%2\ndisplay_name=%3\nsize_bytes=%4\nduplicate=%5")
            .arg(
                path, QUrl::fromLocalFile(path).toString(),
                import_workspace_->candidates.data(index, ImportCandidateListModel::DisplayNameRole)
                    .toString(),
                import_workspace_->candidates.data(index, ImportCandidateListModel::SizeBytesRole)
                    .toString(),
                import_workspace_->candidates.data(index, ImportCandidateListModel::DuplicateRole)
                        .toBool() ?
                    QStringLiteral("true") :
                    QStringLiteral("false"));
    }
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset)
        return {};
    PhotoDebugIdentity identity;
    identity.catalog = catalog_path_;
    identity.asset_id = selected_asset_id_;
    identity.uri = qstring_from_utf8(asset->normalized_uri);
    identity.path = QUrl(identity.uri).toLocalFile();
    if (asset->content_fingerprint)
        identity.fingerprint = qstring_from_utf8(*asset->content_fingerprint);
    identity.media_type = qstring_from_utf8(asset->media_type);
    identity.display_name = qstring_from_utf8(asset_display_name(*asset));
    if (asset->width)
        identity.width = QString::number(*asset->width);
    if (asset->height)
        identity.height = QString::number(*asset->height);
    identity.size_bytes = QString::number(asset->size_bytes);
    identity.has_edits = asset->has_edits;
    identity.import_state = qstring_from_utf8(asset->import_state);
    return format_photo_debug_info(identity);
}

QString StudioPresenter::selectedPhotoParametersDebugInfo() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset || !develop_presenter_->state().develop_loaded_)
        return {};
    const AssetDescriptor descriptor{asset->id, asset->normalized_uri, asset->content_fingerprint};
    auto recipe = recipe_from_develop(descriptor, develop_presenter_->state().develop_);
    if (!recipe)
        return {};
    auto serialized = serialize_recipe(recipe.value());
    if (!serialized)
        return {};

    PhotoParametersDebugInfo parameters;
    parameters.catalog = catalog_path_;
    parameters.asset_id = selected_asset_id_;
    parameters.display_name = qstring_from_utf8(asset_display_name(*asset));
    parameters.recipe_state =
        develop_presenter_->state().develop_ == develop_presenter_->state().saved_develop_ ?
            QStringLiteral("saved") :
            QStringLiteral("pending");
    parameters.recipe_json = qstring_from_utf8(serialized.value());
    return format_photo_parameters_debug_info(parameters);
}

QString StudioPresenter::presetDebugInfo(const QString &path) const
{
    QString input_path = path.trimmed();
    if (input_path.startsWith(QStringLiteral("file:")))
        input_path = QUrl(input_path).toLocalFile();
    const QFileInfo info(input_path);
    if (!info.exists() || !info.isFile())
        return {};
    if (info.size() > static_cast<qint64>(kRecipeStyleFileMaxBytes))
        return {};
    const QString canonical =
        info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();
    QString name = info.completeBaseName();
    QString kind;
    for (const auto &entry : develop_presenter_->state().develop_presets_)
    {
        const auto listed = entry.toMap();
        const QFileInfo listed_info(listed.value(QStringLiteral("path")).toString());
        const QString listed_path = listed_info.canonicalFilePath().isEmpty() ?
                                        listed_info.absoluteFilePath() :
                                        listed_info.canonicalFilePath();
        if (listed_path == canonical)
        {
            name = listed.value(QStringLiteral("name")).toString();
            kind = listed.value(QStringLiteral("kind")).toString();
            break;
        }
    }
    if (kind.isEmpty())
    {
        auto text = read_utf8_text_file(utf8_from_qstring(canonical), kRecipeStyleFileMaxBytes);
        if (!text)
            return {};
        if (is_crs_xmp_document(text.value()))
        {
            kind = QStringLiteral("crs");
            auto parsed_name = crs_xmp_preset_name(text.value());
            if (parsed_name && !parsed_name.value().empty())
                name = QString::fromStdString(parsed_name.value());
        }
        else
        {
            auto style = parse_recipe_style_json(text.value());
            if (!style)
                return {};
            kind = QStringLiteral("style");
            name = QString::fromStdString(style.value().name);
        }
    }
    QFile file(canonical);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray bytes = file.readAll();
    if (bytes.size() != info.size())
        return {};
    PresetDebugIdentity identity;
    identity.name = name;
    identity.path = canonical;
    identity.kind = kind;
    identity.sha256 =
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    identity.size_bytes = QString::number(bytes.size());
    identity.mtime_unix_ms = QString::number(info.lastModified().toMSecsSinceEpoch());
    return format_preset_debug_info(identity);
}

void StudioPresenter::copySelectedPhotoDebugInfo()
{
    const QString text = selectedPhotoDebugInfo();
    if (text.isEmpty())
        return;
    if (!write_clipboard_text(text))
    {
        setError(QCoreApplication::translate(
            "StudioPresenter", "Photo information could not be copied to the clipboard."));
        return;
    }
    setStatus(QCoreApplication::translate("StudioPresenter", "Photo information copied."));
}

void StudioPresenter::copySelectedPhotoParametersDebugInfo()
{
    const QString text = selectedPhotoParametersDebugInfo();
    if (text.isEmpty())
    {
        setError(
            QCoreApplication::translate("StudioPresenter", "Photo parameters could not be read."));
        return;
    }
    if (!write_clipboard_text(text))
    {
        setError(QCoreApplication::translate(
            "StudioPresenter", "Photo parameters could not be copied to the clipboard."));
        return;
    }
    setError({});
    setStatus(QCoreApplication::translate("StudioPresenter", "Photo parameters copied."));
}

void StudioPresenter::copyPresetDebugInfo(const QString &path)
{
    QString input_path = path.trimmed();
    if (input_path.startsWith(QStringLiteral("file:")))
        input_path = QUrl(input_path).toLocalFile();
    if (!QFileInfo::exists(input_path) || !QFileInfo(input_path).isFile())
    {
        setError(QCoreApplication::translate("StudioPresenter", "Preset file was not found."));
        return;
    }
    const QString text = presetDebugInfo(input_path);
    if (text.isEmpty())
    {
        setError(QCoreApplication::translate("StudioPresenter",
                                             "Preset information could not be read."));
        return;
    }
    if (!write_clipboard_text(text))
    {
        setError(QCoreApplication::translate(
            "StudioPresenter", "Preset information could not be copied to the clipboard."));
        return;
    }
    setStatus(QCoreApplication::translate("StudioPresenter", "Preset information copied."));
}

} // namespace ravo
