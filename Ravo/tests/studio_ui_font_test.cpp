#include "studio_test_support.h"
#include "studio_ui_font.h"

#include <gtest/gtest.h>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QScopeGuard>
#include <QSet>

namespace ravo
{
using namespace studio_test_support;
using desktop_internal::studio_ui_font_families;

TEST(StudioUiFont, GenericFamiliesResolveToInstalledFaces)
{
    ensure_qt_core();
    const auto installed = QFontDatabase::families();
    ASSERT_FALSE(installed.isEmpty());
    for (const auto *generic : {"Sans Serif", "sans-serif", "system-ui", "ui-sans-serif"})
    {
        QFont font;
        font.setFamilies({QString::fromLatin1(generic)});
        const auto resolved = studio_ui_font_families(font, QStringLiteral("en_US"));
        ASSERT_FALSE(resolved.isEmpty());
        EXPECT_TRUE(installed.contains(resolved.front(), Qt::CaseInsensitive));
        EXPECT_FALSE(resolved.contains(QString::fromLatin1(generic), Qt::CaseInsensitive));
    }
}

TEST(StudioUiFont, ExplicitPlatformFaceAndLanguageOrderSurviveBootstrap)
{
    ensure_qt_core();
    const auto installed = QFontDatabase::families();
    ASSERT_FALSE(installed.isEmpty());
    QFont font;
    font.setFamilies({installed.front()});
    for (const auto *language : {"en_US", "zh_CN", "zh_TW", "ja_JP", "ko_KR"})
    {
        const auto resolved = studio_ui_font_families(font, QString::fromLatin1(language));
        ASSERT_FALSE(resolved.isEmpty());
        EXPECT_EQ(resolved.front(), installed.front());
        EXPECT_EQ(resolved.size(), QSet<QString>(resolved.begin(), resolved.end()).size());
    }
    // These faces are available on the macOS validation host. Other hosts may
    // provide different language faces; do not claim that coverage here.
    if (installed.contains("PingFang SC") && installed.contains("PingFang TC"))
    {
        const auto traditional = studio_ui_font_families(QFont("Sans Serif"), "zh_TW");
        const auto simplified = studio_ui_font_families(QFont("Sans Serif"), "zh_CN");
        EXPECT_LT(traditional.indexOf("PingFang TC"), traditional.indexOf("PingFang SC"));
        EXPECT_LT(simplified.indexOf("PingFang SC"), simplified.indexOf("PingFang TC"));
    }
}

TEST(StudioUiFont, TestSetupUsesStudioPolicyAndDoesNotOverwriteLaterFontChoice)
{
    ensure_qt_core();
    const auto original = QGuiApplication::font();
    const auto restore = qScopeGuard([&] { QGuiApplication::setFont(original); });
    const auto system = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    EXPECT_EQ(original.families(), studio_ui_font_families(system, "en_US"));
    desktop_internal::apply_studio_ui_font("ja_JP");
    const auto japanese = QGuiApplication::font();
    EXPECT_EQ(japanese.families(), studio_ui_font_families(system, "ja_JP"));
    ensure_qt_core();
    EXPECT_EQ(QGuiApplication::font(), japanese);
}
} // namespace ravo
