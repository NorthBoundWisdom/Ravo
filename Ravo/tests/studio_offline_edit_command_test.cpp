#include <gtest/gtest.h>

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
