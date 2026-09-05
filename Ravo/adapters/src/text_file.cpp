#include "ravo/adapters/text_file.h"

#include <limits>
#include <span>

#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QIODevice>
#include <QtCore/QSaveFile>
#include <QtCore/QString>

namespace ravo
{

namespace
{

[[nodiscard]] Result<QString> to_qt_path(const std::string_view path_utf8)
{
    if (path_utf8.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Input file path must not be empty");
    }
    if (path_utf8.size() > static_cast<std::size_t>(std::numeric_limits<qsizetype>::max()))
    {
        return make_error(ErrorCode::kInvalidArgument, "Input file path is too long");
    }
    return QString::fromUtf8(path_utf8.data(), static_cast<qsizetype>(path_utf8.size()));
}

} // namespace

Result<std::string> read_utf8_text_file(const std::string_view path_utf8,
                                        const std::uintmax_t maximum_bytes)
{
    auto path = to_qt_path(path_utf8);
    if (!path)
    {
        return path.error();
    }

    QFileInfo file_info(path.value());
    if (!file_info.exists())
    {
        return make_error(ErrorCode::kNotFound, "Input file does not exist",
                          {{"path", std::string(path_utf8)}});
    }
    if (!file_info.isFile())
    {
        return make_error(ErrorCode::kUnsupported, "Input path is not a regular file",
                          {{"path", std::string(path_utf8)}});
    }
    const auto size = file_info.size();
    if (size < 0)
    {
        return make_error(ErrorCode::kIo, "Unable to determine input file size",
                          {{"path", std::string(path_utf8)}});
    }
    if (static_cast<std::uintmax_t>(size) > maximum_bytes)
    {
        return make_error(ErrorCode::kValidation, "Input file exceeds the size limit",
                          {{"maximum_bytes", std::to_string(maximum_bytes)},
                           {"path", std::string(path_utf8)},
                           {"size_bytes", std::to_string(size)}});
    }

    QFile file(path.value());
    if (!file.open(QIODevice::ReadOnly))
    {
        return make_error(ErrorCode::kIo, "Unable to open input file",
                          {{"path", std::string(path_utf8)},
                           {"qt_error", file.errorString().toUtf8().toStdString()}});
    }
    const auto content = file.readAll();
    if (file.error() != QFileDevice::NoError)
    {
        return make_error(ErrorCode::kIo, "Unable to read input file",
                          {{"path", std::string(path_utf8)},
                           {"qt_error", file.errorString().toUtf8().toStdString()}});
    }
    return std::string(content.constData(), static_cast<std::size_t>(content.size()));
}

Result<void> write_file_bytes_atomically(const std::string_view path_utf8,
                                         const std::span<const std::uint8_t> bytes)
{
    auto path = to_qt_path(path_utf8);
    if (!path)
    {
        return path.error();
    }
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<qsizetype>::max()))
    {
        return make_error(ErrorCode::kInvalidArgument, "Output content is too large");
    }
    if (QFileInfo(path.value()).exists())
    {
        return make_error(ErrorCode::kConflict, "Output path already exists",
                          {{"path", std::string(path_utf8)}});
    }

    QSaveFile output(path.value());
    if (!output.open(QIODevice::WriteOnly))
    {
        return make_error(ErrorCode::kIo, "Unable to open atomic output file",
                          {{"path", std::string(path_utf8)},
                           {"qt_error", output.errorString().toUtf8().toStdString()}});
    }
    const auto data = QByteArray::fromRawData(reinterpret_cast<const char *>(bytes.data()),
                                              static_cast<qsizetype>(bytes.size()));
    if (output.write(data) != data.size())
    {
        return make_error(ErrorCode::kIo, "Unable to write complete atomic output",
                          {{"path", std::string(path_utf8)},
                           {"qt_error", output.errorString().toUtf8().toStdString()}});
    }
    if (!output.commit())
    {
        return make_error(ErrorCode::kIo, "Unable to commit atomic output file",
                          {{"path", std::string(path_utf8)},
                           {"qt_error", output.errorString().toUtf8().toStdString()}});
    }
    return {};
}

Result<void> write_utf8_text_file_atomically(const std::string_view path_utf8,
                                             const std::string_view content_utf8)
{
    return write_file_bytes_atomically(
        path_utf8,
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(content_utf8.data()),
                                      content_utf8.size()));
}

Result<void> write_utf8_text_file_replace_atomically(const std::string_view path_utf8,
                                                     const std::string_view content_utf8)
{
    auto path = to_qt_path(path_utf8);
    if (!path)
        return path.error();
    if (content_utf8.size() > static_cast<std::size_t>(std::numeric_limits<qsizetype>::max()))
        return make_error(ErrorCode::kInvalidArgument, "Output content is too large");

    QSaveFile output(path.value());
    if (!output.open(QIODevice::WriteOnly))
    {
        return make_error(ErrorCode::kIo, "Unable to open atomic output file",
                          {{"path", std::string(path_utf8)},
                           {"qt_error", output.errorString().toUtf8().toStdString()}});
    }
    const auto data =
        QByteArray::fromRawData(content_utf8.data(), static_cast<qsizetype>(content_utf8.size()));
    if (output.write(data) != data.size())
    {
        return make_error(ErrorCode::kIo, "Unable to write complete atomic output",
                          {{"path", std::string(path_utf8)},
                           {"qt_error", output.errorString().toUtf8().toStdString()}});
    }
    if (!output.commit())
    {
        return make_error(ErrorCode::kIo, "Unable to commit atomic output file",
                          {{"path", std::string(path_utf8)},
                           {"qt_error", output.errorString().toUtf8().toStdString()}});
    }
    return {};
}

std::string sha256_utf8_hex(const std::string_view text)
{
    const QByteArray bytes = QByteArray::fromRawData(text.data(), static_cast<int>(text.size()));
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString();
}

Result<std::string> sha256_file_hex(const std::string_view path_utf8,
                                    const CancellationToken &cancellation)
{
    auto active = cancellation.check();
    if (!active)
        return active.error();
    auto path = to_qt_path(path_utf8);
    if (!path)
        return path.error();
    QFile file(path.value());
    if (!file.open(QIODevice::ReadOnly))
    {
        return make_error(ErrorCode::kIo, "Unable to hash file",
                          {{"path", std::string(path_utf8)},
                           {"reason", "file_hash_open_failed"},
                           {"qt_error", file.errorString().toUtf8().toStdString()}});
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd())
    {
        active = cancellation.check();
        if (!active)
            return active.error();
        const QByteArray chunk = file.read(1 << 20);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError)
        {
            return make_error(ErrorCode::kIo, "Unable to hash file",
                              {{"path", std::string(path_utf8)},
                               {"reason", "file_hash_read_failed"},
                               {"qt_error", file.errorString().toUtf8().toStdString()}});
        }
        hash.addData(chunk);
    }
    active = cancellation.check();
    if (!active)
        return active.error();
    return hash.result().toHex().toStdString();
}

} // namespace ravo
