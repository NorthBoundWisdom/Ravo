#include "ravo/foundation/log.h"

#include <cstdlib>
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

[[nodiscard]] std::filesystem::path default_log_directory(const std::string_view app_name)
{
#ifdef _WIN32
    if (const char *appdata = std::getenv("APPDATA"); appdata != nullptr && appdata[0] != '\0')
    {
        return std::filesystem::path(appdata) / std::string(app_name) / "logs";
    }
#else
    if (const char *home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
    {
#ifdef __APPLE__
        return std::filesystem::path(home) / "Library" / "Logs" / std::string(app_name);
#else
        return std::filesystem::path(home) / ".local" / "share" / std::string(app_name) / "logs";
#endif
    }
#endif
    return std::filesystem::temp_directory_path() / std::string(app_name) / "logs";
}

} // namespace

void init_logging(const std::string_view app_name)
{
    const auto directory = default_log_directory(app_name.empty() ? "Ravo" : app_name);
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    auto &logger = rflog::Logger::instance();
    logger.enableFileLogging(directory, std::string(app_name.empty() ? "ravo" : app_name) + "_");
    logger.setLogLevel(rflog::LogLevel::Debug);
    RFLOG_INFO("logging initialized directory={}", from_u8(directory.generic_u8string()));
}

void shutdown_logging()
{
    rflog::shutdown();
}

} // namespace ravo
