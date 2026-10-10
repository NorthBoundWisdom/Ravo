#include "ravo/desktop/studio_import_workspace.h"
#include <future>
#include <memory>
#include <atomic>
#include <cstdlib>

#include <QColorSpace>
#include <QDir>
#include <QImage>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <QEvent>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <gtest/gtest.h>

#include "ravo/desktop/studio_presenter.h"
#include "ravo/desktop/studio_command_controller.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/develop.h"
#include "studio_import_worker.h"
#include "studio_test_support.h"
#include "interactive_perf_report.h"
#include "ravo/desktop/studio_display_presentation.h"
#include "ravo/services/display_presentation.h"

namespace ravo
{
namespace testing
{
class StudioPipelineTestControl
{
public:
    static void load(StudioPresenter &presenter)
    {
        presenter.develop()->load_develop_for_selection();
    }
    static void drain(StudioPresenter &presenter)
    {
        presenter.executor_.wait_idle();
    }
    static bool loaded(StudioPresenter &presenter)
    {
        return presenter.develop()->state_.develop_loaded_;
    }
    static void clearError(StudioPresenter &presenter)
    {
        presenter.setError({});
    }
    static bool queueFailure(StudioPresenter &presenter)
    {
        SerialExecutor stopped;
        stopped.request_stop();
        stopped.wait();
        StudioDevelopPresenter::Host host;
        host.selected_media_type = [] { return QStringLiteral("image/png"); };
        host.preview_loading = [&](bool loading) { presenter.inspect_.setPreviewLoading(loading); };
        const bool importing = presenter.import_workspace_->importWorkActive();
        StudioDevelopPresenter isolated({presenter.selected_asset_id_, presenter.catalog_path_,
                                         presenter.browse_mode_, presenter.busy_,
                                         presenter.catalog_operation_active_, importing,
                                         presenter.observed_catalog_revision_, presenter.assets_,
                                         presenter.inspect_, presenter.engine_, stopped},
                                        std::move(host), nullptr);
        isolated.load_develop_for_selection();
        return !isolated.state_.develop_loaded_ && !isolated.state_.develop_load_error_.isEmpty();
    }
    static bool probeImports(StudioPresenter &presenter, const std::vector<std::string> &paths,
                             std::shared_ptr<std::atomic<bool>> stop,
                             std::shared_ptr<std::atomic<std::size_t>> count)
    {
        return presenter.imports()->importWorker().executor().post(
            [&presenter, paths, stop, count]
            {
                for (const auto &path : paths)
                {
                    if (stop->load())
                        break;
                    auto imported =
                        presenter.imports()->importWorker().service()->import().import_one(path,
                                                                                           {});
                    if (imported && imported.value().status == ImportItemStatus::kImported)
                        ++*count;
                }
            });
    }
    static void drainImports(StudioPresenter &presenter)
    {
        presenter.imports()->importWorker().executor().wait_idle();
    }
    static bool blockImport(StudioPresenter &presenter, std::shared_ptr<std::promise<void>> entered,
                            std::shared_future<void> release)
    {
        return presenter.imports()->importWorker().executor().post(
            [entered, release]
            {
                entered->set_value();
                release.wait();
            });
    }
    static bool blockPreview(StudioPresenter &presenter,
                             std::shared_ptr<std::promise<void>> entered,
                             std::shared_future<void> release)
    {
        return presenter.executor_.post(
            [entered, release]
            {
                entered->set_value();
                release.wait();
            });
    }
    static Result<Recipe> recipe(StudioPresenter &presenter, const std::string &asset)
    {
        return presenter.executor_.submit(
            [&] { return presenter.service_->develop().load_recipe(asset); });
    }
    static Result<ImportItemResult> importPhoto(StudioPresenter &presenter, const std::string &path)
    {
        return presenter.imports()->importWorker().executor().submit(
            [&]
            {
                return presenter.imports()->importWorker().service()->import().import_one(path, {});
            });
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
    return !presenter.inspect()->previewLoading() && !presenter.inspect()->previewImage().isNull();
}
} // namespace

TEST(StudioPipelinePriority, RecipeLoadRejectsOldAAfterABAAndSessionReplacement)
{
    ensure_qt_core();
    init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    const auto first = directory.filePath("first.png"), second = directory.filePath("second.png");
    ASSERT_TRUE(write_photo(first, 0));
    ASSERT_TRUE(write_photo(second, 80));
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.imports()->importFilePaths({first, second});
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.imports()->importWorkActive() && presenter.visibleCount() == 2; }));
    const auto a = presenter.assets()->assetIdAt(0), b = presenter.assets()->assetIdAt(1);
    presenter.selectAsset(a);
    ASSERT_TRUE(wait_until([&] { return testing::StudioPipelineTestControl::loaded(presenter); }));
    for (const bool replace_session : {false, true})
    {
        // Complete the old read on the worker without delivering its GUI event.
        testing::StudioPipelineTestControl::load(presenter);
        testing::StudioPipelineTestControl::drain(presenter);
        ImportGate gate;
        ASSERT_TRUE(testing::StudioPipelineTestControl::blockPreview(presenter, gate.entered,
                                                                     gate.released));
        ASSERT_EQ(gate.started.wait_for(std::chrono::seconds(5)), std::future_status::ready);
        if (replace_session)
        {
            presenter.openCatalogFromPath(directory.filePath("library.sqlite"));
        }
        else
        {
            presenter.selectAsset(b);
            presenter.selectAsset(a);
        }
        EXPECT_FALSE(testing::StudioPipelineTestControl::loaded(presenter));
        QCoreApplication::sendPostedEvents(presenter.develop(), QEvent::MetaCall);
        EXPECT_FALSE(testing::StudioPipelineTestControl::loaded(presenter));
        gate.open();
        ASSERT_TRUE(
            wait_until([&] { return testing::StudioPipelineTestControl::loaded(presenter); }));
        EXPECT_EQ(presenter.selectedAssetId(), a);
    }
}

