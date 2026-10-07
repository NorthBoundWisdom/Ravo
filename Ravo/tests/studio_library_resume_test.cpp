#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSettings>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/log.h"
#include "studio_startup_controller.h"
#include "studio_test_support.h"

namespace ravo
{
namespace
{
using studio_test_support::wait_until;
std::vector<AssetRecord> seed_library(const QString &catalog, const QString &folder,
                                      const int count)
{
    auto repository = SqliteCatalogRepository::create(catalog.toStdString());
    if (!repository || !QDir{}.mkpath(folder))
        return {};
    std::vector<AssetRecord> records;
    for (int i = 0; i < count; ++i)
    {
        const auto name = QStringLiteral("frame%1.png").arg(i, 4, 10, QLatin1Char('0'));
        const auto path = QDir(folder).filePath(name);
        QImage image(64, 48, QImage::Format_RGB888);
        image.setColorSpace(QColorSpace(QColorSpace::SRgb));
        image.fill(QColor(i % 200, 80, 120));
        if (!image.save(path))
            return {};
        auto location = normalize_local_input(path.toStdString());
        if (!location)
            return {};
        AssetRecord asset;
        asset.id = "ast_resume_" + std::to_string(i);
        asset.normalized_uri = location.value().uri;
        asset.media_type = std::string(kMediaTypePng);
        asset.import_state = std::string(kImportStateImported);
        asset.width = 64;
        asset.height = 48;
        asset.size_bytes = static_cast<std::uint64_t>(QFileInfo(path).size());
        asset.created_unix_ms = 1000 + i;
        if (!repository.value()->commit_imported_asset(asset))
            return {};
        records.push_back(std::move(asset));
    }
    return records;
}
class StudioLibraryResumeTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        studio_test_support::ensure_qt_core();
        init_logging("ravo-studio-resume-tests");
        QSettings{}.remove("desktop/library-resume/v1");
        ASSERT_TRUE(directory.isValid());
    }
    void open(StudioPresenter &presenter, const QString &catalog)
    {
        presenter.openCatalogFromPath(catalog);
        ASSERT_TRUE(wait_until([&] { return !presenter.busy(); }));
        ASSERT_TRUE(presenter.catalogOpen()) << presenter.errorText().toStdString();
    }
    QTemporaryDir directory;
};

TEST_F(StudioLibraryResumeTest, StartupRestoresLoupePastFirstPageAfterOrderChanges)
{
    const auto catalog = directory.filePath("library.sqlite");
    const auto records = seed_library(catalog, directory.filePath("photos"), 400);
    ASSERT_EQ(records.size(), 400U);
    const auto selected = QString::fromStdString(records[350].id);
    {
        StudioPresenter presenter;
        open(presenter, catalog);
        presenter.library()->setSort("name", "asc");
        presenter.selectFolder(QString::fromStdString(uri_parent(records[350].normalized_uri)));
        ASSERT_TRUE(wait_until(
            [&]
            { return presenter.assets()->assetIdAt(0) == QString::fromStdString(records[0].id); }));
        presenter.selectLibraryRow(350);
        ASSERT_TRUE(wait_until([&] { return presenter.selectedAssetId() == selected; }));
        presenter.setBrowseMode("loupe");
    }
    {
        auto repository = SqliteCatalogRepository::open(catalog.toStdString());
        ASSERT_TRUE(repository);
        auto extra = records.front();
        extra.id = "ast_resume_extra";
        extra.normalized_uri = uri_parent(extra.normalized_uri) + "/000-before.png";
        ASSERT_TRUE(repository.value()->commit_imported_asset(extra));
    }
    StudioPresenter presenter;
    StudioStartupController startup(presenter, directory.filePath("unused-default.sqlite"));
    bool finished = false;
    QObject::connect(&startup, &StudioStartupController::finished,
                     [&](bool create)
                     {
                         EXPECT_FALSE(create);
                         EXPECT_EQ(presenter.selectedAssetId(), selected);
                         EXPECT_EQ(presenter.selectedIndex(), 351);
                         EXPECT_EQ(presenter.browseMode(), "loupe");
                         EXPECT_EQ(presenter.library()->sortField(), "name");
                         finished = true;
                     });
    startup.start();
    ASSERT_TRUE(wait_until([&] { return finished; }));
    EXPECT_EQ(presenter.libraryTotal(), 401);
    EXPECT_LE(presenter.assets()->loadedCount(), int(kLibraryPageDefaultSize));
}

TEST_F(StudioLibraryResumeTest, GridAndSelectionAreIndependentForEachLibrary)
{
    const auto a = directory.filePath("a.sqlite");
    const auto b = directory.filePath("b.sqlite");
    const auto first = seed_library(a, directory.filePath("a"), 3);
    const auto second = seed_library(b, directory.filePath("b"), 3);
    ASSERT_EQ(first.size(), 3U);
    ASSERT_EQ(second.size(), 3U);
    {
        StudioPresenter presenter;
        open(presenter, a);
        presenter.selectAsset(QString::fromStdString(first[1].id));
        presenter.setBrowseMode("loupe");
        const auto empty = directory.filePath("new.sqlite");
        presenter.createCatalogFromPath(empty);
        ASSERT_TRUE(wait_until([&] { return !presenter.busy(); }));
        EXPECT_EQ(presenter.browseMode(), "grid");
        open(presenter, b);
        presenter.selectAsset(QString::fromStdString(second[0].id));
        presenter.setBrowseMode("grid");
        open(presenter, a);
        EXPECT_EQ(presenter.browseMode(), "loupe");
        EXPECT_EQ(presenter.selectedAssetId(), QString::fromStdString(first[1].id));
    }
    StudioPresenter presenter;
    presenter.setStartupCatalogPath(b);
    StudioStartupController startup(presenter, a);
    bool finished = false;
    QObject::connect(&startup, &StudioStartupController::finished, [&](bool) { finished = true; });
    startup.start();
    ASSERT_TRUE(wait_until([&] { return finished; }));
    EXPECT_EQ(presenter.catalogPath(), QFileInfo(b).canonicalFilePath());
    EXPECT_EQ(presenter.browseMode(), "grid");
    EXPECT_EQ(presenter.selectedAssetId(), QString::fromStdString(second[0].id));
}

