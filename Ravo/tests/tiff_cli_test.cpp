#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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

inline constexpr std::uint16_t kTagImageWidth = 256U;
inline constexpr std::uint16_t kTagImageLength = 257U;
inline constexpr std::uint16_t kTagBitsPerSample = 258U;
inline constexpr std::uint16_t kTagCompression = 259U;
inline constexpr std::uint16_t kTagPhotometric = 262U;
inline constexpr std::uint16_t kTagStripOffsets = 273U;
inline constexpr std::uint16_t kTagOrientation = 274U;
inline constexpr std::uint16_t kTagSamplesPerPixel = 277U;
inline constexpr std::uint16_t kTagRowsPerStrip = 278U;
inline constexpr std::uint16_t kTagStripByteCounts = 279U;
inline constexpr std::uint16_t kTagPlanarConfiguration = 284U;
inline constexpr std::uint16_t kTagPredictor = 317U;
inline constexpr std::uint16_t kTagSampleFormat = 339U;

struct TiffField
{
    std::uint16_t tag = 0U;
    std::uint16_t type = 0U;
    std::uint32_t count = 0U;
    std::vector<std::uint8_t> payload;
};

struct TiffDocument
{
    std::vector<std::uint8_t> bytes;
    std::vector<TiffField> fields;
};

struct DecodedTiff
{
    TiffDocument document;
    std::vector<std::uint8_t> pixels;
    std::vector<std::uint8_t> encoded_strips;
};

[[nodiscard]] std::uint16_t read_u16_le(const std::span<const std::uint8_t> bytes,
                                        const std::size_t offset)
{
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset]) |
                                      (static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U));
}

