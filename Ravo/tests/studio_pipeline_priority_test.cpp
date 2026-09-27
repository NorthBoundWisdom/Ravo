#include <future>
#include <memory>

#include <QColorSpace>
#include <QDir>
#include <QImage>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "ravo/desktop/studio_presenter.h"
#include "ravo/desktop/studio_command_controller.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/develop.h"
#include "studio_import_worker.h"
#include "studio_test_support.h"

namespace ravo
{
namespace testing
{
class StudioPipelineTestControl
{
public:
    static bool blockImport(StudioPresenter &presenter, std::shared_ptr<std::promise<void>> entered,
                            std::shared_future<void> release)
    {
        return presenter.import_worker_->executor().post(
            [entered, release]
            {
                entered->set_value();
                release.wait();
            });
    }
    static Result<Recipe> recipe(StudioPresenter &presenter, const std::string &asset)
    {
        return presenter.executor_.submit([&] { return presenter.service_->load_recipe(asset); });
    }
    static Result<ImportItemResult> importPhoto(StudioPresenter &presenter, const std::string &path)
    {
        return presenter.import_worker_->executor().submit(
            [&] { return presenter.import_worker_->service()->import_one(path, {}); });
    }
};
} // namespace testing

namespace
{
using namespace studio_test_support;
struct ImportGate
{
    std::shared_ptr<std::promise<void>> entered = std::make_shared<std::promise<void>>();
    std::shared_future<void> started = entered->get_future().share();
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    bool opened = false;
    void open()
    {
        if (!opened)
        {
            opened = true;
            release.set_value();
        }
    }
    ~ImportGate()
    {
        open();
    }
};

bool write_photo(const QString &path, const int offset)
{
    QImage image(640, 400, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            image.setPixelColor(x, y, QColor((x + offset) % 256, y % 256, (x + y) % 256));
    return image.save(path, "PNG");
}

bool ready(StudioPresenter &presenter)
{
    return !presenter.previewLoading() && !presenter.previewImage().isNull();
}
} // namespace

TEST(StudioPipelinePriority, ColdPreviewAndRotationProceedWhileImportPreflightIsBlocked)
{
    ensure_qt_core();
    init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto existing = directory.filePath("existing.png");
    const auto source = directory.filePath("incoming");
    ASSERT_TRUE(QDir().mkpath(source));
    ASSERT_TRUE(write_photo(existing, 0));
    ASSERT_TRUE(write_photo(source + "/next.png", 80));
    StudioPresenter presenter;
    StudioCommandController commands(presenter);
    presenter.createCatalogFromPath(directory.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.importFilePaths({existing});
    ASSERT_TRUE(
        wait_until([&] { return !presenter.importWorkActive() && presenter.visibleCount() == 1; }));
    const auto selected = presenter.selectedAssetId();
    ASSERT_FALSE(selected.isEmpty());
    presenter.openImportPage();
    presenter.setImportSourceRoot(source);
    presenter.setImportMode("add");
    ASSERT_TRUE(
        wait_until([&] { return !presenter.importScanActive() && presenter.importReady(); }));

    // Declared after the presenter so an assertion failure releases the worker
    // before presenter shutdown joins it.
    ImportGate gate;
    ASSERT_TRUE(
        testing::StudioPipelineTestControl::blockImport(presenter, gate.entered, gate.released));
    ASSERT_TRUE(wait_until(
        [&]
        {
            return gate.started.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
        }));
    presenter.startPlannedImport();
    ASSERT_TRUE(presenter.importPreflightActive());
    presenter.setBrowseMode("loupe");
    ASSERT_TRUE(wait_until([&] { return ready(presenter); }, 5000))
        << presenter.errorText().toStdString();
    EXPECT_EQ(presenter.previewImage().width(), 640);
    EXPECT_EQ(presenter.selectedAssetId(), selected);
    EXPECT_TRUE(presenter.importPreflightActive());

    // The live CLI uses this same command entry point to enter Develop and
    // apply fields while background import remains active.
    auto live_edit = commands.applyDevelopFields({{"exposure", 0.25}});
    ASSERT_TRUE(live_edit) << live_edit.error().message;
    EXPECT_TRUE(live_edit.value());
    EXPECT_EQ(presenter.browseMode(), QStringLiteral("develop"));
    ASSERT_TRUE(wait_until([&] { return ready(presenter); }));
    presenter.setCropToolActive(true);
    ASSERT_TRUE(wait_until([&] { return presenter.cropGuideReady() && ready(presenter); }));
    const auto before = presenter.previewImage();
    const auto before_url = presenter.previewUrl();
    const auto rotate = [&](double value, bool live)
    {
        return commands.executeCommand(
            "studio.edit.set_number",
            QVariantMap{{"name", "straighten"}, {"value", value}, {"live", live}});
    };
    EXPECT_TRUE(rotate(12.0, true).value("accepted").toBool());
    ASSERT_TRUE(
        wait_until([&] { return presenter.previewUrl() != before_url && ready(presenter); }, 5000));
    EXPECT_NE(presenter.previewImage(), before);
    const auto first_rotation = presenter.previewImage();
    const auto first_url = presenter.previewUrl();
    EXPECT_TRUE(rotate(22.0, true).value("accepted").toBool());
    ASSERT_TRUE(
        wait_until([&] { return presenter.previewUrl() != first_url && ready(presenter); }, 5000));
    EXPECT_NE(presenter.previewImage(), first_rotation);
    auto stored = testing::StudioPipelineTestControl::recipe(presenter, selected.toStdString());
    ASSERT_TRUE(stored);
    auto params = develop_from_recipe(stored.value());
    ASSERT_TRUE(params);
    EXPECT_DOUBLE_EQ(params.value().straighten_degrees, 0.0);
    EXPECT_TRUE(presenter.importPreflightActive());
    EXPECT_TRUE(rotate(22.0, false).value("accepted").toBool());
    ASSERT_TRUE(wait_until([&] { return ready(presenter); }, 5000))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();
    gate.open();
    ASSERT_TRUE(wait_until([&] { return !presenter.importWorkActive(); }, 15000))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();
    stored = testing::StudioPipelineTestControl::recipe(presenter, selected.toStdString());
    ASSERT_TRUE(stored);
    params = develop_from_recipe(stored.value());
    ASSERT_TRUE(params);
    EXPECT_DOUBLE_EQ(params.value().straighten_degrees, 22.0);
}

TEST(StudioPipelinePriority, CancelQueuedImportThenReplaceCatalog)
{
    ensure_qt_core();
    init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto existing = directory.filePath("existing.png");
    const auto pending = directory.filePath("pending.png");
    ASSERT_TRUE(write_photo(existing, 0));
    ASSERT_TRUE(write_photo(pending, 90));
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("first.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.importFilePaths({existing});
    ASSERT_TRUE(
        wait_until([&] { return !presenter.importWorkActive() && presenter.visibleCount() == 1; }));
    ImportGate gate;
    ASSERT_TRUE(
        testing::StudioPipelineTestControl::blockImport(presenter, gate.entered, gate.released));
    ASSERT_TRUE(wait_until(
        [&]
        {
            return gate.started.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
        }));
    presenter.importFilePaths({pending});
    ASSERT_TRUE(presenter.importWorkActive());
    presenter.setBrowseMode("loupe");
    ASSERT_TRUE(wait_until([&] { return ready(presenter); }, 5000));
    presenter.cancelCatalogOperation();
    gate.open();
    ASSERT_TRUE(wait_until([&] { return !presenter.importWorkActive(); }));
    EXPECT_EQ(presenter.visibleCount(), 1);
    presenter.createCatalogFromPath(directory.filePath("replacement.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    EXPECT_EQ(presenter.visibleCount(), 0);
    EXPECT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();
}

TEST(StudioPipelinePriority, CloseDuringCatalogWorkerHandoff)
{
    ensure_qt_core();
    init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto catalog = directory.filePath("library.sqlite");
    {
        StudioPresenter presenter;
        presenter.createCatalogFromPath(catalog);
        // Destruction must drain any foreground-to-import session handoff.
    }
    StudioPresenter reopened;
    reopened.openCatalogFromPath(catalog);
    ASSERT_TRUE(wait_until([&] { return reopened.catalogOpen() && !reopened.busy(); }));
    EXPECT_TRUE(reopened.errorText().isEmpty()) << reopened.errorText().toStdString();
    EXPECT_EQ(reopened.visibleCount(), 0);
}

TEST(StudioPipelinePriority, CropFrameChangesKeepCompletePhotoAndViewport)
{
    ensure_qt_core();
    init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto photo = directory.filePath("photo.png");
    ASSERT_TRUE(write_photo(photo, 0));
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.importFilePaths({photo});
    ASSERT_TRUE(
        wait_until([&] { return !presenter.importWorkActive() && presenter.visibleCount() == 1; }));
    presenter.setBrowseMode("develop");
    ASSERT_TRUE(wait_until([&] { return ready(presenter); }));
    presenter.setCropToolActive(true);
    ASSERT_TRUE(wait_until([&] { return presenter.cropGuideReady() && ready(presenter); }));
    const auto original = presenter.previewImage();
    const QSize viewport(presenter.previewViewportWidth(), presenter.previewViewportHeight());
    presenter.previewCropRect(0.15, 0.2, 0.65, 0.6);
    EXPECT_EQ(presenter.previewImage(), original);
    presenter.setCropRect(0.15, 0.2, 0.65, 0.6);
    ASSERT_TRUE(wait_until([&] { return ready(presenter); }));
    EXPECT_EQ(presenter.previewImage(), original);
    EXPECT_EQ(QSize(presenter.previewViewportWidth(), presenter.previewViewportHeight()), viewport);
    presenter.setCropRect(0.2, 0.15, 0.6, 0.7);
    ASSERT_TRUE(wait_until([&] { return ready(presenter); }));
    EXPECT_EQ(presenter.previewImage(), original);
    EXPECT_EQ(QSize(presenter.previewViewportWidth(), presenter.previewViewportHeight()), viewport);
    const auto layout = presenter.cropPreviewLayout();
    const double source_scale = layout.value("widthScale").toDouble() / original.width();
    ASSERT_GT(source_scale, 0.0);
    const auto unrotated_url = presenter.previewUrl();
    presenter.previewDevelopNumber("straighten", 22.0);
    ASSERT_TRUE(
        wait_until([&] { return ready(presenter) && presenter.previewUrl() != unrotated_url; }));
    const auto rotated = presenter.previewImage();
    const auto rotated_layout = presenter.cropPreviewLayout();
    // The full rotated source expands instead of being auto-cropped and enlarged.
    EXPECT_GT(rotated.width(), original.width());
    EXPECT_GT(rotated.height(), original.height());
    EXPECT_NEAR(rotated_layout.value("widthScale").toDouble() / rotated.width(), source_scale,
                1e-9);
    EXPECT_LT(rotated_layout.value("width").toDouble(), 1.0);
    EXPECT_LT(rotated_layout.value("height").toDouble(), 1.0);
    presenter.setDevelopNumber("straighten", 22.0);
    ASSERT_TRUE(wait_until([&] { return ready(presenter); }));
    presenter.setCropToolActive(false);
    ASSERT_TRUE(wait_until([&] { return ready(presenter); }));
    EXPECT_NE(presenter.previewImage().size(), original.size());
    presenter.setCropToolActive(true);
    ASSERT_TRUE(wait_until([&] { return presenter.cropGuideReady() && ready(presenter); }));
    EXPECT_EQ(presenter.previewImage(), rotated);
    EXPECT_EQ(presenter.cropPreviewLayout(), rotated_layout);
}

TEST(StudioPipelinePriority, UnrelatedImportDoesNotRejectPhotoEdit)
{
    ensure_qt_core();
    init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto existing = directory.filePath("existing.png");
    const auto other = directory.filePath("other.png");
    ASSERT_TRUE(write_photo(existing, 0));
    ASSERT_TRUE(write_photo(other, 90));
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.importFilePaths({existing});
    ASSERT_TRUE(
        wait_until([&] { return !presenter.importWorkActive() && presenter.visibleCount() == 1; }));
    presenter.setBrowseMode("develop");
    ASSERT_TRUE(wait_until([&] { return ready(presenter); }));
    const auto selected = presenter.selectedAssetId().toStdString();
    auto imported = testing::StudioPipelineTestControl::importPhoto(presenter, other.toStdString());
    ASSERT_TRUE(imported);
    ASSERT_EQ(imported.value().status, ImportItemStatus::kImported);
    presenter.setDevelopNumber("exposure", 0.5);
    ASSERT_TRUE(wait_until([&] { return ready(presenter); }))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();
    auto recipe = testing::StudioPipelineTestControl::recipe(presenter, selected);
    ASSERT_TRUE(recipe);
    auto params = develop_from_recipe(recipe.value());
    ASSERT_TRUE(params);
    EXPECT_DOUBLE_EQ(params.value().exposure_ev, 0.5);
}
} // namespace ravo
