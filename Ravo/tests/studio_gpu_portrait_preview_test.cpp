#include <algorithm>
#include <cstdint>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <gtest/gtest.h>

#include "ravo/engine/engine.h"
#include "ravo/foundation/log.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/recipe/develop.h"
#include "studio_iosurface_snapshot.h"
#include "studio_test_support.h"
#include "studio_preview_handoff.h"

namespace ravo
{
namespace
{
using namespace studio_test_support;

TEST(StudioGpuPreviewTest, HandoffPixelsSurviveSurfaceReuseAndDelayedDelivery)
{
#if defined(Q_OS_MACOS)
    ensure_qt_core();
    for (const auto size : {QSize(639, 960), QSize(1066, 1600)})
    {
        const auto width = static_cast<std::uint32_t>(size.width());
        const auto height = static_cast<std::uint32_t>(size.height());
        auto surface = studio_metal::create_iosurface_rgba8(width, height);
        ASSERT_TRUE(surface) << surface.error().message;
        auto cleanup = qScopeGuard([&] { studio_metal::release_iosurface(surface.value()); });
        QImage image(size, QImage::Format_RGB888);
        for (int y = 0; y < size.height(); ++y)
            for (int x = 0; x < size.width(); ++x)
                image.setPixelColor(x, y, QColor(x % 251, y % 241, (x + y) % 239));
        ASSERT_TRUE(studio_metal::write_rgb8_to_iosurface(surface.value(), image));
        PreviewResult preview;
        preview.width = preview.gpu_display_width = width;
        preview.height = preview.gpu_display_height = height;
        preview.gpu_display_generation = 1;
        preview.gpu_display_native_surface = surface.value();
        ASSERT_TRUE(own_preview_pixels_for_handoff(preview, {}));
        EXPECT_EQ(preview.gpu_display_native_surface, 0U);
        EXPECT_FALSE(preview.color_profile.icc_bytes.empty());
        QImage replacement(size, QImage::Format_RGB888);
        replacement.fill(Qt::magenta);
        ASSERT_TRUE(studio_metal::write_rgb8_to_iosurface(surface.value(), replacement));
        for (int y = 0; y < image.height(); ++y)
            EXPECT_TRUE(std::equal(image.constScanLine(y),
                                   image.constScanLine(y) + image.width() * 3,
                                   preview.rgb.data() + static_cast<std::size_t>(y) * width * 3U));
    }
#else
    GTEST_SKIP() << "Native IOSurface publication is macOS-only";
#endif
}

TEST(StudioGpuPreviewTest, HandoffRejectsCancelledMismatchedAndInvalidSurfaces)
{
    PreviewResult preview;
    preview.gpu_display_generation = 1;
    preview.width = preview.gpu_display_width = 10;
    preview.height = preview.gpu_display_height = 10;
    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel());
    auto cancelled = own_preview_pixels_for_handoff(preview, cancellation.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    preview.gpu_display_width = 11;
    auto mismatch = own_preview_pixels_for_handoff(preview, {});
    ASSERT_FALSE(mismatch);
    EXPECT_EQ(mismatch.error().context.at("reason"), "invalid_gpu_preview_handoff");
    preview.gpu_display_width = 10;
    auto missing = own_preview_pixels_for_handoff(preview, {});
    ASSERT_FALSE(missing);
    EXPECT_TRUE(preview.rgb.empty());
}

TEST(StudioGpuPreviewTest, PortraitRawNativePublicationMatchesCpuGold)
{
#if defined(Q_OS_MACOS)
    ensure_qt_core();
    const auto input = qEnvironmentVariable("RAVO_TEST_GPU_RAW");
    if (input.isEmpty())
        GTEST_SKIP() << "Set RAVO_TEST_GPU_RAW to an explicit RAW source";
    QFile source(input);
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    const auto original_hash =
        QCryptographicHash::hash(source.readAll(), QCryptographicHash::Sha256);
    source.close();
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    ASSERT_EQ(engine.value().gpu_backend(), "metal");
    auto decoded = engine.value().decode_raw_frame(input.toStdString(), {});
    ASSERT_TRUE(decoded) << decoded.error().message;
    auto recipe = recipe_from_develop({"portrait-probe", input.toStdString(), std::nullopt},
                                      develop_raw_import_baseline());
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto working =
        engine.value().linear_working_from_raw(decoded.value(), recipe.value(), 1066U, 1600U, {});
    ASSERT_TRUE(working) << working.error().message;
    for (auto &operation : recipe.value().operations)
        if (operation.id == "ravo.raw.highlights")
            operation.enabled = false;
    auto cpu = engine.value().render_linear_working(working.value(), recipe.value(), {});
    ASSERT_TRUE(cpu) << cpu.error().message;
    InteractivePreviewRenderCache cache;
    auto gpu = engine.value().render_interactive_linear_working(working.value(), recipe.value(),
                                                                cache, {}, std::nullopt, false);
    ASSERT_TRUE(gpu) << gpu.error().message;
    ASSERT_TRUE(gpu.value().rgb.empty());
    const auto frame = engine.value().gpu_display_frame();
    auto snapshot =
        studio_metal::snapshot_iosurface_rgb8(frame.native_surface, frame.width, frame.height);
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    ASSERT_EQ(frame.width, cpu.value().width);
    ASSERT_EQ(frame.height, cpu.value().height);
    std::uint32_t maximum = 0;
    std::uint64_t sum = 0;
    for (std::uint32_t y = 0; y < frame.height; ++y)
    {
        const auto *row = snapshot.value().constScanLine(static_cast<int>(y));
        for (std::uint32_t x = 0; x < frame.width * 3U; ++x)
        {
            const auto index = static_cast<std::size_t>(y) * frame.width * 3U + x;
            const auto difference = static_cast<std::uint32_t>(
                std::abs(static_cast<int>(row[x]) - cpu.value().rgb[index]));
            maximum = std::max(maximum, difference);
            sum += difference;
        }
    }
    const auto output = qEnvironmentVariable("RAVO_TEST_GPU_OUTPUT_DIRECTORY");
    if (!output.isEmpty())
    {
        for (const auto &entry :
             {std::pair{"cpu.png", &cpu.value()}, std::pair{"gpu.png", &gpu.value()}})
        {
            RenderedImage image = cpu.value();
            if (entry.second == &gpu.value())
            {
                image.rgb.resize(cpu.value().rgb.size());
                for (std::uint32_t y = 0; y < image.height; ++y)
                    std::copy_n(snapshot.value().constScanLine(static_cast<int>(y)),
                                image.width * 3U,
                                image.rgb.data() + static_cast<std::size_t>(y) * image.width * 3U);
            }
            auto encoded = engine.value().encode_png(image);
            ASSERT_TRUE(encoded) << encoded.error().message;
            QFile file(QDir(output).filePath(entry.first));
            ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
            ASSERT_EQ(file.write(reinterpret_cast<const char *>(encoded.value().data()),
                                 static_cast<qint64>(encoded.value().size())),
                      static_cast<qint64>(encoded.value().size()));
        }
    }
    EXPECT_LE(maximum, 2U) << "mean packed delta="
                           << static_cast<double>(sum) /
                                  static_cast<double>(cpu.value().rgb.size());
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    EXPECT_EQ(QCryptographicHash::hash(source.readAll(), QCryptographicHash::Sha256),
              original_hash);
#else
    GTEST_SKIP() << "Native IOSurface publication is macOS-only";
#endif
}

TEST(StudioGpuPreviewTest, PortraitRawLoupePublishesOwnedPixels)
{
    ensure_qt_core();
    init_logging("ravo-portrait-preview-tests");
    const auto input = qEnvironmentVariable("RAVO_TEST_GPU_RAW");
    if (input.isEmpty())
        GTEST_SKIP() << "Set RAVO_TEST_GPU_RAW to an explicit RAW source";
    QTemporaryDir directory;
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath("catalog.sqlite"));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.imports()->importFilePaths({input});
    ASSERT_TRUE(
        wait_until([&] { return presenter.visibleCount() == 1 && !presenter.busy(); }, 30000));
    presenter.setBrowseMode("loupe");
    presenter.selectAsset(presenter.assets()->assetIdAt(0));
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.inspect()->previewLoading() &&
                   !presenter.inspect()->previewImage().isNull();
        },
        30000))
        << presenter.errorText().toStdString();
    const auto output = qEnvironmentVariable("RAVO_TEST_GPU_OUTPUT_DIRECTORY");
    if (!output.isEmpty())
        ASSERT_TRUE(presenter.inspect()->previewImage().save(QDir(output).filePath("loupe.png")));
    EXPECT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();
#if defined(Q_OS_MACOS)
    if (presenter.inspect()->gpuPreviewGeneration() > 0)
    {
        auto snapshot = studio_metal::snapshot_iosurface_rgb8(
            presenter.inspect()->gpuPreviewNativeSurface(),
            static_cast<std::uint32_t>(presenter.inspect()->gpuPreviewWidth()),
            static_cast<std::uint32_t>(presenter.inspect()->gpuPreviewHeight()));
        ASSERT_TRUE(snapshot) << snapshot.error().message;
        EXPECT_EQ(snapshot.value(),
                  presenter.inspect()->previewImage().convertToFormat(QImage::Format_RGB888));
    }
#endif
}
} // namespace
} // namespace ravo
