#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <QColorSpace>
#include <QCoreApplication>
#include <QCryptographicHash>
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

struct JpegSamplingFactors
{
    std::uint8_t y_horizontal = 0U;
    std::uint8_t y_vertical = 0U;
    std::uint8_t cb_horizontal = 0U;
    std::uint8_t cb_vertical = 0U;
    std::uint8_t cr_horizontal = 0U;
    std::uint8_t cr_vertical = 0U;
};

struct JpegHeaderSegment
{
    std::uint8_t id = 0U;
    std::size_t payload_offset = 0U;
    std::size_t payload_size = 0U;
};

[[nodiscard]] std::optional<std::vector<JpegHeaderSegment>>
jpeg_header_segments(const std::vector<std::uint8_t> &bytes)
{
    if (bytes.size() < 4U || bytes[0] != 0xFFU || bytes[1] != 0xD8U)
    {
        return std::nullopt;
    }
    std::vector<JpegHeaderSegment> segments;
    std::size_t offset = 2U;
    while (offset < bytes.size())
    {
        if (bytes[offset] != 0xFFU)
        {
            return std::nullopt;
        }
        while (offset < bytes.size() && bytes[offset] == 0xFFU)
        {
            ++offset;
        }
        if (offset >= bytes.size())
        {
            return std::nullopt;
        }
        const std::uint8_t id = bytes[offset++];
        if (id == 0xD9U || id == 0xDAU)
        {
            return segments;
        }
        if (id == 0xD8U || (id >= 0xD0U && id <= 0xD7U))
        {
            continue;
        }
        if (bytes.size() - offset < 2U)
        {
            return std::nullopt;
        }
        const std::uint16_t length = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(bytes[offset]) << 8U) | bytes[offset + 1U]);
        if (length < 2U || length > bytes.size() - offset)
        {
            return std::nullopt;
        }
        segments.push_back({id, offset + 2U, static_cast<std::size_t>(length) - 2U});
        offset += length;
    }
    return std::nullopt;
}

[[nodiscard]] bool is_jpeg_frame_marker(const std::uint8_t id) noexcept
{
    return (id >= 0xC0U && id <= 0xC3U) || (id >= 0xC5U && id <= 0xC7U) ||
           (id >= 0xC9U && id <= 0xCBU) || (id >= 0xCDU && id <= 0xCFU);
}

