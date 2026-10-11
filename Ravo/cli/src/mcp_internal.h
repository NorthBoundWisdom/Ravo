#pragma once

#include <iosfwd>
#include <optional>
#include <QString>
#include <QStringList>
#include "ravo/foundation/json.h"

namespace ravo::cli_internal
{
inline constexpr std::size_t kMcpMaximumInputBytes = 8U * 1024U * 1024U;
inline constexpr std::size_t kMcpMaximumImageBytes = 16U * 1024U * 1024U;
struct McpInvocation
{
    QStringList arguments;
    std::optional<JsonValue> request;
    bool image = false;
    bool write = false;
    std::string session_id;
    std::string operation_id;
    int timeout_ms = 120000;
};
[[nodiscard]] JsonValue mcp_tool_definitions();
[[nodiscard]] Result<McpInvocation> mcp_invocation(std::string_view tool,
                                                   const JsonValue &arguments,
                                                   const QString &request_path,
                                                   const QString &image_path);
int run_mcp_server(std::ostream &output, std::ostream &diagnostics);
} // namespace ravo::cli_internal
