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

class HeicTempDirectory
{
public:
    HeicTempDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("ravo-heic-adapter-" + generate_catalog_id()))
    {
        std::filesystem::create_directories(path_);
    }

    ~HeicTempDirectory()
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

[[nodiscard]] QByteArray
minimal_heic_ftyp(const QByteArray &major_brand = QByteArrayLiteral("heic"),
                  const QByteArray &compatible = QByteArrayLiteral("mif1"))
{
    QByteArray brands = major_brand;
    brands.append(QByteArray(4, '\0')); // minor version
    brands.append(compatible);
    const std::uint32_t box_size = static_cast<std::uint32_t>(8U + brands.size());
    QByteArray bytes;
    append_u32_be(bytes, box_size);
    bytes.append(QByteArrayLiteral("ftyp"));
    bytes.append(brands);
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
void expect_heic_unsupported(const Result<Value> &result, const std::string_view source)
{
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::kUnsupported);
    ASSERT_TRUE(result.error().context.contains("format"));
    EXPECT_EQ(result.error().context.at("format"), "heic");
    ASSERT_TRUE(result.error().context.contains("reason"));
    EXPECT_EQ(result.error().context.at("reason"), "unsupported_heic_input");
    ASSERT_TRUE(result.error().context.contains("source"));
    EXPECT_EQ(result.error().context.at("source"), source);
}

template <typename Value>
void expect_not_heic(const Result<Value> &result)
{
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::kUnsupported);
    const auto format = result.error().context.find("format");
    EXPECT_TRUE(format == result.error().context.end() || format->second != "heic");
}

TEST(HeicAdapterTest, ReportsStableUnsupportedForPathAndMemory)
{
    HeicTempDirectory temporary;
    QtRasterDecoder decoder;

    const auto path = temporary.path() / "content-mislabeled.jpg";
    const QByteArray encoded = minimal_heic_ftyp();
    write_file(path, encoded);

    expect_heic_unsupported(decoder.probe(path.string()), path.string());
    expect_heic_unsupported(decoder.decode(path.string(), 64U, CancellationToken{}), path.string());
    expect_heic_unsupported(decoder.decode_memory(vector_bytes(encoded), 64U, CancellationToken{}),
                            "memory");

    const QByteArray mif1 = minimal_heic_ftyp(QByteArrayLiteral("mif1"), QByteArrayLiteral("heic"));
    expect_heic_unsupported(decoder.decode_memory(vector_bytes(mif1), 0U, CancellationToken{}),
                            "memory");
}

TEST(HeicAdapterTest, MagicRecognitionDoesNotClaimExtensionOnlyOrAvif)
{
    HeicTempDirectory temporary;
    QtRasterDecoder decoder;

    for (const QByteArray &payload :
         {minimal_heic_ftyp(QByteArrayLiteral("heix")),
          minimal_heic_ftyp(QByteArrayLiteral("msf1"), QByteArrayLiteral("hevc"))})
    {
        expect_heic_unsupported(
            decoder.decode_memory(vector_bytes(payload), 0U, CancellationToken{}), "memory");
    }

    // AVIF remains a separate undecided format — do not claim HEIC.
    const QByteArray avif = minimal_heic_ftyp(QByteArrayLiteral("avif"), QByteArrayLiteral("mif1"));
    expect_not_heic(decoder.decode_memory(vector_bytes(avif), 0U, CancellationToken{}));

    for (const QByteArray &payload : {QByteArray("ftyp", 4), QByteArray("random", 6)})
    {
        expect_not_heic(decoder.decode_memory(vector_bytes(payload), 0U, CancellationToken{}));
    }

    const auto extension_only = temporary.path() / "extension-only.heic";
    write_file(extension_only, QByteArray("random", 6));
    expect_not_heic(decoder.probe(extension_only.string()));
    expect_not_heic(decoder.decode(extension_only.string(), 0U, CancellationToken{}));
}

TEST(HeicAdapterTest, PrioritizesCancellationAndKeepsSourceImmutable)
{
    HeicTempDirectory temporary;
    QtRasterDecoder decoder;
    const auto path = temporary.path() / "cancel.heic";
    const QByteArray encoded = minimal_heic_ftyp();
    write_file(path, encoded);
    const QByteArray hash_before = QCryptographicHash::hash(encoded, QCryptographicHash::Sha256);
    const auto size_before = std::filesystem::file_size(path);
    const auto mtime_before = std::filesystem::last_write_time(path);

    CancellationSource cancelled;
    ASSERT_TRUE(cancelled.cancel("heic test"));
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

TEST(HeicAdapterTest, RejectsSparsePathFromMagicWithoutReadingTheWholeFile)
{
    HeicTempDirectory temporary;
    QtRasterDecoder decoder;
    const auto path = temporary.path() / "large.heic";
    const QByteArray header = minimal_heic_ftyp();
    QFile file(QString::fromStdString(path.string()));
    ASSERT_TRUE(file.open(QIODevice::ReadWrite | QIODevice::Truncate));
    ASSERT_EQ(file.write(header), header.size());
    constexpr qint64 kSparseSize = 2LL * 1024LL * 1024LL * 1024LL + 4096LL;
    ASSERT_TRUE(file.resize(kSparseSize));
    file.close();
    const auto size_before = std::filesystem::file_size(path);
    const auto mtime_before = std::filesystem::last_write_time(path);

    expect_heic_unsupported(decoder.probe(path.string()), path.string());
    expect_heic_unsupported(decoder.decode(path.string(), 0U, CancellationToken{}), path.string());

    EXPECT_EQ(std::filesystem::file_size(path), size_before);
    EXPECT_EQ(std::filesystem::last_write_time(path), mtime_before);
    QFile verify(QString::fromStdString(path.string()));
    ASSERT_TRUE(verify.open(QIODevice::ReadOnly));
    EXPECT_EQ(verify.read(header.size()), header);
}

} // namespace
} // namespace ravo

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
