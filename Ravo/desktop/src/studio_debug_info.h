#pragma once

#include <utility>

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QString>
#include <QStringList>
#include <initializer_list>

namespace ravo
{

inline constexpr auto kPhotoDebugInfoKind = "ravo.debug.photo";
inline constexpr auto kPhotoParametersDebugInfoKind = "ravo.debug.parameters";
inline constexpr auto kPresetDebugInfoKind = "ravo.debug.preset";
inline constexpr int kDebugInfoVersion = 1;

struct PhotoDebugIdentity
{
    QString catalog;
    QString asset_id;
    QString uri;
    QString path;
    QString fingerprint;
    QString media_type;
    QString display_name;
    QString width;
    QString height;
    QString size_bytes;
    bool has_edits = false;
    QString import_state;
};

struct PresetDebugIdentity
{
    QString name;
    QString path;
    QString kind;
    QString sha256;
    QString size_bytes;
    QString mtime_unix_ms;
};

struct PhotoParametersDebugInfo
{
    QString catalog;
    QString asset_id;
    QString display_name;
    QString recipe_state;
    QString recipe_json;
};

// Pasteable Studio identity. First line is "<kind> <version>". Following lines
// are "key=value" with a single-line value; parsers split on the first '='.
[[nodiscard]] inline QString sanitize_debug_info_value(QString value)
{
    value.replace(QLatin1Char('\r'), QLatin1Char(' '));
    value.replace(QLatin1Char('\n'), QLatin1Char(' '));
    value.replace(QChar(QChar::Null), QLatin1Char(' '));
    return value;
}

[[nodiscard]] inline QString
format_debug_info_block(const QString &kind, const int version,
                        const std::initializer_list<std::pair<QString, QString>> fields)
{
    QStringList lines;
    lines.push_back(kind + QLatin1Char(' ') + QString::number(version));
    for (const auto &field : fields)
    {
        lines.push_back(field.first + QLatin1Char('=') + sanitize_debug_info_value(field.second));
    }
    return lines.join(QLatin1Char('\n'));
}

[[nodiscard]] inline QString format_photo_debug_info(const PhotoDebugIdentity &identity)
{
    return format_debug_info_block(
        QLatin1String(kPhotoDebugInfoKind), kDebugInfoVersion,
        {{QStringLiteral("catalog"), identity.catalog},
         {QStringLiteral("asset_id"), identity.asset_id},
         {QStringLiteral("uri"), identity.uri},
         {QStringLiteral("path"), identity.path},
         {QStringLiteral("fingerprint"), identity.fingerprint},
         {QStringLiteral("media_type"), identity.media_type},
         {QStringLiteral("display_name"), identity.display_name},
         {QStringLiteral("width"), identity.width},
         {QStringLiteral("height"), identity.height},
         {QStringLiteral("size_bytes"), identity.size_bytes},
         {QStringLiteral("has_edits"),
          identity.has_edits ? QStringLiteral("true") : QStringLiteral("false")},
         {QStringLiteral("import_state"), identity.import_state}});
}

[[nodiscard]] inline QString format_preset_debug_info(const PresetDebugIdentity &identity)
{
    return format_debug_info_block(QLatin1String(kPresetDebugInfoKind), kDebugInfoVersion,
                                   {{QStringLiteral("name"), identity.name},
                                    {QStringLiteral("path"), identity.path},
                                    {QStringLiteral("kind"), identity.kind},
                                    {QStringLiteral("sha256"), identity.sha256},
                                    {QStringLiteral("size_bytes"), identity.size_bytes},
                                    {QStringLiteral("mtime_unix_ms"), identity.mtime_unix_ms}});
}

[[nodiscard]] inline QString
format_photo_parameters_debug_info(const PhotoParametersDebugInfo &parameters)
{
    return format_debug_info_block(QLatin1String(kPhotoParametersDebugInfoKind), kDebugInfoVersion,
                                   {{QStringLiteral("catalog"), parameters.catalog},
                                    {QStringLiteral("asset_id"), parameters.asset_id},
                                    {QStringLiteral("display_name"), parameters.display_name},
                                    {QStringLiteral("recipe_state"), parameters.recipe_state},
                                    {QStringLiteral("recipe_json"), parameters.recipe_json}});
}

[[nodiscard]] inline bool write_clipboard_text(const QString &text)
{
    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance()) == nullptr)
        return false;
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr)
        return false;
    clipboard->setText(text, QClipboard::Clipboard);
    return true;
}

} // namespace ravo
