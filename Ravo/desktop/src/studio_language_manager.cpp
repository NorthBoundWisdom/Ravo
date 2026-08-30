#include "studio_language_manager.h"

#include <utility>

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSettings>
#include <QSet>
#include <QTranslator>
#include <QtQml/QQmlEngine>

static void initialize_locale_manifest_resource()
{
    Q_INIT_RESOURCE(studio_locale_manifest);
}

namespace
{

constexpr auto kLanguageSettingKey = "desktop/language";
constexpr auto kManifestPath = ":/ravo/studio/i18n/locales.json";

bool alias_matches(const QString &normalized, const QString &alias)
{
    if (alias.endsWith(QLatin1String("_*")))
        return normalized.startsWith(alias.left(alias.size() - 1));
    return normalized == alias;
}

} // namespace

namespace ravo
{

StudioLanguageManager::StudioLanguageManager(QObject *parent)
    : QObject(parent)
{
}

StudioLanguageManager::StudioLanguageManager(QStringList translation_directories, QObject *parent)
    : QObject(parent), translation_directories_override_(std::move(translation_directories))
{
}

StudioLanguageManager::~StudioLanguageManager()
{
    if (translator_ && QCoreApplication::instance())
        QCoreApplication::removeTranslator(translator_.get());
}

bool StudioLanguageManager::initialize(const QString &requested_language)
{
    if (!loadManifest())
        return false;

    QString selected = normalizeLanguage(requested_language);
    if (selected.isEmpty() && !requested_language.trimmed().isEmpty())
    {
        setError(QCoreApplication::translate("StudioLanguageManager", "Unsupported language: %1")
                     .arg(requested_language));
        return false;
    }
    if (selected.isEmpty())
    {
        QSettings settings;
        const QString stored = settings.value(QLatin1String(kLanguageSettingKey)).toString();
        selected = normalizeLanguage(stored);
        if (selected.isEmpty() && !stored.trimmed().isEmpty())
        {
            settings.remove(QLatin1String(kLanguageSettingKey));
            settings.sync();
            if (settings.status() != QSettings::NoError)
            {
                setError(QCoreApplication::translate(
                    "StudioLanguageManager", "Unable to repair the stored language setting."));
                return false;
            }
            selected = source_language_;
        }
    }
    if (selected.isEmpty())
        selected = systemLanguage();
    return activate(selected, false);
}

QString StudioLanguageManager::language() const
{
    return language_;
}

QStringList StudioLanguageManager::supportedLanguages() const
{
    QStringList result;
    result.reserve(definitions_.size());
    for (const auto &item : definitions_)
        result.push_back(item.code);
    return result;
}

QVariantList StudioLanguageManager::languageOptions() const
{
    QVariantList result;
    result.reserve(definitions_.size());
    for (const auto &item : definitions_)
    {
        result.push_back(QVariantMap{{QStringLiteral("code"), item.code},
                                     {QStringLiteral("label"), item.native_name}});
    }
    return result;
}

QString StudioLanguageManager::lastError() const
{
    return last_error_;
}

bool StudioLanguageManager::setLanguage(const QString &language)
{
    return activate(language, true);
}

void StudioLanguageManager::setQmlEngine(QQmlEngine *engine)
{
    qml_engine_ = engine;
}

bool StudioLanguageManager::loadManifest()
{
    if (manifest_loaded_)
        return true;

    initialize_locale_manifest_resource();
    QFile file(QString::fromLatin1(kManifestPath));
    if (!file.open(QIODevice::ReadOnly))
    {
        setError(QCoreApplication::translate("StudioLanguageManager",
                                             "Unable to open the language manifest."));
        return false;
    }

    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
    {
        setError(QCoreApplication::translate("StudioLanguageManager",
                                             "The language manifest is invalid."));
        return false;
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("schema")).toString() !=
        QLatin1String("ravo-studio-locales/v1"))
    {
        setError(QCoreApplication::translate("StudioLanguageManager",
                                             "The language manifest version is unsupported."));
        return false;
    }

    const QString source_locale = root.value(QStringLiteral("sourceLocale")).toString();
    const QJsonArray locales = root.value(QStringLiteral("locales")).toArray();
    QVector<LanguageDefinition> parsed;
    QSet<QString> codes;
    QSet<QString> exact_aliases;
    bool source_found = false;
    for (const auto &value : locales)
    {
        const QJsonObject object = value.toObject();
        LanguageDefinition item;
        item.code = object.value(QStringLiteral("code")).toString();
        item.native_name = object.value(QStringLiteral("nativeName")).toString();
        item.catalog = object.value(QStringLiteral("catalog")).toString();
        for (const auto &alias_value : object.value(QStringLiteral("aliases")).toArray())
            item.aliases.push_back(alias_value.toString().toLower());

        if (item.code.isEmpty() || item.native_name.isEmpty() || item.catalog.isEmpty() ||
            QFileInfo(item.catalog).fileName() != item.catalog ||
            !item.catalog.endsWith(QLatin1String(".ts")) || codes.contains(item.code))
        {
            setError(QCoreApplication::translate("StudioLanguageManager",
                                                 "The language manifest contains an invalid locale."));
            return false;
        }
        codes.insert(item.code);
        source_found = source_found || item.code == source_locale;
        const QString canonical = item.code.toLower();
        if (exact_aliases.contains(canonical))
        {
            setError(QCoreApplication::translate("StudioLanguageManager",
                                                 "The language manifest contains conflicting aliases."));
            return false;
        }
        exact_aliases.insert(canonical);
        for (const auto &alias : item.aliases)
        {
            if (alias.isEmpty() || exact_aliases.contains(alias))
            {
                setError(QCoreApplication::translate(
                    "StudioLanguageManager", "The language manifest contains conflicting aliases."));
                return false;
            }
            exact_aliases.insert(alias);
        }
        parsed.push_back(std::move(item));
    }
    if (parsed.isEmpty() || source_locale.isEmpty() || !source_found)
    {
        setError(QCoreApplication::translate("StudioLanguageManager",
                                             "The language manifest has no source locale."));
        return false;
    }