TEST(StudioPipelinePriority, WhiteBalancePickPreflightStaysOnGuiThread)
{
    ensure_qt_core();
    init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    const auto photo = directory.filePath("photo.png");
    ASSERT_TRUE(write_photo(photo, 0));
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.imports()->importFilePaths({photo});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.imports()->importWorkActive() &&
                   testing::StudioPipelineTestControl::loaded(presenter);
        }));
    const auto before = testing::StudioPipelineTestControl::recipe(
        presenter, presenter.selectedAssetId().toStdString());
    ASSERT_TRUE(before);
    presenter.develop()->pickWhiteBalance(0.5, 0.5);
    EXPECT_EQ(presenter.errorText(),
              QCoreApplication::translate("DevelopPanel",
                                          "White-balance pick requires a Bayer RAW original"));
    const auto after = testing::StudioPipelineTestControl::recipe(
        presenter, presenter.selectedAssetId().toStdString());
    ASSERT_TRUE(after);
    EXPECT_EQ(serialize_recipe(before.value()).value(), serialize_recipe(after.value()).value());
}

TEST(StudioPipelinePriority, RecipeHistoryReadAndQueueFailuresRemainUnloaded)
{
    ensure_qt_core();
    init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    const auto photo = directory.filePath("photo.png"),
               catalog = directory.filePath("library.sqlite");
    ASSERT_TRUE(write_photo(photo, 0));
    StudioPresenter presenter;
    presenter.createCatalogFromPath(catalog);
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.imports()->importFilePaths({photo});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.imports()->importWorkActive() && presenter.visibleCount() == 1 &&
                   testing::StudioPipelineTestControl::loaded(presenter);
        }));
    testing::StudioPipelineTestControl::drain(presenter);
    // Private fixture fault: recipe still reads successfully; only history fails.
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", "recipe-history-fault");
        db.setDatabaseName(catalog);
        ASSERT_TRUE(db.open());
        QSqlQuery query(db);
        ASSERT_TRUE(query.exec("ALTER TABLE asset_recipe_history RENAME TO unavailable_history"));
    }
    QSqlDatabase::removeDatabase("recipe-history-fault");
    ASSERT_TRUE(testing::StudioPipelineTestControl::recipe(
        presenter, presenter.selectedAssetId().toStdString()));
    testing::StudioPipelineTestControl::clearError(presenter);
    testing::StudioPipelineTestControl::load(presenter);
    ASSERT_TRUE(wait_until([&] { return !presenter.errorText().isEmpty(); }));
    EXPECT_FALSE(testing::StudioPipelineTestControl::loaded(presenter));
    EXPECT_FALSE(presenter.develop()->canAdjustExposure());
    const auto before = testing::StudioPipelineTestControl::recipe(
        presenter, presenter.selectedAssetId().toStdString());
    ASSERT_TRUE(before);
    presenter.develop()->setDevelopNumber("exposure", 2);
    testing::StudioPipelineTestControl::drain(presenter);
    const auto after = testing::StudioPipelineTestControl::recipe(
        presenter, presenter.selectedAssetId().toStdString());
    ASSERT_TRUE(after);
    EXPECT_EQ(serialize_recipe(before.value()).value(), serialize_recipe(after.value()).value());
    EXPECT_TRUE(testing::StudioPipelineTestControl::queueFailure(presenter));
    EXPECT_FALSE(presenter.inspect()->previewLoading());
}

