#include <gtest/gtest.h>
#include <QFile>
#include <QTemporaryDir>
#include "ravo/desktop/studio_presenter.h"
#include "ravo/desktop/studio_command_controller.h"
#include "studio_test_support.h"

namespace ravo
{
using namespace studio_test_support;

TEST(StudioCommands, LightroomCatalogImportRoutesAndReturnsToIdleOnInvalidSource)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    StudioPresenter presenter;
    StudioCommandController controller(presenter);
    const auto command = QStringLiteral("studio.library.import_lightroom_path");
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    QFile source(directory.filePath(QStringLiteral("invalid.lrcat")));
    ASSERT_TRUE(source.open(QIODevice::WriteOnly));
    source.write("not a SQLite catalog");
    source.close();
    controller.executeCommand(command, source.fileName());
    ASSERT_TRUE(wait_until([&] { return !presenter.busy() && !presenter.errorText().isEmpty(); }));
    EXPECT_TRUE(presenter.errorText().contains(QStringLiteral("Lightroom")))
        << presenter.errorText().toStdString();
    EXPECT_EQ(presenter.visibleCount(), 0);
}
} // namespace ravo
