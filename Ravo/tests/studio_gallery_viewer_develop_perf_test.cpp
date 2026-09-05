#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <QElapsedTimer>
#include <QColor>
#include <QColorSpace>
#include <QImage>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/domain/types.h"
#include "ravo/foundation/log.h"

#include "interactive_perf_report.h"
#if defined(Q_OS_MACOS)
#include "studio_iosurface_snapshot.h"
#endif
#include "studio_test_support.h"

namespace ravo
{
namespace
{
using namespace studio_test_support;
using interactive_perf_report::CaseMeta;
using interactive_perf_report::emit_case;
using interactive_perf_report::recorded_samples_from_env;
using interactive_perf_report::warmups_from_env;

[[nodiscard]] bool preview_settled(StudioPresenter &presenter)
{
    if (presenter.previewLoading() || presenter.busy())
        return false;
    if (presenter.previewUrl().isLocalFile())
        return true;
    return presenter.previewUrl().scheme() == QLatin1String("image") &&
           !presenter.previewImage().isNull();
}

[[nodiscard]] std::optional<std::int64_t> measure_until(StudioPresenter &presenter,
                                                        const std::function<void()> &start,
                                                        const std::function<bool()> &ready,
                                                        int timeout_ms = 30000)
{
    QElapsedTimer timer;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    std::optional<std::int64_t> elapsed_us;
    const auto maybe_finish = [&]()
    {
        if (!elapsed_us.has_value() && ready())
        {
            elapsed_us = timer.nsecsElapsed() / 1000;
            loop.quit();
        }
    };
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    const auto c1 =
        QObject::connect(&presenter, &StudioPresenter::previewChanged, &presenter, maybe_finish);
    const auto c2 =
        QObject::connect(&presenter, &StudioPresenter::browseModeChanged, &presenter, maybe_finish);
    const auto c3 =
        QObject::connect(&presenter, &StudioPresenter::selectionChanged, &presenter, maybe_finish);
    const auto c4 =
        QObject::connect(&presenter, &StudioPresenter::busyChanged, &presenter, maybe_finish);
    timer.start();
    start();
    maybe_finish();
    if (!elapsed_us.has_value())
    {
        timeout.start(timeout_ms);
        loop.exec();
    }
    QObject::disconnect(c1);
    QObject::disconnect(c2);
    QObject::disconnect(c3);
    QObject::disconnect(c4);
    return elapsed_us;
}

[[nodiscard]] std::string infer_source_kind(StudioPresenter &presenter)
{
    const auto media = presenter.selectedMediaType().toStdString();
    if (media.find("raw") != std::string::npos || media.find("dng") != std::string::npos ||
        media.find("arw") != std::string::npos)
        return "raw";
    if (!media.empty())
        return "raster";
    return "unknown";
}

} // namespace

#if defined(Q_OS_MACOS)
TEST(StudioGpuPreviewSnapshotTest, RejectsAnInvalidSurface)
{
    auto snapshot = studio_metal::snapshot_iosurface_rgb8(0U, 64U, 48U);
    ASSERT_FALSE(snapshot);
    EXPECT_EQ(snapshot.error().code, ErrorCode::kValidation);
    EXPECT_EQ(snapshot.error().context.at("reason"), "invalid_gpu_display_surface");
}
#endif

TEST(StudioPresenterTest, DevelopFirstFrameWaitsForSelectedRecipePublication)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString photo = directory.filePath(QStringLiteral("loaded-recipe.png"));
    QImage image(64, 48, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(40, 60, 80));
    ASSERT_TRUE(image.save(photo, "PNG"));
    const QString catalog = directory.filePath(QStringLiteral("library.sqlite"));
    QString asset_id;

