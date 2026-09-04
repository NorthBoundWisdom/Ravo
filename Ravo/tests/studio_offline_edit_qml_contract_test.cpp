#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QVariantMap>

#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/studio_presenter.h"
#include "studio_test_support.h"

namespace ravo
{
namespace
{
using namespace studio_test_support;

TEST(StudioQmlContract, OfflineEditDialogExposesCreateManageChrome)
{
    QFile dialog(QStringLiteral(RAVO_STUDIO_OFFLINE_EDIT_QML));
    ASSERT_TRUE(dialog.open(QIODevice::ReadOnly | QIODevice::Text))
        << dialog.errorString().toStdString();
    const auto dialog_source = QString::fromUtf8(dialog.readAll());
    EXPECT_TRUE(dialog_source.contains(QStringLiteral("objectName: \"OfflineEditDialog\"")));
    EXPECT_TRUE(dialog_source.contains(QStringLiteral("objectName: \"offlineEditCreate\"")));
    EXPECT_TRUE(dialog_source.contains(QStringLiteral("objectName: \"offlineEditPin\"")));
    EXPECT_TRUE(dialog_source.contains(QStringLiteral("objectName: \"offlineEditDelete\"")));
    EXPECT_TRUE(dialog_source.contains(QStringLiteral("objectName: \"offlineEditReconnect\"")));
    EXPECT_TRUE(dialog_source.contains(QStringLiteral("objectName: \"offlineEditEvict\"")));
    EXPECT_TRUE(dialog_source.contains(QStringLiteral("objectName: \"offlineEditStatus\"")));
    EXPECT_TRUE(dialog_source.contains(QStringLiteral("photoOfflineEditCreate")));
    EXPECT_TRUE(dialog_source.contains(QStringLiteral("identity while media_state=proxy")));

    QFile menu(QStringLiteral(RAVO_STUDIO_PHOTO_CONTEXT_MENU_QML));
    ASSERT_TRUE(menu.open(QIODevice::ReadOnly | QIODevice::Text))
        << menu.errorString().toStdString();
    const auto menu_source = QString::fromUtf8(menu.readAll());
    EXPECT_TRUE(menu_source.contains(QStringLiteral("objectName: \"offlineEditMenuItem\"")));
    EXPECT_TRUE(menu_source.contains(QStringLiteral("root.commands.offlineEdit")));

    QFile actions(QStringLiteral(RAVO_STUDIO_ACTIONS_QML));
    ASSERT_TRUE(actions.open(QIODevice::ReadOnly | QIODevice::Text))
        << actions.errorString().toStdString();
    const auto action_source = QString::fromUtf8(actions.readAll());
    EXPECT_TRUE(action_source.contains(QStringLiteral("property alias offlineEdit")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("root.ids.photoOfflineEdit")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("OfflineEditDialog")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("openOfflineEditDialog")));

    QFile main_qml(QStringLiteral(RAVO_STUDIO_MAIN_QML));
    ASSERT_TRUE(main_qml.open(QIODevice::ReadOnly | QIODevice::Text))
        << main_qml.errorString().toStdString();
    const auto main_source = QString::fromUtf8(main_qml.readAll());
    EXPECT_TRUE(main_source.contains(QStringLiteral("ids.photoOfflineEdit")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("openOfflineEditDialog")));
}

TEST(StudioCommands, OfflineEditCommandsExposeCreateManage)
{
    ensure_qt_core();
    StudioPresenter presenter;
    StudioCommandController controller(presenter);
    const auto ids = controller.ids();
    EXPECT_EQ(ids.value(QStringLiteral("photoOfflineEdit")).toString(),
              QStringLiteral("studio.photo.offline_edit"));
    EXPECT_EQ(ids.value(QStringLiteral("photoOfflineEditCreate")).toString(),
              QStringLiteral("studio.photo.offline_edit_create"));
    EXPECT_EQ(ids.value(QStringLiteral("photoOfflineEditDelete")).toString(),
              QStringLiteral("studio.photo.offline_edit_delete"));
    EXPECT_EQ(ids.value(QStringLiteral("photoOfflineEditPin")).toString(),
              QStringLiteral("studio.photo.offline_edit_pin"));
    EXPECT_EQ(ids.value(QStringLiteral("photoOfflineEditEvict")).toString(),
              QStringLiteral("studio.photo.offline_edit_evict"));
    EXPECT_EQ(ids.value(QStringLiteral("photoOfflineEditReconnect")).toString(),
              QStringLiteral("studio.photo.offline_edit_reconnect"));
}

} // namespace
} // namespace ravo
