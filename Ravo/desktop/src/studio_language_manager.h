#pragma once

#include <memory>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVector>

class QQmlEngine;
class QTranslator;

namespace ravo
{

class StudioLanguageManager final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString language READ language NOTIFY languageChanged)
    Q_PROPERTY(QStringList supportedLanguages READ supportedLanguages CONSTANT)
    Q_PROPERTY(QVariantList languageOptions READ languageOptions CONSTANT)
    Q_PROPERTY(QString lastError READ lastError NOTIFY languageErrorChanged)

public:
    explicit StudioLanguageManager(QObject *parent = nullptr);
    StudioLanguageManager(QStringList translation_directories, QObject *parent = nullptr);
    ~StudioLanguageManager() override;

    [[nodiscard]] bool initialize(const QString &requested_language = {});
    [[nodiscard]] QString language() const;
    [[nodiscard]] QStringList supportedLanguages() const;
    [[nodiscard]] QVariantList languageOptions() const;
    [[nodiscard]] QString lastError() const;

    Q_INVOKABLE bool setLanguage(const QString &language);
    void setQmlEngine(QQmlEngine *engine);

signals:
    void languageChanged();
    void languageErrorChanged();

private:
    struct LanguageDefinition
    {
        QString code;
        QString native_name;
        QString catalog;
        QStringList aliases;
    };

    [[nodiscard]] bool loadManifest();
    [[nodiscard]] bool activate(const QString &requested_language, bool persist);
    [[nodiscard]] QString normalizeLanguage(const QString &language) const;
    [[nodiscard]] QString systemLanguage() const;
    [[nodiscard]] QStringList translationDirectories() const;
    [[nodiscard]] const LanguageDefinition *definition(const QString &code) const;
    void setError(QString message);

    std::unique_ptr<QTranslator> translator_;
    QQmlEngine *qml_engine_ = nullptr;
    QVector<LanguageDefinition> definitions_;
    QStringList translation_directories_override_;
    QString source_language_ = QStringLiteral("en_US");
    QString language_ = QStringLiteral("en_US");
    QString last_error_;
    bool manifest_loaded_ = false;
};

} // namespace ravo
