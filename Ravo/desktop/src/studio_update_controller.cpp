#include "ravo/desktop/studio_update_controller.h"

#include <array>
#include <optional>
#include <utility>

#include <QCoreApplication>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSettings>
#include <QVariant>

namespace ravo
{
namespace
{
constexpr auto kSettingsKey = "desktop/updates/automatic";
constexpr auto kApiUrl = "https://api.github.com/repos/NorthBoundWisdom/Ravo/releases/latest";
constexpr qsizetype kMaxResponseBytes = 1024 * 1024;
constexpr int kTimeoutMs = 15000;

std::optional<std::array<quint64, 3>> version(QString text)
{
    static const QRegularExpression pattern(
        QStringLiteral("\\Av?(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\z"));
    const auto match = pattern.match(text);
    if (!match.hasMatch())
        return std::nullopt;
    std::array<quint64, 3> result{};
    for (int i = 0; i < 3; ++i)
    {
        bool ok = false;
        result[static_cast<std::size_t>(i)] = match.captured(i + 1).toULongLong(&ok);
        if (!ok)
            return std::nullopt;
    }
    return result;
}

bool valid_release_url(const QUrl &url, const QString &tag)
{
    return url.isValid() && url.scheme() == QLatin1String("https") &&
           url.host() == QLatin1String("github.com") && url.port() == -1 &&
           url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment() &&
           url.path() == QStringLiteral("/NorthBoundWisdom/Ravo/releases/tag/") + tag;
}
} // namespace

QString StudioUpdateController::compiledVersion()
{
    return QStringLiteral(RAVO_VERSION);
}

StudioUpdateController::StudioUpdateController(QObject *parent)
    : StudioUpdateController(
          compiledVersion(), std::make_unique<QNetworkAccessManager>(),
          [](const QUrl &url) { return QDesktopServices::openUrl(url); }, parent)
{
}

StudioUpdateController::StudioUpdateController(QString current_version,
                                               std::unique_ptr<QNetworkAccessManager> network,
                                               UrlOpener opener, QObject *parent)
    : QObject(parent)
    , current_version_(std::move(current_version))
    , network_(std::move(network))
    , opener_(std::move(opener))
{
    Q_ASSERT(network_ && network_->thread() == thread() && opener_);
    deadline_.setParent(this);
    deadline_.setObjectName(QStringLiteral("updateDeadline"));
    deadline_.setSingleShot(true);
    deadline_.setInterval(kTimeoutMs);
    connect(&deadline_, &QTimer::timeout, this, [this] { failRequest(Error::Timeout); });
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this,
            &StudioUpdateController::cancel);
}

StudioUpdateController::~StudioUpdateController()
{
    releaseReply();
}

bool StudioUpdateController::initialize()
{
    QSettings settings;
    const auto stored = settings.value(QLatin1String(kSettingsKey));
    if (settings.status() != QSettings::NoError)
    {
        settings_error_ = SettingsError::Read;
        emit settingsChanged();
        return false;
    }
    if (stored.isValid())
    {
        // INI round trips may return text. Do not coerce arbitrary values to false.
        if (stored.metaType().id() == QMetaType::Bool ||
            stored.toString() == QLatin1String("true") ||
            stored.toString() == QLatin1String("false"))
            automatic_ = stored.toString() == QLatin1String("true");
        else
        {
            settings.remove(QLatin1String(kSettingsKey));
            settings.sync();
            if (settings.status() != QSettings::NoError)
            {
                settings_error_ = SettingsError::Write;
                emit settingsChanged();
                return false;
            }
            automatic_ = true;
        }
    }
    settings_error_ = SettingsError::None;
    emit settingsChanged();
    return true;
}

bool StudioUpdateController::setAutomatic(const bool enabled)
{
    QSettings settings;
    const auto previous = settings.value(QLatin1String(kSettingsKey));
    settings.setValue(QLatin1String(kSettingsKey), enabled);
    settings.sync();
    if (settings.status() != QSettings::NoError)
    {
        // Restore QSettings' shared in-process cache as well as our typed value.
        if (previous.isValid())
            settings.setValue(QLatin1String(kSettingsKey), previous);
        else
            settings.remove(QLatin1String(kSettingsKey));
        settings_error_ = SettingsError::Write;
        emit settingsChanged();
        return false;
    }
    automatic_ = enabled;
    settings_error_ = SettingsError::None;
    emit settingsChanged();
    return true;
}

QString StudioUpdateController::settingsError() const
{
    switch (settings_error_)
    {
    case SettingsError::Read:
        return QCoreApplication::translate("StudioUpdates", "Unable to read update preferences.");
    case SettingsError::Write:
        return QCoreApplication::translate("StudioUpdates", "Unable to save update preferences.");
    case SettingsError::None:
        return {};
    }
    return {};
}

QString StudioUpdateController::errorCode() const
{
    switch (error_)
    {
    case Error::None:
        return {};
    case Error::Network:
        return QStringLiteral("update_network_error");
    case Error::Timeout:
        return QStringLiteral("update_timeout");
    case Error::RateLimited:
        return QStringLiteral("update_rate_limited");
    case Error::Http:
        return QStringLiteral("update_http_error");
    case Error::InvalidRelease:
        return QStringLiteral("update_invalid_release");
    case Error::TooLarge:
        return QStringLiteral("update_response_too_large");
    case Error::Browser:
        return QStringLiteral("update_browser_error");
    case Error::Cancelled:
        return QStringLiteral("update_cancelled");
    }
    return {};
}