    {
        StudioPresenter setup;
        setup.createCatalogFromPath(catalog);
        ASSERT_TRUE(wait_until([&] { return setup.catalogOpen() && !setup.busy(); }))
            << setup.errorText().toStdString();
        setup.importFilePaths({photo});
        ASSERT_TRUE(wait_until(
            [&]
            {
                return setup.visibleCount() == 1 && !setup.selectedAssetId().isEmpty() &&
                       !setup.busy();
            }))
            << setup.errorText().toStdString();
        asset_id = setup.selectedAssetId();
        setup.openDevelop();
        ASSERT_TRUE(
            wait_until([&] { return !setup.previewLoading() && setup.previewUrl().isLocalFile(); }))
            << setup.errorText().toStdString();
        setup.setDevelopNumber(QStringLiteral("exposure"), 1.0);
        ASSERT_TRUE(wait_until(
            [&]
            {
                return !setup.previewLoading() && setup.previewUrl().isLocalFile() &&
                       std::abs(setup.editExposure() - 1.0) < 1.0e-9;
            }))
            << setup.errorText().toStdString();
    }

    StudioPresenter presenter;
    presenter.openCatalogFromPath(catalog);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.catalogOpen() && !presenter.busy() &&
                   presenter.selectedAssetId() == asset_id;
        }))
        << presenter.errorText().toStdString();

    std::optional<QColor> first_live_center;
    QObject::connect(&presenter, &StudioPresenter::previewChanged, &presenter,
                     [&]
                     {
                         const QUrl url = presenter.previewUrl();
                         const QImage frame = presenter.previewImage();
                         if (!first_live_center && url.scheme() == QLatin1String("image") &&
                             url.path() == QLatin1String("/live") && !frame.isNull())
                         {
                             first_live_center =
                                 frame.pixelColor(frame.width() / 2, frame.height() / 2);
                         }
                     });
    presenter.openDevelop();
    ASSERT_TRUE(wait_until([&] { return first_live_center.has_value(); }))
        << presenter.errorText().toStdString();
#if defined(Q_OS_MACOS)
    EXPECT_GT(presenter.gpuPreviewGeneration(), 0U);
#endif
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.previewLoading() && presenter.previewUrl().isLocalFile(); }))
        << presenter.errorText().toStdString();
    ASSERT_NEAR(presenter.editExposure(), 1.0, 1.0e-9);
    const QImage settled = presenter.previewImage();
    ASSERT_FALSE(settled.isNull());
    const QColor settled_center = settled.pixelColor(settled.width() / 2, settled.height() / 2);
    EXPECT_NEAR(first_live_center->red(), settled_center.red(), 1);
    EXPECT_NEAR(first_live_center->green(), settled_center.green(), 1);
    EXPECT_NEAR(first_live_center->blue(), settled_center.blue(), 1);
}

