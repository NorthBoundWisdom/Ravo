#include "ravo/domain/uri.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

namespace ravo
{
namespace
{

[[nodiscard]] std::string from_u8(const std::u8string &value)
{
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}

[[nodiscard]] std::filesystem::path to_path(const std::string_view text)
{
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

[[nodiscard]] int hex_value(const char character)
{
    if (character >= '0' && character <= '9')
    {
        return character - '0';
    }
    if (character >= 'A' && character <= 'F')
    {
        return character - 'A' + 10;
    }
    if (character >= 'a' && character <= 'f')
    {
        return character - 'a' + 10;
    }
    return -1;
}

[[nodiscard]] bool is_ascii_path_byte(const unsigned char character) noexcept
{
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '/' || character == '-' ||
           character == '_' || character == '.' || character == '~' || character == ':';
}

[[nodiscard]] std::string percent_encode_path(const std::string_view path)
{
    std::string encoded;
    encoded.reserve(path.size());
    for (const char raw : path)
    {
        const auto character = static_cast<unsigned char>(raw);
        if (character >= 0x80U || is_ascii_path_byte(character))
        {
            encoded.push_back(raw);
            continue;
        }
        static constexpr char hex[] = "0123456789ABCDEF";
        encoded.push_back('%');
        encoded.push_back(hex[character >> 4U]);
        encoded.push_back(hex[character & 0x0fU]);
    }
    return encoded;
}

[[nodiscard]] std::string percent_decode_path(const std::string_view encoded)
{
    std::string decoded;
    decoded.reserve(encoded.size());
    for (std::size_t index = 0; index < encoded.size();)
    {
        if (encoded[index] == '%' && index + 2 < encoded.size())
        {
            const int high = hex_value(encoded[index + 1]);
            const int low = hex_value(encoded[index + 2]);
            if (high >= 0 && low >= 0)
            {
                decoded.push_back(static_cast<char>((high << 4) | low));
                index += 3;
                continue;
            }
        }
        decoded.push_back(encoded[index]);
        ++index;
    }
    return decoded;
}

[[nodiscard]] std::string to_file_uri(const std::string_view generic_path)
{
    const std::string encoded = percent_encode_path(generic_path);
    if (encoded.starts_with('/'))
    {
        return "file://" + encoded;
    }
    return "file:///" + encoded;
}

[[nodiscard]] std::string strip_file_scheme(std::string_view input)
{
    if (input.starts_with("file://"))
    {
        input.remove_prefix(7);
        if (input.size() >= 3 &&
            ((input[0] >= 'A' && input[0] <= 'Z') || (input[0] >= 'a' && input[0] <= 'z')) &&
            input[1] == ':' && input[2] == '/')
        {
            return std::string(input);
        }
        if (input.starts_with('/') && input.size() > 1 && input[1] != '/')
        {
            return std::string(input);
        }
        if (input.starts_with("localhost/"))
        {
            return std::string(input.substr(9));
        }
    }
    return std::string(input);
}

} // namespace

Result<NormalizedLocation> normalize_local_input(const std::string_view input)
{
    if (input.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Local path must not be empty");
    }

    const std::string raw_path = percent_decode_path(strip_file_scheme(input));
    std::error_code error;
    const auto absolute = std::filesystem::absolute(to_path(raw_path), error);
    if (error)
    {
        return make_error(ErrorCode::kIo, "Unable to resolve an absolute path",
                          {{"path", std::string(input)}, {"detail", error.message()}});
    }
    auto canonical = std::filesystem::weakly_canonical(absolute, error);
    if (error)
    {
        canonical = absolute.lexically_normal();
    }

    NormalizedLocation location;
    location.path = from_u8(canonical.generic_u8string());
    location.uri = to_file_uri(location.path);
    return location;
}

Result<FileIdentity> read_file_identity(const std::string_view path)
{
    std::error_code error;
    const auto file_path = to_path(path);
    if (!std::filesystem::is_regular_file(file_path, error) || error)
    {
        return make_error(ErrorCode::kNotFound, "Input is not a regular file",
                          {{"path", std::string(path)}});
    }
    const auto size = std::filesystem::file_size(file_path, error);
    if (error)
    {
        return make_error(ErrorCode::kIo, "Unable to read file size",
                          {{"path", std::string(path)}, {"detail", error.message()}});
    }
    const auto file_time = std::filesystem::last_write_time(file_path, error);
    if (error)
    {
        return make_error(ErrorCode::kIo, "Unable to read file modification time",
                          {{"path", std::string(path)}, {"detail", error.message()}});
    }
    FileIdentity identity;
    identity.size_bytes = static_cast<std::uint64_t>(size);
    // MSVC's filesystem file_clock has no to_sys. Convert through the current
    // offset between file_clock and system_clock instead of a clock-specific API.
    using FileClock = std::filesystem::file_time_type::clock;
    const auto sys_time = std::chrono::system_clock::now() + (file_time - FileClock::now());
    identity.mtime_unix_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(sys_time.time_since_epoch()).count();
    return identity;
}

std::string uri_parent(const std::string_view uri)
{
    const auto slash = uri.find_last_of('/');
    if (slash == std::string_view::npos || slash < 7U)
    {
        return {};
    }
    const auto parent = std::string(uri.substr(0, slash));
    if (parent == "file:" || parent == "file:/" || parent == "file://" || parent == "file:///")
    {
        return {};
    }
    return parent;
}

std::string uri_display_name(const std::string_view uri)
{
    const auto slash = uri.find_last_of('/');
    std::string_view segment = uri;
    if (slash != std::string_view::npos && slash + 1U < uri.size())
    {
        segment = uri.substr(slash + 1U);
    }
    auto decoded = percent_decode_path(segment);
    if (decoded.empty())
    {
        return std::string(uri);
    }
    return decoded;
}

} // namespace ravo
