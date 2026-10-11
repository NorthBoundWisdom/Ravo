#include <algorithm>
#include <cstring>
#include <memory>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSettings>
#include <QUuid>
#include <gtest/gtest.h>

#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/desktop/studio_update_controller.h"
#include "studio_test_support.h"

namespace ravo
{
namespace
{
using namespace studio_test_support;
using State = StudioUpdateController::State;

class UpdateReply final : public QNetworkReply
{
public:
    UpdateReply(const QNetworkRequest &request, QObject *parent)
        : QNetworkReply(parent)
    {
        setRequest(request);
        setUrl(request.url());
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }
    void abort() override
    {
        aborted = true;
        setError(OperationCanceledError, QStringLiteral("cancelled"));
        setFinished(true);
        emit finished();
    }
    void complete(QByteArray bytes, int status = 200, NetworkError error = NoError)
    {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        if (error != NoError)
            setError(error, QStringLiteral("injected failure"));
        feed(std::move(bytes));
        setFinished(true);
        emit finished();
    }
    void feed(QByteArray bytes)
    {
        bytes_.append(bytes);
        emit readyRead();
    }
    qint64 bytesAvailable() const override
    {
        return bytes_.size() + QNetworkReply::bytesAvailable();
    }
    bool aborted = false;

protected:
    qint64 readData(char *data, qint64 size) override
    {
        const auto count = std::min(size, static_cast<qint64>(bytes_.size()));
        if (count <= 0)
            return -1;
        std::memcpy(data, bytes_.constData(), static_cast<std::size_t>(count));
        bytes_.remove(0, static_cast<qsizetype>(count));
        return count;
    }

private:
    QByteArray bytes_;
};

class UpdateNetwork final : public QNetworkAccessManager
{
public:
    QPointer<UpdateReply> reply;
    int requests = 0;
    QNetworkRequest lastRequest;

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request,
                                 QIODevice *) override
    {
        EXPECT_EQ(operation, GetOperation);
        ++requests;
        lastRequest = request;
        reply = new UpdateReply(request, this);
        return reply;
    }
};

QByteArray release(const QString &tag = QStringLiteral("v0.9.18"), const QString &url = {},
                   bool draft = false, bool prerelease = false)
{
    return QJsonDocument(
               QJsonObject{
                   {QStringLiteral("tag_name"), tag},
                   {QStringLiteral("html_url"),
                    url.isEmpty() ?
                        QStringLiteral("https://github.com/NorthBoundWisdom/Ravo/releases/tag/") +
                            tag :
                        url},
                   {QStringLiteral("draft"), draft},
                   {QStringLiteral("prerelease"), prerelease}})
        .toJson(QJsonDocument::Compact);
}

class StudioUpdateTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensure_qt_core();
        oldOrganization = QCoreApplication::organizationName();
        oldApplication = QCoreApplication::applicationName();
        QCoreApplication::setOrganizationName(QStringLiteral("RavoUpdates-") +
                                              QUuid::createUuid().toString(QUuid::Id128));
        QCoreApplication::setApplicationName(QStringLiteral("UpdateContract"));
        auto transport = std::make_unique<UpdateNetwork>();
        network = transport.get();
        updates =
            std::make_unique<StudioUpdateController>(QStringLiteral("0.9.17"), std::move(transport),
                                                     [this](const QUrl &url)
                                                     {
                                                         opened.push_back(url);
                                                         return browserWorks;
                                                     });
        ASSERT_TRUE(updates->initialize());
        QObject::connect(updates.get(), &StudioUpdateController::presentationRequested,
                         updates.get(), [this] { ++presentations; });
    }
    void TearDown() override
    {
        updates.reset();
        QSettings().clear();
        QCoreApplication::setOrganizationName(oldOrganization);
        QCoreApplication::setApplicationName(oldApplication);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    QString oldOrganization, oldApplication;
    UpdateNetwork *network = nullptr;
    std::unique_ptr<StudioUpdateController> updates;
    QList<QUrl> opened;
    int presentations = 0;
    bool browserWorks = true;
};

