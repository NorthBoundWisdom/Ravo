#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <atomic>
#include <future>
#include <chrono>
#include <cstdlib>

#include <QColor>
#include <QColorSpace>
#include <QImage>
#include <QSqlDatabase>
#include <QSqlQuery>

#include "ravo/foundation/cancellation.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"
#include "ravo/services/catalog_service.h"
#include "ravo/services/display_presentation.h"

#include "catalog_test_support.h"
#include "interactive_perf_report.h"

namespace ravo
{
namespace
{

TEST(DisplayPresentationResourceProbe, MeasuresConcurrentIccWorkersAndTailLatency)
{
    if (!std::getenv("RAVO_DISPLAY_RESOURCE_PROBE"))
        GTEST_SKIP() << "set RAVO_DISPLAY_RESOURCE_PROBE=1 for the measure-only ICC workload";
    struct Observer final : DisplayConversionObserver
    {
        std::atomic<std::int64_t> active{0}, peak{0}, created{0};
        void worker_started(bool spawned) noexcept override
        {
            if (spawned)
                ++created;
            const auto count = ++active;
            auto previous = peak.load();
            while (previous < count && !peak.compare_exchange_weak(previous, count))
            {
            }
        }
        void worker_stopped() noexcept override
        {
            --active;
        }
    };
    auto source = make_srgb_color_profile();
    ASSERT_TRUE(source);
    DisplayPresentationState monitor;
    monitor.valid = true;
    monitor.source = DisplayProfileSource::kInjectedPath;
    monitor.monitor_profile.kind = ColorProfileKind::kIcc;
    const auto bytes = QColorSpace(QColorSpace::AdobeRgb).iccProfile();
    monitor.monitor_profile.icc_bytes.assign(bytes.cbegin(), bytes.cend());
    monitor.profile_fingerprint = color_profile_fingerprint(monitor.monitor_profile);
    std::vector<std::uint8_t> pixels(1024U * 768U * 3U, 128);
    for (const int concurrency : {1, 2, 4})
    {
        Observer observer;
        std::promise<void> start;
        auto gate = start.get_future().share();
        std::vector<std::future<Result<std::vector<std::int64_t>>>> tasks;
        for (int worker = 0; worker < concurrency; ++worker)
            tasks.push_back(std::async(
                std::launch::async,
                [&]() -> Result<std::vector<std::int64_t>>
                {
                    gate.wait();
                    std::vector<std::int64_t> samples;
                    for (int run = 0; run < 32; ++run)
                    {
                        const auto begun = std::chrono::steady_clock::now();
                        auto output = apply_display_presentation_rgb8(
                            pixels, 1024, 768, source.value(), monitor, {}, &observer);
                        if (!output)
                            return output.error();
                        samples.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now() - begun)
                                              .count());
                    }
                    return samples;
                }));
        start.set_value();
        std::vector<std::int64_t> samples;
        for (auto &task : tasks)
        {
            auto result = task.get();
            ASSERT_TRUE(result) << result.error().message;
            samples.insert(samples.end(), result.value().begin(), result.value().end());
        }
        EXPECT_EQ(observer.active.load(), 0);
        EXPECT_GT(observer.created.load(), 0);
        EXPECT_LE(observer.peak.load(), concurrency * 8);
        interactive_perf_report::CaseMeta meta;
        meta.case_id = "icc_concurrent_" + std::to_string(concurrency);
        meta.path = "display_icc_resources";
        meta.source_kind = "synthetic_rgb8_srgb_to_adobe_rgb";
        meta.warmups = 0;
        meta.recorded_samples = samples.size();
        meta.worker_threads_created = observer.created.load();
        meta.peak_active_participants = observer.peak.load();
        meta.workers = concurrency;
        interactive_perf_report::emit_case(meta, samples);
    }
}

TEST(DisplayPresentationTest, DiscoverEmitsMachineVisibleState)
{
    const auto state = discover_monitor_presentation("primary");
    ASSERT_TRUE(state) << state.error().message;
    EXPECT_TRUE(state.value().valid);
    EXPECT_FALSE(state.value().profile_fingerprint.empty());
    EXPECT_EQ(state.value().contract_version, kDisplayPresentationContractVersion);
    EXPECT_TRUE(state.value().source == DisplayProfileSource::kSystemMonitor ||
                state.value().source == DisplayProfileSource::kFallbackSrgb);
    EXPECT_FALSE(state.value().reason.empty());
}

