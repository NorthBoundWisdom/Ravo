#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "ravo/engine/iq_quality_evaluation.h"
#include "ravo/foundation/cancellation.h"

#ifndef RAVO_REPOSITORY_ROOT
#error "RAVO_REPOSITORY_ROOT must be defined for fixture corpus paths"
#endif

namespace ravo
{
namespace
{

namespace fs = std::filesystem;

class IqQualityTempCorpus
{
public:
    IqQualityTempCorpus()
    {
        root_ =
            fs::temp_directory_path() /
            fs::path("ravo-iq-corpus-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(root_);
    }

    ~IqQualityTempCorpus()
    {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    [[nodiscard]] const fs::path &root() const noexcept
    {
        return root_;
    }

    void write_manifest(const std::string &body) const
    {
        std::ofstream output(root_ / "manifest.json", std::ios::binary | std::ios::trunc);
        output << body;
    }

    void write_file(const fs::path &relative, const std::string &body) const
    {
        const auto path = root_ / relative;
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << body;
    }

private:
    fs::path root_;
};

[[nodiscard]] fs::path in_tree_fixture_corpus()
{
    return fs::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" / "iq_evaluation_corpus";
}

TEST(IqQualityEvaluationTest, MissingCorpusFailCloses)
{
    auto missing = resolve_iq_evaluation_corpus(std::string{});
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().context.at("reason"), "iq_corpus_unavailable");

    auto absent = resolve_iq_evaluation_corpus(std::string("/tmp/ravo-iq-corpus-does-not-exist"));
    ASSERT_FALSE(absent);
    EXPECT_EQ(absent.error().context.at("reason"), "iq_corpus_file_missing");

    auto bundle = evaluate_iq_fixture_support(std::string{});
    ASSERT_FALSE(bundle);
    EXPECT_EQ(bundle.error().context.at("reason"), "iq_corpus_unavailable");
}

TEST(IqQualityEvaluationTest, FixtureCorpusRunsCpuDenoiseEvaluation)
{
    IqQualityTempCorpus temporary;
    temporary.write_manifest(R"({
  "schema": "ravo.iq.evaluation-corpus/v1",
  "schema_version": 1,
  "corpus_id": "synthetic-first-ready",
  "license": "CC0-1.0",
  "notice_path": "NOTICE",
  "cases": [
    {
      "case_id": "denoise-synthetic-a",
      "kind": "denoise_fixture",
      "synthetic": true,
      "notes": "CPU profile-denoise fixture"
    },
    {
      "case_id": "camera-synthetic-a",
      "kind": "camera_profile_fixture",
      "synthetic": true,
      "camera_make": "Ravo",
      "camera_model": "Fixture",
      "iso": 800
    }
  ]
})");
    temporary.write_file("NOTICE", "Synthetic IQ fixture corpus for ADR-0152.\n");

    auto corpus = resolve_iq_evaluation_corpus(temporary.root().string());
    ASSERT_TRUE(corpus) << corpus.error().message;
    EXPECT_EQ(corpus.value().schema, kIqEvaluationCorpusContractVersion);
    EXPECT_EQ(corpus.value().cases.size(), 2U);

    auto denoise = evaluate_denoise_cpu_reference(corpus.value(), 0.35, CancellationToken{});
    ASSERT_TRUE(denoise) << denoise.error().message;
    EXPECT_EQ(denoise.value().schema, kIqDenoiseEvaluationContractVersion);
    EXPECT_EQ(denoise.value().backend, "cpu");
    EXPECT_EQ(denoise.value().operation_id, "ravo.detail.denoiseprofile");
    EXPECT_EQ(denoise.value().case_id, "denoise-synthetic-a");
    EXPECT_TRUE(denoise.value().finite);
    EXPECT_TRUE(denoise.value().cpu_gold_aligned);
    EXPECT_FALSE(denoise.value().learned_denoise_admitted);
    EXPECT_FALSE(denoise.value().decode_only);
    EXPECT_EQ(denoise.value().support_claim_status, kIqSupportClaimFixtureEvidenceReady);
    EXPECT_GT(denoise.value().width, 0U);
    EXPECT_GT(denoise.value().height, 0U);
    EXPECT_GE(denoise.value().mean_abs_delta, 0.0);
    EXPECT_GE(denoise.value().max_abs_delta, denoise.value().mean_abs_delta);

