#include <QCoreApplication>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "ravo/desktop/studio_assistant_controller.h"
#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/studio_presenter.h"

namespace ravo
{
namespace
{

void ensure_qt_core()
{
    if (QCoreApplication::instance() != nullptr)
        return;
    static int argc = 1;
    static char executable[] = "ravo-desktop-assistant-tests";
    static char *argv[] = {executable, nullptr};
    static auto *application = new QCoreApplication(argc, argv);
    static_cast<void>(application);
}

class AssistantSettingsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensure_qt_core();
        ASSERT_TRUE(directory_.isValid());
        previous_format_ = QSettings::defaultFormat();
        previous_organization_ = QCoreApplication::organizationName();
        previous_application_ = QCoreApplication::applicationName();
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory_.path());
        QCoreApplication::setOrganizationName(QStringLiteral("RavoAssistantTest"));
        QCoreApplication::setApplicationName(QStringLiteral("AssistantContract"));
        qputenv("XAI_API_KEY", QByteArray());
    }

    void TearDown() override
    {
        QCoreApplication::setOrganizationName(previous_organization_);
        QCoreApplication::setApplicationName(previous_application_);
        QSettings::setDefaultFormat(previous_format_);
    }

    QTemporaryDir directory_;
    QSettings::Format previous_format_ = QSettings::NativeFormat;
    QString previous_organization_;
    QString previous_application_;
};

TEST_F(AssistantSettingsTest, DefaultsAndRejectsInvalidEndpointAndModel)
{
    StudioAssistantController assistant;
    ASSERT_TRUE(assistant.initialize());
    EXPECT_EQ(assistant.endpoint(), StudioAssistantController::defaultEndpoint());
    EXPECT_EQ(assistant.model(), StudioAssistantController::defaultModel());
    EXPECT_FALSE(assistant.configured());

    EXPECT_FALSE(assistant.setEndpoint(QStringLiteral("ftp://example.invalid/v1")));
    EXPECT_EQ(assistant.endpoint(), StudioAssistantController::defaultEndpoint());
    EXPECT_FALSE(assistant.setModel(QString()));
    EXPECT_EQ(assistant.model(), StudioAssistantController::defaultModel());
    EXPECT_FALSE(assistant.setModel(QStringLiteral("bad model")));
    EXPECT_FALSE(assistant.send(QStringLiteral("hello")));
}

TEST_F(AssistantSettingsTest, PersistsValidEndpointModelAndKey)
{
    {
        StudioAssistantController assistant;
        ASSERT_TRUE(assistant.initialize());
        ASSERT_TRUE(assistant.setEndpoint(QStringLiteral("https://example.test/v1/")));
        ASSERT_TRUE(assistant.setModel(QStringLiteral("grok-4.5")));
        ASSERT_TRUE(assistant.setApiKey(QStringLiteral("test-key")));
        EXPECT_EQ(assistant.endpoint(), QStringLiteral("https://example.test/v1"));
        EXPECT_TRUE(assistant.configured());
    }
    StudioAssistantController reopened;
    ASSERT_TRUE(reopened.initialize());
    EXPECT_EQ(reopened.endpoint(), QStringLiteral("https://example.test/v1"));
    EXPECT_EQ(reopened.model(), QStringLiteral("grok-4.5"));
    EXPECT_EQ(reopened.apiKey(), QStringLiteral("test-key"));
}

TEST_F(AssistantSettingsTest, RepairsMalformedStoredEndpoint)
{
    {
        QSettings settings;
        settings.setValue(QStringLiteral("desktop/assistant/endpoint"), QStringLiteral("not a url"));
        settings.sync();
        ASSERT_EQ(settings.status(), QSettings::NoError);
    }
    StudioAssistantController assistant;
    ASSERT_TRUE(assistant.initialize());
    EXPECT_EQ(assistant.endpoint(), StudioAssistantController::defaultEndpoint());
    {
        QSettings settings;
        EXPECT_FALSE(settings.contains(QStringLiteral("desktop/assistant/endpoint")));
    }
}

TEST(StudioAssistantCommands, ToggleOpensAndDismissCloses)
{
    ensure_qt_core();
    StudioPresenter presenter;
    StudioCommandController controller(presenter);
    const auto ids = controller.ids();
    const auto assistant = ids.value(QStringLiteral("windowAssistant")).toString();
    ASSERT_FALSE(assistant.isEmpty());
    EXPECT_FALSE(controller.assistantOpen());
    const auto opened = controller.executeAction(assistant, QStringLiteral("keyboard"));
    EXPECT_TRUE(opened.value(QStringLiteral("accepted")).toBool());
    EXPECT_TRUE(controller.assistantOpen());
    EXPECT_TRUE(controller.action(assistant).value(QStringLiteral("checkable")).toBool());
    EXPECT_TRUE(controller.action(assistant).value(QStringLiteral("checked")).toBool());
    const auto dismiss = ids.value(QStringLiteral("windowDismiss")).toString();
    ASSERT_FALSE(dismiss.isEmpty());
    const auto closed = controller.executeCommand(dismiss, QVariant(), QStringLiteral("keyboard"));
    EXPECT_TRUE(closed.value(QStringLiteral("accepted")).toBool());
    EXPECT_FALSE(controller.assistantOpen());
    EXPECT_FALSE(controller.action(assistant).value(QStringLiteral("checked")).toBool());
}

TEST(StudioQmlContract, AssistantPanelIsFloatingAndSettingsOwnUrlAndModel)
{
    QFile main(QStringLiteral(RAVO_STUDIO_MAIN_QML));
    ASSERT_TRUE(main.open(QIODevice::ReadOnly | QIODevice::Text))
        << main.errorString().toStdString();
    const auto main_source = QString::fromUtf8(main.readAll());
    EXPECT_TRUE(main_source.contains(QStringLiteral("AssistantPanel")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("studioAssistant")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("studioCommands.assistantOpen")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("qsTr(\"Assistant\")")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("ids.windowAssistant")));

    QFile actions(QStringLiteral(RAVO_STUDIO_ACTIONS_QML));
    ASSERT_TRUE(actions.open(QIODevice::ReadOnly | QIODevice::Text))
        << actions.errorString().toStdString();
    EXPECT_TRUE(QString::fromUtf8(actions.readAll()).contains(QStringLiteral("ids.windowAssistant")));

    QFile panel(QStringLiteral(RAVO_STUDIO_ASSISTANT_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto panel_source = QString::fromUtf8(panel.readAll());
    EXPECT_TRUE(panel_source.contains(QStringLiteral("signal closeRequested")));
    EXPECT_FALSE(panel_source.contains(QStringLiteral("Popup {")));
    EXPECT_FALSE(panel_source.contains(QStringLiteral("Overlay.overlay")));
    EXPECT_FALSE(panel_source.contains(QStringLiteral("XMLHttpRequest")));

    QFile settings(QStringLiteral(RAVO_STUDIO_SETTINGS_PAGE_QML));
    ASSERT_TRUE(settings.open(QIODevice::ReadOnly | QIODevice::Text))
        << settings.errorString().toStdString();
    const auto settings_source = QString::fromUtf8(settings.readAll());
    EXPECT_TRUE(settings_source.contains(QStringLiteral("assistant.setEndpoint")));
    EXPECT_TRUE(settings_source.contains(QStringLiteral("assistant.setModel")));
    EXPECT_TRUE(settings_source.contains(QStringLiteral("assistant.setApiKey")));
}

} // namespace
} // namespace ravo
