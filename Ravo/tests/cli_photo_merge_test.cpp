#include <filesystem>
#include <sstream>
#include <QColorSpace>
#include <QImage>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "cli_test_support.h"
#include "ravo/cli/application.h"
#include "ravo/foundation/json.h"

namespace ravo
{
namespace
{
struct MergeCliRun
{
    int exit_code;
    JsonValue json;
};
Result<MergeCliRun> run_merge_cli(const EngineFacade &engine, const std::vector<std::string> &args)
{
    std::vector<std::string_view> views;
    for (const auto &arg : args)
        views.push_back(arg);
    std::ostringstream output, error;
    CliApplication application(engine, output, error);
    const auto code = application.run(views);
    auto parsed = parse_json(output.str());
    if (!parsed)
        return parsed.error();
    return MergeCliRun{code, std::move(parsed).value()};
}
} // namespace
TEST_F(CliTest, PhotoMergeJsonRequiresRevisionAndDescribesImmutableArtifact)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const auto catalog = root.filePath("library.sqlite").toStdString();
    auto created = run_merge_cli(engine, {"catalog", "create", "--catalog", catalog, "--json"});
    ASSERT_TRUE(created);
    ASSERT_EQ(created.value().exit_code, 0);
    QImage image(64, 48, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    std::vector<std::string> paths;
    for (int i = 0; i < 2; ++i)
    {
        const auto path = root.filePath(QString("source-%1.png").arg(i));
        paths.push_back(path.toStdString());
        image.fill(QColor(80 + i * 40, 80 + i * 40, 80 + i * 40));
        ASSERT_TRUE(image.save(path, "PNG"));
        auto imported = run_merge_cli(
            engine, {"catalog", "import", "--catalog", catalog, "--input", paths.back(), "--json"});
        ASSERT_TRUE(imported);
        ASSERT_EQ(imported.value().exit_code, 0);
    }
    auto listed = run_merge_cli(engine, {"catalog", "list", "--catalog", catalog, "--json"});
    ASSERT_TRUE(listed);
    const auto *data = listed.value().json.find("data");
    ASSERT_NE(data, nullptr);
    const auto *assets = data->find("assets")->array_if();
    ASSERT_EQ(assets->size(), 2);
    const auto first = *(*assets)[0].find("id")->string_if(),
               second = *(*assets)[1].find("id")->string_if();
    const auto revision = data->find("revision")->number_if()->text;
    const auto output = root.filePath("merged.tiff").toStdString();
    std::vector<std::string> command{"catalog",
                                     "hdr-merge",
                                     "--catalog",
                                     catalog,
                                     "--asset-id",
                                     first,
                                     "--asset-id",
                                     second,
                                     "--exposure-stop",
                                     "0",
                                     "--exposure-stop",
                                     "1",
                                     "--no-align",
                                     "--output",
                                     output,
                                     "--json"};
    auto missing = run_merge_cli(engine, command);
    ASSERT_TRUE(missing);
    EXPECT_NE(missing.value().exit_code, 0);
    command.insert(command.end(), {"--revision", revision});
    auto result = run_merge_cli(engine, command);
    ASSERT_TRUE(result);
    ASSERT_EQ(result.value().exit_code, 0) << serialize_json(result.value().json);
    const auto *merged = result.value().json.find("data");
    ASSERT_NE(merged, nullptr);
    EXPECT_EQ(*merged->find("schema")->string_if(), "ravo.photo_merge");
    const auto *artifact = merged->find("artifact");
    ASSERT_NE(artifact, nullptr);
    EXPECT_EQ(*artifact->find("mime_type")->string_if(), "image/tiff");
    EXPECT_EQ(artifact->find("content_sha256")->string_if()->size(), 64);
    EXPECT_TRUE(*merged->find("originals_unchanged")->boolean_if());
    EXPECT_TRUE(std::filesystem::exists(output));
    auto stale = run_merge_cli(engine, command);
    ASSERT_TRUE(stale);
    EXPECT_NE(stale.value().exit_code, 0);
    EXPECT_TRUE(std::filesystem::exists(output));
}
TEST_F(CliTest, PhotoMergeFlagsRejectUnsupportedOwnersAndMalformedNumbers)
{
    for (const auto &args : std::vector<std::vector<std::string>>{
             {"catalog", "list", "--no-align", "--json"},
             {"catalog", "panorama", "--exposure-stop", "1", "--json"},
             {"catalog", "hdr-merge", "--deghost", "NaN", "--json"}})
    {
        auto result = run_merge_cli(engine, args);
        ASSERT_TRUE(result);
        EXPECT_NE(result.value().exit_code, 0);
    }
}
} // namespace ravo