TEST(DisplayPresentationTest, RecipeAndExportProfileUnchangedWhenPresentationChanges)
{
    DevelopParams develop;
    develop.exposure_ev = 0.35;
    develop.output_color.output_profile = "adobe_rgb";
    develop.output_color.proof_mode = "softproof";
    develop.output_color.proof_profile = "srgb";
    const AssetDescriptor asset{"asset-display", "file:///fixture.raw", std::nullopt};
    auto before_recipe = recipe_from_develop(asset, develop);
    ASSERT_TRUE(before_recipe) << before_recipe.error().message;
    auto before_json = serialize_recipe(before_recipe.value());
    ASSERT_TRUE(before_json) << before_json.error().message;
    const auto before_output = develop.output_color;

    const std::array<float, 9> boost{1.1F, 0.0F, 0.0F, 0.0F, 0.9F, 0.0F, 0.0F, 0.0F, 1.0F};
    auto presentation = make_synthetic_matrix_monitor_presentation(boost, "screen-a");
    ASSERT_TRUE(presentation) << presentation.error().message;
    auto refreshed = refresh_monitor_presentation(presentation.value(), "screen-b");
    ASSERT_TRUE(refreshed) << refreshed.error().message;
    EXPECT_EQ(refreshed.value().screen_token, "screen-b");
    EXPECT_EQ(refreshed.value().profile_fingerprint, presentation.value().profile_fingerprint);

    auto after_recipe = recipe_from_develop(asset, develop);
    ASSERT_TRUE(after_recipe) << after_recipe.error().message;
    auto after_json = serialize_recipe(after_recipe.value());
    ASSERT_TRUE(after_json) << after_json.error().message;
    EXPECT_EQ(after_json.value(), before_json.value());
    EXPECT_EQ(develop.output_color.output_profile, before_output.output_profile);
    EXPECT_EQ(develop.output_color.proof_mode, before_output.proof_mode);
    EXPECT_EQ(develop.output_color.proof_profile, before_output.proof_profile);
    EXPECT_EQ(develop.exposure_ev, 0.35);
}

