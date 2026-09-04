#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ravo/foundation/cancellation.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"
#include "ravo/services/display_presentation.h"

namespace ravo
{
namespace
{

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

TEST(DisplayPresentationTest, NonAppleHostDiscoveryFailsClosedToSrgb)
{
#if defined(__APPLE__)
    GTEST_SKIP() << "macOS owns CoreGraphics discovery; Win/Linux stub is residual";
#else
    auto state = discover_monitor_presentation("primary");
    ASSERT_TRUE(state) << state.error().message;
    EXPECT_EQ(state.value().source, DisplayProfileSource::kFallbackSrgb);
    EXPECT_TRUE(state.value().valid);
#if defined(_WIN32)
    EXPECT_EQ(state.value().reason, "windows_monitor_discovery_unavailable");
#elif defined(__linux__)
    EXPECT_EQ(state.value().reason, "linux_monitor_discovery_unavailable");
#else
    EXPECT_EQ(state.value().reason, "host_monitor_discovery_unavailable");
#endif
    EXPECT_EQ(state.value().monitor_profile.identifier.find("srgb") != std::string::npos ||
                  state.value().monitor_profile.identifier.find("sRGB") != std::string::npos ||
                  !state.value().monitor_profile.identifier.empty(),
              true);
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

} // namespace
} // namespace ravo