TEST(StudioGalleryViewerDevelopPerformanceProbe, MeasuresGallerySelectLoupeDevelopLatency)
{
    const char *catalog_path = std::getenv("RAVO_INTERACTIVE_PERF_CATALOG");
    const char *asset_id_env = std::getenv("RAVO_INTERACTIVE_PERF_ASSET_ID");
    if (catalog_path == nullptr || asset_id_env == nullptr)
    {
        GTEST_SKIP() << "set RAVO_INTERACTIVE_PERF_CATALOG and RAVO_INTERACTIVE_PERF_ASSET_ID";
    }

    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");

    const std::size_t warmups = warmups_from_env();
    const std::size_t recorded = recorded_samples_from_env();
    ASSERT_GT(recorded, 0U);

    StudioPresenter presenter;
    presenter.openCatalogFromPath(QString::fromUtf8(catalog_path));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }, 30000))
        << presenter.errorText().toStdString();
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.libraryTotal() > 0 && presenter.assets() != nullptr &&
                   presenter.assets()->rowCount() > 0;
        },
        30000))
        << "library did not materialize assets";

    const QString primary = QString::fromUtf8(asset_id_env);
    ASSERT_FALSE(primary.isEmpty());
    QString adjacent = primary;
    if (presenter.assets() != nullptr && presenter.assets()->rowCount() > 1)
    {
        for (int row = 0; row < presenter.assets()->rowCount(); ++row)
        {
            const auto id = presenter.assets()->assetIdAt(row);
            if (!id.isEmpty() && id != primary)
            {
                adjacent = id;
                break;
            }
        }
    }

    presenter.setBrowseMode(QStringLiteral("grid"));
    ASSERT_TRUE(wait_until([&] { return presenter.browseMode() == QLatin1String("grid"); }));

    auto run_case = [&](const char *case_id,
                        const std::function<std::optional<std::int64_t>()> &once,
                        const std::string &cache_state)
    {
        for (std::size_t i = 0; i < warmups; ++i)
            ASSERT_TRUE(once().has_value())
                << case_id << " warmup failed: " << presenter.errorText().toStdString();
        std::vector<std::int64_t> samples;
        samples.reserve(recorded);
        for (std::size_t i = 0; i < recorded; ++i)
        {
            auto sample = once();
            ASSERT_TRUE(sample.has_value())
                << case_id << " sample failed: " << presenter.errorText().toStdString();
            samples.push_back(*sample);
        }
        CaseMeta meta;
        meta.case_id = case_id;
        meta.path = "gallery_viewer_develop";
        meta.unit = "us";
        meta.cache_state = cache_state;
        meta.source_kind = infer_source_kind(presenter);
        meta.file_count = presenter.assets() != nullptr ?
                              static_cast<std::size_t>(presenter.assets()->rowCount()) :
                              0U;
        meta.warmups = warmups;
        meta.recorded_samples = recorded;
        meta.asset_id = primary.toStdString();
        meta.catalog_path = catalog_path;
        emit_case(meta, samples);
        if (const char *budget = std::getenv("RAVO_GALLERY_VIEWER_DEVELOP_P90_BUDGET_MS"))
        {
            const auto summary = interactive_perf_report::summarize(samples);
            EXPECT_LE(summary.p90, std::stoll(budget) * 1000) << case_id;
        }
    };

    // Cold first gallery→loupe select (single sample after catalog open; still schema-emitted).
    {
        presenter.setBrowseMode(QStringLiteral("grid"));
        auto cold = measure_until(
            presenter,
            [&]
            {
                presenter.selectAsset(primary);
                presenter.setBrowseMode(QStringLiteral("loupe"));
            },
            [&]
            {
                return presenter.selectedAssetId() == primary &&
                       presenter.browseMode() == QLatin1String("loupe") &&
                       preview_settled(presenter);
            });
        ASSERT_TRUE(cold.has_value()) << presenter.errorText().toStdString();
        CaseMeta meta;
        meta.case_id = "gallery_select_to_loupe_cold";
        meta.path = "gallery_viewer_develop";
        meta.unit = "us";
        meta.cache_state = "cold";
        meta.source_kind = infer_source_kind(presenter);
        meta.file_count = presenter.assets() != nullptr ?
                              static_cast<std::size_t>(presenter.assets()->rowCount()) :
                              0U;
        meta.warmups = 0;
        meta.recorded_samples = 1;
        meta.asset_id = primary.toStdString();
        meta.catalog_path = catalog_path;
        emit_case(meta, {*cold});
    }

    run_case(
        "gallery_select_to_loupe",
        [&]() -> std::optional<std::int64_t>
        {
            presenter.setBrowseMode(QStringLiteral("grid"));
            return measure_until(
                presenter,
                [&]
                {
                    presenter.selectAsset(primary);
                    presenter.setBrowseMode(QStringLiteral("loupe"));
                },
                [&]
                {
                    return presenter.selectedAssetId() == primary &&
                           presenter.browseMode() == QLatin1String("loupe") &&
                           preview_settled(presenter);
                });
        },
        "warm");

    if (adjacent != primary)
    {
        run_case(
            "adjacent_photo_revisit",
            [&]() -> std::optional<std::int64_t>
            {
                presenter.selectAsset(adjacent);
                if (!wait_until(
                        [&]
                        {
                            return presenter.selectedAssetId() == adjacent &&
                                   preview_settled(presenter);
                        },
                        30000))
                {
                    return std::nullopt;
                }
                return measure_until(
                    presenter, [&] { presenter.selectAsset(primary); },
                    [&]
                    {
                        return presenter.selectedAssetId() == primary && preview_settled(presenter);
                    });
            },
            "warm");
    }

    run_case(
        "loupe_to_develop_first_frame",
        [&]() -> std::optional<std::int64_t>
        {
            presenter.setBrowseMode(QStringLiteral("loupe"));
            if (!wait_until(
                    [&]
                    {
                        return presenter.browseMode() == QLatin1String("loupe") &&
                               preview_settled(presenter);
                    },
                    30000))
            {
                return std::nullopt;
            }
            // Require a *new* owned live interactive publication after the mode
            // switch. A leftover image://live URL from a prior Develop sample
            // must not satisfy the gate immediately.
            const QUrl previous = presenter.previewUrl();
            return measure_until(
                presenter, [&] { presenter.setBrowseMode(QStringLiteral("develop")); },
                [&]
                {
                    if (presenter.browseMode() != QLatin1String("develop"))
                        return false;
                    const QUrl current = presenter.previewUrl();
                    return current != previous && current.scheme() == QLatin1String("image") &&
                           current.path() == QLatin1String("/live") &&
                           !presenter.previewImage().isNull();
                });
        },
        "warm");
}

