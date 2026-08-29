#include "ravo/desktop/studio_assistant_controller.h"

#include <utility>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QSettings>
#include <QUrl>
#include <QVariantMap>

namespace ravo
{
namespace
{

constexpr auto kEndpointKey = "desktop/assistant/endpoint";
constexpr auto kModelKey = "desktop/assistant/model";
constexpr auto kApiKeyKey = "desktop/assistant/api_key";
constexpr auto kDefaultEndpoint = "https://api.x.ai/v1";
constexpr auto kDefaultModel = "grok-4.5";
constexpr int kMaxModelLength = 128;
constexpr int kMaxPromptLength = 16000;

[[nodiscard]] QVariantMap chat_message(const QString &role, const QString &text)
{
    return {{QStringLiteral("role"), role}, {QStringLiteral("text"), text}};
}

} // namespace

QString StudioAssistantController::defaultEndpoint()
{
    return QString::fromLatin1(kDefaultEndpoint);
}

QString StudioAssistantController::defaultModel()
{
    return QString::fromLatin1(kDefaultModel);
}

QString StudioAssistantController::normalizeEndpoint(const QString &endpoint)
{
    const QString trimmed = endpoint.trimmed();
    if (trimmed.isEmpty())
        return {};
    QUrl url(trimmed);
    if (!url.isValid() || url.host().isEmpty())
        return {};
    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("https") && scheme != QLatin1String("http"))
        return {};
    url.setScheme(scheme);
    url.setFragment(QString());
    QString path = url.path();
    while (path.size() > 1 && path.endsWith(QLatin1Char('/')))
        path.chop(1);
    url.setPath(path);
    if (url.port() == -1)
        url.setPort(-1);
    return url.toString(QUrl::FullyEncoded | QUrl::RemoveUserInfo);
}

QString StudioAssistantController::normalizeModel(const QString &model)
{
    const QString trimmed = model.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > kMaxModelLength)
        return {};
    for (const QChar character : trimmed)
    {
        if (character.isSpace() || character.category() == QChar::Other_Control)
            return {};
    }
    return trimmed;
}

StudioAssistantController::StudioAssistantController(QObject *parent)
    : QObject(parent)
    , network_(std::make_unique<QNetworkAccessManager>())
    , endpoint_(defaultEndpoint())
    , model_(defaultModel())
{
}

StudioAssistantController::~StudioAssistantController()
{
    cancel();
}

bool StudioAssistantController::initialize()
{
    QSettings settings;
    const QString stored_endpoint = settings.value(QLatin1String(kEndpointKey)).toString();
    if (!stored_endpoint.trimmed().isEmpty())
    {
        const QString normalized = normalizeEndpoint(stored_endpoint);
        if (normalized.isEmpty())
        {
            settings.remove(QLatin1String(kEndpointKey));
            settings.sync();
            if (settings.status() != QSettings::NoError)
            {
                setError(QCoreApplication::translate(
                    "StudioAssistant", "Unable to repair the stored assistant endpoint."));
                return false;
            }
            endpoint_ = defaultEndpoint();
        }
        else
        {
            endpoint_ = normalized;
        }
    }
    const QString stored_model = settings.value(QLatin1String(kModelKey)).toString();
    if (!stored_model.trimmed().isEmpty())
    {
        const QString normalized = normalizeModel(stored_model);
        if (normalized.isEmpty())
        {
            settings.remove(QLatin1String(kModelKey));
            settings.sync();
            if (settings.status() != QSettings::NoError)
            {
                setError(QCoreApplication::translate(
                    "StudioAssistant", "Unable to repair the stored assistant model."));
                return false;
            }
            model_ = defaultModel();
        }
        else
        {
            model_ = normalized;
        }
    }
    api_key_ = settings.value(QLatin1String(kApiKeyKey)).toString();
    emit settingsChanged();
    return true;
}

QString StudioAssistantController::endpoint() const
{
    return endpoint_;
}

QString StudioAssistantController::model() const
{
    return model_;
}

QString StudioAssistantController::apiKey() const
{
    return api_key_;
}

bool StudioAssistantController::configured() const
{
    return !endpoint_.isEmpty() && !model_.isEmpty() && !resolvedApiKey().isEmpty();
}

QString StudioAssistantController::lastError() const
{
    return last_error_;
}

bool StudioAssistantController::busy() const noexcept
{
    return reply_ != nullptr;
}

QVariantList StudioAssistantController::messages() const
{
    return messages_;
}

bool StudioAssistantController::persist(const QString &key, const QString &value)
{
    QSettings settings;
    settings.setValue(key, value);
    settings.sync();
    if (settings.status() != QSettings::NoError)
    {
        setError(QCoreApplication::translate("StudioAssistant",
                                             "Unable to save the assistant setting."));
        return false;
    }
    return true;
}

bool StudioAssistantController::setEndpoint(const QString &endpoint)
{
    const QString normalized = normalizeEndpoint(endpoint);
    if (normalized.isEmpty())
    {
        setError(QCoreApplication::translate(
            "StudioAssistant", "Assistant URL must be an http or https endpoint."));
        return false;
    }
    if (normalized == endpoint_)
        return true;
    if (!persist(QLatin1String(kEndpointKey), normalized))
        return false;
    endpoint_ = normalized;
    setError({});
    emit settingsChanged();
    return true;
}

bool StudioAssistantController::setModel(const QString &model)
{
    const QString normalized = normalizeModel(model);
    if (normalized.isEmpty())
    {
        setError(QCoreApplication::translate(
            "StudioAssistant", "Assistant model must be a non-empty identifier."));
        return false;
    }
    if (normalized == model_)
        return true;
    if (!persist(QLatin1String(kModelKey), normalized))
        return false;
    model_ = normalized;
    setError({});
    emit settingsChanged();
    return true;
}

