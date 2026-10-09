#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QVariantMap>
#include <gtest/gtest.h>

#include "ravo/desktop/studio_window_geometry.h"
#include "ravo/desktop/studio_panel_layout.h"

namespace ravo
{
namespace
{

void ensure_qt_core()
{
    if (QCoreApplication::instance() != nullptr)
        return;
    static int argc = 1;
    static char executable[] = "ravo-desktop-window-geometry-tests";
    static char *argv[] = {executable, nullptr};
    static auto *application = new QCoreApplication(argc, argv);
    static_cast<void>(application);
}

class WindowGeometryTest : public ::testing::Test
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
        QCoreApplication::setOrganizationName(QStringLiteral("RavoWindowGeometryTest"));
        QCoreApplication::setApplicationName(QStringLiteral("WindowGeometryContract"));
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

TEST_F(WindowGeometryTest, DefaultsWhenNothingIsStored)
{
    StudioWindowGeometry geometry;
    ASSERT_TRUE(geometry.initialize());
    EXPECT_FALSE(geometry.hasStoredGeometry());
    EXPECT_EQ(geometry.startupWidth(), StudioWindowGeometry::kDefaultWidth);
    EXPECT_EQ(geometry.startupHeight(), StudioWindowGeometry::kDefaultHeight);
    EXPECT_FALSE(geometry.startupMaximized());
    EXPECT_TRUE(geometry.lastError().isEmpty());
}

TEST_F(WindowGeometryTest, PersistsWindowedGeometryAndReloads)
{
    {
        StudioWindowGeometry geometry;
        ASSERT_TRUE(geometry.initialize());
        ASSERT_TRUE(geometry.rememberWindowed(120, 80, 1280, 800));
        EXPECT_TRUE(geometry.hasStoredGeometry());
        EXPECT_EQ(geometry.startupX(), 120);
        EXPECT_EQ(geometry.startupY(), 80);
        EXPECT_EQ(geometry.startupWidth(), 1280);
        EXPECT_EQ(geometry.startupHeight(), 800);
        EXPECT_FALSE(geometry.startupMaximized());
    }
    StudioWindowGeometry reopened;
    ASSERT_TRUE(reopened.initialize());
    EXPECT_TRUE(reopened.hasStoredGeometry());
    EXPECT_EQ(reopened.startupX(), 120);
    EXPECT_EQ(reopened.startupY(), 80);
    EXPECT_EQ(reopened.startupWidth(), 1280);
    EXPECT_EQ(reopened.startupHeight(), 800);
    EXPECT_FALSE(reopened.startupMaximized());
}

TEST_F(WindowGeometryTest, MaximizedKeepsLastWindowedSize)
{
    StudioWindowGeometry geometry;
    ASSERT_TRUE(geometry.initialize());
    ASSERT_TRUE(geometry.rememberWindowed(40, 50, 1100, 720));
    ASSERT_TRUE(geometry.setMaximized(true));
    EXPECT_TRUE(geometry.startupMaximized());
    EXPECT_EQ(geometry.startupWidth(), 1100);
    EXPECT_EQ(geometry.startupHeight(), 720);

    StudioWindowGeometry reopened;
    ASSERT_TRUE(reopened.initialize());
    EXPECT_TRUE(reopened.startupMaximized());
    EXPECT_EQ(reopened.startupX(), 40);
    EXPECT_EQ(reopened.startupY(), 50);
    EXPECT_EQ(reopened.startupWidth(), 1100);
    EXPECT_EQ(reopened.startupHeight(), 720);
}

TEST_F(WindowGeometryTest, RejectsInvalidWindowedGeometryWithoutWriting)
{
    StudioWindowGeometry geometry;
    ASSERT_TRUE(geometry.initialize());
    EXPECT_FALSE(geometry.rememberWindowed(0, 0, 100, 100));
    EXPECT_FALSE(geometry.hasStoredGeometry());
    EXPECT_FALSE(geometry.lastError().isEmpty());
    {
        QSettings settings;
        EXPECT_FALSE(settings.contains(QStringLiteral("desktop/window/width")));
    }
}

TEST_F(WindowGeometryTest, RepairsMalformedStoredGeometry)
{
    {
        QSettings settings;
        settings.setValue(QStringLiteral("desktop/window/x"), QStringLiteral("left"));
        settings.setValue(QStringLiteral("desktop/window/y"), 10);
        settings.setValue(QStringLiteral("desktop/window/width"), 1280);
        settings.setValue(QStringLiteral("desktop/window/height"), 800);
        settings.setValue(QStringLiteral("desktop/window/maximized"), false);
        settings.sync();
        ASSERT_EQ(settings.status(), QSettings::NoError);
    }
    StudioWindowGeometry geometry;
    ASSERT_TRUE(geometry.initialize());
    EXPECT_FALSE(geometry.hasStoredGeometry());
    EXPECT_EQ(geometry.startupWidth(), StudioWindowGeometry::kDefaultWidth);
    {
        QSettings settings;
        EXPECT_FALSE(settings.contains(QStringLiteral("desktop/window/x")));
        EXPECT_FALSE(settings.contains(QStringLiteral("desktop/window/width")));
    }
}

TEST_F(WindowGeometryTest, RepairsIncompleteStoredGeometry)
{
    {
        QSettings settings;
        settings.setValue(QStringLiteral("desktop/window/width"), 1600);
        settings.sync();
        ASSERT_EQ(settings.status(), QSettings::NoError);
    }
    StudioWindowGeometry geometry;
    ASSERT_TRUE(geometry.initialize());
    EXPECT_FALSE(geometry.hasStoredGeometry());
    {
        QSettings settings;
        EXPECT_FALSE(settings.contains(QStringLiteral("desktop/window/width")));
    }
}

TEST_F(WindowGeometryTest, PanelSizesPersistAcrossOwnerDestruction)
{
    {
        StudioPanelLayout layout;
        ASSERT_TRUE(layout.initialize());
        EXPECT_EQ(layout.leftWidth(), 240);
        EXPECT_EQ(layout.rightWidth(), 320);
        EXPECT_EQ(layout.filmstripHeight(), 108);
        ASSERT_TRUE(layout.setSideWidths(285, 410));
        ASSERT_TRUE(layout.setFilmstripHeight(215));
        // Destruction flushes even before the debounce timer fires.
    }
    StudioPanelLayout reopened;
    ASSERT_TRUE(reopened.initialize());
    EXPECT_EQ(reopened.leftWidth(), 285);
    EXPECT_EQ(reopened.rightWidth(), 410);
    EXPECT_EQ(reopened.filmstripHeight(), 215);
    EXPECT_TRUE(reopened.lastError().isEmpty());
}

TEST_F(WindowGeometryTest, InvalidPanelSizesDoNotReplaceStoredPreference)
{
    StudioPanelLayout layout;
    ASSERT_TRUE(layout.initialize());
    ASSERT_TRUE(layout.setSideWidths(160, 800));
    ASSERT_TRUE(layout.setFilmstripHeight(400));
    ASSERT_TRUE(layout.flush());
    EXPECT_FALSE(layout.setSideWidths(159, 320));
    EXPECT_FALSE(layout.setSideWidths(641, 320));
    EXPECT_FALSE(layout.setSideWidths(240, 259));
    EXPECT_FALSE(layout.setSideWidths(240, 801));
    EXPECT_FALSE(layout.setFilmstripHeight(87));
    EXPECT_FALSE(layout.setFilmstripHeight(401));
    StudioPanelLayout reopened;
    ASSERT_TRUE(reopened.initialize());
    EXPECT_EQ(reopened.leftWidth(), 160);
    EXPECT_EQ(reopened.rightWidth(), 800);
    EXPECT_EQ(reopened.filmstripHeight(), 400);
}

TEST_F(WindowGeometryTest, SettingsResetPersistsDefaultPanelSizes)
{
    StudioPanelLayout layout;
    ASSERT_TRUE(layout.initialize());
    ASSERT_TRUE(layout.setSideWidths(400, 500));
    ASSERT_TRUE(layout.setFilmstripHeight(250));
    EXPECT_EQ(layout.constraints().value("leftMin").toInt(), 160);
    EXPECT_EQ(layout.constraints().value("rightMax").toInt(), 800);
    layout.resetToDefaults();
    ASSERT_TRUE(layout.flush());
    StudioPanelLayout reopened;
    ASSERT_TRUE(reopened.initialize());
    EXPECT_EQ(reopened.leftWidth(), 240);
    EXPECT_EQ(reopened.rightWidth(), 320);
    EXPECT_EQ(reopened.filmstripHeight(), 108);
}

TEST_F(WindowGeometryTest, MalformedPanelLayoutFailsExplicitly)
{
    for (const QVariant &stored : {QVariant(QStringLiteral("broken")),
                                   QVariant(QVariantMap{{QStringLiteral("leftWidth"), 240}}),
                                   QVariant(QVariantMap{{QStringLiteral("leftWidth"), 240},
                                                        {QStringLiteral("rightWidth"), 320},
                                                        {QStringLiteral("filmstripHeight"), 999}})})
    {
        QSettings settings;
        settings.setValue(QStringLiteral("desktop/panel-layout/v1"), stored);
        settings.sync();
        StudioPanelLayout layout;
        EXPECT_FALSE(layout.initialize());
        EXPECT_FALSE(layout.lastError().isEmpty());
    }
}

TEST_F(WindowGeometryTest, PanelLayoutSaveFailureIsVisibleAndCanBeRetried)
{
    const auto settings_directory = QFileInfo(QSettings{}.fileName()).absolutePath();
    ASSERT_TRUE(QDir().mkpath(settings_directory));
    ASSERT_TRUE(QDir(settings_directory).removeRecursively());
    QFile blocker(settings_directory);
    ASSERT_TRUE(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    StudioPanelLayout layout;
    ASSERT_TRUE(layout.setFilmstripHeight(190));
    EXPECT_FALSE(layout.flush());
    EXPECT_FALSE(layout.lastError().isEmpty());
    ASSERT_TRUE(blocker.remove());
    ASSERT_TRUE(QDir().mkpath(settings_directory));
    ASSERT_TRUE(layout.flush());
    EXPECT_TRUE(layout.lastError().isEmpty());
    StudioPanelLayout reopened;
    ASSERT_TRUE(reopened.initialize());
    EXPECT_EQ(reopened.filmstripHeight(), 190);
}

} // namespace
} // namespace ravo