    auto profile = probe_camera_profile_quality(corpus.value(), CancellationToken{});
    ASSERT_TRUE(profile) << profile.error().message;
    EXPECT_EQ(profile.value().schema, kIqCameraProfileProbeContractVersion);
    EXPECT_EQ(profile.value().case_id, "camera-synthetic-a");
    EXPECT_FALSE(profile.value().document_present);
    EXPECT_FALSE(profile.value().colour_accuracy_closed);
    EXPECT_EQ(profile.value().camera_make, std::optional<std::string>{"Ravo"});
}

TEST(IqQualityEvaluationTest, DenoiseFailsClosedWithoutDenoiseCase)
{
    IqQualityTempCorpus temporary;
    temporary.write_manifest(R"({
  "schema": "ravo.iq.evaluation-corpus/v1",
  "schema_version": 1,
  "corpus_id": "camera-only",
  "license": "CC0-1.0",
  "notice_path": "NOTICE",
  "cases": [
    {
      "case_id": "camera-only",
      "kind": "camera_profile_fixture",
      "synthetic": true
    }
  ]
})");
    auto corpus = resolve_iq_evaluation_corpus(temporary.root().string());
    ASSERT_TRUE(corpus) << corpus.error().message;
    auto denoise = evaluate_denoise_cpu_reference(corpus.value(), 0.35, CancellationToken{});
    ASSERT_FALSE(denoise);
    EXPECT_EQ(denoise.error().context.at("reason"), "iq_corpus_unavailable");
}

TEST(IqQualityEvaluationTest, CameraProfileDocumentHashIsDeterministic)
{
    IqQualityTempCorpus temporary;
    temporary.write_manifest(R"({
  "schema": "ravo.iq.evaluation-corpus/v1",
  "schema_version": 1,
  "corpus_id": "camera-doc",
  "license": "CC0-1.0",
  "notice_path": "NOTICE",
  "cases": [
    {
      "case_id": "denoise-synthetic",
      "kind": "denoise_fixture",
      "synthetic": true
    },
    {
      "case_id": "camera-doc",
      "kind": "camera_profile_fixture",
      "synthetic": false,
      "relative_path": "cases/profile.json",
      "camera_make": "Ravo",
      "camera_model": "DocCam",
      "iso": 1600,
      "illuminant": "D65"
    }
  ]
})");
    temporary.write_file("NOTICE", "doc hash fixture\n");
    temporary.write_file("cases/profile.json",
                         "{\"schema\":\"fixture\",\"payload\":\"iq-01-c2-hash\"}\n");

    auto corpus = resolve_iq_evaluation_corpus(temporary.root().string());
    ASSERT_TRUE(corpus) << corpus.error().message;
    auto first = probe_camera_profile_quality(corpus.value(), CancellationToken{});
    auto second = probe_camera_profile_quality(corpus.value(), CancellationToken{});
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_TRUE(first.value().document_present);
    ASSERT_TRUE(first.value().document_sha256.has_value());
    EXPECT_EQ(first.value().document_sha256->size(), 64U);
    EXPECT_EQ(first.value().document_sha256, second.value().document_sha256);
    EXPECT_EQ(first.value().document_bytes, second.value().document_bytes);
    EXPECT_EQ(first.value().camera_model, std::optional<std::string>{"DocCam"});
    EXPECT_FALSE(first.value().colour_accuracy_closed);
}

TEST(IqQualityEvaluationTest, InTreeFixtureCorpusEmitsC2SupportClaimBundle)
{
    const auto root = in_tree_fixture_corpus();
    ASSERT_TRUE(fs::is_directory(root)) << root.string();

    auto bundle = evaluate_iq_fixture_support(root.string(), 0.35, CancellationToken{});
    ASSERT_TRUE(bundle) << bundle.error().message;
    EXPECT_EQ(bundle.value().schema, kIqFixtureSupportContractVersion);
    EXPECT_EQ(bundle.value().maturity, "C2");
    EXPECT_EQ(bundle.value().support_claim_status, kIqSupportClaimFixtureEvidenceReady);
    EXPECT_FALSE(bundle.value().camera_product_support_claimed);
    EXPECT_FALSE(bundle.value().learned_denoise_admitted);
    EXPECT_TRUE(bundle.value().cpu_gold_aligned);
    EXPECT_FALSE(bundle.value().decode_only);
    EXPECT_EQ(bundle.value().residual_c3, "licensed_real_corpus_and_human_review");
    EXPECT_EQ(bundle.value().corpus_id, "ravo-iq-fixture-c2");
    EXPECT_EQ(bundle.value().denoise.operation_id, "ravo.detail.denoiseprofile");
    EXPECT_TRUE(bundle.value().camera_profile.document_present);
    ASSERT_TRUE(bundle.value().camera_profile.document_sha256.has_value());
    EXPECT_EQ(bundle.value().camera_profile.document_sha256->size(), 64U);
}

} // namespace
} // namespace ravo
