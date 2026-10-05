#include <memory>

#include <QElapsedTimer>
#include <QEventLoop>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <gtest/gtest.h>

#include "studio_test_support.h"

namespace ravo
{
namespace
{
using namespace studio_test_support;

void advance(const int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

std::unique_ptr<QQuickItem> load_progress(QQmlEngine &engine, QQuickWindow &window)
{
    QQmlComponent component(
        &engine, QUrl::fromLocalFile(QString::fromUtf8(RAVO_REPOSITORY_ROOT) +
                                     "/Ravo/desktop/qml/gallery/DeferredPreviewProgress.qml"));
    if (component.isError())
    {
        ADD_FAILURE() << component.errorString().toStdString();
        return {};
    }
    auto item = std::unique_ptr<QQuickItem>(qobject_cast<QQuickItem *>(component.create()));
    if (item)
    {
        item->setParentItem(window.contentItem());
        window.resize(240, 80);
        window.show();
    }
    return item;
}

TEST(StudioLibraryProgress, SmallAndShortPreviewBatchesStayHidden)
{
    ensure_qt_core();
    QQmlEngine engine;
    QQuickWindow window;
    auto progress = load_progress(engine, window);
    ASSERT_TRUE(progress);
    progress->setProperty("total", 7);
    progress->setProperty("workActive", true);
    advance(650);
    EXPECT_FALSE(progress->isVisible());
    EXPECT_FALSE(progress->property("revealed").toBool());
    progress->setProperty("completed", 7);
    progress->setProperty("workActive", false);
    progress->setProperty("completed", 0);
    progress->setProperty("total", 8);
    progress->setProperty("workActive", true);
    advance(50);
    progress->setProperty("completed", 8);
    progress->setProperty("workActive", false);
    advance(650);
    EXPECT_FALSE(progress->isVisible());
    EXPECT_FALSE(progress->property("revealed").toBool());
}

TEST(StudioLibraryProgress, SustainedBatchRevealsOnceAndRemainsUntilCompletion)
{
    ensure_qt_core();
    QQmlEngine engine;
    QQuickWindow window;
    auto progress = load_progress(engine, window);
    ASSERT_TRUE(progress);
    progress->setSize(QSizeF(220, 4));
    const auto bounds = progress->boundingRect();
    QElapsedTimer elapsed;
    elapsed.start();
    progress->setProperty("total", 8);
    progress->setProperty("workActive", true);
    EXPECT_FALSE(progress->isVisible());
    ASSERT_TRUE(wait_until([&] { return progress->isVisible(); }, 2000));
    EXPECT_GE(elapsed.elapsed(), 400);
    EXPECT_EQ(progress->boundingRect(), bounds);
    progress->setProperty("completed", 7);
    EXPECT_TRUE(progress->isVisible()); // Do not blink when the queue drops below the threshold.
    EXPECT_EQ(progress->boundingRect(), bounds);
    progress->setProperty("completed", 8);
    progress->setProperty("workActive", false);
    EXPECT_FALSE(progress->isVisible());
    EXPECT_EQ(progress->boundingRect(), bounds);
    progress->setProperty("total", 1);
    progress->setProperty("completed", 0);
    progress->setProperty("workActive", true);
    advance(650);
    EXPECT_FALSE(progress->isVisible());
}

TEST(StudioLibraryProgress, RapidReplacedBatchesDoNotAccumulateRevealTime)
{
    ensure_qt_core();
    QQmlEngine engine;
    QQuickWindow window;
    auto progress = load_progress(engine, window);
    ASSERT_TRUE(progress);
    for (int batch = 0; batch < 12; ++batch)
    {
        progress->setProperty("completed", 0);
        progress->setProperty("total", 8);
        progress->setProperty("workActive", true);
        advance(50);
        EXPECT_FALSE(progress->isVisible());
        progress->setProperty("completed", 8);
        progress->setProperty("workActive", false);
    }
    advance(550);
    EXPECT_FALSE(progress->property("revealed").toBool());
}
} // namespace
} // namespace ravo
