#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <QElapsedTimer>
#include <QImage>
#include <QEventLoop>
#include <QTimer>
#include <QUrl>

#include "ravo/desktop/studio_presenter.h"
#include "ravo/foundation/log.h"

#include "interactive_perf_report.h"
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

} // namespace ravo
