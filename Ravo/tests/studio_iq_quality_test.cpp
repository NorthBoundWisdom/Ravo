#include <filesystem>
#include <string>

#include <QString>
#include <QVariantMap>
#include <gtest/gtest.h>

#include "ravo/desktop/studio_presenter.h"
#include "ravo/foundation/log.h"

#include "studio_test_support.h"

#ifndef RAVO_REPOSITORY_ROOT
#error "RAVO_REPOSITORY_ROOT must be defined"
#endif

namespace ravo
{
using namespace studio_test_support;
namespace
{

namespace fs = std::filesystem;

[[nodiscard]] QString fixture_corpus_path()
{
    return QString::fromStdString(
        (fs::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" / "iq_evaluation_corpus")
            .string());
}

TEST(StudioIqQualityTest, PolicyIsConstantAndFailClosedWithoutCorpus)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    StudioPresenter presenter;
    const auto policy = presenter.iqQualityPolicy();
    EXPECT_EQ(policy.value(QStringLiteral("maturity")).toString(), QStringLiteral("C2"));
    EXPECT_EQ(policy.value(QStringLiteral("supportClaimStatus")).toString(),
              QStringLiteral("fixture_evidence_ready"));
    EXPECT_FALSE(policy.value(QStringLiteral("cameraProductSupportClaimed")).toBool());
    EXPECT_FALSE(policy.value(QStringLiteral("learnedDenoiseAdmitted")).toBool());
    EXPECT_TRUE(policy.value(QStringLiteral("failClosedWithoutCorpus")).toBool());
    EXPECT_TRUE(policy.value(QStringLiteral("cpuGoldAligned")).toBool());
    EXPECT_FALSE(policy.value(QStringLiteral("decodeOnly")).toBool());
    EXPECT_EQ(policy.value(QStringLiteral("residualC3")).toString(),
              QStringLiteral("licensed_real_corpus_and_human_review"));

    const auto missing = presenter.evaluateIqQuality();
    EXPECT_FALSE(missing.value(QStringLiteral("ok")).toBool());
    const auto context = missing.value(QStringLiteral("context")).toMap();
    EXPECT_EQ(context.value(QStringLiteral("reason")).toString(),
              QStringLiteral("iq_corpus_unavailable"));
}

TEST(StudioIqQualityTest, EvaluateFixtureCorpusReturnsSupportClaimEvidence)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    StudioPresenter presenter;
    const auto report = presenter.evaluateIqQuality(fixture_corpus_path(), 0.35);
    ASSERT_TRUE(report.value(QStringLiteral("ok")).toBool())
        << report.value(QStringLiteral("message")).toString().toStdString();
    EXPECT_EQ(report.value(QStringLiteral("maturity")).toString(), QStringLiteral("C2"));
    EXPECT_EQ(report.value(QStringLiteral("supportClaimStatus")).toString(),
              QStringLiteral("fixture_evidence_ready"));
    EXPECT_FALSE(report.value(QStringLiteral("cameraProductSupportClaimed")).toBool());
    EXPECT_FALSE(report.value(QStringLiteral("learnedDenoiseAdmitted")).toBool());
    EXPECT_TRUE(report.value(QStringLiteral("cpuGoldAligned")).toBool());
    const auto denoise = report.value(QStringLiteral("denoise")).toMap();
    EXPECT_EQ(denoise.value(QStringLiteral("backend")).toString(), QStringLiteral("cpu"));
    EXPECT_EQ(denoise.value(QStringLiteral("operationId")).toString(),
              QStringLiteral("ravo.detail.denoiseprofile"));
    const auto camera = report.value(QStringLiteral("cameraProfile")).toMap();
    EXPECT_TRUE(camera.value(QStringLiteral("documentPresent")).toBool());
    EXPECT_EQ(camera.value(QStringLiteral("documentSha256")).toString().size(), 64);
}

} // namespace
} // namespace ravo
