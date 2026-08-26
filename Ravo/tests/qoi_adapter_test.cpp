#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QIODevice>
#include <gtest/gtest.h>

#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"

namespace ravo
{
namespace
{

class QoiTempDirectory
{
public:
    QoiTempDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("ravo-qoi-adapter-" + generate_catalog_id()))
    {
        std::filesystem::create_directories(path_);
    }

    ~QoiTempDirectory()
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

void append_u32_be(QByteArray &bytes, const std::uint32_t value)
{
    bytes.append(static_cast<char>((value >> 24U) & 0xFFU));
    bytes.append(static_cast<char>((value >> 16U) & 0xFFU));
    bytes.append(static_cast<char>((value >> 8U) & 0xFFU));
    bytes.append(static_cast<char>(value & 0xFFU));
}

[[nodiscard]] QByteArray minimal_qoi(const std::uint8_t channels = 3U,
                                     const std::uint8_t colorspace = 0U)
{
    QByteArray bytes("qoif", 4);
    append_u32_be(bytes, 1U);
    append_u32_be(bytes, 1U);
    bytes.append(static_cast<char>(channels));
    bytes.append(static_cast<char>(colorspace));
    if (channels == 4U)
    {
        bytes.append(static_cast<char>(0xFFU));
        bytes.append(static_cast<char>(0x12U));
        bytes.append(static_cast<char>(0x34U));
        bytes.append(static_cast<char>(0x56U));
        bytes.append(static_cast<char>(0x78U));
    }
    else
    {
        bytes.append(static_cast<char>(0xFEU));
        bytes.append(static_cast<char>(0x12U));
        bytes.append(static_cast<char>(0x34U));
        bytes.append(static_cast<char>(0x56U));
    }
    bytes.append(QByteArray(7, '\0'));
    bytes.append('\1');
    return bytes;
}

[[nodiscard]] std::vector<std::uint8_t> vector_bytes(const QByteArray &bytes)
{
    return {reinterpret_cast<const std::uint8_t *>(bytes.constData()),
            reinterpret_cast<const std::uint8_t *>(bytes.constData()) + bytes.size()};
}

void write_file(const std::filesystem::path &path, const QByteArray &bytes)
{
    QFile file(QString::fromStdString(path.string()));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(file.write(bytes), bytes.size());
    file.close();
}

[[nodiscard]] QByteArray read_file(const std::filesystem::path &path)
{
    QFile file(QString::fromStdString(path.string()));
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

template <typename Value>
void expect_qoi_unsupported(const Result<Value> &result, const std::string_view source)
{
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::kUnsupported);
    ASSERT_TRUE(result.error().context.contains("format"));
    EXPECT_EQ(result.error().context.at("format"), "qoi");
    ASSERT_TRUE(result.error().context.contains("reason"));
    EXPECT_EQ(result.error().context.at("reason"), "unsupported_qoi_input");
    ASSERT_TRUE(result.error().context.contains("source"));
    EXPECT_EQ(result.error().context.at("source"), source);
}

template <typename Value>
void expect_not_qoi(const Result<Value> &result)
{
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::kUnsupported);
    const auto format = result.error().context.find("format");
    EXPECT_TRUE(format == result.error().context.end() || format->second != "qoi");
}

TEST(QoiAdapterTest, ReportsStableUnsupportedForPathAndMemory)
{
    QoiTempDirectory temporary;
    QtRasterDecoder decoder;

    const auto path = temporary.path() / "content-mislabeled.png";
    const QByteArray encoded = minimal_qoi(3U, 0U);
    write_file(path, encoded);

    expect_qoi_unsupported(decoder.probe(path.string()), path.string());
    expect_qoi_unsupported(decoder.decode(path.string(), 64U, CancellationToken{}), path.string());
    expect_qoi_unsupported(decoder.decode_memory(vector_bytes(encoded), 64U, CancellationToken{}),
                           "memory");

    const QByteArray rgba_linear = minimal_qoi(4U, 1U);
    expect_qoi_unsupported(
        decoder.decode_memory(vector_bytes(rgba_linear), 0U, CancellationToken{}, 3), "memory");
}

TEST(QoiAdapterTest, MagicRecognitionDoesNotClaimToValidateQoiPayloads)
{
    QoiTempDirectory temporary;
    QtRasterDecoder decoder;
    for (const QByteArray &payload :
         {QByteArray("qoif", 4), QByteArray("qoif-invalid-header", 19), minimal_qoi()})
    {
        expect_qoi_unsupported(
            decoder.decode_memory(vector_bytes(payload), 0U, CancellationToken{}), "memory");
    }

    for (const QByteArray &payload : {QByteArray("qo", 3), QByteArray("random", 6)})
    {
        expect_not_qoi(decoder.decode_memory(vector_bytes(payload), 0U, CancellationToken{}));
    }

    const auto extension_only = temporary.path() / "extension-only.qoi";
    write_file(extension_only, QByteArray("random", 6));
    expect_not_qoi(decoder.probe(extension_only.string()));
    expect_not_qoi(decoder.decode(extension_only.string(), 0U, CancellationToken{}));
}

TEST(QoiAdapterTest, PrioritizesCancellationAndKeepsSourceImmutable)
{
    QoiTempDirectory temporary;
    QtRasterDecoder decoder;
    const auto path = temporary.path() / "cancel.qoi";
    const QByteArray encoded = minimal_qoi(4U, 0U);
    write_file(path, encoded);
    const QByteArray hash_before = QCryptographicHash::hash(encoded, QCryptographicHash::Sha256);
    const auto size_before = std::filesystem::file_size(path);
    const auto mtime_before = std::filesystem::last_write_time(path);

    CancellationSource cancelled;
    ASSERT_TRUE(cancelled.cancel("qoi test"));
    const auto from_path = decoder.decode(path.string(), 0U, cancelled.token());
    ASSERT_FALSE(from_path);
    EXPECT_EQ(from_path.error().code, ErrorCode::kCancelled);
    const auto from_memory = decoder.decode_memory(vector_bytes(encoded), 0U, cancelled.token());
    ASSERT_FALSE(from_memory);
    EXPECT_EQ(from_memory.error().code, ErrorCode::kCancelled);

    EXPECT_EQ(QCryptographicHash::hash(read_file(path), QCryptographicHash::Sha256), hash_before);
    EXPECT_EQ(std::filesystem::file_size(path), size_before);
    EXPECT_EQ(std::filesystem::last_write_time(path), mtime_before);
}

TEST(QoiAdapterTest, RejectsSparsePathFromMagicWithoutReadingTheWholeFile)
{
    QoiTempDirectory temporary;
    QtRasterDecoder decoder;
    const auto path = temporary.path() / "large.qoi";
    QFile file(QString::fromStdString(path.string()));
    ASSERT_TRUE(file.open(QIODevice::ReadWrite | QIODevice::Truncate));
    ASSERT_EQ(file.write("qoif", 4), 4);
    constexpr qint64 kSparseSize = 2LL * 1024LL * 1024LL * 1024LL + 4096LL;
    ASSERT_TRUE(file.resize(kSparseSize));
    file.close();
    const auto size_before = std::filesystem::file_size(path);
    const auto mtime_before = std::filesystem::last_write_time(path);

    expect_qoi_unsupported(decoder.probe(path.string()), path.string());
    expect_qoi_unsupported(decoder.decode(path.string(), 0U, CancellationToken{}), path.string());

    EXPECT_EQ(std::filesystem::file_size(path), size_before);
    EXPECT_EQ(std::filesystem::last_write_time(path), mtime_before);
    QFile verify(QString::fromStdString(path.string()));
    ASSERT_TRUE(verify.open(QIODevice::ReadOnly));
    EXPECT_EQ(verify.read(4), QByteArray("qoif", 4));
}

} // namespace
} // namespace ravo

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