TEST(StudioRapidRawTonePerformanceProbe, MeasuresAllToneControls)
{
    const char *catalog_path = std::getenv("RAVO_INTERACTIVE_PERF_CATALOG");
    const char *asset_id = std::getenv("RAVO_INTERACTIVE_PERF_ASSET_ID");
    if (catalog_path == nullptr || asset_id == nullptr)
        GTEST_SKIP() << "set RAVO_INTERACTIVE_PERF_CATALOG and RAVO_INTERACTIVE_PERF_ASSET_ID";

    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    const std::size_t runs = recorded_samples_from_env(15U);
    ASSERT_GT(runs, 0U);
    StudioPresenter presenter;
    presenter.openCatalogFromPath(QString::fromUtf8(catalog_path));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }, 30000))
        << presenter.errorText().toStdString();
    presenter.selectAsset(QString::fromUtf8(asset_id));
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.selectedAssetId() == QString::fromUtf8(asset_id) &&
                   !presenter.busy();
        },
        30000));
    presenter.setBrowseMode(QStringLiteral("develop"));
    ASSERT_TRUE(wait_until([&] { return preview_settled(presenter); }, 30000));
    ASSERT_TRUE(presenter.editRapidRawToneControlsEnabled());

    const std::array<QString, 7> fields{
        QStringLiteral("rapidrawEvShift"),   QStringLiteral("rapidrawExposure"),
        QStringLiteral("rapidrawContrast"),  QStringLiteral("rapidrawHighlights"),
        QStringLiteral("rapidrawShadows"),   QStringLiteral("rapidrawWhites"),
        QStringLiteral("rapidrawBlacks"),
    };
    const std::array<double, 7> baselines{
        presenter.editRapidRawEvShift(),   presenter.editRapidRawExposure(),
        presenter.editRapidRawContrast(),  presenter.editRapidRawHighlights(),
        presenter.editRapidRawShadows(),   presenter.editRapidRawWhites(),
        presenter.editRapidRawBlacks(),
    };
    std::vector<std::int64_t> samples;
    samples.reserve(runs);
    for (std::size_t run = 0U; run < runs; ++run)
    {
        const std::size_t index = run % fields.size();
        const std::size_t cycle = run / fields.size();
        const double direction = cycle % 2U == 0U ? -1.0 : 1.0;
        const double magnitude = static_cast<double>(cycle / 2U + 1U);
        const double delta = direction * magnitude * (index < 2U ? 0.01 : 1.0);
        const QUrl previous = presenter.previewUrl();
        auto elapsed = measure_until(
            presenter,
            [&] { presenter.previewDevelopNumber(fields[index], baselines[index] + delta); },
            [&]
            {
                const QUrl current = presenter.previewUrl();
                return current != previous && current.scheme() == QLatin1String("image") &&
                       current.path() == QLatin1String("/live") &&
                       !presenter.previewImage().isNull();
            },
            5000);
        ASSERT_TRUE(elapsed.has_value()) << presenter.errorText().toStdString();
        samples.push_back(*elapsed);
    }
    CaseMeta meta;
    meta.case_id = "rapidraw_all_tone_controls";
    meta.path = "gallery_viewer_develop";
    meta.unit = "us";
    meta.cache_state = "warm";
    meta.source_kind = "raw";
    meta.warmups = 0U;
    meta.recorded_samples = runs;
    meta.asset_id = asset_id;
    meta.catalog_path = catalog_path;
    emit_case(meta, samples);
    if (const char *budget = std::getenv("RAVO_INTERACTIVE_PERF_P90_BUDGET_MS"))
        EXPECT_LE(interactive_perf_report::summarize(samples).p90, std::stoll(budget) * 1000);
}

