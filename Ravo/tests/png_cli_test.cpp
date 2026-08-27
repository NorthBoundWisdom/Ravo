#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <zlib.h>

#include <QColorSpace>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QIODevice>
#include <gtest/gtest.h>

#include "ravo/cli/application.h"
#include "ravo/domain/types.h"
#include "ravo/engine/engine.h"
#include "ravo/foundation/json.h"
#include "ravo/foundation/log.h"

namespace ravo
{
namespace
{

inline constexpr std::array<std::uint8_t, 8U> kPngSignature{0x89U, 'P',   'N',   'G',
                                                            0x0DU, 0x0AU, 0x1AU, 0x0AU};

struct PngChunk
{
    std::array<char, 4U> type{};
    std::vector<std::uint8_t> payload;
};

struct PngIhdr
{
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint8_t bit_depth = 0U;
    std::uint8_t color_type = 0U;
    std::uint8_t compression = 0U;
    std::uint8_t filter = 0U;
    std::uint8_t interlace = 0U;
};

struct DecodedPng
{
    PngIhdr header;
    std::vector<PngChunk> chunks;
    std::vector<std::uint8_t> pixels;
    std::vector<std::uint8_t> idat;
};

[[nodiscard]] std::uint32_t read_u32_be(const std::span<const std::uint8_t> bytes,
                                        const std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3U]);
}

[[nodiscard]] std::optional<std::vector<PngChunk>>
png_chunks(const std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < kPngSignature.size() ||
        !std::equal(kPngSignature.begin(), kPngSignature.end(), bytes.begin()))
    {
        return std::nullopt;
    }
    std::vector<PngChunk> result;
    std::size_t offset = kPngSignature.size();
    bool ended = false;
    while (offset < bytes.size())
    {
        if (bytes.size() - offset < 12U)
        {
            return std::nullopt;
        }
        const std::uint32_t length = read_u32_be(bytes, offset);
        offset += 4U;
        if (length > bytes.size() - offset - 8U)
        {
            return std::nullopt;
        }
        PngChunk chunk;
        std::copy_n(reinterpret_cast<const char *>(bytes.data() + offset), 4U, chunk.type.begin());
        offset += 4U;
        chunk.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                             bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
        offset += length + 4U;
        ended = chunk.type == std::array<char, 4U>{'I', 'E', 'N', 'D'};
        result.push_back(std::move(chunk));
        if (ended)
        {
            break;
        }
    }
    if (!ended || offset != bytes.size())
    {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] std::vector<const PngChunk *> chunks_named(const std::vector<PngChunk> &chunks,
                                                         const std::array<char, 4U> &type)
{
    std::vector<const PngChunk *> result;
    for (const PngChunk &chunk : chunks)
    {
        if (chunk.type == type)
        {
            result.push_back(&chunk);
        }
    }
    return result;
}

