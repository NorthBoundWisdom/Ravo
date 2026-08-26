#pragma once

#include <memory>

#include <QObject>
#include <QString>
#include <QStringList>

class QQmlEngine;
class QTranslator;

namespace ravo
{

class StudioLanguageManager final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString language READ language NOTIFY languageChanged)
    Q_PROPERTY(QStringList supportedLanguages READ supportedLanguages CONSTANT)
    Q_PROPERTY(QString lastError READ lastError NOTIFY languageErrorChanged)

public:
    explicit StudioLanguageManager(QObject *parent = nullptr);
    ~StudioLanguageManager() override;

    [[nodiscard]] bool initialize(const QString &requested_language = {});
    [[nodiscard]] QString language() const;
    [[nodiscard]] QStringList supportedLanguages() const;
    [[nodiscard]] QString lastError() const;

    Q_INVOKABLE bool setLanguage(const QString &language);
    void setQmlEngine(QQmlEngine *engine);

signals:
    void languageChanged();
    void languageErrorChanged();

private:
    [[nodiscard]] bool activate(const QString &requested_language, bool persist);
    [[nodiscard]] static QString normalizeLanguage(const QString &language);
    [[nodiscard]] static QString systemLanguage();
    [[nodiscard]] QStringList translationDirectories() const;
    void setError(QString message);

    std::unique_ptr<QTranslator> translator_;
    QQmlEngine *qml_engine_ = nullptr;
    QString language_ = QStringLiteral("en_US");
    QString last_error_;
};

} // namespace ravo
