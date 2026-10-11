#pragma once

#include <map>
#include <QProcess>
#include "studio_test_support.h"

namespace ravo::studio_test_support
{
class McpProcess
{
public:
    McpProcess();
    ~McpProcess();
    [[nodiscard]] bool start();
    void send(const JsonValue &message);
    void raw(const QByteArray &bytes);
    [[nodiscard]] Result<JsonValue> response(std::int64_t id, int timeout_ms = 30000);
    [[nodiscard]] Result<JsonValue> request(std::string_view method,
                                            JsonValue params = JsonValue::Object{});
    [[nodiscard]] Result<JsonValue> tool(std::string_view name,
                                         JsonValue args = JsonValue::Object{});
    [[nodiscard]] bool received(std::int64_t id);
    [[nodiscard]] bool close(int timeout_ms = 10000);
    [[nodiscard]] QByteArray diagnostics();

private:
    void drain();
    QProcess process_;
    QByteArray buffered_;
    std::map<std::string, JsonValue, std::less<>> replies_;
    std::int64_t next_id_ = 1;
};
[[nodiscard]] Result<JsonValue> mcp_data(const JsonValue &response);
[[nodiscard]] std::string json_string(const JsonValue &, std::string_view field);
[[nodiscard]] JsonValue exposure_patch(double value);
} // namespace ravo::studio_test_support