TEST_F(StudioLibraryResumeTest, DeletedBookmarkSelectsFirstAndEmptyLibraryReturnsGrid)
{
    const auto catalog = directory.filePath("library.sqlite");
    const auto records = seed_library(catalog, directory.filePath("photos"), 2);
    ASSERT_EQ(records.size(), 2U);
    {
        StudioPresenter presenter;
        open(presenter, catalog);
        presenter.selectAsset(QString::fromStdString(records[0].id));
        presenter.setBrowseMode("loupe");
    }
    {
        auto repository = SqliteCatalogRepository::open(catalog.toStdString());
        ASSERT_TRUE(repository);
        ASSERT_TRUE(repository.value()->remove_asset(records[0].id));
    }
    {
        StudioPresenter presenter;
        open(presenter, catalog);
        EXPECT_EQ(presenter.selectedAssetId(), QString::fromStdString(records[1].id));
        EXPECT_EQ(presenter.browseMode(), "loupe");
        EXPECT_TRUE(presenter.errorText().isEmpty());
    }
    {
        auto repository = SqliteCatalogRepository::open(catalog.toStdString());
        ASSERT_TRUE(repository);
        ASSERT_TRUE(repository.value()->remove_asset(records[1].id));
    }
    StudioPresenter presenter;
    open(presenter, catalog);
    EXPECT_TRUE(presenter.selectedAssetId().isEmpty());
    EXPECT_EQ(presenter.browseMode(), "grid");
    EXPECT_TRUE(presenter.errorText().isEmpty());
}

TEST_F(StudioLibraryResumeTest, LastImportScopeRestoresAndCanReturnToAllPhotos)
{
    const auto catalog = directory.filePath("library.sqlite");
    ASSERT_EQ(seed_library(catalog, directory.filePath("photos"), 1).size(), 1U);
    const auto photo = directory.filePath("fresh.png");
    QImage image(64, 48, QImage::Format_RGB888);
    image.fill(Qt::cyan);
    ASSERT_TRUE(image.save(photo));
    {
        StudioPresenter presenter;
        open(presenter, catalog);
        presenter.imports()->importFilePaths({photo});
        ASSERT_TRUE(wait_until(
            [&]
            {
                return !presenter.busy() && !presenter.imports()->importWorkActive() &&
                       presenter.lastImportCount() == 1;
            }));
        presenter.selectLastImport();
        ASSERT_TRUE(wait_until(
            [&] { return presenter.lastImportSelected() && presenter.libraryTotal() == 1; }));
        presenter.setBrowseMode("loupe");
    }
    StudioPresenter presenter;
    open(presenter, catalog);
    EXPECT_TRUE(presenter.lastImportSelected());
    EXPECT_EQ(presenter.lastImportCount(), 1);
    EXPECT_EQ(presenter.libraryTotal(), 1);
    EXPECT_EQ(presenter.browseMode(), "loupe");
    presenter.selectFolder({});
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.lastImportSelected() && presenter.libraryTotal() == 2; }));
}

TEST_F(StudioLibraryResumeTest, CorruptBookmarkFailsWithoutReplacingItOrOpeningDefault)
{
    const auto catalog = directory.filePath("library.sqlite");
    ASSERT_EQ(seed_library(catalog, directory.filePath("photos"), 1).size(), 1U);
    {
        StudioPresenter presenter;
        open(presenter, catalog);
    }
    QSettings settings;
    settings.beginGroup("desktop/library-resume/v1/catalogs");
    const auto keys = settings.allKeys();
    ASSERT_EQ(keys.size(), 1);
    settings.setValue(keys.front(), QByteArray("invalid-json"));
    settings.endGroup();
    settings.sync();
    const auto other = directory.filePath("default.sqlite");
    ASSERT_TRUE(SqliteCatalogRepository::create(other.toStdString()));
    {
        StudioPresenter presenter;
        StudioStartupController startup(presenter, other);
        bool finished = false;
        QObject::connect(&startup, &StudioStartupController::finished,
                         [&](bool create)
                         {
                             EXPECT_FALSE(create);
                             finished = true;
                         });
        startup.start();
        ASSERT_TRUE(wait_until([&] { return finished; }));
        EXPECT_FALSE(presenter.catalogOpen());
        EXPECT_FALSE(presenter.errorText().isEmpty());
    }
    settings.beginGroup("desktop/library-resume/v1/catalogs");
    EXPECT_EQ(settings.value(keys.front()).toByteArray(), QByteArray("invalid-json"));
}
} // namespace
} // namespace ravo