[[nodiscard]] std::uint32_t read_u32_le(const std::span<const std::uint8_t> bytes,
                                        const std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

[[nodiscard]] std::optional<std::size_t> tiff_type_size(const std::uint16_t type)
{
    switch (type)
    {
    case 1U:
    case 2U:
    case 6U:
    case 7U:
        return 1U;
    case 3U:
    case 8U:
        return 2U;
    case 4U:
    case 9U:
    case 11U:
        return 4U;
    case 5U:
    case 10U:
    case 12U:
        return 8U;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<TiffDocument>
parse_classic_little_endian_tiff(std::vector<std::uint8_t> bytes)
{
    if (bytes.size() < 8U || bytes[0] != 'I' || bytes[1] != 'I' || read_u16_le(bytes, 2U) != 42U)
    {
        return std::nullopt;
    }
    const std::uint32_t ifd_offset = read_u32_le(bytes, 4U);
    if (ifd_offset > bytes.size() || bytes.size() - ifd_offset < 2U)
    {
        return std::nullopt;
    }
    const std::uint16_t field_count = read_u16_le(bytes, ifd_offset);
    const std::uint64_t ifd_size = 2U + static_cast<std::uint64_t>(field_count) * 12U + 4U;
    if (ifd_size > bytes.size() - ifd_offset)
    {
        return std::nullopt;
    }

    TiffDocument document;
    document.bytes = std::move(bytes);
    document.fields.reserve(field_count);
    for (std::uint16_t index = 0U; index < field_count; ++index)
    {
        const std::size_t offset = static_cast<std::size_t>(ifd_offset) + 2U + index * 12U;
        TiffField field;
        field.tag = read_u16_le(document.bytes, offset);
        field.type = read_u16_le(document.bytes, offset + 2U);
        field.count = read_u32_le(document.bytes, offset + 4U);
        const auto element_size = tiff_type_size(field.type);
        if (!element_size ||
            field.count > std::numeric_limits<std::size_t>::max() / element_size.value())
        {
            return std::nullopt;
        }
        const std::size_t payload_size =
            static_cast<std::size_t>(field.count) * element_size.value();
        std::size_t payload_offset = offset + 8U;
        if (payload_size > 4U)
        {
            payload_offset = read_u32_le(document.bytes, offset + 8U);
        }
        if (payload_offset > document.bytes.size() ||
            payload_size > document.bytes.size() - payload_offset)
        {
            return std::nullopt;
        }
        field.payload.assign(document.bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset),
                             document.bytes.begin() +
                                 static_cast<std::ptrdiff_t>(payload_offset + payload_size));
        document.fields.push_back(std::move(field));
    }
    return document;
}

[[nodiscard]] const TiffField *unique_field(const TiffDocument &document, const std::uint16_t tag)
{
    const TiffField *result = nullptr;
    for (const TiffField &field : document.fields)
    {
        if (field.tag != tag)
        {
            continue;
        }
        if (result != nullptr)
        {
            return nullptr;
        }
        result = &field;
    }
    return result;
}

[[nodiscard]] std::optional<std::vector<std::uint32_t>>
unsigned_values(const TiffField *const field)
{
    if (field == nullptr)
    {
        return std::nullopt;
    }
    std::vector<std::uint32_t> values;
    values.reserve(field->count);
    if (field->type == 1U)
    {
        for (const std::uint8_t value : field->payload)
        {
            values.push_back(value);
        }
        return values;
    }
    if (field->type == 3U && field->payload.size() == static_cast<std::size_t>(field->count) * 2U)
    {
        for (std::size_t offset = 0U; offset < field->payload.size(); offset += 2U)
        {
            values.push_back(read_u16_le(field->payload, offset));
        }
        return values;
    }
    if (field->type == 4U && field->payload.size() == static_cast<std::size_t>(field->count) * 4U)
    {
        for (std::size_t offset = 0U; offset < field->payload.size(); offset += 4U)
        {
            values.push_back(read_u32_le(field->payload, offset));
        }
        return values;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::uint32_t> unsigned_scalar(const TiffDocument &document,
                                                           const std::uint16_t tag)
{
    const auto values = unsigned_values(unique_field(document, tag));
    if (!values || values->size() != 1U)
    {
        return std::nullopt;
    }
    return values->front();
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
    uLongf result_size = static_cast<uLongf>(result.size());
    const int status = uncompress(result.data(), &result_size, compressed.data(),
                                  static_cast<uLong>(compressed.size()));
    if (status != Z_OK || result_size != result.size())
    {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] std::optional<DecodedTiff> decode_tiff(TiffDocument document)
{
    const auto width = unsigned_scalar(document, kTagImageWidth);
    const auto height = unsigned_scalar(document, kTagImageLength);
    const auto samples = unsigned_scalar(document, kTagSamplesPerPixel);
    const auto rows_per_strip = unsigned_scalar(document, kTagRowsPerStrip);
    const auto compression = unsigned_scalar(document, kTagCompression);
    const auto predictor = unsigned_scalar(document, kTagPredictor).value_or(1U);
    const auto bits = unsigned_values(unique_field(document, kTagBitsPerSample));
    const auto offsets = unsigned_values(unique_field(document, kTagStripOffsets));
    const auto byte_counts = unsigned_values(unique_field(document, kTagStripByteCounts));
    if (!width || !height || !samples || !rows_per_strip || !compression || !bits || !offsets ||
        !byte_counts || width.value() == 0U || height.value() == 0U || samples.value() == 0U ||
        rows_per_strip.value() == 0U || offsets->size() != byte_counts->size() ||
        !std::all_of(bits->begin(), bits->end(),
                     [](const std::uint32_t value) { return value == 8U; }))
    {
        return std::nullopt;
    }
    const std::uint64_t row_bytes64 = static_cast<std::uint64_t>(width.value()) * samples.value();
    const std::uint64_t total_bytes64 = row_bytes64 * height.value();
    if (row_bytes64 > std::numeric_limits<std::size_t>::max() ||
        total_bytes64 > std::numeric_limits<std::size_t>::max())
    {
        return std::nullopt;
    }
    const std::size_t row_bytes = static_cast<std::size_t>(row_bytes64);
    DecodedTiff decoded;
    decoded.document = std::move(document);
    decoded.pixels.reserve(static_cast<std::size_t>(total_bytes64));
    for (std::size_t index = 0U; index < offsets->size(); ++index)
    {
        const std::size_t offset = offsets->at(index);
        const std::size_t byte_count = byte_counts->at(index);
        if (offset > decoded.document.bytes.size() ||
            byte_count > decoded.document.bytes.size() - offset)
        {
            return std::nullopt;
        }
        const std::uint32_t first_row = static_cast<std::uint32_t>(index) * rows_per_strip.value();
        if (first_row >= height.value())
        {
            return std::nullopt;
        }
        const std::uint32_t strip_rows =
            std::min(rows_per_strip.value(), height.value() - first_row);
        const std::size_t expected = row_bytes * strip_rows;
        const auto payload =
            std::span<const std::uint8_t>(decoded.document.bytes).subspan(offset, byte_count);
        decoded.encoded_strips.insert(decoded.encoded_strips.end(), payload.begin(), payload.end());
        if (compression.value() == 1U)
        {
            if (payload.size() != expected)
            {
                return std::nullopt;
            }
            decoded.pixels.insert(decoded.pixels.end(), payload.begin(), payload.end());
        }
        else if (compression.value() == 8U)
        {
            auto inflated = inflate_exact(payload, expected);
            if (!inflated)
            {
                return std::nullopt;
            }
            decoded.pixels.insert(decoded.pixels.end(), inflated->begin(), inflated->end());
        }
        else
        {
            return std::nullopt;
        }
    }
    if (decoded.pixels.size() != total_bytes64)
    {
        return std::nullopt;
    }
    if (predictor == 2U)
    {
        for (std::uint32_t row = 0U; row < height.value(); ++row)
        {
            const std::size_t row_offset = static_cast<std::size_t>(row) * row_bytes;
            for (std::size_t byte = samples.value(); byte < row_bytes; ++byte)
            {
                decoded.pixels[row_offset + byte] =
                    static_cast<std::uint8_t>(decoded.pixels[row_offset + byte] +
                                              decoded.pixels[row_offset + byte - samples.value()]);
            }
        }
    }
    else if (predictor != 1U)
    {
        return std::nullopt;
    }
    return decoded;
}

[[nodiscard]] std::optional<DecodedTiff> read_tiff(const std::filesystem::path &path)
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
    auto parsed = parse_classic_little_endian_tiff(std::move(bytes));
    if (!parsed)
    {
        return std::nullopt;
    }
    return decode_tiff(std::move(parsed).value());
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

class TiffCliTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        root_ = std::filesystem::temp_directory_path() / ("ravo-tiff-cli-" + generate_catalog_id());
        std::filesystem::create_directories(root_);
        catalog_ = (root_ / "library.sqlite").string();
        color_source_ = root_ / "color.png";
        neutral_source_ = root_ / "neutral.png";
        color_pixels_ = write_source(color_source_, 128U, 96U, false);
        neutral_pixels_ = write_source(neutral_source_, 32U, 24U, true);
        color_hash_ = file_hash(color_source_);
        neutral_hash_ = file_hash(neutral_source_);

        application_ = std::make_unique<CliApplication>(engine_, stdout_stream_, stderr_stream_);
        const auto created = run({"catalog", "create", "--path", catalog_});
        ASSERT_EQ(created.exit_code, 0) << stdout_stream_.str();
        ASSERT_TRUE(created.body) << created.body.error().message;
        color_asset_ = import_source(color_source_);
        neutral_asset_ = import_source(neutral_source_);
    }

    void TearDown() override
    {
        EXPECT_EQ(file_hash(color_source_), color_hash_);
        EXPECT_EQ(file_hash(neutral_source_), neutral_hash_);
        application_.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] std::vector<std::uint8_t> write_source(const std::filesystem::path &path,
                                                         const std::uint32_t width,
                                                         const std::uint32_t height,
                                                         const bool neutral)
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
                    static_cast<std::uint8_t>(((column / 4U) * 17U + (row / 3U) * 11U) & 0xFFU);
                const std::uint8_t green =
                    neutral ?
                        red :
                        static_cast<std::uint8_t>(((column / 7U) * 29U + row * 5U + 41U) & 0xFFU);
                const std::uint8_t blue =
                    neutral ?
                        red :
                        static_cast<std::uint8_t>((column * 3U + (row / 5U) * 37U + 113U) & 0xFFU);
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

    [[nodiscard]] std::vector<std::string> export_arguments(const std::string &asset_id,
                                                            const std::filesystem::path &output,
                                                            std::vector<std::string> options) const
    {
        std::vector<std::string> arguments{"catalog",    "export", "--catalog", catalog_,
                                           "--asset-id", asset_id, "--output",  output.string()};
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
    std::filesystem::path color_source_;
    std::filesystem::path neutral_source_;
    std::string catalog_;
    std::string color_asset_;
    std::string neutral_asset_;
    std::vector<std::uint8_t> color_pixels_;
    std::vector<std::uint8_t> neutral_pixels_;
    QByteArray color_hash_;
    QByteArray neutral_hash_;
    std::ostringstream stdout_stream_;
    std::ostringstream stderr_stream_;
    std::unique_ptr<CliApplication> application_;
};

TEST_F(TiffCliTest, DefaultsAndBothFormatSpellingsProduceCanonicalTagsAndPixels)
{
    for (const std::string_view format : std::array<std::string_view, 2U>{"tiff", "tif"})
    {
        const auto output = root_ / (std::string(format) + "-default.tif");
        const auto result =
            run(export_arguments(color_asset_, output, {"--format", std::string(format)}));
        ASSERT_EQ(result.exit_code, 0) << stdout_stream_.str();
        ASSERT_TRUE(result.body) << result.body.error().message;
        const auto &data = required_child(result.body.value(), "data");
        const auto &reported_format = required_child(data, "format");
        ASSERT_NE(reported_format.string_if(), nullptr);
        EXPECT_EQ(*reported_format.string_if(), "tiff");

        const auto decoded = read_tiff(output);
        ASSERT_TRUE(decoded);
        EXPECT_EQ(unsigned_scalar(decoded->document, kTagImageWidth), 128U);
        EXPECT_EQ(unsigned_scalar(decoded->document, kTagImageLength), 96U);
        EXPECT_EQ(unsigned_values(unique_field(decoded->document, kTagBitsPerSample)),
                  std::optional<std::vector<std::uint32_t>>({8U, 8U, 8U}));
        EXPECT_EQ(unsigned_scalar(decoded->document, kTagCompression), 8U);
        EXPECT_EQ(unsigned_scalar(decoded->document, kTagPredictor), 2U);
        EXPECT_EQ(unsigned_scalar(decoded->document, kTagSamplesPerPixel), 3U);
        EXPECT_EQ(unsigned_scalar(decoded->document, kTagPhotometric), 2U);
        EXPECT_EQ(unsigned_values(unique_field(decoded->document, kTagSampleFormat)),
                  std::optional<std::vector<std::uint32_t>>({1U, 1U, 1U}));
        EXPECT_EQ(unsigned_scalar(decoded->document, kTagOrientation), 1U);
        EXPECT_EQ(unsigned_scalar(decoded->document, kTagPlanarConfiguration), 1U);
        EXPECT_EQ(decoded->pixels, color_pixels_);
    }
}

TEST_F(TiffCliTest, ExplicitCompressionLevelsOrderAndValueDuplicatesReachTheEncoder)
{
    const auto uncompressed_output = root_ / "uncompressed.tif";
    const auto uncompressed = run(export_arguments(
        color_asset_, uncompressed_output,
        {"--tiff-compression", "deflate", "--tiff-compression", "none", "--tiff-compression-level",
         "9", "--tiff-compression-level", "1", "--tiff-sample-type", "uint16", "--tiff-sample-type",
         "uint8", "--format", "tiff"}));
    ASSERT_EQ(uncompressed.exit_code, 0) << stdout_stream_.str();
    const auto uncompressed_tiff = read_tiff(uncompressed_output);
    ASSERT_TRUE(uncompressed_tiff);
    EXPECT_EQ(unsigned_scalar(uncompressed_tiff->document, kTagCompression), 1U);
    EXPECT_FALSE(unsigned_scalar(uncompressed_tiff->document, kTagPredictor));
    EXPECT_EQ(uncompressed_tiff->pixels, color_pixels_);

    const auto deflate_one_output = root_ / "deflate-1.tif";
    const auto deflate_one = run(export_arguments(
        color_asset_, deflate_one_output,
        {"--format", "tiff", "--tiff-compression", "deflate", "--tiff-compression-level", "1"}));
    ASSERT_EQ(deflate_one.exit_code, 0) << stdout_stream_.str();
    const auto deflate_one_tiff = read_tiff(deflate_one_output);
    ASSERT_TRUE(deflate_one_tiff);
    EXPECT_EQ(unsigned_scalar(deflate_one_tiff->document, kTagCompression), 8U);
    EXPECT_EQ(unsigned_scalar(deflate_one_tiff->document, kTagPredictor), 1U);
    EXPECT_EQ(deflate_one_tiff->pixels, color_pixels_);

    const auto deflate_nine_output = root_ / "deflate-9.tif";
    const auto deflate_nine = run(export_arguments(
        color_asset_, deflate_nine_output,
        {"--tiff-compression-level", "9", "--tiff-compression", "deflate", "--format", "tif"}));
    ASSERT_EQ(deflate_nine.exit_code, 0) << stdout_stream_.str();
    const auto deflate_nine_tiff = read_tiff(deflate_nine_output);
    ASSERT_TRUE(deflate_nine_tiff);
    EXPECT_EQ(unsigned_scalar(deflate_nine_tiff->document, kTagCompression), 8U);
    EXPECT_EQ(unsigned_scalar(deflate_nine_tiff->document, kTagPredictor), 1U);
    EXPECT_EQ(deflate_nine_tiff->pixels, color_pixels_);
    EXPECT_NE(deflate_one_tiff->encoded_strips, deflate_nine_tiff->encoded_strips);

    const auto predictor_output = root_ / "predictor.tif";
    const auto predictor =
        run(export_arguments(color_asset_, predictor_output,
                             {"--tiff-compression", "deflate_predictor", "--tiff-compression-level",
                              "5", "--format", "tiff"}));
    ASSERT_EQ(predictor.exit_code, 0) << stdout_stream_.str();
    const auto predictor_tiff = read_tiff(predictor_output);
    ASSERT_TRUE(predictor_tiff);
    EXPECT_EQ(unsigned_scalar(predictor_tiff->document, kTagCompression), 8U);
    EXPECT_EQ(unsigned_scalar(predictor_tiff->document, kTagPredictor), 2U);
    EXPECT_EQ(predictor_tiff->pixels, color_pixels_);
}

TEST_F(TiffCliTest, ConditionalGrayscaleIsExplicitAndDuplicateToggleFails)
{
    const auto default_output = root_ / "neutral-rgb.tif";
    const auto defaults =
        run(export_arguments(neutral_asset_, default_output, {"--format", "tiff"}));
    ASSERT_EQ(defaults.exit_code, 0) << stdout_stream_.str();
    const auto default_tiff = read_tiff(default_output);
    ASSERT_TRUE(default_tiff);
    EXPECT_EQ(unsigned_scalar(default_tiff->document, kTagSamplesPerPixel), 3U);
    EXPECT_EQ(default_tiff->pixels, neutral_pixels_);

    const auto grayscale_output = root_ / "neutral-grayscale.tif";
    const auto grayscale = run(export_arguments(
        neutral_asset_, grayscale_output, {"--tiff-grayscale-if-neutral", "--format", "tif"}));
    ASSERT_EQ(grayscale.exit_code, 0) << stdout_stream_.str();
    const auto grayscale_tiff = read_tiff(grayscale_output);
    ASSERT_TRUE(grayscale_tiff);
    EXPECT_EQ(unsigned_scalar(grayscale_tiff->document, kTagSamplesPerPixel), 1U);
    EXPECT_EQ(unsigned_scalar(grayscale_tiff->document, kTagPhotometric), 1U);
    std::vector<std::uint8_t> expected_grayscale;
    expected_grayscale.reserve(neutral_pixels_.size() / 3U);
    for (std::size_t offset = 0U; offset < neutral_pixels_.size(); offset += 3U)
    {
        expected_grayscale.push_back(neutral_pixels_[offset]);
    }
    EXPECT_EQ(grayscale_tiff->pixels, expected_grayscale);

    const auto color_output = root_ / "color-stays-rgb.tif";
    const auto color = run(export_arguments(color_asset_, color_output,
                                            {"--format", "tiff", "--tiff-grayscale-if-neutral"}));
    ASSERT_EQ(color.exit_code, 0) << stdout_stream_.str();
    const auto color_tiff = read_tiff(color_output);
    ASSERT_TRUE(color_tiff);
    EXPECT_EQ(unsigned_scalar(color_tiff->document, kTagSamplesPerPixel), 3U);
    EXPECT_EQ(color_tiff->pixels, color_pixels_);

    const auto duplicate_output = root_ / "duplicate-grayscale.tif";
    const auto duplicate = run(export_arguments(
        neutral_asset_, duplicate_output,
        {"--format", "tiff", "--tiff-grayscale-if-neutral", "--tiff-grayscale-if-neutral"}));
    EXPECT_EQ(duplicate.exit_code, 2);
    EXPECT_FALSE(std::filesystem::exists(duplicate_output));
}

TEST_F(TiffCliTest, HighPrecisionNamesReachExistingStructuredUnsupportedBoundary)
{
    for (const std::string_view sample_type :
         std::array<std::string_view, 3U>{"uint16", "float16", "float32"})
    {
        const auto output = root_ / (std::string(sample_type) + ".tif");
        const auto result = run(
            export_arguments(color_asset_, output,
                             {"--tiff-sample-type", std::string(sample_type), "--format", "tiff"}));
        expect_error(result, 5, "unsupported", "unsupported_tiff_high_precision_source");
        const auto &context =
            required_child(required_child(result.body.value(), "error"), "context");
        const auto &actual_sample_type = required_child(context, "sample_type");
        ASSERT_NE(actual_sample_type.string_if(), nullptr);
        EXPECT_EQ(*actual_sample_type.string_if(), sample_type);
        EXPECT_FALSE(std::filesystem::exists(output));
    }
}

TEST_F(TiffCliTest, InvalidValuesPreserveCanonicalValidationAndPublishNothing)
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
        Case{"sample",
             {"--format", "tiff", "--tiff-sample-type", "u8"},
             4,
             "validation",
             "invalid_tiff_sample_type"},
        Case{"compression",
             {"--format", "tiff", "--tiff-compression", "deflate-predictor"},
             4,
             "validation",
             "invalid_tiff_compression"},
        Case{"level-low",
             {"--format", "tiff", "--tiff-compression-level", "0"},
             4,
             "validation",
             "invalid_tiff_compression_level"},
        Case{"level-high",
             {"--format", "tiff", "--tiff-compression-level", "10"},
             4,
             "validation",
             "invalid_tiff_compression_level"},
    };
    for (const Case &test_case : cases)
    {
        const auto output = root_ / ("invalid-" + test_case.suffix + ".tif");
        const auto result = run(export_arguments(color_asset_, output, test_case.options));
        expect_error(result, test_case.exit_code, test_case.code, test_case.reason);
        EXPECT_FALSE(std::filesystem::exists(output));
    }

    const auto integer_output = root_ / "invalid-integer.tif";
    const auto integer = run(export_arguments(
        color_asset_, integer_output, {"--format", "tiff", "--tiff-compression-level", "six"}));
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
    EXPECT_EQ(*option.string_if(), "--tiff-compression-level");
    EXPECT_EQ(*value.string_if(), "six");
    EXPECT_FALSE(std::filesystem::exists(integer_output));

    const auto missing_output = root_ / "missing-value.tif";
    const auto missing = run(
        export_arguments(color_asset_, missing_output, {"--format", "tiff", "--tiff-compression"}));
    ASSERT_EQ(missing.exit_code, 2) << stdout_stream_.str();
    EXPECT_FALSE(std::filesystem::exists(missing_output));
}

TEST_F(TiffCliTest, ExplicitTiffFlagsRequireATiffExportRegardlessOfOrder)
{
    struct Case
    {
        std::string suffix;
        std::vector<std::string> options;
    };
    const std::array cases{
        Case{"png", {"--format", "png", "--tiff-compression", "none"}},
        Case{"jpeg", {"--tiff-compression-level", "9", "--format", "jpeg"}},
        Case{"implicit-png", {"--tiff-sample-type", "uint8"}},
    };
    for (const Case &test_case : cases)
    {
        const auto output = root_ / (test_case.suffix + ".out");
        const auto result = run(export_arguments(color_asset_, output, test_case.options));
        expect_error(result, 2, "invalid_argument", "tiff_options_require_tiff_export");
        EXPECT_FALSE(std::filesystem::exists(output));
    }

    const auto list =
        run({"catalog", "list", "--catalog", catalog_, "--tiff-grayscale-if-neutral"});
    expect_error(list, 2, "invalid_argument", "tiff_options_require_tiff_export");
}

} // namespace
} // namespace ravo

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    ravo::init_logging("ravo-tiff-cli-tests");
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ravo::shutdown_logging();
    return result;
}