TEST_F(StudioUpdateTest, ManualCheckUsesPublicApiAndOpensOnlyAfterConfirmation)
{
    EXPECT_FALSE(updates->openReleasePage());
    updates->checkManually();
    EXPECT_TRUE(updates->busy());
    EXPECT_EQ(updates->state(), State::Checking);
    EXPECT_EQ(presentations, 1);
    EXPECT_EQ(
        network->lastRequest.url(),
        QUrl(QStringLiteral("https://api.github.com/repos/NorthBoundWisdom/Ravo/releases/latest")));
    EXPECT_TRUE(network->lastRequest.rawHeader("Authorization").isEmpty());
    EXPECT_EQ(network->lastRequest.rawHeader("User-Agent"), QByteArray("Ravo-Studio/0.9.17"));
    network->reply->complete(release());
    EXPECT_FALSE(updates->busy());
    EXPECT_EQ(updates->state(), State::UpdateAvailable);
    EXPECT_EQ(updates->latestVersion(), QStringLiteral("0.9.18"));
    EXPECT_TRUE(opened.isEmpty());
    EXPECT_TRUE(updates->openReleasePage());
    ASSERT_EQ(opened.size(), 1);
    EXPECT_EQ(
        opened.front(),
        QUrl(QStringLiteral("https://github.com/NorthBoundWisdom/Ravo/releases/tag/v0.9.18")));
}

TEST_F(StudioUpdateTest, NumericVersionsDoNotOfferDowngrades)
{
    for (const auto &tag : {"v0.9.17", "v0.9.9", "v0.8.99", "0.9.17"})
    {
        updates->checkManually();
        network->reply->complete(release(QString::fromLatin1(tag)));
        EXPECT_EQ(updates->state(), State::UpToDate) << tag;
        EXPECT_FALSE(updates->openReleasePage());
    }
    for (const auto &tag : {"v0.10.0", "v1.0.0"})
    {
        updates->checkManually();
        network->reply->complete(release(QString::fromLatin1(tag)));
        EXPECT_EQ(updates->state(), State::UpdateAvailable) << tag;
    }
}

TEST_F(StudioUpdateTest, StartupRunsOnceAndOnlyAnnouncesAvailableUpdates)
{
    updates->checkAtStartup();
    updates->checkAtStartup();
    EXPECT_EQ(network->requests, 1);
    EXPECT_EQ(presentations, 0);
    network->reply->complete(release(QStringLiteral("v0.9.17")));
    EXPECT_EQ(presentations, 0);
    updates->checkAtStartup();
    EXPECT_EQ(network->requests, 1);
}

TEST_F(StudioUpdateTest, StartupAvailableAndManualPromotionShareOneRequest)
{
    updates->checkAtStartup();
    updates->checkManually();
    updates->checkManually();
    EXPECT_EQ(network->requests, 1);
    EXPECT_EQ(presentations, 2);
    network->reply->complete(release());
    EXPECT_EQ(presentations, 3);
}

TEST_F(StudioUpdateTest, AutomaticAvailableResultRequestsConfirmation)
{
    updates->checkAtStartup();
    network->reply->complete(release());
    EXPECT_EQ(presentations, 1);
    EXPECT_TRUE(opened.isEmpty());
}

TEST_F(StudioUpdateTest, DisabledPreferencePersistsAndManualCheckStillWorks)
{
    ASSERT_TRUE(updates->automatic());
    ASSERT_TRUE(updates->setAutomatic(false));
    ASSERT_TRUE(updates->initialize());
    EXPECT_FALSE(updates->automatic());
    updates->checkAtStartup();
    EXPECT_EQ(network->requests, 0);
    updates->checkManually();
    EXPECT_EQ(network->requests, 1);
}

TEST_F(StudioUpdateTest, InvalidPreferenceRepairsAndWriteFailureKeepsTypedValue)
{
    QSettings settings;
    settings.setValue(QStringLiteral("desktop/updates/automatic"), QStringLiteral("garbage"));
    settings.sync();
    ASSERT_TRUE(updates->initialize());
    EXPECT_TRUE(updates->automatic());
    EXPECT_FALSE(QSettings().contains(QStringLiteral("desktop/updates/automatic")));
    // Use a fresh settings identity whose INI path is a directory: portable
    // deterministic I/O failure without relying on root/host permission behavior.
    QCoreApplication::setApplicationName(QStringLiteral("BlockedUpdateContract"));
    ASSERT_TRUE(QDir().mkpath(QSettings().fileName()));
    EXPECT_FALSE(updates->setAutomatic(false));
    EXPECT_TRUE(updates->automatic());
    EXPECT_FALSE(updates->settingsError().isEmpty());
    QDir(QSettings().fileName()).removeRecursively();
    QCoreApplication::setApplicationName(QStringLiteral("UpdateContract"));
    EXPECT_TRUE(updates->setAutomatic(false));
    EXPECT_TRUE(updates->settingsError().isEmpty());
}

