#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "ravo/engine/iq_quality_evaluation.h"
#include "ravo/foundation/cancellation.h"

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

private:
    fs::path root_;
};

TEST(IqQualityEvaluationTest, MissingCorpusFailCloses)
{
    auto missing = resolve_iq_evaluation_corpus(std::string{});
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().context.at("reason"), "iq_corpus_unavailable");

    auto absent = resolve_iq_evaluation_corpus(std::string("/tmp/ravo-iq-corpus-does-not-exist"));
    ASSERT_FALSE(absent);
    EXPECT_EQ(absent.error().context.at("reason"), "iq_corpus_file_missing");
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
    std::ofstream notice(temporary.root() / "NOTICE", std::ios::binary | std::ios::trunc);
    notice << "Synthetic IQ fixture corpus for ADR-0152 first Ready.\n";

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
    EXPECT_GT(denoise.value().width, 0U);
    EXPECT_GT(denoise.value().height, 0U);
    EXPECT_GE(denoise.value().mean_abs_delta, 0.0);

    auto profile = probe_camera_profile_quality(corpus.value(), CancellationToken{});
    ASSERT_TRUE(profile) << profile.error().message;
    EXPECT_EQ(profile.value().schema, kIqCameraProfileProbeContractVersion);
    EXPECT_EQ(profile.value().case_id, "camera-synthetic-a");
    EXPECT_FALSE(profile.value().document_present);
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

} // namespace
} // namespace ravo
