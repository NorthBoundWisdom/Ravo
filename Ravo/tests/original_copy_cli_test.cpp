#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <QColorSpace>
#include <QCoreApplication>
#include <QFile>
#include <QImage>
#include <QIODevice>
#include <gtest/gtest.h>

#include "ravo/cli/application.h"
#include "ravo/domain/types.h"
#include "ravo/domain/uri.h"
#include "ravo/engine/engine.h"
#include "ravo/foundation/json.h"
#include "ravo/foundation/log.h"

namespace ravo
{
namespace
{

[[nodiscard]] const JsonValue &required_child(const JsonValue &value, const std::string_view key)
{
    const auto *child = value.find(key);
    EXPECT_NE(child, nullptr) << key;
    static const JsonValue missing;
    return child == nullptr ? missing : *child;
}

void write_source_png(const std::filesystem::path &path)
{
    QImage image(96, 64, QImage::Format_RGB888);
    std::uint32_t state = 0x243F6A88U;
    for (int y = 0; y < image.height(); ++y)
    {
        auto *row = image.scanLine(y);
        for (int x = 0; x < image.width() * 3; ++x)
        {
            state = state * 1103515245U + 12345U;
            row[x] = static_cast<std::uint8_t>(state >> 24U);
        }
    }
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    ASSERT_TRUE(image.save(QString::fromStdString(path.string()), "PNG"));
}

[[nodiscard]] QByteArray read_file(const std::filesystem::path &path)
{
    QFile file(QString::fromStdString(path.string()));
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

void write_file(const std::filesystem::path &path, const std::string_view bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(output);
}

class OriginalCopyCliTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        root_ = std::filesystem::temp_directory_path() /
                ("ravo-original-copy-cli-" + generate_catalog_id());
        std::filesystem::create_directories(root_);
        catalog_ = (root_ / "library.sqlite").string();
        source_ = root_ / "source.png";
        write_source_png(source_);
        source_bytes_ = read_file(source_);
        const auto normalized_source = normalize_local_input(source_.string());
        ASSERT_TRUE(normalized_source) << normalized_source.error().message;
        normalized_source_ = normalized_source.value().path;

        application_ = std::make_unique<CliApplication>(engine_, stdout_stream_, stderr_stream_);
        int exit_code = 0;
        auto created = run_json({"catalog", "create", "--path", catalog_, "--json"}, exit_code);
        ASSERT_EQ(exit_code, 0) << stdout_stream_.str();
        ASSERT_TRUE(created) << created.error().message;

        auto imported = run_json(
            {"catalog", "import", "--catalog", catalog_, "--input", source_.string(), "--json"},
            exit_code);
        ASSERT_EQ(exit_code, 0) << stdout_stream_.str();
        ASSERT_TRUE(imported) << imported.error().message;
        const auto &items = required_child(required_child(imported.value(), "data"), "items");
        ASSERT_NE(items.array_if(), nullptr);
        ASSERT_EQ(items.array_if()->size(), 1U);
        const auto &item = items.array_if()->front();
        const auto &status = required_child(item, "status");
        ASSERT_NE(status.string_if(), nullptr);
        ASSERT_EQ(*status.string_if(), "imported");
        const auto &id = required_child(required_child(item, "asset"), "id");
        ASSERT_NE(id.string_if(), nullptr);
        asset_id_ = *id.string_if();
    }

    void TearDown() override
    {
        application_.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] Result<JsonValue> run_json(const std::vector<std::string_view> &arguments,
                                             int &exit_code)
    {
        stdout_stream_.str({});
        stdout_stream_.clear();
        stderr_stream_.str({});
        stderr_stream_.clear();
        exit_code = application_->run(arguments);
        EXPECT_TRUE(stderr_stream_.str().empty());
        return parse_json(stdout_stream_.str());
    }

