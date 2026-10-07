#include <cmath>
#include <QColorSpace>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/foundation/log.h"
#include "studio_test_support.h"

namespace ravo
{
TEST(StudioCropTest, AutoLevelChangesOnlyRotationAndRejectsInvalidModes)
{
    studio_test_support::ensure_qt_core();
    init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto photo = directory.filePath(QStringLiteral("tilted-lines.png"));
    QImage image(640, 480, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(Qt::black);
    for (int x = 0; x < image.width(); ++x)
        for (int line = 0; line < 6; ++line)
            for (int thickness = -2; thickness <= 2; ++thickness)
            {
                const int y = 40 + line * 60 + int(std::lround(.0874886635 * x)) + thickness;
                image.setPixelColor(x, y, Qt::white);
            }
    ASSERT_TRUE(image.save(photo));
    QFile source(photo);
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    const auto source_bytes = source.readAll();
    source.close();
    StudioPresenter presenter;
    StudioCommandController commands(presenter);
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(studio_test_support::wait_until(
        [&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.imports()->importFilePaths({photo});
    ASSERT_TRUE(studio_test_support::wait_until(
        [&] { return !presenter.selectedAssetId().isEmpty() && !presenter.busy(); }));
    presenter.setBrowseMode(QStringLiteral("develop"));
    ASSERT_TRUE(
        studio_test_support::wait_until([&] { return !presenter.inspect()->previewLoading(); }));
    presenter.develop()->setDevelopNumbers({{"straighten", .7},
                                            {"perspectiveVertical", .08},
                                            {"perspectiveHorizontal", -.05},
                                            {"perspectiveShear", .015},
                                            {"perspectiveConstrainCrop", 0.0}});
    ASSERT_TRUE(
        studio_test_support::wait_until([&] { return !presenter.inspect()->previewLoading(); }));
    const auto before = presenter.develop()->editPerspective();
    const auto command = QStringLiteral("studio.edit.auto_perspective");
    const auto accepted = commands.executeCommand(command, QStringLiteral("level"));
    ASSERT_TRUE(accepted.value("accepted").toBool());
    ASSERT_TRUE(studio_test_support::wait_until(
        [&]
        {
            return std::abs(presenter.develop()->editStraighten() - .7) > .01 ||
                   !presenter.errorText().isEmpty();
        }));
    ASSERT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();
    EXPECT_NEAR(presenter.develop()->editStraighten(), 5.0, .3);
    const auto after = presenter.develop()->editPerspective();
    for (const auto *key :
         {"vertical", "horizontal", "shear", "constrainCrop", "interpolationIndex"})
        EXPECT_EQ(after.value(key), before.value(key)) << key;
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    EXPECT_EQ(source.readAll(), source_bytes);
    EXPECT_FALSE(
        commands.executeCommand(command, QStringLiteral("invalid")).value("accepted").toBool());
}
} // namespace ravo