TEST(StudioDisplayResourceProbe, MeasuresIntentToDisplayWithConcurrentImport)
{
    if (!std::getenv("RAVO_DISPLAY_RESOURCE_PROBE"))
        GTEST_SKIP()
            << "set RAVO_DISPLAY_RESOURCE_PROBE=1 for the synthetic import/edit measurement";
    ensure_qt_core();
    init_logging("ravo-display-resource-probe");
    QTemporaryDir directory;
    const auto first = directory.filePath("selected.png");
    QImage image(1024, 768, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            image.setPixelColor(x, y, QColor(x % 256, y % 256, (x + y) % 256));
    ASSERT_TRUE(image.save(first, "PNG"));
    std::vector<std::string> paths;
    for (int index = 0; index < 96; ++index)
    {
        const auto path = directory.filePath(QStringLiteral("import-%1.png").arg(index));
        image.setPixelColor(0, 0, QColor(index, index, index));
        ASSERT_TRUE(image.save(path, "PNG"));
        paths.push_back(path.toStdString());
    }
    StudioDisplayPresentation display;
    ASSERT_TRUE(display.valid());
    auto srgb = make_srgb_color_profile();
    ASSERT_TRUE(srgb);
    if (color_profile_fingerprint(srgb.value()) == display.presentationState().profile_fingerprint)
        GTEST_SKIP() << "System monitor is sRGB; this probe requires an actual ICC conversion";
    StudioPresenter presenter;
    presenter.bindDisplayPresentation(&display);
    presenter.createCatalogFromPath(directory.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.imports()->importFilePaths({first});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.imports()->importWorkActive() &&
                   testing::StudioPipelineTestControl::loaded(presenter);
        }));
    presenter.setBrowseMode("develop");
    ASSERT_TRUE(wait_until([&] { return ready(presenter); }));
    for (const bool concurrent_import : {false, true})
    {
        auto stop = std::make_shared<std::atomic<bool>>(false);
        auto count = std::make_shared<std::atomic<std::size_t>>(0);
        struct Drain
        {
            StudioPresenter &presenter;
            std::shared_ptr<std::atomic<bool>> stop;
            ~Drain()
            {
                stop->store(true);
                testing::StudioPipelineTestControl::drainImports(presenter);
            }
        } drain{presenter, stop};
        if (concurrent_import)
            ASSERT_TRUE(
                testing::StudioPipelineTestControl::probeImports(presenter, paths, stop, count));
        std::vector<std::int64_t> samples;
        std::optional<std::int64_t> elapsed;
        const auto connection = QObject::connect(
            presenter.develop(), &StudioDevelopPresenter::interactivePreviewPublished, &presenter,
            [&](qulonglong, qlonglong us) { elapsed = us; });
        for (int run = 0; run < 66; ++run)
        {
            elapsed.reset();
            presenter.develop()->previewDevelopNumber("exposure", (run % 7 - 3) * 0.01);
            ASSERT_TRUE(wait_until([&] { return elapsed.has_value(); }, 10000))
                << presenter.errorText().toStdString();
            if (run >= 2)
                samples.push_back(*elapsed);
        }
        QObject::disconnect(connection);
        if (concurrent_import)
            EXPECT_GT(count->load(), 0U);
        interactive_perf_report::CaseMeta meta;
        meta.case_id = concurrent_import ? "display_intent_with_import" : "display_intent_idle";
        meta.path = "studio_display_icc";
        meta.source_kind = "generated_srgb_png_system_monitor_icc";
        meta.recorded_samples = samples.size();
        meta.file_count = count->load();
        interactive_perf_report::emit_case(meta, samples);
    }
}