[[nodiscard]] std::optional<JpegSamplingFactors>
jpeg_sampling_factors(const std::vector<std::uint8_t> &bytes)
{
    const auto segments = jpeg_header_segments(bytes);
    if (!segments)
    {
        return std::nullopt;
    }
    for (const JpegHeaderSegment &segment : *segments)
    {
        if (!is_jpeg_frame_marker(segment.id) || segment.payload_size != 15U)
        {
            continue;
        }
        const std::size_t offset = segment.payload_offset;
        if (bytes[offset + 5U] != 3U || bytes[offset + 6U] != 1U || bytes[offset + 9U] != 2U ||
            bytes[offset + 12U] != 3U)
        {
            return std::nullopt;
        }
        const auto factors = [&](const std::size_t index)
        {
            return std::pair{static_cast<std::uint8_t>(bytes[offset + index] >> 4U),
                             static_cast<std::uint8_t>(bytes[offset + index] & 0x0FU)};
        };
        const auto y = factors(7U);
        const auto cb = factors(10U);
        const auto cr = factors(13U);
        return JpegSamplingFactors{y.first, y.second, cb.first, cb.second, cr.first, cr.second};
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::uint16_t>
jpeg_first_luminance_quantizer(const std::vector<std::uint8_t> &bytes)
{
    const auto segments = jpeg_header_segments(bytes);
    if (!segments)
    {
        return std::nullopt;
    }
    for (const JpegHeaderSegment &segment : *segments)
    {
        if (segment.id != 0xDBU)
        {
            continue;
        }
        std::size_t offset = segment.payload_offset;
        const std::size_t end = offset + segment.payload_size;
        while (end - offset >= 65U)
        {
            const std::uint8_t header = bytes[offset++];
            if ((header & 0x0FU) == 0U && (header >> 4U) == 0U)
            {
                return static_cast<std::uint16_t>(bytes[offset]);
            }
            offset += (header >> 4U) == 0U ? 64U : 128U;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> read_bytes(const std::filesystem::path &path)
{
    QFile file(QString::fromStdString(path.string()));
    if (!file.open(QIODevice::ReadOnly))
    {
        return std::nullopt;
    }
    const QByteArray encoded = file.readAll();
    return std::vector<std::uint8_t>(reinterpret_cast<const std::uint8_t *>(encoded.constData()),
                                     reinterpret_cast<const std::uint8_t *>(encoded.constData()) +
                                         encoded.size());
}

struct CliRun
{
    int exit_code = 0;
    Result<JsonValue> body;
};

class JpegCliExportTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensure_jpeg_cli_qt_core();
        root_ = std::filesystem::temp_directory_path() /
                ("ravo-jpeg-cli-export-" + generate_catalog_id());
        std::filesystem::create_directories(root_);
        catalog_ = (root_ / "library.sqlite").string();
        source_ = root_ / "color.png";
        write_source(source_, 48U, 32U);
        source_hash_ = file_hash(source_);
        application_ = std::make_unique<CliApplication>(engine_, stdout_stream_, stderr_stream_);
        const auto created = run({"catalog", "create", "--path", catalog_});
        ASSERT_EQ(created.exit_code, 0) << stdout_stream_.str();
        asset_ = import_source(source_);
    }

    void TearDown() override
    {
        EXPECT_EQ(file_hash(source_), source_hash_);
        application_.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] static QByteArray file_hash(const std::filesystem::path &path)
    {
        QFile file(QString::fromStdString(path.string()));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
    }

    void write_source(const std::filesystem::path &path, const std::uint32_t width,
                      const std::uint32_t height)
    {
        QImage image(static_cast<int>(width), static_cast<int>(height), QImage::Format_RGB888);
        for (std::uint32_t row = 0U; row < height; ++row)
        {
            auto *const scanline = image.scanLine(static_cast<int>(row));
            for (std::uint32_t column = 0U; column < width; ++column)
            {
                scanline[column * 3U] =
                    static_cast<std::uint8_t>(((column * 17U) + (row * 3U)) & 0xFFU);
                scanline[column * 3U + 1U] =
                    static_cast<std::uint8_t>(((column * 5U) + (row * 29U) + 41U) & 0xFFU);
                scanline[column * 3U + 2U] =
                    static_cast<std::uint8_t>(((column * 31U) + (row * 7U) + 113U) & 0xFFU);
            }
        }
        image.setColorSpace(QColorSpace(QColorSpace::SRgb));
        ASSERT_TRUE(image.save(QString::fromStdString(path.string()), "PNG"));
    }

    [[nodiscard]] CliRun run(std::vector<std::string> arguments)
    {
        arguments.emplace_back("--json");
        std::vector<std::string_view> views;
        views.reserve(arguments.size());
        for (const std::string &argument : arguments)
        {
            views.emplace_back(argument);
        }
        stdout_stream_.str({});
        stdout_stream_.clear();
        stderr_stream_.str({});
        stderr_stream_.clear();
        const int exit_code = application_->run(views);
        EXPECT_TRUE(stderr_stream_.str().empty());
        return {exit_code, parse_json(stdout_stream_.str())};
    }

    [[nodiscard]] std::string import_source(const std::filesystem::path &path)
    {
        const auto imported =
            run({"catalog", "import", "--catalog", catalog_, "--input", path.string()});
        EXPECT_EQ(imported.exit_code, 0) << stdout_stream_.str();
        const auto &items = required_child(required_child(imported.body.value(), "data"), "items");
        const auto &id = required_child(required_child(items.array_if()->front(), "asset"), "id");
        return *id.string_if();
    }

    [[nodiscard]] std::vector<std::string> export_arguments(const std::filesystem::path &output,
                                                            std::vector<std::string> options) const
    {
        std::vector<std::string> arguments{"catalog",    "export", "--catalog", catalog_,
                                           "--asset-id", asset_,   "--output",  output.string()};
        arguments.insert(arguments.end(), std::make_move_iterator(options.begin()),
                         std::make_move_iterator(options.end()));
        return arguments;
    }

    void expect_error(const CliRun &result, const int exit_code, const std::string_view code,
                      const std::string_view reason) const
    {
        ASSERT_EQ(result.exit_code, exit_code) << stdout_stream_.str();
        ASSERT_TRUE(result.body) << result.body.error().message;
        const auto &error = required_child(result.body.value(), "error");
        const auto &actual_code = required_child(error, "code");
        ASSERT_NE(actual_code.string_if(), nullptr);
        EXPECT_EQ(*actual_code.string_if(), code);
        const auto &context = required_child(error, "context");
        const auto &actual_reason = required_child(context, "reason");
        ASSERT_NE(actual_reason.string_if(), nullptr);
        EXPECT_EQ(*actual_reason.string_if(), reason);
    }

    EngineFacade engine_ = []
    {
        auto created = EngineFacade::create_phase1();
        return std::move(created).value();
    }();
    std::filesystem::path root_;
    std::filesystem::path source_;
    std::string catalog_;
    std::string asset_;
    QByteArray source_hash_;
    std::ostringstream stdout_stream_;
    std::ostringstream stderr_stream_;
    std::unique_ptr<CliApplication> application_;
};

TEST_F(JpegCliExportTest, DefaultsAndEverySubsamplingReachSofFactors)
{
    struct Case
    {
        std::string suffix;
        std::vector<std::string> options;
        std::uint8_t y_horizontal = 0U;
        std::uint8_t y_vertical = 0U;
        int quality = kDefaultJpegQuality;
    };
    const std::array cases{
        Case{"default", {"--format", "jpeg"}, 1U, 1U, 95},
        Case{"auto", {"--format", "jpeg", "--jpeg-subsampling", "auto"}, 1U, 1U, 95},
        Case{"444",
             {"--format", "jpeg", "--jpeg-subsampling", "444", "--quality", "85"},
             1U,
             1U,
             85},
        Case{"440",
             {"--jpeg-subsampling", "440", "--format", "jpeg", "--quality", "85"},
             1U,
             2U,
             85},
        Case{"422",
             {"--format", "jpeg", "--jpeg-subsampling", "422", "--quality", "85"},
             2U,
             1U,
             85},
        Case{"420",
             {"--format", "jpeg", "--jpeg-subsampling", "420", "--quality", "85"},
             2U,
             2U,
             85},
        Case{"last-value",
             {"--quality", "10", "--quality", "85", "--jpeg-subsampling", "420",
              "--jpeg-subsampling", "444", "--format", "jpeg"},
             1U,
             1U,
             85},
    };
    std::optional<std::uint16_t> default_quantizer;
    std::optional<std::uint16_t> low_quantizer;
    for (const Case &test_case : cases)
    {
        const auto output = root_ / (test_case.suffix + ".jpg");
        const auto result = run(export_arguments(output, test_case.options));
        ASSERT_EQ(result.exit_code, 0) << test_case.suffix << " " << stdout_stream_.str();
        const auto bytes = read_bytes(output);
        ASSERT_TRUE(bytes);
        const auto sampling = jpeg_sampling_factors(*bytes);
        ASSERT_TRUE(sampling) << test_case.suffix;
        EXPECT_EQ(sampling->y_horizontal, test_case.y_horizontal) << test_case.suffix;
        EXPECT_EQ(sampling->y_vertical, test_case.y_vertical) << test_case.suffix;
        EXPECT_EQ(sampling->cb_horizontal, 1U);
        EXPECT_EQ(sampling->cb_vertical, 1U);
        EXPECT_EQ(sampling->cr_horizontal, 1U);
        EXPECT_EQ(sampling->cr_vertical, 1U);
        const auto quantizer = jpeg_first_luminance_quantizer(*bytes);
        ASSERT_TRUE(quantizer) << test_case.suffix;
        if (test_case.suffix == "default")
        {
            default_quantizer = quantizer;
        }
        if (test_case.quality == 85)
        {
            if (!low_quantizer)
            {
                low_quantizer = quantizer;
            }
            EXPECT_EQ(*quantizer, *low_quantizer) << test_case.suffix;
        }
    }
    ASSERT_TRUE(default_quantizer);
    ASSERT_TRUE(low_quantizer);
    EXPECT_LT(*default_quantizer, *low_quantizer);

    const auto high_output = root_ / "quality-100.jpg";
    const auto high = run(export_arguments(
        high_output, {"--format", "jpeg", "--quality", "100", "--jpeg-subsampling", "444"}));
    ASSERT_EQ(high.exit_code, 0) << stdout_stream_.str();
    const auto high_bytes = read_bytes(high_output);
    ASSERT_TRUE(high_bytes);
    const auto high_quantizer = jpeg_first_luminance_quantizer(*high_bytes);
    ASSERT_TRUE(high_quantizer);
    EXPECT_LT(*high_quantizer, *default_quantizer);
}

TEST_F(JpegCliExportTest, JpegFlagsRequireAJpegExportBeforeCatalogOpen)
{
    struct Case
    {
        std::string suffix;
        std::vector<std::string> options;
    };
    const std::array cases{
        Case{"png", {"--format", "png", "--quality", "90"}},
        Case{"tiff", {"--jpeg-subsampling", "420", "--format", "tiff"}},
        Case{"original", {"--quality", "80", "--format", "original"}},
        Case{"implicit-png", {"--quality", "90"}},
    };
    for (const Case &test_case : cases)
    {
        const auto output = root_ / (test_case.suffix + ".out");
        const auto result = run(export_arguments(output, test_case.options));
        expect_error(result, 2, "invalid_argument", "jpeg_options_require_jpeg_export");
        EXPECT_FALSE(std::filesystem::exists(output));
    }

    const auto list = run({"catalog", "list", "--catalog", catalog_, "--jpeg-subsampling", "auto"});
    expect_error(list, 2, "invalid_argument", "jpeg_options_require_jpeg_export");

    const auto missing_catalog = root_ / "missing.sqlite";
    const auto isolated =
        run({"catalog", "list", "--catalog", missing_catalog.string(), "--quality", "90"});
    expect_error(isolated, 2, "invalid_argument", "jpeg_options_require_jpeg_export");
}

TEST_F(JpegCliExportTest, InvalidJpegOptionsKeepStructuredErrorsAndNoOutput)
{
    const auto unknown_output = root_ / "unknown.jpg";
    const auto unknown =
        run(export_arguments(unknown_output, {"--format", "jpeg", "--jpeg-subsampling", "4:2:0"}));
    expect_error(unknown, 4, "validation", "invalid_jpeg_subsampling");
    const auto &unknown_context =
        required_child(required_child(unknown.body.value(), "error"), "context");
    EXPECT_EQ(*required_child(unknown_context, "format").string_if(), "jpeg");
    EXPECT_EQ(*required_child(unknown_context, "option").string_if(), "--jpeg-subsampling");
    EXPECT_EQ(*required_child(unknown_context, "value").string_if(), "4:2:0");
    EXPECT_FALSE(std::filesystem::exists(unknown_output));

    const auto range_output = root_ / "range.jpg";
    const auto range = run(export_arguments(range_output, {"--format", "jpeg", "--quality", "4"}));
    expect_error(range, 4, "validation", "invalid_jpeg_quality");
    const auto &range_context =
        required_child(required_child(range.body.value(), "error"), "context");
    EXPECT_EQ(*required_child(range_context, "format").string_if(), "jpeg");
    EXPECT_EQ(*required_child(range_context, "option").string_if(), "--quality");
    EXPECT_EQ(*required_child(range_context, "value").string_if(), "4");
    EXPECT_FALSE(std::filesystem::exists(range_output));

    const auto syntax_output = root_ / "syntax.jpg";
    const auto syntax =
        run(export_arguments(syntax_output, {"--format", "jpeg", "--quality", "high"}));
    expect_error(syntax, 2, "invalid_argument", "invalid_jpeg_quality");
    const auto &syntax_context =
        required_child(required_child(syntax.body.value(), "error"), "context");
    EXPECT_EQ(*required_child(syntax_context, "format").string_if(), "jpeg");
    EXPECT_EQ(*required_child(syntax_context, "option").string_if(), "--quality");
    EXPECT_EQ(*required_child(syntax_context, "value").string_if(), "high");
    EXPECT_FALSE(std::filesystem::exists(syntax_output));

    const auto missing_output = root_ / "missing.jpg";
    const auto missing =
        run(export_arguments(missing_output, {"--format", "jpeg", "--jpeg-subsampling"}));
    ASSERT_EQ(missing.exit_code, 2) << stdout_stream_.str();
    EXPECT_FALSE(std::filesystem::exists(missing_output));
}

} // namespace
} // namespace ravo