[[nodiscard]] std::optional<PngIhdr> png_ihdr(const std::vector<PngChunk> &chunks)
{
    const auto headers = chunks_named(chunks, {'I', 'H', 'D', 'R'});
    if (headers.size() != 1U || headers.front()->payload.size() != 13U)
    {
        return std::nullopt;
    }
    const auto bytes = std::span<const std::uint8_t>(headers.front()->payload);
    return PngIhdr{read_u32_be(bytes, 0U),
                   read_u32_be(bytes, 4U),
                   bytes[8U],
                   bytes[9U],
                   bytes[10U],
                   bytes[11U],
                   bytes[12U]};
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
inflate_exact(const std::span<const std::uint8_t> compressed, const std::size_t expected_size)
{
    if (compressed.size() > static_cast<std::size_t>(std::numeric_limits<uLong>::max()) ||
        expected_size > static_cast<std::size_t>(std::numeric_limits<uLongf>::max()))
    {
        return std::nullopt;
    }
    std::vector<std::uint8_t> result(expected_size);
    uLongf size = static_cast<uLongf>(result.size());
    const int status =
        uncompress(result.data(), &size, compressed.data(), static_cast<uLong>(compressed.size()));
    if (status != Z_OK || size != result.size())
    {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] std::uint8_t paeth_predictor(const std::uint8_t left, const std::uint8_t above,
                                           const std::uint8_t upper_left)
{
    const int prediction = static_cast<int>(left) + static_cast<int>(above) - upper_left;
    const int left_distance = std::abs(prediction - left);
    const int above_distance = std::abs(prediction - above);
    const int upper_left_distance = std::abs(prediction - upper_left);
    if (left_distance <= above_distance && left_distance <= upper_left_distance)
    {
        return left;
    }
    if (above_distance <= upper_left_distance)
    {
        return above;
    }
    return upper_left;
}

[[nodiscard]] std::optional<DecodedPng> decode_png(const std::vector<std::uint8_t> &bytes)
{
    const auto chunks = png_chunks(bytes);
    if (!chunks)
    {
        return std::nullopt;
    }
    const auto header = png_ihdr(chunks.value());
    if (!header || header->bit_depth != 8U || header->color_type != 2U || header->interlace != 0U)
    {
        return std::nullopt;
    }
    std::vector<std::uint8_t> compressed;
    for (const PngChunk *chunk : chunks_named(chunks.value(), {'I', 'D', 'A', 'T'}))
    {
        compressed.insert(compressed.end(), chunk->payload.begin(), chunk->payload.end());
    }
    const std::size_t stride = static_cast<std::size_t>(header->width) * 3U;
    const std::size_t packed_size = (stride + 1U) * header->height;
    auto packed = inflate_exact(compressed, packed_size);
    if (!packed)
    {
        return std::nullopt;
    }
    DecodedPng decoded;
    decoded.header = header.value();
    decoded.chunks = chunks.value();
    decoded.idat = std::move(compressed);
    decoded.pixels.resize(stride * header->height);
    for (std::uint32_t row = 0U; row < header->height; ++row)
    {
        const std::size_t packed_offset = static_cast<std::size_t>(row) * (stride + 1U);
        const std::uint8_t filter = packed.value()[packed_offset];
        if (filter > 4U)
        {
            return std::nullopt;
        }
        for (std::size_t column = 0U; column < stride; ++column)
        {
            const std::uint8_t encoded = packed.value()[packed_offset + 1U + column];
            const std::uint8_t left =
                column >= 3U ?
                    decoded.pixels[static_cast<std::size_t>(row) * stride + column - 3U] :
                    0U;
            const std::uint8_t above =
                row > 0U ? decoded.pixels[(static_cast<std::size_t>(row) - 1U) * stride + column] :
                           0U;
            const std::uint8_t upper_left =
                row > 0U && column >= 3U ?
                    decoded.pixels[(static_cast<std::size_t>(row) - 1U) * stride + column - 3U] :
                    0U;
            std::uint8_t value = encoded;
            switch (filter)
            {
            case 0U:
                break;
            case 1U:
                value = static_cast<std::uint8_t>(encoded + left);
                break;
            case 2U:
                value = static_cast<std::uint8_t>(encoded + above);
                break;
            case 3U:
                value = static_cast<std::uint8_t>(encoded +
                                                  ((static_cast<unsigned int>(left) + above) / 2U));
                break;
            case 4U:
                value =
                    static_cast<std::uint8_t>(encoded + paeth_predictor(left, above, upper_left));
                break;
            default:
                return std::nullopt;
            }
            decoded.pixels[static_cast<std::size_t>(row) * stride + column] = value;
        }
    }
    return decoded;
}

[[nodiscard]] std::optional<DecodedPng> read_png(const std::filesystem::path &path)
{
    QFile file(QString::fromStdString(path.string()));
    if (!file.open(QIODevice::ReadOnly))
    {
        return std::nullopt;
    }
    const QByteArray encoded = file.readAll();
    std::vector<std::uint8_t> bytes(reinterpret_cast<const std::uint8_t *>(encoded.constData()),
                                    reinterpret_cast<const std::uint8_t *>(encoded.constData()) +
                                        encoded.size());
    return decode_png(bytes);
}

[[nodiscard]] QByteArray file_hash(const std::filesystem::path &path)
{
    QFile file(QString::fromStdString(path.string()));
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

[[nodiscard]] const JsonValue &required_child(const JsonValue &value, const std::string_view key)
{
    const auto *child = value.find(key);
    EXPECT_NE(child, nullptr) << key;
    static const JsonValue missing;
    return child == nullptr ? missing : *child;
}

struct CliRun
{
    int exit_code = 0;
    Result<JsonValue> body;
};

class PngCliTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        root_ = std::filesystem::temp_directory_path() / ("ravo-png-cli-" + generate_catalog_id());
        std::filesystem::create_directories(root_);
        catalog_ = (root_ / "library.sqlite").string();
        source_ = root_ / "color.png";
        pixels_ = write_source(source_, 48U, 32U);
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

    [[nodiscard]] std::vector<std::uint8_t> write_source(const std::filesystem::path &path,
                                                         const std::uint32_t width,
                                                         const std::uint32_t height)
    {
        QImage image(static_cast<int>(width), static_cast<int>(height), QImage::Format_RGB888);
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 3U);
        for (std::uint32_t row = 0U; row < height; ++row)
        {
            auto *const scanline = image.scanLine(static_cast<int>(row));
            for (std::uint32_t column = 0U; column < width; ++column)
            {
                const std::size_t offset = (static_cast<std::size_t>(row) * width + column) * 3U;
                const std::uint8_t red =
                    static_cast<std::uint8_t>(((column * 17U) + (row * 3U)) & 0xFFU);
                const std::uint8_t green =
                    static_cast<std::uint8_t>(((column * 5U) + (row * 29U) + 41U) & 0xFFU);
                const std::uint8_t blue =
                    static_cast<std::uint8_t>(((column * 31U) + (row * 7U) + 113U) & 0xFFU);
                pixels[offset] = red;
                pixels[offset + 1U] = green;
                pixels[offset + 2U] = blue;
                scanline[column * 3U] = red;
                scanline[column * 3U + 1U] = green;
                scanline[column * 3U + 2U] = blue;
            }
        }
        image.setColorSpace(QColorSpace(QColorSpace::SRgb));
        EXPECT_TRUE(image.save(QString::fromStdString(path.string()), "PNG"));
        return pixels;
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
        if (!imported.body)
        {
            ADD_FAILURE() << imported.body.error().message;
            return {};
        }
        const auto &items = required_child(required_child(imported.body.value(), "data"), "items");
        if (items.array_if() == nullptr || items.array_if()->size() != 1U)
        {
            ADD_FAILURE() << "Expected one imported item";
            return {};
        }
        const auto &item = items.array_if()->front();
        const auto &status = required_child(item, "status");
        const auto &id = required_child(required_child(item, "asset"), "id");
        if (status.string_if() == nullptr || id.string_if() == nullptr)
        {
            ADD_FAILURE() << "Import response is incomplete";
            return {};
        }
        EXPECT_EQ(*status.string_if(), "imported");
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
    std::vector<std::uint8_t> pixels_;
    QByteArray source_hash_;
    std::ostringstream stdout_stream_;
    std::ostringstream stderr_stream_;
    std::unique_ptr<CliApplication> application_;
};

TEST_F(PngCliTest, DefaultsAndExplicitEightBitPreserveCanonicalRgb8)
{
    for (const auto &[suffix, options] :
         std::array<std::pair<std::string_view, std::vector<std::string>>, 3U>{
             {{"implicit", {}},
              {"explicit-png", {"--format", "png"}},
              {"explicit-defaults",
               {"--png-bit-depth", "16", "--png-bit-depth", "8", "--png-compression", "9",
                "--png-compression", "5", "--format", "png"}}}})
    {
        const auto output = root_ / (std::string(suffix) + ".png");
        const auto result = run(export_arguments(output, options));
        ASSERT_EQ(result.exit_code, 0) << stdout_stream_.str();
        ASSERT_TRUE(result.body) << result.body.error().message;
        const auto &data = required_child(result.body.value(), "data");
        const auto &reported_format = required_child(data, "format");
        ASSERT_NE(reported_format.string_if(), nullptr);
        EXPECT_EQ(*reported_format.string_if(), "png");
        const auto decoded = read_png(output);
        ASSERT_TRUE(decoded);
        EXPECT_EQ(decoded->header.bit_depth, 8U);
        EXPECT_EQ(decoded->header.color_type, 2U);
        EXPECT_EQ(decoded->header.interlace, 0U);
        EXPECT_TRUE(chunks_named(decoded->chunks, {'e', 'X', 'I', 'f'}).empty());
        EXPECT_TRUE(chunks_named(decoded->chunks, {'p', 'H', 'Y', 's'}).empty());
        EXPECT_EQ(decoded->pixels, pixels_);
    }
}

TEST_F(PngCliTest, CompressionLevelsOrderAndValueDuplicatesReachTheEncoder)
{
    const auto uncompressed_output = root_ / "compression-0.png";
    const auto uncompressed =
        run(export_arguments(uncompressed_output, {"--png-compression", "9", "--png-compression",
                                                   "0", "--format", "png"}));
    ASSERT_EQ(uncompressed.exit_code, 0) << stdout_stream_.str();
    const auto uncompressed_png = read_png(uncompressed_output);
    ASSERT_TRUE(uncompressed_png);
    EXPECT_EQ(uncompressed_png->pixels, pixels_);

    const auto compressed_output = root_ / "compression-9.png";
    const auto compressed =
        run(export_arguments(compressed_output, {"--format", "png", "--png-compression", "9"}));
    ASSERT_EQ(compressed.exit_code, 0) << stdout_stream_.str();
    const auto compressed_png = read_png(compressed_output);
    ASSERT_TRUE(compressed_png);
    EXPECT_EQ(compressed_png->pixels, pixels_);
    EXPECT_NE(uncompressed_png->idat, compressed_png->idat);
}

TEST_F(PngCliTest, SixteenBitNameReachesExistingStructuredUnsupportedBoundary)
{
    const auto output = root_ / "sixteen.png";
    const auto result = run(export_arguments(output, {"--png-bit-depth", "16", "--format", "png"}));
    expect_error(result, 5, "unsupported", "unsupported_png_16bit_source");
    const auto &context = required_child(required_child(result.body.value(), "error"), "context");
    const auto &requested = required_child(context, "requested_bit_depth");
    ASSERT_NE(requested.string_if(), nullptr);
    EXPECT_EQ(*requested.string_if(), "16");
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST_F(PngCliTest, InvalidValuesPreserveCanonicalValidationAndPublishNothing)
{
    struct Case
    {
        std::string suffix;
        std::vector<std::string> options;
        int exit_code = 0;
        std::string code;
        std::string reason;
    };
    const std::array cases{
        Case{"bit-depth",
             {"--format", "png", "--png-bit-depth", "8-bit"},
             4,
             "validation",
             "invalid_png_bit_depth"},
        Case{"compression-low",
             {"--format", "png", "--png-compression", "-1"},
             4,
             "validation",
             "invalid_png_compression"},
        Case{"compression-high",
             {"--format", "png", "--png-compression", "10"},
             4,
             "validation",
             "invalid_png_compression"},
    };
    for (const Case &test_case : cases)
    {
        const auto output = root_ / ("invalid-" + test_case.suffix + ".png");
        const auto result = run(export_arguments(output, test_case.options));
        expect_error(result, test_case.exit_code, test_case.code, test_case.reason);
        EXPECT_FALSE(std::filesystem::exists(output));
    }

    const auto integer_output = root_ / "invalid-integer.png";
    const auto integer =
        run(export_arguments(integer_output, {"--format", "png", "--png-compression", "six"}));
    ASSERT_EQ(integer.exit_code, 2) << stdout_stream_.str();
    ASSERT_TRUE(integer.body) << integer.body.error().message;
    const auto &integer_error = required_child(integer.body.value(), "error");
    const auto &integer_code = required_child(integer_error, "code");
    ASSERT_NE(integer_code.string_if(), nullptr);
    EXPECT_EQ(*integer_code.string_if(), "invalid_argument");
    const auto &integer_context = required_child(integer_error, "context");
    const auto &option = required_child(integer_context, "option");
    const auto &value = required_child(integer_context, "value");
    ASSERT_NE(option.string_if(), nullptr);
    ASSERT_NE(value.string_if(), nullptr);
    EXPECT_EQ(*option.string_if(), "--png-compression");
    EXPECT_EQ(*value.string_if(), "six");
    EXPECT_FALSE(std::filesystem::exists(integer_output));

    const auto missing_output = root_ / "missing-value.png";
    const auto missing =
        run(export_arguments(missing_output, {"--format", "png", "--png-bit-depth"}));
    ASSERT_EQ(missing.exit_code, 2) << stdout_stream_.str();
    EXPECT_FALSE(std::filesystem::exists(missing_output));
}

TEST_F(PngCliTest, ExplicitPngFlagsRequireAPngExportRegardlessOfOrder)
{
    struct Case
    {
        std::string suffix;
        std::vector<std::string> options;
    };
    const std::array cases{
        Case{"jpeg", {"--format", "jpeg", "--png-compression", "5"}},
        Case{"tiff", {"--png-bit-depth", "8", "--format", "tiff"}},
        Case{"original", {"--png-compression", "0", "--format", "original"}},
    };
    for (const Case &test_case : cases)
    {
        const auto output = root_ / (test_case.suffix + ".out");
        const auto result = run(export_arguments(output, test_case.options));
        expect_error(result, 2, "invalid_argument", "png_options_require_png_export");
        EXPECT_FALSE(std::filesystem::exists(output));
    }

    const auto list = run({"catalog", "list", "--catalog", catalog_, "--png-bit-depth", "8"});
    expect_error(list, 2, "invalid_argument", "png_options_require_png_export");
}

} // namespace
} // namespace ravo

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    ravo::init_logging("ravo-png-cli-tests");
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ravo::shutdown_logging();
    return result;
}
