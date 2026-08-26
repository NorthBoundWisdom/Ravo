#include "ravo/foundation/log.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/core/PatternFormatterOptions.h"
#include "quill/sinks/ConsoleSink.h"
#include "quill/sinks/FileSink.h"

namespace ravo
{
namespace
{

quill::Logger *g_logger = nullptr;

[[nodiscard]] std::string from_u8(const std::u8string &value)
{
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}

[[nodiscard]] std::optional<std::string> environment_variable(const char *name)
{
#ifdef _WIN32
    char *value = nullptr;
    size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr)
    {
        return std::nullopt;
    }
    std::string result(value);
    std::free(value);
    if (result.empty())
    {
        return std::nullopt;
    }
    return result;
#else
    if (const char *value = std::getenv(name); value != nullptr && value[0] != '\0')
    {
        return std::string(value);
    }
    return std::nullopt;
#endif
}

[[nodiscard]] std::filesystem::path default_log_directory(const std::string_view app_name)
{
#ifdef _WIN32
    if (const auto appdata = environment_variable("APPDATA"))
    {
        return std::filesystem::path(*appdata) / std::string(app_name) / "logs";
    }
#else
    if (const auto home = environment_variable("HOME"))
    {
#ifdef __APPLE__
        return std::filesystem::path(*home) / "Library" / "Logs" / std::string(app_name);
#else
        return std::filesystem::path(*home) / ".local" / "share" / std::string(app_name) / "logs";
#endif
    }
#endif
    return std::filesystem::temp_directory_path() / std::string(app_name) / "logs";
}

[[nodiscard]] std::string timestamp_for_filename()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time_value = std::chrono::system_clock::to_time_t(now);
    std::tm tm_time{};
#if defined(_WIN32)
    static_cast<void>(localtime_s(&tm_time, &time_value));
#else
    static_cast<void>(localtime_r(&time_value, &tm_time));
#endif
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d_%H-%M-%S", &tm_time);
    return std::string(buffer);
}

} // namespace

void init_logging(const std::string_view app_name, const bool console_enabled)
{
    if (g_logger != nullptr)
    {
        return;
    }

    const std::string name = app_name.empty() ? std::string("Ravo") : std::string(app_name);
    const auto directory = default_log_directory(name);
    std::error_code error;
    std::filesystem::create_directories(directory, error);

    quill::Backend::start();

    quill::FileSinkConfig file_config;
    file_config.set_open_mode('a');
    const auto file_path = directory / (name + "_" + timestamp_for_filename() + ".log");
    auto file_sink = quill::Frontend::create_or_get_sink<quill::FileSink>(
        from_u8(file_path.generic_u8string()), file_config);

    const quill::PatternFormatterOptions pattern{
        "%(time) [%(log_level)] %(message)", "%Y-%m-%dT%H:%M:%S.%QmsZ", quill::Timezone::GmtTime};
    std::vector<std::shared_ptr<quill::Sink>> sinks;
    if (console_enabled)
    {
        sinks.push_back(quill::Frontend::create_or_get_sink<quill::ConsoleSink>("ravo_console"));
    }
    sinks.push_back(std::move(file_sink));
    g_logger = quill::Frontend::create_or_get_logger("ravo", std::move(sinks), pattern);
    g_logger->set_log_level(quill::LogLevel::Debug);
    LOG_INFO(g_logger, "logging initialized directory={}", from_u8(directory.generic_u8string()));
}

void shutdown_logging()
{
    if (g_logger == nullptr)
    {
        return;
    }
    g_logger->flush_log();
    g_logger = nullptr;
    if (quill::Backend::is_running())
    {
        quill::Backend::stop();
    }
}

quill::Logger *logger()
{
    if (g_logger == nullptr)
    {
        std::fputs("ravo::logger() used before init_logging\n", stderr);
        std::abort();
    }
    return g_logger;
}

} // namespace ravo