QString StudioUpdateController::lastError() const
{
    switch (error_)
    {
    case Error::None:
        return {};
    case Error::Network:
        return QCoreApplication::translate(
            "StudioUpdates",
            "Unable to connect to GitHub. Check your internet connection and try again.");
    case Error::Timeout:
        return QCoreApplication::translate("StudioUpdates",
                                           "The update check timed out. Please try again.");
    case Error::RateLimited:
        return QCoreApplication::translate(
            "StudioUpdates", "GitHub is limiting update requests. Please try again later.");
    case Error::Http:
        return QCoreApplication::translate(
            "StudioUpdates",
            "GitHub could not provide the latest release. Please try again later.");
    case Error::InvalidRelease:
        return QCoreApplication::translate("StudioUpdates",
                                           "GitHub returned invalid release information.");
    case Error::TooLarge:
        return QCoreApplication::translate("StudioUpdates",
                                           "The update response exceeded the size limit.");
    case Error::Browser:
        return QCoreApplication::translate("StudioUpdates",
                                           "Unable to open the release page in your browser.");
    case Error::Cancelled:
        return QCoreApplication::translate("StudioUpdates", "The update check was cancelled.");
    }
    return {};
}

void StudioUpdateController::checkAtStartup()
{
    if (startup_checked_)
        return;
    startup_checked_ = true;
    if (automatic_)
        check(false);
}

void StudioUpdateController::retranslate()
{
    emit changed();
    emit settingsChanged();
}

void StudioUpdateController::checkManually()
{
    check(true);
}

void StudioUpdateController::check(const bool manual)
{
    if (reply_)
    {
        manual_ = manual_ || manual;
        if (manual)
            emit presentationRequested();
        return;
    }
    manual_ = manual;
    latest_version_.clear();
    release_url_.clear();
    error_ = Error::None;
    response_.clear();
    if (!version(current_version_))
    {
        publishFailure(Error::InvalidRelease);
        return;
    }
    QNetworkRequest request{QUrl(QString::fromLatin1(kApiUrl))};
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2026-03-10");
    request.setRawHeader("User-Agent", "Ravo-Studio/" + current_version_.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    reply_ = network_->get(request);
    reply_->setReadBufferSize(kMaxResponseBytes + 1);
    connect(reply_, &QNetworkReply::readyRead, this, &StudioUpdateController::readResponse);
    connect(reply_, &QNetworkReply::finished, this, &StudioUpdateController::finish);
    deadline_.start();
    state_ = State::Checking;
    emit changed();
    if (manual)
        emit presentationRequested();
}

void StudioUpdateController::readResponse()
{
    if (!reply_)
        return;
    const auto size = reply_->header(QNetworkRequest::ContentLengthHeader).toLongLong();
    if (size > kMaxResponseBytes)
    {
        failRequest(Error::TooLarge);
        return;
    }
    response_ += reply_->read(kMaxResponseBytes + 1 - response_.size());
    if (response_.size() > kMaxResponseBytes)
        failRequest(Error::TooLarge);
}

void StudioUpdateController::finish()
{
    readResponse();
    if (!reply_)
        return;
    const int status = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto network_error = reply_->error();
    releaseReply();
    if (status == 403 || status == 429)
        return publishFailure(Error::RateLimited);
    if (status != 0 && status != 200)
        return publishFailure(Error::Http);
    if (network_error != QNetworkReply::NoError || status == 0)
        return publishFailure(Error::Network);
    const auto document = QJsonDocument::fromJson(response_);
    const auto object = document.object();
    const auto tag = object.value(QStringLiteral("tag_name")).toString();
    const auto remote = version(tag);
    const QUrl url(object.value(QStringLiteral("html_url")).toString());
    const auto draft = object.value(QStringLiteral("draft"));
    const auto prerelease = object.value(QStringLiteral("prerelease"));
    if (!document.isObject() || !remote || !draft.isBool() || draft.toBool() ||
        !prerelease.isBool() || prerelease.toBool() || !valid_release_url(url, tag))
        return publishFailure(Error::InvalidRelease);
    latest_version_ = tag.startsWith(QLatin1Char('v')) ? tag.mid(1) : tag;
    release_url_ = url;
    state_ = *remote > *version(current_version_) ? State::UpdateAvailable : State::UpToDate;
    response_.clear();
    emit changed();
    if (manual_ || state_ == State::UpdateAvailable)
        emit presentationRequested();
}

void StudioUpdateController::releaseReply()
{
    deadline_.stop();
    if (!reply_)
        return;
    auto *reply = std::exchange(reply_, nullptr);
    disconnect(reply, nullptr, this, nullptr);
    if (!reply->isFinished())
        reply->abort();
    reply->deleteLater();
}

void StudioUpdateController::publishFailure(const Error error)
{
    response_.clear();
    error_ = error;
    state_ = State::Failed;
    emit changed();
    if (manual_)
        emit presentationRequested();
}

void StudioUpdateController::failRequest(const Error error)
{
    if (!reply_)
        return;
    releaseReply();
    publishFailure(error);
}

void StudioUpdateController::cancel()
{
    if (!reply_)
        return;
    releaseReply();
    response_.clear();
    error_ = Error::Cancelled;
    state_ = State::Cancelled;
    emit changed();
}

bool StudioUpdateController::openReleasePage()
{
    if (state_ != State::UpdateAvailable)
        return false;
    if (opener_(release_url_))
        return true;
    manual_ = true;
    publishFailure(Error::Browser);
    return false;
}
} // namespace ravo
