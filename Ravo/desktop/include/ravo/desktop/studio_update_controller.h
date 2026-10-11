#pragma once

#include <functional>
#include <memory>

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

namespace ravo
{
// Main-thread application owner; independent of catalog and photo state. The
// injected network manager is transferred to this owner and must share its thread.
class StudioUpdateController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY changed)
    Q_PROPERTY(QUrl releaseUrl READ releaseUrl NOTIFY changed)
    Q_PROPERTY(State state READ state NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY changed)
    Q_PROPERTY(bool automatic READ automatic NOTIFY settingsChanged)
    Q_PROPERTY(QString settingsError READ settingsError NOTIFY settingsChanged)
    Q_PROPERTY(QString errorCode READ errorCode NOTIFY changed)
    Q_PROPERTY(QString lastError READ lastError NOTIFY changed)

public:
    enum class State
    {
        Idle,
        Checking,
        UpToDate,
        UpdateAvailable,
        Failed,
        Cancelled
    };
    Q_ENUM(State)
    using UrlOpener = std::function<bool(const QUrl &)>;

    explicit StudioUpdateController(QObject *parent = nullptr);
    // Injection seam for deterministic tests; production uses the compiled version,
    // its own QNetworkAccessManager and QDesktopServices.
    StudioUpdateController(QString current_version, std::unique_ptr<QNetworkAccessManager> network,
                           UrlOpener opener, QObject *parent = nullptr);
    ~StudioUpdateController() override;

    static QString compiledVersion();
    bool initialize();
    void retranslate();
    QString currentVersion() const
    {
        return current_version_;
    }
    QString latestVersion() const
    {
        return latest_version_;
    }
    QUrl releaseUrl() const
    {
        return release_url_;
    }
    State state() const noexcept
    {
        return state_;
    }
    bool busy() const noexcept
    {
        return reply_ != nullptr;
    }
    bool updateAvailable() const noexcept
    {
        return state_ == State::UpdateAvailable;
    }
    bool automatic() const noexcept
    {
        return automatic_;
    }
    QString settingsError() const;
    QString errorCode() const;
    QString lastError() const;

    Q_INVOKABLE bool setAutomatic(bool enabled);
    Q_INVOKABLE void checkManually();
    Q_INVOKABLE void checkAtStartup();
    Q_INVOKABLE bool openReleasePage();
    Q_INVOKABLE void cancel();

signals:
    void changed();
    void settingsChanged();
    // Manual progress/results or an automatic available-update notification.
    // The view defers this intent while another modal surface is active.
    void presentationRequested();

private:
    enum class Error
    {
        None,
        Network,
        Timeout,
        RateLimited,
        Http,
        InvalidRelease,
        TooLarge,
        Browser,
        Cancelled
    };
    enum class SettingsError
    {
        None,
        Read,
        Write
    };
    void check(bool manual);
    void readResponse();
    void finish();
    void failRequest(Error error);
    void publishFailure(Error error);
    void releaseReply();

    const QString current_version_;
    std::unique_ptr<QNetworkAccessManager> network_;
    UrlOpener opener_;
    QNetworkReply *reply_ = nullptr;
    QTimer deadline_;
    QByteArray response_;
    QString latest_version_;
    QUrl release_url_;
    State state_ = State::Idle;
    Error error_ = Error::None;
    SettingsError settings_error_ = SettingsError::None;
    bool automatic_ = true;
    bool startup_checked_ = false;
    bool manual_ = false;
};
} // namespace ravo
