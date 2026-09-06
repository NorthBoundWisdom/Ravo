#include <QFile>
#include <QPointer>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/foundation/log.h"
#include "studio_startup_controller.h"
#include "studio_test_support.h"

namespace ravo
{
namespace
{
using namespace studio_test_support;

TEST(StudioStartupTest, FirstRunRequestsCreateOnceWithoutOpeningALibrary)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    StudioPresenter presenter;
    StudioStartupController startup(presenter, directory.filePath("new.sqlite"));
    int completions = 0;
    bool create = false;
    QObject::connect(&startup, &StudioStartupController::finished,
                     [&](bool value)
                     {
                         ++completions;
                         create = value;
                     });
    startup.start();
    startup.start();
    EXPECT_EQ(completions, 1);
    EXPECT_TRUE(create);
    EXPECT_FALSE(presenter.busy());
    EXPECT_FALSE(presenter.catalogOpen());
    EXPECT_FALSE(QFile::exists(directory.filePath("new.sqlite")));
}

TEST(StudioStartupTest, DefaultLibraryPublishesCompleteStateBeforeHandoff)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath("library.sqlite");
    {
        const auto created = SqliteCatalogRepository::create(path.toStdString());
        ASSERT_TRUE(created) << created.error().message;
    }
    StudioPresenter presenter;
    StudioStartupController startup(presenter, path);
    int completions = 0;
    QObject::connect(&startup, &StudioStartupController::finished,
                     [&](bool create)
                     {
                         ++completions;
                         EXPECT_FALSE(create);
                         EXPECT_FALSE(presenter.busy());
                         EXPECT_TRUE(presenter.catalogOpen());
                         EXPECT_EQ(presenter.catalogPath(), path);
                         EXPECT_TRUE(presenter.errorText().isEmpty());
                         EXPECT_EQ(presenter.statusText(), QStringLiteral("Library opened."));
                     });
    startup.start();
    EXPECT_TRUE(presenter.busy());
    EXPECT_EQ(completions, 0);
    ASSERT_TRUE(wait_until([&] { return completions == 1; }));
    startup.start();
    EXPECT_EQ(completions, 1);
}

TEST(StudioStartupTest, ExplicitFailureShowsErrorWithoutOpeningDefaultLibrary)
{
    ensure_qt_core();
    init_logging("ravo-studio-startup-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto default_path = directory.filePath("default.sqlite");
    {
        const auto created = SqliteCatalogRepository::create(default_path.toStdString());
        ASSERT_TRUE(created) << created.error().message;
    }
    const auto invalid_path = directory.filePath("corrupt.sqlite");
    QFile invalid(invalid_path);
    ASSERT_TRUE(invalid.open(QIODevice::WriteOnly));
    ASSERT_GT(invalid.write("not a catalog"), 0);
    invalid.close();
    StudioPresenter presenter;
    presenter.setStartupCatalogPath(invalid_path);
    StudioStartupController startup(presenter, default_path);
    bool finished = false;
    QObject::connect(&startup, &StudioStartupController::finished,
                     [&](bool create)
                     {
                         finished = true;
                         EXPECT_FALSE(create);
                         EXPECT_FALSE(presenter.busy());
                         EXPECT_FALSE(presenter.catalogOpen());
                         EXPECT_FALSE(presenter.errorText().isEmpty());
                         EXPECT_EQ(presenter.statusText(), QStringLiteral("Open failed."));
                     });
    startup.start();
    ASSERT_TRUE(wait_until([&] { return finished; }));
}

TEST(StudioStartupTest, DestroyingStartupDuringOpenDropsQueuedHandoff)
{
    ensure_qt_core();
    init_logging("ravo-studio-startup-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    bool finished = false;
    QPointer<StudioStartupController> observed;
    {
        StudioPresenter presenter;
        presenter.setStartupCatalogPath(directory.filePath("missing.sqlite"));
        {
            StudioStartupController startup(presenter, directory.filePath("default.sqlite"));
            observed = &startup;
            QObject::connect(&startup, &StudioStartupController::finished,
                             [&](bool) { finished = true; });
            startup.start();
            EXPECT_TRUE(presenter.busy());
        }
        EXPECT_TRUE(observed.isNull());
        ASSERT_TRUE(wait_until([&] { return !presenter.busy(); }));
    }
    EXPECT_FALSE(finished);
}
} // namespace
} // namespace ravo
