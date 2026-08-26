#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <QCoreApplication>
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

void ensure_jpeg_cli_qt_core()
{
    static const bool logging = []
    {
        ravo::init_logging("ravo-jpeg-cli-tests");
        return true;
    }();
    static_cast<void>(logging);
    if (QCoreApplication::instance() != nullptr)
    {
        return;
    }
    static int argc = 1;
    static char executable[] = "ravo-jpeg-cli-tests";
    static char *argv[] = {executable, nullptr};
    static auto *application = new QCoreApplication(argc, argv);
    static_cast<void>(application);
}

class JpegCliTempDirectory
{
public:
    JpegCliTempDirectory()
        : path_(std::filesystem::temp_directory_path() / ("ravo-jpeg-cli-" + generate_catalog_id()))
    {
        std::filesystem::create_directories(path_);
    }

    ~JpegCliTempDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] const JsonValue &required_child(const JsonValue &value, const std::string_view key)
{
    const auto *child = value.find(key);
    EXPECT_NE(child, nullptr) << key;
    static const JsonValue missing;
    return child == nullptr ? missing : *child;
}

TEST(JpegCliTest, CatalogImportItemPreservesTheCompleteStructuredJpegError)
{
    ensure_jpeg_cli_qt_core();
    JpegCliTempDirectory temporary;
    const auto catalog_path = (temporary.path() / "library.sqlite").string();
    const auto jpeg_path = temporary.path() / "truncated.jpg";
    {
        std::ofstream stream(jpeg_path, std::ios::binary);
        ASSERT_TRUE(stream.is_open());
        const std::array<std::uint8_t, 2> soi{0xFFU, 0xD8U};
        stream.write(reinterpret_cast<const char *>(soi.data()), soi.size());
        ASSERT_TRUE(stream.good());
    }

    auto engine_result = EngineFacade::create_phase1();
    ASSERT_TRUE(engine_result) << engine_result.error().message;
    auto engine = std::move(engine_result).value();
    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);

    EXPECT_EQ(application.run(std::vector<std::string_view>{"catalog", "create", "--path",
                                                            catalog_path, "--json"}),
              0)
        << stdout_stream.str();
    stdout_stream.str({});
    stdout_stream.clear();

    EXPECT_EQ(application.run(std::vector<std::string_view>{"catalog", "import", "--catalog",
                                                            catalog_path, "--input",
                                                            jpeg_path.string(), "--json"}),
              0)
        << stdout_stream.str();
    const auto response = parse_json(stdout_stream.str());
    ASSERT_TRUE(response) << response.error().message;
    const auto &data = required_child(response.value(), "data");
    const auto &items = required_child(data, "items");
    ASSERT_NE(items.array_if(), nullptr);
    ASSERT_EQ(items.array_if()->size(), 1U);
    const auto &item = items.array_if()->front();
    const auto &status = required_child(item, "status");
    ASSERT_NE(status.string_if(), nullptr);
    EXPECT_EQ(*status.string_if(), "failed");
    EXPECT_EQ(item.find("asset"), nullptr);

    const auto &error = required_child(item, "error");
    const auto &code = required_child(error, "code");
    const auto &message = required_child(error, "message");
    ASSERT_NE(code.string_if(), nullptr);
    ASSERT_NE(message.string_if(), nullptr);
    EXPECT_EQ(*code.string_if(), "validation");
    EXPECT_EQ(*message.string_if(), "JPEG image is truncated or incomplete");
    const auto &context = required_child(error, "context");
    const auto &format = required_child(context, "format");
    const auto &reason = required_child(context, "reason");
    const auto &source = required_child(context, "source");
    ASSERT_NE(format.string_if(), nullptr);
    ASSERT_NE(reason.string_if(), nullptr);
    ASSERT_NE(source.string_if(), nullptr);
    EXPECT_EQ(*format.string_if(), "jpeg");
    EXPECT_EQ(*reason.string_if(), "incomplete_jpeg_stream");
    const auto normalized = normalize_local_input(jpeg_path.string());
    ASSERT_TRUE(normalized) << normalized.error().message;
    EXPECT_EQ(*source.string_if(), normalized.value().path);
    EXPECT_TRUE(stderr_stream.str().empty());

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{"catalog", "list", "--catalog",
                                                            catalog_path, "--json"}),
              0)
        << stdout_stream.str();
    const auto listed = parse_json(stdout_stream.str());
    ASSERT_TRUE(listed) << listed.error().message;
    const auto &assets = required_child(required_child(listed.value(), "data"), "assets");
    ASSERT_NE(assets.array_if(), nullptr);
    EXPECT_TRUE(assets.array_if()->empty());
}

} // namespace
} // namespace ravo
