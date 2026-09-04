#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "ravo/cli/application.h"
#include "ravo/foundation/json.h"

#include "cli_test_support.h"

#ifndef RAVO_REPOSITORY_ROOT
#error "RAVO_REPOSITORY_ROOT must be defined"
#endif

namespace ravo
{
namespace
{

namespace fs = std::filesystem;

[[nodiscard]] fs::path fixture_corpus()
{
    return fs::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" / "iq_evaluation_corpus";
}

TEST_F(CliTest, IqEvaluateFailClosesWithoutCorpus)
{
    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    const std::vector<std::string_view> arguments{"iq", "evaluate", "--json"};
    EXPECT_NE(application.run(std::span{arguments}), 0);
    auto parsed = parse_json(stdout_stream.str());
    ASSERT_TRUE(parsed) << stdout_stream.str();
    ASSERT_NE(parsed.value().find("ok"), nullptr);
    ASSERT_NE(parsed.value().find("ok")->boolean_if(), nullptr);
    EXPECT_FALSE(*parsed.value().find("ok")->boolean_if());
    const auto *error = parsed.value().find("error");
    ASSERT_NE(error, nullptr);
    const auto *context = error->find("context");
    ASSERT_NE(context, nullptr);
    ASSERT_NE(context->find("reason"), nullptr);
    EXPECT_EQ(*context->find("reason")->string_if(), "iq_corpus_unavailable");
    EXPECT_TRUE(stderr_stream.str().empty());
}

TEST_F(CliTest, IqEvaluateRunsInTreeFixtureCorpus)
{
    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    const auto root = fixture_corpus().string();
    const std::vector<std::string_view> arguments{"iq", "evaluate", "--corpus", root, "--json"};
    EXPECT_EQ(application.run(std::span{arguments}), 0) << stdout_stream.str();
    auto parsed = parse_json(stdout_stream.str());
    ASSERT_TRUE(parsed) << stdout_stream.str();
    ASSERT_NE(parsed.value().find("ok")->boolean_if(), nullptr);
    EXPECT_TRUE(*parsed.value().find("ok")->boolean_if());
    const auto *data = parsed.value().find("data");
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(*data->find("schema")->string_if(), "ravo.iq.fixture-support/v1");
    EXPECT_EQ(*data->find("maturity")->string_if(), "C2");
    EXPECT_EQ(*data->find("support_claim_status")->string_if(), "fixture_evidence_ready");
    EXPECT_FALSE(*data->find("camera_product_support_claimed")->boolean_if());
    EXPECT_FALSE(*data->find("learned_denoise_admitted")->boolean_if());
    EXPECT_TRUE(*data->find("cpu_gold_aligned")->boolean_if());
    EXPECT_FALSE(*data->find("decode_only")->boolean_if());
    EXPECT_EQ(*data->find("residual_c3")->string_if(), "licensed_real_corpus_and_human_review");
    const auto *denoise = data->find("denoise");
    ASSERT_NE(denoise, nullptr);
    EXPECT_EQ(*denoise->find("backend")->string_if(), "cpu");
    EXPECT_EQ(*denoise->find("operation_id")->string_if(), "ravo.detail.denoiseprofile");
    const auto *camera = data->find("camera_profile");
    ASSERT_NE(camera, nullptr);
    EXPECT_TRUE(*camera->find("document_present")->boolean_if());
    EXPECT_EQ(camera->find("document_sha256")->string_if()->size(), 64U);
    EXPECT_TRUE(stderr_stream.str().empty());
}

} // namespace
} // namespace ravo
