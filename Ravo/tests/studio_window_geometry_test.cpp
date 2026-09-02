#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "ravo/desktop/studio_window_geometry.h"

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

} // namespace
} // namespace ravo
