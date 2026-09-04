#include <QCoreApplication>
#include <gtest/gtest.h>

#include "ravo/desktop/studio_display_presentation.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{
namespace
{

void ensure_qt_core()
{
    if (QCoreApplication::instance() != nullptr)
        return;
    static int argc = 1;
    static char executable[] = "ravo-desktop-display-presentation-tests";
    static char *argv[] = {executable, nullptr};
    static auto *application = new QCoreApplication(argc, argv);
    static_cast<void>(application);
}

TEST(StudioDisplayPresentationTest, ScreenTokenRefreshLeavesRecipeUnchanged)
{
    ensure_qt_core();
    StudioDisplayPresentation owner;
    ASSERT_TRUE(owner.injectSyntheticMatrixForTesting());
    const auto before_token = owner.screenToken();
    const auto before_fingerprint =
        owner.state().value(QStringLiteral("profileFingerprint")).toString();

    DevelopParams develop;
    develop.exposure_ev = -0.25;
    develop.output_color.output_profile = "adobe_rgb";
    const AssetDescriptor asset{"asset-display-owner", "file:///fixture.raw", std::nullopt};
    auto before_recipe = recipe_from_develop(asset, develop);
    ASSERT_TRUE(before_recipe) << before_recipe.error().message;
    auto before_json = serialize_recipe(before_recipe.value());
    ASSERT_TRUE(before_json) << before_json.error().message;

    ASSERT_TRUE(owner.applyScreenTokenForTesting(QStringLiteral("screen-b")));
    EXPECT_EQ(owner.screenToken(), QStringLiteral("screen-b"));
    EXPECT_EQ(owner.state().value(QStringLiteral("profileFingerprint")).toString(),
              before_fingerprint);
    EXPECT_NE(before_token, owner.screenToken());

    auto after_recipe = recipe_from_develop(asset, develop);
    ASSERT_TRUE(after_recipe) << after_recipe.error().message;
    auto after_json = serialize_recipe(after_recipe.value());
    ASSERT_TRUE(after_json) << after_json.error().message;
    EXPECT_EQ(after_json.value(), before_json.value());
    EXPECT_EQ(develop.output_color.output_profile, "adobe_rgb");
    EXPECT_EQ(develop.exposure_ev, -0.25);
}

TEST(StudioDisplayPresentationTest, InitialStateIsMachineVisible)
{
    ensure_qt_core();
    StudioDisplayPresentation owner;
    EXPECT_FALSE(owner.screenToken().isEmpty());
    EXPECT_FALSE(owner.source().isEmpty());
    EXPECT_FALSE(owner.reason().isEmpty());
    EXPECT_TRUE(owner.state().contains(QStringLiteral("contractVersion")));
}

TEST(StudioDisplayPresentationTest, ViewContractsAreMachineVisible)
{
    ensure_qt_core();
    StudioDisplayPresentation owner;
    const auto contracts = owner.viewContracts();
    ASSERT_FALSE(contracts.isEmpty());
    bool saw_display_transformed = false;
    bool saw_scopes = false;
    for (const auto &entry : contracts)
    {
        const auto map = entry.toMap();
        EXPECT_FALSE(map.value(QStringLiteral("viewId")).toString().isEmpty());
        EXPECT_FALSE(map.value(QStringLiteral("pixelKind")).toString().isEmpty());
        EXPECT_EQ(map.value(QStringLiteral("softProofInteraction")).toString(),
                  QStringLiteral("after_soft_proof_display_only"));
        if (map.value(QStringLiteral("pixelKind")).toString() ==
            QStringLiteral("display_transformed"))
            saw_display_transformed = true;
        if (map.value(QStringLiteral("viewId")).toString() == QStringLiteral("scopes"))
            saw_scopes = true;
    }
    EXPECT_TRUE(saw_display_transformed);
    EXPECT_TRUE(saw_scopes);
}

} // namespace
} // namespace ravo