TEST(DisplayPresentationTest, SyntheticMatrixProducesRepeatableCpuReference)
{
    const std::array<float, 9> swap_rg{0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    auto presentation = make_synthetic_matrix_monitor_presentation(swap_rg, "matrix");
    ASSERT_TRUE(presentation) << presentation.error().message;
    EXPECT_EQ(presentation.value().source, DisplayProfileSource::kSyntheticMatrix);

    const std::vector<std::uint8_t> source{10U, 20U, 30U, 40U, 50U, 60U};
    ColorProfileState source_profile;
    auto once = apply_display_presentation_rgb8(source, 2U, 1U, source_profile,
                                                presentation.value(), CancellationToken{});
    ASSERT_TRUE(once) << once.error().message;
    auto twice = apply_display_presentation_rgb8(source, 2U, 1U, source_profile,
                                                 presentation.value(), CancellationToken{});
    ASSERT_TRUE(twice) << twice.error().message;
    EXPECT_EQ(once.value().rgb8, twice.value().rgb8);
    EXPECT_EQ(once.value().rgb8[0], 20U);
    EXPECT_EQ(once.value().rgb8[1], 10U);
    EXPECT_EQ(once.value().rgb8[2], 30U);
}

TEST(DisplayPresentationTest, SyntheticLutIdentityCopiesPixels)
{
    auto presentation = make_synthetic_lut_monitor_presentation(17U, "lut");
    ASSERT_TRUE(presentation) << presentation.error().message;
    EXPECT_EQ(presentation.value().source, DisplayProfileSource::kSyntheticLut);
    const std::vector<std::uint8_t> source{1U, 2U, 3U};
    ColorProfileState unused;
    auto out = apply_display_presentation_rgb8(source, 1U, 1U, unused, presentation.value(),
                                               CancellationToken{});
    ASSERT_TRUE(out) << out.error().message;
    EXPECT_EQ(out.value().rgb8, source);
}

TEST(DisplayPresentationTest, CorruptInjectedIccFallsBackToSrgbWithVisibleReason)
{
    const auto dir = std::filesystem::temp_directory_path() / "ravo_display_presentation_test";
    std::filesystem::create_directories(dir);
    const auto path = dir / "corrupt.icc";
    {
        std::ofstream out(path, std::ios::binary);
        out << "not-an-icc-profile";
    }
    const auto state = inject_monitor_presentation_from_icc_path(path.string(), "inject");
    ASSERT_TRUE(state) << state.error().message;
    EXPECT_EQ(state.value().source, DisplayProfileSource::kFallbackSrgb);
    EXPECT_FALSE(state.value().reason.empty());
    EXPECT_NE(state.value().reason, "injected_icc_path");
    EXPECT_TRUE(state.value().valid);
}

TEST(DisplayPresentationTest, InjectedValidIccKeepsSourceInjected)
{
    auto baseline = discover_monitor_presentation("seed");
    ASSERT_TRUE(baseline) << baseline.error().message;
    ASSERT_FALSE(baseline.value().monitor_profile.icc_bytes.empty());

    const auto dir = std::filesystem::temp_directory_path() / "ravo_display_presentation_test";
    std::filesystem::create_directories(dir);
    const auto path = dir / "valid.icc";
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(baseline.value().monitor_profile.icc_bytes.data()),
                  static_cast<std::streamsize>(baseline.value().monitor_profile.icc_bytes.size()));
    }
    const auto state = inject_monitor_presentation_from_icc_path(path.string(), "inject");
    ASSERT_TRUE(state) << state.error().message;
    EXPECT_EQ(state.value().source, DisplayProfileSource::kInjectedPath);
    EXPECT_EQ(state.value().reason, "injected_icc_path");

    const std::vector<std::uint8_t> pixels{128U, 64U, 32U};
    auto converted = apply_display_presentation_rgb8(
        pixels, 1U, 1U, baseline.value().monitor_profile, state.value(), CancellationToken{});
    ASSERT_TRUE(converted) << converted.error().message;
    EXPECT_EQ(converted.value().rgb8.size(), 3U);
}

TEST(DisplayPresentationTest, StateJsonExposesMachineVisibleFields)
{
    auto presentation = make_synthetic_lut_monitor_presentation(8U, "json");
    ASSERT_TRUE(presentation) << presentation.error().message;
    const auto json = display_presentation_state_to_json(presentation.value());
    const auto *object = json.object_if();
    ASSERT_NE(object, nullptr);
    const auto *source = json.find("source");
    const auto *screen = json.find("screen_token");
    const auto *valid = json.find("valid");
    ASSERT_NE(source, nullptr);
    ASSERT_NE(screen, nullptr);
    ASSERT_NE(valid, nullptr);
    ASSERT_NE(source->string_if(), nullptr);
    ASSERT_NE(screen->string_if(), nullptr);
    ASSERT_NE(valid->boolean_if(), nullptr);
    EXPECT_EQ(*source->string_if(), "synthetic_lut");
    EXPECT_EQ(*screen->string_if(), "json");
    EXPECT_TRUE(*valid->boolean_if());
}

TEST(DisplayPresentationTest, NonAppleHostDiscoveryIsMachineVisible)
{
#if defined(__APPLE__)
    GTEST_SKIP() << "macOS owns CoreGraphics discovery; Win/Linux covered here";
#else
    auto state = discover_monitor_presentation("primary");
    ASSERT_TRUE(state) << state.error().message;
    EXPECT_TRUE(state.value().valid);
    EXPECT_FALSE(state.value().reason.empty());
#if defined(_WIN32)
    // Best-effort ICM: system_monitor on success, otherwise explicit windows_* fallback.
    EXPECT_TRUE(state.value().source == DisplayProfileSource::kSystemMonitor ||
                state.value().source == DisplayProfileSource::kFallbackSrgb);
    if (state.value().source == DisplayProfileSource::kFallbackSrgb)
    {
        EXPECT_NE(state.value().reason.find("windows_"), std::string::npos) << state.value().reason;
    }
    else
    {
        EXPECT_EQ(state.value().reason, "windows_icm_display_icc");
    }
#elif defined(__linux__)
    EXPECT_EQ(state.value().source, DisplayProfileSource::kFallbackSrgb);
    EXPECT_EQ(state.value().reason, "linux_monitor_discovery_unavailable");
#else
    EXPECT_EQ(state.value().source, DisplayProfileSource::kFallbackSrgb);
    EXPECT_EQ(state.value().reason, "host_monitor_discovery_unavailable");
#endif
#endif
}

