#include <QColorSpace>
#include <QCryptographicHash>
#include <QImage>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "ravo/desktop/import_candidate_list_model.h"
#include "ravo/desktop/studio_presenter.h"
#include "studio_test_support.h"
#include "ravo/foundation/log.h"

namespace ravo
{
using namespace studio_test_support;

namespace
{
bool photo(const QString &path, const QColor &color)
{
    QImage image(32, 24, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(color);
    return image.save(path, "PNG");
}

QByteArray file_sha256(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}
} // namespace

// Temp media + temp catalog only. Opt-in real corpus is not exercised here (C1/C2).
TEST(StudioImportRoundtrip, CopyPreservesSourceHashAndGalleryMembership)
{
    ensure_qt_core();
    init_logging("ravo-import-roundtrip");
    QTemporaryDir directory;
    const auto source = directory.filePath("source");
    const auto destination = directory.filePath("destination");
    ASSERT_TRUE(QDir().mkpath(source));
    ASSERT_TRUE(QDir().mkpath(destination));
    ASSERT_TRUE(photo(source + "/keep.png", Qt::green));
    ASSERT_TRUE(photo(source + "/also.png", Qt::yellow));
    const auto keep_hash = file_sha256(source + "/keep.png");
    const auto also_hash = file_sha256(source + "/also.png");
    ASSERT_FALSE(keep_hash.isEmpty());
    ASSERT_FALSE(also_hash.isEmpty());

    const auto catalog = directory.filePath("library.sqlite");
    {
        StudioPresenter presenter;
        presenter.createCatalogFromPath(catalog);
        ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));

        presenter.openImportPage();
        presenter.importSourceFolders()->resetWithRoots(
            {{directory.path(), QStringLiteral("fixture"), true}});
        presenter.importDestinationFolders()->resetWithRoots(
            {{directory.path(), QStringLiteral("fixture"), true}});
        presenter.setImportSourceRoot(source);
        presenter.setImportDestination(destination);
        ASSERT_TRUE(
            wait_until([&] { return !presenter.importScanActive() && presenter.importReady(); }));
        ASSERT_EQ(presenter.importCandidates()->rowCount(), 2);
        ASSERT_EQ(presenter.importCandidates()->selectedCount(), 2);

        // Destination preview may be empty while debounce runs; wait for inactive.
        ASSERT_TRUE(wait_until([&] { return !presenter.importDestinationPreviewActive(); }, 5000));

        presenter.startPlannedImport();
        ASSERT_TRUE(wait_until(
            [&] { return !presenter.importPreflightActive() && !presenter.importWorkActive(); },
            30000));
        ASSERT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();
        EXPECT_EQ(presenter.lastImportCount(), 2);

        EXPECT_EQ(file_sha256(source + "/keep.png"), keep_hash);
        EXPECT_EQ(file_sha256(source + "/also.png"), also_hash);
        EXPECT_TRUE(QFile::exists(destination + "/keep.png"));
        EXPECT_TRUE(QFile::exists(destination + "/also.png"));
        EXPECT_EQ(file_sha256(destination + "/keep.png"), keep_hash);
        EXPECT_EQ(file_sha256(destination + "/also.png"), also_hash);

        presenter.closeImportPage();
        presenter.openImportPage();
        presenter.setImportSourceRoot(source);
        ASSERT_TRUE(wait_until([&] { return !presenter.importScanActive(); }));
        EXPECT_EQ(presenter.importDuplicateCount(), 2);
        EXPECT_FALSE(presenter.importReady());
        EXPECT_GE(presenter.libraryTotal(), 2);
        presenter.closeImportPage();
        // Same-catalog second Presenter reopen is covered by existing workspace reopen
        // fixtures; this tranche asserts membership on the live session after copy.
    }
}

TEST(StudioImportRoundtrip, CancelViaPageCloseLeavesSourcesUntouched)
{
    ensure_qt_core();
    init_logging("ravo-import-roundtrip");
    QTemporaryDir directory;
    const auto source = directory.filePath("source");
    const auto destination = directory.filePath("destination");
    ASSERT_TRUE(QDir().mkpath(source));
    ASSERT_TRUE(QDir().mkpath(destination));
    ASSERT_TRUE(photo(source + "/solo.png", Qt::cyan));
    const auto original = file_sha256(source + "/solo.png");
    ASSERT_FALSE(original.isEmpty());

    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.openImportPage();
    presenter.importSourceFolders()->resetWithRoots(
        {{directory.path(), QStringLiteral("fixture"), true}});
    presenter.importDestinationFolders()->resetWithRoots(
        {{directory.path(), QStringLiteral("fixture"), true}});
    presenter.setImportSourceRoot(source);
    presenter.setImportDestination(destination);
    ASSERT_TRUE(
        wait_until([&] { return !presenter.importScanActive() && presenter.importReady(); }));
    presenter.startPlannedImport();
    presenter.closeImportPage();
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.importPreflightActive() && !presenter.importWorkActive(); }));
    EXPECT_EQ(file_sha256(source + "/solo.png"), original);
    EXPECT_TRUE(QDir(destination).entryList(QDir::Files).isEmpty());
}
} // namespace ravo