TEST_F(StudioUpdateTest, InvalidReleaseAndNonReleaseLinksFailExplicitly)
{
    QList<QByteArray> responses{QByteArray("not json"), QByteArray("[]"), QByteArray("{}"),
                                release(QStringLiteral("v0.9.18"), {}, true),
                                release(QStringLiteral("v0.9.18"), {}, false, true)};
    for (const auto &tag :
         {"v1.0.0-rc.1", "v1.0", "v01.0.0", "v1.0.0\n", "v18446744073709551616.0.0"})
        responses.push_back(release(QString::fromLatin1(tag)));
    for (const auto &url : {"http://github.com/NorthBoundWisdom/Ravo/releases/tag/v0.9.18",
                            "https://example.test/NorthBoundWisdom/Ravo/releases/tag/v0.9.18",
                            "https://github.com/other/repo/releases/tag/v0.9.18",
                            "https://github.com/NorthBoundWisdom/Ravo/releases/tag/v0.9.19",
                            "https://user@github.com/NorthBoundWisdom/Ravo/releases/tag/v0.9.18",
                            "https://github.com/NorthBoundWisdom/Ravo/releases/tag/v0.9.18?x=1"})
        responses.push_back(release(QStringLiteral("v0.9.18"), QString::fromLatin1(url)));
    for (const auto &bytes : responses)
    {
        updates->checkManually();
        network->reply->complete(bytes);
        EXPECT_EQ(updates->state(), State::Failed) << bytes.constData();
        EXPECT_EQ(updates->errorCode(), QStringLiteral("update_invalid_release"));
        EXPECT_FALSE(updates->openReleasePage());
    }
    EXPECT_TRUE(opened.isEmpty());
}