TEST(DisplayPresentationTest, MacosCgScreenTokenRoundTrip)
{
    const auto token = make_macos_cg_screen_token(42U);
    EXPECT_EQ(token, "cg:42");
    const auto parsed = parse_macos_cg_screen_token(token);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, 42U);
    EXPECT_FALSE(parse_macos_cg_screen_token("primary").has_value());
    EXPECT_FALSE(parse_macos_cg_screen_token("cg:").has_value());
    EXPECT_FALSE(parse_macos_cg_screen_token("cg:12x").has_value());
}

TEST(DisplayPresentationTest, RefreshSystemPresentationUpdatesScreenTokenWithoutRecipeMutation)
{
    DevelopParams develop;
    develop.exposure_ev = 0.1;
    develop.output_color.output_profile = "srgb";
    const AssetDescriptor asset{"asset-display-refresh", "file:///fixture.raw", std::nullopt};
    auto before_recipe = recipe_from_develop(asset, develop);
    ASSERT_TRUE(before_recipe) << before_recipe.error().message;
    auto before_json = serialize_recipe(before_recipe.value());
    ASSERT_TRUE(before_json) << before_json.error().message;

    auto first = discover_monitor_presentation("primary");
    ASSERT_TRUE(first) << first.error().message;
    auto second = refresh_monitor_presentation(first.value(), "screen-moved");
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_FALSE(second.value().screen_token.empty());
    EXPECT_TRUE(second.value().valid);

    auto after_recipe = recipe_from_develop(asset, develop);
    ASSERT_TRUE(after_recipe) << after_recipe.error().message;
    auto after_json = serialize_recipe(after_recipe.value());
    ASSERT_TRUE(after_json) << after_json.error().message;
    EXPECT_EQ(after_json.value(), before_json.value());
}

#if defined(__APPLE__)
TEST(DisplayPresentationTest, MacosDiscoverHonorsCgScreenToken)
{
    const auto token = macos_display_screen_token_for_point(0.0, 0.0);
    EXPECT_TRUE(token.rfind("cg:", 0) == 0) << token;
    auto state = discover_monitor_presentation(token);
    ASSERT_TRUE(state) << state.error().message;
    EXPECT_TRUE(state.value().valid);
    EXPECT_EQ(state.value().screen_token, token);
    EXPECT_TRUE(state.value().source == DisplayProfileSource::kSystemMonitor ||
                state.value().source == DisplayProfileSource::kFallbackSrgb);
}
#endif