    definitions_ = std::move(parsed);
    source_language_ = source_locale;
    language_ = source_locale;
    manifest_loaded_ = true;
    return true;
}

bool StudioLanguageManager::activate(const QString &requested_language, const bool persist)
{
    if (!loadManifest())
        return false;
    const QString selected = normalizeLanguage(requested_language);
    const auto *selected_definition = definition(selected);
    if (selected_definition == nullptr)
    {
        setError(QCoreApplication::translate("StudioLanguageManager", "Unsupported language: %1")
                     .arg(requested_language));
        return false;
    }

    if (selected == language_)
    {
        if (persist)
        {
            QSettings settings;
            settings.setValue(QLatin1String(kLanguageSettingKey), selected);
            settings.sync();
            if (settings.status() != QSettings::NoError)
            {
                setError(QCoreApplication::translate("StudioLanguageManager",
                                                     "Unable to save the language setting."));
                return false;
            }
        }
        return true;
    }

    std::unique_ptr<QTranslator> candidate;
    if (selected != source_language_)
    {
        candidate = std::make_unique<QTranslator>();
        const QString file_name = QFileInfo(selected_definition->catalog).completeBaseName() +
                                  QStringLiteral(".qm");
        QStringList attempted_paths;
        bool loaded = false;
        for (const auto &directory : translationDirectories())
        {
            const QString file_path = QDir(directory).filePath(file_name);
            attempted_paths.push_back(file_path);
            if (!QFileInfo(file_path).isFile())
                continue;
            if (!candidate->load(file_path))
            {
                setError(QCoreApplication::translate(
                             "StudioLanguageManager", "Unable to load translation package for %1: %2")
                             .arg(selected, file_path));
                return false;
            }
            loaded = true;
            break;
        }
        if (!loaded)
        {
            setError(QCoreApplication::translate(
                         "StudioLanguageManager",
                         "Translation package for %1 is missing. Searched: %2")
                         .arg(selected, attempted_paths.join(QStringLiteral(", "))));
            return false;
        }
        if (!QCoreApplication::installTranslator(candidate.get()))
        {
            setError(QCoreApplication::translate(
                         "StudioLanguageManager", "Unable to install translation package for %1.")
                         .arg(selected));
            return false;
        }
    }

    if (persist)
    {
        QSettings settings;
        settings.setValue(QLatin1String(kLanguageSettingKey), selected);
        settings.sync();
        if (settings.status() != QSettings::NoError)
        {
            if (candidate)
                QCoreApplication::removeTranslator(candidate.get());
            setError(QCoreApplication::translate("StudioLanguageManager",
                                                 "Unable to save the language setting."));
            return false;
        }
    }
    if (translator_)
        QCoreApplication::removeTranslator(translator_.get());
    translator_ = std::move(candidate);
    language_ = selected;
    if (!last_error_.isEmpty())
    {
        last_error_.clear();
        emit languageErrorChanged();
    }
    if (qml_engine_)
        qml_engine_->retranslate();
    emit languageChanged();
    return true;
}

QString StudioLanguageManager::normalizeLanguage(const QString &language) const
{
    const QString normalized =
        language.trimmed().replace(QLatin1Char('-'), QLatin1Char('_')).toLower();
    if (normalized.isEmpty())
        return {};
    for (const auto &item : definitions_)
    {
        if (normalized == item.code.toLower())
            return item.code;
        for (const auto &alias : item.aliases)
        {
            if (alias_matches(normalized, alias))
                return item.code;
        }
    }
    return {};
}

QString StudioLanguageManager::systemLanguage() const
{
    for (const auto &candidate : QLocale::system().uiLanguages())
    {
        const QString normalized = normalizeLanguage(candidate);
        if (!normalized.isEmpty())
            return normalized;
    }
    return source_language_;
}

QStringList StudioLanguageManager::translationDirectories() const
{
    if (!translation_directories_override_.isEmpty())
        return translation_directories_override_;
    const QDir application_directory(QCoreApplication::applicationDirPath());
    QStringList directories{application_directory.filePath(QStringLiteral("i18n"))};
#ifdef Q_OS_MACOS
    directories.push_back(application_directory.filePath(QStringLiteral("../Resources/i18n")));
#endif
    return directories;
}

const StudioLanguageManager::LanguageDefinition *
StudioLanguageManager::definition(const QString &code) const
{
    for (const auto &item : definitions_)
    {
        if (item.code == code)
            return &item;
    }
    return nullptr;
}

void StudioLanguageManager::setError(QString message)
{
    if (last_error_ == message)
        return;
    last_error_ = std::move(message);
    qWarning().noquote() << last_error_;
    emit languageErrorChanged();
}

} // namespace ravo