TEST_F(StudioUpdateTest, HttpOfflineAndTimeoutAreFailuresNotUpToDate)
{
    updates->checkAtStartup();
    network->reply->complete({}, 0, QNetworkReply::HostNotFoundError);
    EXPECT_EQ(updates->errorCode(), QStringLiteral("update_network_error"));
    EXPECT_EQ(presentations, 0);
    for (const int status : {403, 429, 404, 500, 302})
    {
        updates->checkManually();
        network->reply->complete({}, status);
        EXPECT_EQ(updates->state(), State::Failed);
        EXPECT_EQ(updates->errorCode(), status == 403 || status == 429 ?
                                            QStringLiteral("update_rate_limited") :
                                            QStringLiteral("update_http_error"));
    }
    updates->checkManually();
    auto *timer = updates->findChild<QTimer *>(QStringLiteral("updateDeadline"));
    ASSERT_NE(timer, nullptr);
    EXPECT_EQ(timer->interval(), 15000);
    ASSERT_TRUE(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    EXPECT_TRUE(network->reply->aborted);
    EXPECT_EQ(updates->errorCode(), QStringLiteral("update_timeout"));
}

TEST_F(StudioUpdateTest, OversizedChunkedReplyAbortsAndLateSignalsAreIgnored)
{
    updates->checkManually();
    const auto old = network->reply;
    old->feed(QByteArray(1024 * 1024, 'x'));
    EXPECT_TRUE(updates->busy());
    old->feed(QByteArray("x"));
    EXPECT_FALSE(updates->busy());
    EXPECT_TRUE(old->aborted);
    EXPECT_EQ(updates->errorCode(), QStringLiteral("update_response_too_large"));
    updates->checkManually();
    old->complete(release());
    EXPECT_TRUE(updates->busy());
    network->reply->complete(release());
    EXPECT_EQ(updates->state(), State::UpdateAvailable);
}

TEST_F(StudioUpdateTest, CancellationAndDestructionReleaseRequestsWithoutPresentation)
{
    updates->checkAtStartup();
    const auto old = network->reply;
    updates->cancel();
    EXPECT_TRUE(old->aborted);
    EXPECT_EQ(updates->state(), State::Cancelled);
    old->complete(release());
    EXPECT_EQ(presentations, 0);
    updates->checkManually();
    const auto pending = network->reply;
    updates.reset();
    EXPECT_TRUE(pending.isNull());
}

TEST_F(StudioUpdateTest, BrowserFailureIsVisibleAndDoesNotClaimSuccess)
{
    updates->checkManually();
    network->reply->complete(release());
    browserWorks = false;
    EXPECT_FALSE(updates->openReleasePage());
    EXPECT_EQ(updates->errorCode(), QStringLiteral("update_browser_error"));
    EXPECT_EQ(updates->state(), State::Failed);
    EXPECT_EQ(opened.size(), 1);
}

TEST_F(StudioUpdateTest, HelpCommandJoinsActiveCheckAndRejectsArguments)
{
    StudioPresenter presenter;
    StudioCommandController commands(presenter);
    const auto id = commands.ids().value(QStringLiteral("windowCheckUpdates")).toString();
    ASSERT_FALSE(id.isEmpty());
    QObject::connect(&commands, &StudioCommandController::presentationCommandRequested,
                     updates.get(),
                     [this, id](const QString &command, const QVariant &)
                     {
                         if (command == id)
                             updates->checkManually();
                     });
    EXPECT_TRUE(commands.executeCommand(id).value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(network->requests, 1);
    commands.setModalOpen(true);
    EXPECT_TRUE(commands.executeCommand(id).value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(network->requests, 1);
    EXPECT_FALSE(commands.executeCommand(id, QStringLiteral("invalid"))
                     .value(QStringLiteral("accepted"))
                     .toBool());
}

TEST_F(StudioUpdateTest, ProductionDialogsDeferAndExplicitButtonOpensRelease)
{
    QQmlEngine engine;
    engine.addImportPath(QStringLiteral("qrc:/"));
    engine.addImportPath(QStringLiteral(RAVO_GEOCONTROLS_QML_IMPORT_ROOT));
    QQuickWindow window;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(
                                         RAVO_REPOSITORY_ROOT
                                         "/Ravo/desktop/qml/chrome/StudioApplicationDialogs.qml")));
    ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
    std::unique_ptr<QObject> dialogs(component.createWithInitialProperties(
        {{QStringLiteral("updates"), QVariant::fromValue(updates.get())},
         {QStringLiteral("windowHost"), QVariant::fromValue(&window)},
         {QStringLiteral("otherModalOpen"), true}}));
    ASSERT_NE(dialogs, nullptr) << component.errorString().toStdString();
    auto *dialog = dialogs->findChild<QObject *>(QStringLiteral("updateDialog"));
    ASSERT_NE(dialog, nullptr);
    updates->checkAtStartup();
    network->reply->complete(release());
    QCoreApplication::processEvents();
    EXPECT_FALSE(dialog->property("visible").toBool());
    dialogs->setProperty("otherModalOpen", false);
    ASSERT_TRUE(wait_until([&] { return dialog->property("visible").toBool(); }));
    EXPECT_TRUE(dialogs->property("modalOpen").toBool());
    EXPECT_TRUE(opened.isEmpty());
    ASSERT_TRUE(QMetaObject::invokeMethod(
        dialog, "finished",
        Q_ARG(QString, QCoreApplication::translate("StudioApplicationDialogs", "Later"))));
    ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "close"));
    QCoreApplication::processEvents();
    EXPECT_TRUE(opened.isEmpty());
    updates->checkManually();
    network->reply->complete(release());
    ASSERT_TRUE(wait_until([&] { return dialog->property("visible").toBool(); }));
    ASSERT_TRUE(QMetaObject::invokeMethod(
        dialog, "finished",
        Q_ARG(QString,
              QCoreApplication::translate("StudioApplicationDialogs", "Open GitHub Release"))));
    ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "close"));
    ASSERT_TRUE(wait_until([&] { return opened.size() == 1; }));
}
} // namespace
} // namespace ravo
