#include <initializer_list>
#include <string_view>

#include <QByteArray>
#include <QImageReader>
#include <QImageWriter>
#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <gtest/gtest.h>

namespace ravo
{
namespace
{

void expect_format(const QList<QByteArray> &available,
                   const std::initializer_list<std::string_view> aliases,
                   const std::string_view display_name)
{
    for (const auto alias : aliases)
    {
        if (available.contains(QByteArray(alias.data(), static_cast<qsizetype>(alias.size()))))
        {
            return;
        }
    }
    FAIL() << "Required Qt image format is unavailable: " << display_name;
}

TEST(RuntimeDependencyTest, RequiredRasterCodecsAndSqliteDriverAreAvailable)
{
    const auto readable = QImageReader::supportedImageFormats();
    expect_format(readable, {"png"}, "PNG reader");
    expect_format(readable, {"jpeg", "jpg"}, "JPEG reader");
    expect_format(readable, {"bmp"}, "BMP reader");
    expect_format(readable, {"gif"}, "GIF reader");
    expect_format(readable, {"webp"}, "WebP reader");
    expect_format(readable, {"tiff", "tif"}, "TIFF reader");

    const auto writable = QImageWriter::supportedImageFormats();
    expect_format(writable, {"png"}, "PNG writer");
    expect_format(writable, {"jpeg", "jpg"}, "JPEG writer");
    expect_format(writable, {"tiff", "tif"}, "TIFF writer");

    EXPECT_TRUE(QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")));
}

} // namespace
} // namespace ravo