TEST(StudioPipelinePriority, DestinationFoldersProceedWhileImportWorkerIsBlocked)
{
    ensure_qt_core();
    init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    const auto source = directory.filePath("source");
    const auto destination = directory.filePath("destination");
    ASSERT_TRUE(QDir().mkpath(source));
    ASSERT_TRUE(QDir().mkpath(destination));
    ASSERT_TRUE(write_photo(source + "/next.png", 0));
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.imports()->openImportPage();
    presenter.imports()->setImportSourceRoot(source);
    presenter.imports()->setImportDestination(destination);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.imports()->importScanActive() &&
                   !presenter.imports()->importDestinationPreviewActive();
        }));
    // Declare after presenter: a failed assertion releases the worker before
    // presenter destruction joins it.
    ImportGate gate;
    ASSERT_TRUE(
        testing::StudioPipelineTestControl::blockImport(presenter, gate.entered, gate.released));
    ASSERT_EQ(gate.started.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    presenter.imports()->setImportOrganization(QStringLiteral("month"));
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.imports()->importDestinationPreviewActive() &&
                   presenter.imports()->importDestinationPreview().size() == 3;
        }))
        << presenter.imports()->importDestinationPreviewError().toStdString();
    EXPECT_EQ(
        presenter.imports()->importDestinationPreview().back().toMap().value("photoCount").toUInt(),
        1U);
    EXPECT_TRUE(QDir(destination).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).empty());
    presenter.imports()->setImportOrganization(QStringLiteral("date"));
    presenter.imports()->closeImportPage();
    EXPECT_TRUE(presenter.imports()->importDestinationPreview().empty());
    gate.open();
    EXPECT_TRUE(wait_until([&] { return !presenter.imports()->importDestinationPreviewActive(); }));
    EXPECT_TRUE(presenter.imports()->importDestinationPreview().empty());
}

