#pragma once

#include <memory>

#include <QObject>
#include <QString>
#include <QVariantList>

class QNetworkAccessManager;
class QNetworkReply;

namespace ravo
{

class StudioAssistantController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString endpoint READ endpoint NOTIFY settingsChanged)
    Q_PROPERTY(QString model READ model NOTIFY settingsChanged)
    Q_PROPERTY(QString apiKey READ apiKey NOTIFY settingsChanged)
    Q_PROPERTY(bool configured READ configured NOTIFY settingsChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY errorChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QVariantList messages READ messages NOTIFY messagesChanged)

public:
    explicit StudioAssistantController(QObject *parent = nullptr);
    ~StudioAssistantController() override;

    [[nodiscard]] bool initialize();
    [[nodiscard]] QString endpoint() const;
    [[nodiscard]] QString model() const;
    [[nodiscard]] QString apiKey() const;
    [[nodiscard]] bool configured() const;
    [[nodiscard]] QString lastError() const;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] QVariantList messages() const;

    Q_INVOKABLE bool setEndpoint(const QString &endpoint);
    Q_INVOKABLE bool setModel(const QString &model);
    Q_INVOKABLE bool setApiKey(const QString &api_key);
    Q_INVOKABLE bool send(const QString &text, const QString &photo_context = QString());
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void clearMessages();

    [[nodiscard]] static QString defaultEndpoint();
    [[nodiscard]] static QString defaultModel();
    [[nodiscard]] static QString normalizeEndpoint(const QString &endpoint);
    [[nodiscard]] static QString normalizeModel(const QString &model);

signals:
    void settingsChanged();
    void errorChanged();
    void busyChanged();
    void messagesChanged();

private:
    void setError(QString message);
    void finishReply();
    [[nodiscard]] QString resolvedApiKey() const;
    [[nodiscard]] bool persist(const QString &key, const QString &value);
    void handleFinished();

    std::unique_ptr<QNetworkAccessManager> network_;
    QNetworkReply *reply_ = nullptr;
    QString endpoint_;
    QString model_;
    QString api_key_;
    QString last_error_;
    QVariantList messages_;
};

} // namespace ravo