TEST(StudioGalleryViewerDevelopPerformanceProbe, LargeLibrarySyntheticPageObservation)
{
    // PERF-01 observation expand only — synthetic N assets, no budget admit / no PERF-02.
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-perf01-large-library");
    std::size_t n = 1000U;
    if (const char *raw = std::getenv("RAVO_PERF01_LARGE_LIBRARY_N"))
    {
        const auto parsed = std::strtoull(raw, nullptr, 10);
        if (parsed >= 200ULL && parsed <= 20000ULL)
            n = static_cast<std::size_t>(parsed);
    }

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalog = directory.filePath(QStringLiteral("large-library.sqlite"));
    auto repository = SqliteCatalogRepository::create(catalog.toStdString());
    ASSERT_TRUE(repository) << repository.error().message;
    for (std::size_t index = 0; index < n; ++index)
    {
        AssetRecord asset;
        asset.id = "ast_perf01_" + std::to_string(index);
        asset.normalized_uri = "file:///synthetic/perf01/photo-" + std::to_string(index) + ".jpg";
        asset.media_type = "image/jpeg";
        asset.size_bytes = static_cast<std::uint64_t>(1000U + index);
        asset.mtime_unix_ms = static_cast<std::int64_t>(20'000U + index);
        asset.width = 64U;
        asset.height = 48U;
        asset.created_unix_ms = static_cast<std::int64_t>(30'000U + index);
        ASSERT_TRUE(repository.value()->commit_imported_asset(asset)) << index;
    }
    ASSERT_TRUE(repository.value()->close());
    repository.value().reset();

    const std::size_t warmups = warmups_from_env();
    const std::size_t recorded = recorded_samples_from_env();
    std::vector<std::int64_t> samples;
    samples.reserve(recorded);
    for (std::size_t i = 0; i < warmups + recorded; ++i)
    {
        auto open = SqliteCatalogRepository::open(catalog.toStdString());
        ASSERT_TRUE(open) << open.error().message;
        LibraryPageRequest request;
        request.limit = kLibraryPageDefaultSize;
        QElapsedTimer timer;
        timer.start();
        auto page = open.value()->list_assets_page(request);
        const auto elapsed_us = timer.nsecsElapsed() / 1000;
        ASSERT_TRUE(page) << page.error().message;
        EXPECT_EQ(page.value().total, n);
        EXPECT_LE(page.value().materialized_rows, kLibraryPageDefaultSize);
        ASSERT_TRUE(open.value()->close());
        if (i >= warmups)
            samples.push_back(elapsed_us);
    }
    ASSERT_FALSE(samples.empty());
    CaseMeta meta;
    meta.case_id = "large_library_page_first";
    meta.path = "large_library";
    meta.unit = "us";
    meta.cache_state = "warm";
    meta.source_kind = "synthetic_metadata";
    meta.file_count = n;
    meta.warmups = warmups;
    meta.recorded_samples = recorded;
    meta.catalog_path = catalog.toStdString();
    emit_case(meta, samples);
    // Observation only: never admit a product budget here.
    EXPECT_GT(samples.front(), 0);
}

} // namespace ravo
