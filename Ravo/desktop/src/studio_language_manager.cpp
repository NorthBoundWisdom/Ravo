#include "studio_language_manager.h"

#include <utility>

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QSettings>
#include <QTranslator>
#include <QtQml/QQmlEngine>

namespace ravo
{
namespace
{

constexpr auto kLanguageSettingKey = "desktop/language";
constexpr auto kChineseLanguage = "zh_CN";
constexpr auto kEnglishLanguage = "en_US";
constexpr auto kTranslationFilePrefix = "RavoStudio_";

} // namespace

StudioLanguageManager::StudioLanguageManager(QObject *parent)
    : QObject(parent)
{
}

StudioLanguageManager::~StudioLanguageManager()
{
    if (translator_ && QCoreApplication::instance())
        QCoreApplication::removeTranslator(translator_.get());
}

bool StudioLanguageManager::initialize(const QString &requested_language)
{
    QString selected = normalizeLanguage(requested_language);
    if (selected.isEmpty() && !requested_language.trimmed().isEmpty())
    {
        setError(QCoreApplication::translate("StudioLanguageManager", "Unsupported language: %1")
                     .arg(requested_language));
        return false;
    }
    if (selected.isEmpty())
    {
        const QSettings settings;
        selected = normalizeLanguage(settings.value(QLatin1String(kLanguageSettingKey)).toString());
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
    return {QString::fromLatin1(kEnglishLanguage), QString::fromLatin1(kChineseLanguage)};
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

bool StudioLanguageManager::activate(const QString &requested_language, const bool persist)
{
    const QString selected = normalizeLanguage(requested_language);
    if (selected.isEmpty())
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
        }
        return true;
    }

    std::unique_ptr<QTranslator> candidate;
    if (selected == QLatin1String(kChineseLanguage))
    {
        candidate = std::make_unique<QTranslator>();
        const QString file_name =
            QString::fromLatin1(kTranslationFilePrefix) + selected + QStringLiteral(".qm");
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
                setError(QCoreApplication::translate("StudioLanguageManager",
                                                     "Unable to load translation package: %1")
                             .arg(file_path));
                return false;
            }
            loaded = true;
            break;
        }
        if (!loaded)
        {
            setError(
                QCoreApplication::translate("StudioLanguageManager",
                                            "Chinese translation package is missing. Searched: %1")
                    .arg(attempted_paths.join(QStringLiteral(", "))));
            return false;
        }
        if (!QCoreApplication::installTranslator(candidate.get()))
        {
            setError(QCoreApplication::translate(
                "StudioLanguageManager", "Unable to install the Chinese translation package."));
            return false;
        }
    }

    if (translator_)
        QCoreApplication::removeTranslator(translator_.get());
    translator_ = std::move(candidate);
    language_ = selected;
    if (persist)
    {
        QSettings settings;
        settings.setValue(QLatin1String(kLanguageSettingKey), selected);
    }
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

QString StudioLanguageManager::normalizeLanguage(const QString &language)
{
    const QString normalized =
        language.trimmed().replace(QLatin1Char('-'), QLatin1Char('_')).toLower();
    if (normalized == QLatin1String("zh") || normalized == QLatin1String("zh_cn") ||
        normalized.startsWith(QLatin1String("zh_cn_")))
        return QString::fromLatin1(kChineseLanguage);
    if (normalized == QLatin1String("en") || normalized == QLatin1String("en_us") ||
        normalized.startsWith(QLatin1String("en_us_")))
        return QString::fromLatin1(kEnglishLanguage);
    return {};
}

QString StudioLanguageManager::systemLanguage()
{
    for (const auto &candidate : QLocale::system().uiLanguages())
    {
        const QString normalized = normalizeLanguage(candidate);
        if (!normalized.isEmpty())
            return normalized;
    }
    return QString::fromLatin1(kEnglishLanguage);
}

QStringList StudioLanguageManager::translationDirectories() const
{
    const QDir application_directory(QCoreApplication::applicationDirPath());
    QStringList directories{application_directory.filePath(QStringLiteral("i18n"))};
#ifdef Q_OS_MACOS
    directories.push_back(application_directory.filePath(QStringLiteral("../Resources/i18n")));
#endif
    return directories;
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