TEST(DisplayPresentationTest, ViewContractsDeclarePixelKindsAndSoftProofOrder)
{
    const auto contracts = display_presentation_view_contracts();
    ASSERT_FALSE(contracts.empty());
    bool saw_loupe = false;
    bool saw_scopes = false;
    bool saw_gallery = false;
    for (const auto &entry : contracts)
    {
        EXPECT_FALSE(entry.view_id.empty());
        EXPECT_EQ(entry.soft_proof_interaction, "after_soft_proof_display_only");
        if (entry.view_id == "loupe_preview" || entry.view_id == "develop_preview" ||
            entry.view_id == "before_after" || entry.view_id == "comparison" ||
            entry.view_id == "magnifier" || entry.view_id == "gallery_thumbnail" ||
            entry.view_id == "gpu_native_preview")
        {
            EXPECT_EQ(entry.pixel_kind, DisplayViewPixelKind::kDisplayTransformed);
            saw_loupe = saw_loupe || entry.view_id == "loupe_preview";
            saw_gallery = saw_gallery || entry.view_id == "gallery_thumbnail";
        }
        if (entry.view_id == "scopes")
        {
            EXPECT_EQ(entry.pixel_kind, DisplayViewPixelKind::kAnalysisDiagnostic);
            saw_scopes = true;
        }
        if (entry.view_id == "gpu_native_preview")
        {
            EXPECT_EQ(entry.pixel_kind, DisplayViewPixelKind::kDisplayTransformed);
        }
    }
    EXPECT_TRUE(saw_loupe);
    EXPECT_TRUE(saw_scopes);
    EXPECT_TRUE(saw_gallery);

    const auto json = display_presentation_view_contracts_to_json();
    const auto *object = json.object_if();
    ASSERT_NE(object, nullptr);
    const auto *views = json.find("views");
    ASSERT_NE(views, nullptr);
    ASSERT_NE(views->array_if(), nullptr);
    EXPECT_EQ(views->array_if()->size(), contracts.size());
    const auto *host = json.find("supported_host_discovery");
    ASSERT_NE(host, nullptr);
    ASSERT_NE(host->string_if(), nullptr);
    EXPECT_FALSE(host->string_if()->empty());
}

TEST(DisplayPresentationTest, MissingInjectedIccFallsBackWithVisibleReason)
{
    const auto dir = std::filesystem::temp_directory_path() / "ravo_display_presentation_test";
    std::filesystem::create_directories(dir);
    const auto path = dir / "missing-display.icc";
    std::filesystem::remove(path);
    const auto state = inject_monitor_presentation_from_icc_path(path.string(), "missing");
    ASSERT_TRUE(state) << state.error().message;
    EXPECT_EQ(state.value().source, DisplayProfileSource::kFallbackSrgb);
    EXPECT_EQ(state.value().reason, "injected_icc_unreadable");
    EXPECT_TRUE(state.value().valid);
}

TEST(DisplayPresentationTest, SoftProofRecipeFieldsSurvivePresentationApply)
{
    DevelopParams develop;
    develop.output_color.output_profile = "adobe_rgb";
    develop.output_color.proof_mode = "softproof";
    develop.output_color.proof_profile = "srgb";
    const AssetDescriptor asset{"asset-softproof", "file:///fixture.raw", std::nullopt};
    auto before_recipe = recipe_from_develop(asset, develop);
    ASSERT_TRUE(before_recipe) << before_recipe.error().message;
    auto before_json = serialize_recipe(before_recipe.value());
    ASSERT_TRUE(before_json) << before_json.error().message;

    auto presentation = make_synthetic_lut_monitor_presentation(17U, "softproof-screen");
    ASSERT_TRUE(presentation) << presentation.error().message;
    const std::vector<std::uint8_t> pixels{10U, 20U, 30U};
    ColorProfileState source;
    auto converted = apply_display_presentation_rgb8(pixels, 1U, 1U, source, presentation.value(),
                                                     CancellationToken{});
    ASSERT_TRUE(converted) << converted.error().message;

    auto after_recipe = recipe_from_develop(asset, develop);
    ASSERT_TRUE(after_recipe) << after_recipe.error().message;
    auto after_json = serialize_recipe(after_recipe.value());
    ASSERT_TRUE(after_json) << after_json.error().message;
    EXPECT_EQ(after_json.value(), before_json.value());
    EXPECT_EQ(develop.output_color.proof_mode, "softproof");
    EXPECT_EQ(develop.output_color.proof_profile, "srgb");
}