TEST(StudioPipelinePriority, ReselectCroppedPhotoUsesThumbnailAspectBeforeFullPreview)
{
    ensure_qt_core();
    init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    const auto first = directory.filePath("first.png");
    const auto second = directory.filePath("second.png");
    ASSERT_TRUE(write_photo(first, 0));
    ASSERT_TRUE(write_photo(second, 80));
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("library.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.imports()->importFilePaths({first, second});
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.imports()->importWorkActive() && presenter.visibleCount() == 2; }));
    const auto cropped = presenter.assets()->assetIdAt(0);
    const auto landscape = presenter.assets()->assetIdAt(1);
    presenter.selectAsset(cropped);
    presenter.setBrowseMode("develop");
    ASSERT_TRUE(wait_until([&] { return ready(presenter); }));
    presenter.develop()->setCropToolActive(true);
    ASSERT_TRUE(
        wait_until([&] { return presenter.develop()->cropGuideReady() && ready(presenter); }));
    presenter.develop()->setCropRect(0, 0, 0.5, 1);
    ASSERT_TRUE(wait_until([&] { return ready(presenter); }));
    presenter.develop()->setCropToolActive(false);
    ASSERT_TRUE(wait_until([&] { return ready(presenter); }));
    ASSERT_EQ(presenter.inspect()->previewImage().size(), QSize(320, 400));
    presenter.setBrowseMode("grid");
    presenter.ensureThumbnail(cropped);
    ASSERT_TRUE(wait_until(
        [&]
        {
            const QImage thumbnail(presenter.selectedThumbnailUrl().toLocalFile());
            return !thumbnail.isNull() && thumbnail.width() < thumbnail.height() &&
                   !presenter.previewWorkActive();
        }));
    presenter.selectAsset(landscape);
    presenter.setBrowseMode("loupe");
    ASSERT_TRUE(wait_until([&] { return ready(presenter); }));
    ImportGate gate;
    ASSERT_TRUE(
        testing::StudioPipelineTestControl::blockPreview(presenter, gate.entered, gate.released));
    ASSERT_EQ(gate.started.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    for (const auto &id : {cropped, landscape, cropped})
    {
        presenter.selectAsset(id);
        EXPECT_TRUE(presenter.inspect()->previewLoading());
        EXPECT_TRUE(presenter.inspect()->previewUrl().isEmpty());
        const QImage thumbnail(presenter.selectedThumbnailUrl().toLocalFile());
        ASSERT_FALSE(thumbnail.isNull());
        EXPECT_EQ(thumbnail.width() < thumbnail.height(), id == cropped);
        const double aspect = static_cast<double>(thumbnail.width()) / thumbnail.height();
        EXPECT_NEAR(static_cast<double>(presenter.inspect()->previewViewportWidth()) /
                        presenter.inspect()->previewViewportHeight(),
                    aspect, 0.002);
        EXPECT_NEAR(static_cast<double>(presenter.inspect()->navigatorViewportWidth()) /
                        presenter.inspect()->navigatorViewportHeight(),
                    aspect, 0.002);
    }
    gate.open();
    ASSERT_TRUE(wait_until([&] { return ready(presenter); }));
    EXPECT_EQ(presenter.selectedAssetId(), cropped);
    EXPECT_EQ(presenter.inspect()->previewImage().size(), QSize(320, 400));
}

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
    presenter.imports()->importFilePaths({existing});
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.imports()->importWorkActive() && presenter.visibleCount() == 1; }));
    const auto selected = presenter.selectedAssetId();
    ASSERT_FALSE(selected.isEmpty());
    presenter.imports()->openImportPage();
    presenter.imports()->setImportSourceRoot(source);
    presenter.imports()->setImportMode("add");
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.imports()->importScanActive() && presenter.imports()->importReady();
        }));

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
    presenter.imports()->startPlannedImport();
    ASSERT_TRUE(presenter.imports()->importPreflightActive());
    presenter.setBrowseMode("loupe");
    ASSERT_TRUE(wait_until([&] { return ready(presenter); }, 5000))
        << presenter.errorText().toStdString();
    EXPECT_EQ(presenter.inspect()->previewImage().width(), 640);
    EXPECT_EQ(presenter.selectedAssetId(), selected);
    EXPECT_TRUE(presenter.imports()->importPreflightActive());

    // The live CLI uses this same command entry point to enter Develop and
    // apply fields while background import remains active.
    auto live_edit = commands.applyDevelopFields({{"exposure", 0.25}});
    ASSERT_TRUE(live_edit) << live_edit.error().message;
    EXPECT_TRUE(live_edit.value());
    EXPECT_EQ(presenter.browseMode(), QStringLiteral("develop"));
    ASSERT_TRUE(wait_until([&] { return ready(presenter); }));
    presenter.develop()->setCropToolActive(true);
    ASSERT_TRUE(
        wait_until([&] { return presenter.develop()->cropGuideReady() && ready(presenter); }));
    const auto before = presenter.inspect()->previewImage();
    const auto before_url = presenter.inspect()->previewUrl();
    const auto rotate = [&](double value, bool live)
    {
        return commands.executeCommand(
            "studio.edit.set_number",
            QVariantMap{{"name", "straighten"}, {"value", value}, {"live", live}});
    };
    EXPECT_TRUE(rotate(12.0, true).value("accepted").toBool());
    ASSERT_TRUE(wait_until(
        [&] { return presenter.inspect()->previewUrl() != before_url && ready(presenter); }, 5000));
    EXPECT_NE(presenter.inspect()->previewImage(), before);
    const auto first_rotation = presenter.inspect()->previewImage();
    const auto first_url = presenter.inspect()->previewUrl();
    EXPECT_TRUE(rotate(22.0, true).value("accepted").toBool());
    ASSERT_TRUE(wait_until(
        [&] { return presenter.inspect()->previewUrl() != first_url && ready(presenter); }, 5000));
    EXPECT_NE(presenter.inspect()->previewImage(), first_rotation);
    auto stored = testing::StudioPipelineTestControl::recipe(presenter, selected.toStdString());
    ASSERT_TRUE(stored);
    auto params = develop_from_recipe(stored.value());
    ASSERT_TRUE(params);
    EXPECT_DOUBLE_EQ(params.value().straighten_degrees, 0.0);
    EXPECT_TRUE(presenter.imports()->importPreflightActive());
    EXPECT_TRUE(rotate(22.0, false).value("accepted").toBool());
    ASSERT_TRUE(wait_until([&] { return ready(presenter); }, 5000))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();
    gate.open();
    ASSERT_TRUE(wait_until([&] { return !presenter.imports()->importWorkActive(); }, 15000))
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
    presenter.imports()->importFilePaths({existing});
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.imports()->importWorkActive() && presenter.visibleCount() == 1; }));
    ImportGate gate;
    ASSERT_TRUE(
        testing::StudioPipelineTestControl::blockImport(presenter, gate.entered, gate.released));
    ASSERT_TRUE(wait_until(
        [&]
        {
            return gate.started.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
        }));
    presenter.imports()->importFilePaths({pending});
    ASSERT_TRUE(presenter.imports()->importWorkActive());
    presenter.setBrowseMode("loupe");
    ASSERT_TRUE(wait_until([&] { return ready(presenter); }, 5000));
    presenter.cancelCatalogOperation();
    gate.open();
    ASSERT_TRUE(wait_until([&] { return !presenter.imports()->importWorkActive(); }));
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
    presenter.imports()->importFilePaths({photo});
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.imports()->importWorkActive() && presenter.visibleCount() == 1; }));
    presenter.setBrowseMode("develop");
    ASSERT_TRUE(wait_until([&] { return ready(presenter); }));
    presenter.develop()->setCropToolActive(true);
    ASSERT_TRUE(
        wait_until([&] { return presenter.develop()->cropGuideReady() && ready(presenter); }));
    const auto original = presenter.inspect()->previewImage();
    const QSize viewport(presenter.inspect()->previewViewportWidth(),
                         presenter.inspect()->previewViewportHeight());
    presenter.develop()->previewCropRect(0.15, 0.2, 0.65, 0.6);
    EXPECT_EQ(presenter.inspect()->previewImage(), original);
    presenter.develop()->setCropRect(0.15, 0.2, 0.65, 0.6);
    ASSERT_TRUE(wait_until([&] { return ready(presenter); }));
    EXPECT_EQ(presenter.inspect()->previewImage(), original);
    EXPECT_EQ(QSize(presenter.inspect()->previewViewportWidth(),
                    presenter.inspect()->previewViewportHeight()),
              viewport);
    presenter.develop()->setCropRect(0.2, 0.15, 0.6, 0.7);
    ASSERT_TRUE(wait_until([&] { return ready(presenter); }));
    EXPECT_EQ(presenter.inspect()->previewImage(), original);
    EXPECT_EQ(QSize(presenter.inspect()->previewViewportWidth(),
                    presenter.inspect()->previewViewportHeight()),
              viewport);
    const auto layout = presenter.inspect()->cropPreviewLayout();
    const double source_scale = layout.value("widthScale").toDouble() / original.width();
    ASSERT_GT(source_scale, 0.0);
    const auto unrotated_url = presenter.inspect()->previewUrl();
    presenter.develop()->previewDevelopNumber("straighten", 22.0);
    ASSERT_TRUE(wait_until(
        [&] { return ready(presenter) && presenter.inspect()->previewUrl() != unrotated_url; }));
    const auto rotated = presenter.inspect()->previewImage();
    const auto rotated_layout = presenter.inspect()->cropPreviewLayout();
    // The full rotated source expands instead of being auto-cropped and enlarged.
    EXPECT_GT(rotated.width(), original.width());
    EXPECT_GT(rotated.height(), original.height());
    EXPECT_NEAR(rotated_layout.value("widthScale").toDouble() / rotated.width(), source_scale,
                1e-9);
    EXPECT_LT(rotated_layout.value("width").toDouble(), 1.0);
    EXPECT_LT(rotated_layout.value("height").toDouble(), 1.0);
    presenter.develop()->setDevelopNumber("straighten", 22.0);
    ASSERT_TRUE(wait_until([&] { return ready(presenter); }));
    presenter.develop()->setCropToolActive(false);
    ASSERT_TRUE(wait_until([&] { return ready(presenter); }));
    EXPECT_NE(presenter.inspect()->previewImage().size(), original.size());
    presenter.develop()->setCropToolActive(true);
    ASSERT_TRUE(
        wait_until([&] { return presenter.develop()->cropGuideReady() && ready(presenter); }));
    EXPECT_EQ(presenter.inspect()->previewImage(), rotated);
    EXPECT_EQ(presenter.inspect()->cropPreviewLayout(), rotated_layout);
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
    presenter.imports()->importFilePaths({existing});
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.imports()->importWorkActive() && presenter.visibleCount() == 1; }));
    presenter.setBrowseMode("develop");
    ASSERT_TRUE(wait_until([&] { return ready(presenter); }));
    const auto selected = presenter.selectedAssetId().toStdString();
    auto imported = testing::StudioPipelineTestControl::importPhoto(presenter, other.toStdString());
    ASSERT_TRUE(imported);
    ASSERT_EQ(imported.value().status, ImportItemStatus::kImported);
    presenter.develop()->setDevelopNumber("exposure", 0.5);
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