bool StudioAssistantController::setApiKey(const QString &api_key)
{
    if (api_key == api_key_)
        return true;
    if (!persist(QLatin1String(kApiKeyKey), api_key))
        return false;
    api_key_ = api_key;
    setError({});
    emit settingsChanged();
    return true;
}

QString StudioAssistantController::resolvedApiKey() const
{
    if (!api_key_.trimmed().isEmpty())
        return api_key_.trimmed();
    return QProcessEnvironment::systemEnvironment().value(QStringLiteral("XAI_API_KEY")).trimmed();
}

void StudioAssistantController::setError(QString message)
{
    if (last_error_ == message)
        return;
    last_error_ = std::move(message);
    emit errorChanged();
}

void StudioAssistantController::cancel()
{
    if (reply_ == nullptr)
        return;
    reply_->abort();
}

void StudioAssistantController::clearMessages()
{
    if (messages_.isEmpty())
        return;
    messages_.clear();
    emit messagesChanged();
}

void StudioAssistantController::finishReply()
{
    if (reply_ == nullptr)
        return;
    reply_->deleteLater();
    reply_ = nullptr;
    emit busyChanged();
}

bool StudioAssistantController::send(const QString &text, const QString &photo_context)
{
    const QString prompt = text.trimmed();
    if (prompt.isEmpty() || prompt.size() > kMaxPromptLength)
    {
        setError(QCoreApplication::translate("StudioAssistant",
                                             "Assistant prompt is empty or too long."));
        return false;
    }
    if (busy())
    {
        setError(QCoreApplication::translate("StudioAssistant",
                                             "An assistant request is already in progress."));
        return false;
    }
    const QString key = resolvedApiKey();
    if (endpoint_.isEmpty() || model_.isEmpty() || key.isEmpty())
    {
        setError(QCoreApplication::translate(
            "StudioAssistant",
            "Set the assistant URL, model, and API key in Settings before sending."));
        return false;
    }

    QUrl url(endpoint_);
    QString path = url.path();
    if (!path.endsWith(QLatin1String("/chat/completions")))
    {
        if (path.endsWith(QLatin1Char('/')))
            path.chop(1);
        path += QLatin1String("/chat/completions");
        url.setPath(path);
    }

    QJsonArray payload_messages;
    QString system = QCoreApplication::translate(
        "StudioAssistant",
        "You are the Ravo Studio photo-editing assistant. Answer about the current catalog, "
        "Develop edits, and export. Do not invent file changes.");
    if (!photo_context.trimmed().isEmpty())
    {
        system += QLatin1Char('\n');
        system += QCoreApplication::translate("StudioAssistant", "Selected photo: %1")
                      .arg(photo_context.trimmed());
    }
    payload_messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                                        {QStringLiteral("content"), system}});
    for (const auto &item : std::as_const(messages_))
    {
        const auto row = item.toMap();
        payload_messages.append(QJsonObject{
            {QStringLiteral("role"), row.value(QStringLiteral("role")).toString()},
            {QStringLiteral("content"), row.value(QStringLiteral("text")).toString()}});
    }
    payload_messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                        {QStringLiteral("content"), prompt}});

    QJsonObject body;
    body.insert(QStringLiteral("model"), model_);
    body.insert(QStringLiteral("messages"), payload_messages);
    body.insert(QStringLiteral("stream"), false);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader(QByteArrayLiteral("Authorization"),
                         QByteArrayLiteral("Bearer ") + key.toUtf8());
    request.setTransferTimeout(60000);

    messages_.push_back(chat_message(QStringLiteral("user"), prompt));
    emit messagesChanged();
    setError({});
    reply_ = network_->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    emit busyChanged();
    QObject::connect(reply_, &QNetworkReply::finished, this,
                     &StudioAssistantController::handleFinished);
    return true;
}

void StudioAssistantController::handleFinished()
{
    if (reply_ == nullptr)
        return;
    const auto status =
        reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray bytes = reply_->readAll();
    const auto network_error = reply_->error();
    const bool aborted = network_error == QNetworkReply::OperationCanceledError;
    finishReply();
    if (aborted)
    {
        setError(QCoreApplication::translate("StudioAssistant", "Assistant request cancelled."));
        return;
    }
    if (network_error != QNetworkReply::NoError && status == 0)
    {
        setError(QCoreApplication::translate("StudioAssistant",
                                             "Assistant request failed to reach the endpoint."));
        return;
    }
    const auto document = QJsonDocument::fromJson(bytes);
    if (!document.isObject())
    {
        setError(QCoreApplication::translate("StudioAssistant",
                                             "Assistant response was not valid JSON."));
        return;
    }
    const auto object = document.object();
    if (status >= 400)
    {
        const auto error = object.value(QStringLiteral("error")).toObject();
        const QString message = error.value(QStringLiteral("message")).toString();
        setError(message.isEmpty() ?
                     QCoreApplication::translate("StudioAssistant",
                                                 "Assistant endpoint returned HTTP %1.")
                         .arg(status) :
                     message);
        return;
    }
    const auto choices = object.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty())
    {
        setError(QCoreApplication::translate("StudioAssistant",
                                             "Assistant response contained no choices."));
        return;
    }
    const QString content =
        choices.at(0).toObject().value(QStringLiteral("message")).toObject().value(
            QStringLiteral("content")).toString().trimmed();
    if (content.isEmpty())
    {
        setError(QCoreApplication::translate("StudioAssistant",
                                             "Assistant response contained no text."));
        return;
    }
    messages_.push_back(chat_message(QStringLiteral("assistant"), content));
    emit messagesChanged();
    setError({});
}

} // namespace ravo