    EngineFacade engine_ = []
    {
        auto created = EngineFacade::create_phase1();
        return std::move(created).value();
    }();
    std::filesystem::path root_;
    std::filesystem::path source_;
    std::string catalog_;
    std::string asset_id_;
    std::string normalized_source_;
    QByteArray source_bytes_;
    std::ostringstream stdout_stream_;
    std::ostringstream stderr_stream_;
    std::unique_ptr<CliApplication> application_;
};

TEST_F(OriginalCopyCliTest, AllThreeAliasesReturnCanonicalSuccessJsonAndExactBytes)
{
    const std::array<std::string_view, 3> aliases{"original", "copy", "original-copy"};
    for (std::size_t index = 0; index < aliases.size(); ++index)
    {
        const auto output = root_ / ("alias-" + std::to_string(index) + ".png");
        const std::string output_text = output.string();
        const auto normalized_output = normalize_local_input(output_text);
        ASSERT_TRUE(normalized_output) << normalized_output.error().message;
        int exit_code = 0;
        const auto response =
            run_json({"catalog", "export", "--catalog", catalog_, "--asset-id", asset_id_,
                      "--output", output_text, "--format", aliases[index], "--json"},
                     exit_code);
        ASSERT_EQ(exit_code, 0) << stdout_stream_.str();
        ASSERT_TRUE(response) << response.error().message;
        const auto &data = required_child(response.value(), "data");
        const auto &format = required_child(data, "format");
        const auto &bytes = required_child(data, "bytes");
        const auto &published_output = required_child(data, "output");
        ASSERT_NE(format.string_if(), nullptr);
        ASSERT_NE(bytes.number_if(), nullptr);
        ASSERT_NE(published_output.string_if(), nullptr);
        EXPECT_EQ(*format.string_if(), "original");
        EXPECT_EQ(bytes.number_if()->text, std::to_string(source_bytes_.size()));
        EXPECT_EQ(*published_output.string_if(), normalized_output.value().path);
        EXPECT_EQ(read_file(output), source_bytes_);
        EXPECT_FALSE(std::filesystem::exists(output_text + ".xmp"));
    }
}

TEST_F(OriginalCopyCliTest, ConflictPreservesWinnerAndCompleteStructuredContext)
{
    const auto output = root_ / "conflict.png";
    const std::string output_text = output.string();
    const auto normalized_output = normalize_local_input(output_text);
    ASSERT_TRUE(normalized_output) << normalized_output.error().message;
    write_file(output, "winner");

    int exit_code = 0;
    const auto response =
        run_json({"catalog", "export", "--catalog", catalog_, "--asset-id", asset_id_, "--output",
                  output_text, "--format", "original", "--json"},
                 exit_code);
    ASSERT_EQ(exit_code, 6) << stdout_stream_.str();
    ASSERT_TRUE(response) << response.error().message;
    const auto &error = required_child(response.value(), "error");
    const auto &code = required_child(error, "code");
    const auto &message = required_child(error, "message");
    const auto &context = required_child(error, "context");
    ASSERT_NE(code.string_if(), nullptr);
    ASSERT_NE(message.string_if(), nullptr);
    ASSERT_NE(context.object_if(), nullptr);
    EXPECT_EQ(*code.string_if(), "conflict");
    EXPECT_EQ(*message.string_if(), "Export output already exists");
    EXPECT_EQ(context.object_if()->size(), 3U);
    const auto &reason = required_child(context, "reason");
    const auto &source = required_child(context, "source");
    const auto &published_output = required_child(context, "output");
    ASSERT_NE(reason.string_if(), nullptr);
    ASSERT_NE(source.string_if(), nullptr);
    ASSERT_NE(published_output.string_if(), nullptr);
    EXPECT_EQ(*reason.string_if(), "original_copy_output_exists");
    EXPECT_EQ(*source.string_if(), normalized_source_);
    EXPECT_EQ(*published_output.string_if(), normalized_output.value().path);
    EXPECT_EQ(read_file(output), QByteArray("winner"));
}

TEST_F(OriginalCopyCliTest, IoFailurePublishesNothingAndPreservesCompleteStructuredContext)
{
    const auto output = root_ / "missing-parent" / "output.png";
    const std::string output_text = output.string();
    const auto normalized_output = normalize_local_input(output_text);
    ASSERT_TRUE(normalized_output) << normalized_output.error().message;

    int exit_code = 0;
    const auto response =
        run_json({"catalog", "export", "--catalog", catalog_, "--asset-id", asset_id_, "--output",
                  output_text, "--format", "original-copy", "--json"},
                 exit_code);
    ASSERT_EQ(exit_code, 6) << stdout_stream_.str();
    ASSERT_TRUE(response) << response.error().message;
    const auto &error = required_child(response.value(), "error");
    const auto &code = required_child(error, "code");
    const auto &message = required_child(error, "message");
    const auto &context = required_child(error, "context");
    ASSERT_NE(code.string_if(), nullptr);
    ASSERT_NE(message.string_if(), nullptr);
    ASSERT_NE(context.object_if(), nullptr);
    EXPECT_EQ(*code.string_if(), "io");
    EXPECT_EQ(*message.string_if(), "Export directory does not exist");
    EXPECT_EQ(context.object_if()->size(), 3U);
    const auto &reason = required_child(context, "reason");
    const auto &source = required_child(context, "source");
    const auto &published_output = required_child(context, "output");
    ASSERT_NE(reason.string_if(), nullptr);
    ASSERT_NE(source.string_if(), nullptr);
    ASSERT_NE(published_output.string_if(), nullptr);
    EXPECT_EQ(*reason.string_if(), "original_copy_output_parent_missing");
    EXPECT_EQ(*source.string_if(), normalized_source_);
    EXPECT_EQ(*published_output.string_if(), normalized_output.value().path);
    EXPECT_FALSE(std::filesystem::exists(output));
    EXPECT_FALSE(std::filesystem::exists(output_text + ".ravo-export-tmp"));
}

// CliApplication intentionally has no cancellation injection boundary. Direct
// internal/service hook tests freeze cancellation and complete TaskError
// context; this target must not be reported as end-to-end CLI cancellation.

} // namespace
} // namespace ravo

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    ravo::init_logging("ravo-original-copy-cli-tests");
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ravo::shutdown_logging();
    return result;
}