[[nodiscard]] std::int64_t read_schema_revision(const std::string &database_path)
{
    const auto connection =
        QStringLiteral("display-presentation-rev-%1").arg(QString::fromStdString(database_path));
    std::int64_t revision = -1;
    {
        if (QSqlDatabase::contains(connection))
            QSqlDatabase::removeDatabase(connection);
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(QString::fromStdString(database_path));
        if (!db.open())
            return -1;
        {
            QSqlQuery query(db);
            if (query.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) &&
                query.next())
            {
                revision = query.value(0).toLongLong();
            }
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return revision;
}

TEST_F(CatalogServiceTest, DisplayPresentationRefreshLeavesHistoryRevisionAndExportBytes)
{
    ASSERT_TRUE(open_service(true));
    const auto jpeg_path = (root / "display-contract.jpg").string();
    QImage image(32, 24, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(12, 34, 56));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import().import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;

    DevelopParams develop;
    develop.exposure_ev = 0.15;
    develop.output_color.output_profile = "srgb";
    develop.output_color.proof_mode = "softproof";
    develop.output_color.proof_profile = "adobe_rgb";
    auto saved = service->develop().save_develop_with_history(asset_id, develop);
    ASSERT_TRUE(saved) << saved.error().message;
    const auto revision_before = read_schema_revision(database_path);
    ASSERT_GE(revision_before, 0);

    auto history_before = service->develop().list_recipe_history(asset_id);
    ASSERT_TRUE(history_before) << history_before.error().message;
    ASSERT_FALSE(history_before.value().empty());
    const auto history_id = history_before.value().front().id;
    const auto history_json = history_before.value().front().recipe_json;

    auto recipe_before = service->develop().load_recipe(asset_id);
    ASSERT_TRUE(recipe_before) << recipe_before.error().message;
    auto recipe_json_before = serialize_recipe(recipe_before.value());
    ASSERT_TRUE(recipe_json_before) << recipe_json_before.error().message;

    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = (root / "display-before.png").string();
    export_request.format = ExportFormat::kPng;
    auto exported_before = service->exports().export_asset(export_request);
    ASSERT_TRUE(exported_before) << exported_before.error().message;
    const auto export_sha_before = file_sha256(export_request.output_path);
    ASSERT_FALSE(export_sha_before.isEmpty());

    auto first = discover_monitor_presentation("primary");
    ASSERT_TRUE(first) << first.error().message;
    auto second = refresh_monitor_presentation(first.value(), "screen-moved");
    ASSERT_TRUE(second) << second.error().message;
    const std::array<float, 9> boost{1.2F, 0.0F, 0.0F, 0.0F, 0.8F, 0.0F, 0.0F, 0.0F, 1.0F};
    auto synthetic = make_synthetic_matrix_monitor_presentation(boost, "inject-screen");
    ASSERT_TRUE(synthetic) << synthetic.error().message;
    const auto dir = root / "display-icc";
    std::filesystem::create_directories(dir);
    const auto icc_path = dir / "seed.icc";
    {
        std::ofstream out(icc_path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(first.value().monitor_profile.icc_bytes.data()),
                  static_cast<std::streamsize>(first.value().monitor_profile.icc_bytes.size()));
    }
    auto injected = inject_monitor_presentation_from_icc_path(icc_path.string(), "inject");
    ASSERT_TRUE(injected) << injected.error().message;

    EXPECT_EQ(read_schema_revision(database_path), revision_before);
    auto history_after = service->develop().list_recipe_history(asset_id);
    ASSERT_TRUE(history_after) << history_after.error().message;
    ASSERT_EQ(history_after.value().size(), history_before.value().size());
    EXPECT_EQ(history_after.value().front().id, history_id);
    EXPECT_EQ(history_after.value().front().recipe_json, history_json);

    auto recipe_after = service->develop().load_recipe(asset_id);
    ASSERT_TRUE(recipe_after) << recipe_after.error().message;
    auto recipe_json_after = serialize_recipe(recipe_after.value());
    ASSERT_TRUE(recipe_json_after) << recipe_json_after.error().message;
    EXPECT_EQ(recipe_json_after.value(), recipe_json_before.value());

    ExportRequest export_again;
    export_again.asset_id = asset_id;
    export_again.output_path = (root / "display-after.png").string();
    export_again.format = ExportFormat::kPng;
    auto exported_after = service->exports().export_asset(export_again);
    ASSERT_TRUE(exported_after) << exported_after.error().message;
    const auto export_sha_after = file_sha256(export_again.output_path);
    EXPECT_EQ(export_sha_after, export_sha_before);
}

} // namespace
} // namespace ravo
