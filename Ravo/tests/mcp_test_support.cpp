#include "mcp_test_support.h"

namespace ravo::studio_test_support
{
McpProcess::McpProcess() = default;
McpProcess::~McpProcess()
{
    if (!close())
    {
        process_.kill();
        static_cast<void>(process_.waitForFinished(3000));
    }
}
bool McpProcess::start()
{
    process_.start(QString::fromUtf8(RAVO_CLI_EXECUTABLE), {"mcp"});
    return process_.waitForStarted(5000);
}
void McpProcess::raw(const QByteArray &bytes)
{
    process_.write(bytes);
}
void McpProcess::send(const JsonValue &message)
{
    raw(QByteArray::fromStdString(serialize_json(message)) + '\n');
}
void McpProcess::drain()
{
    buffered_ += process_.readAllStandardOutput();
    for (auto end = buffered_.indexOf('\n'); end >= 0; end = buffered_.indexOf('\n'))
    {
        const auto line = buffered_.left(end);
        buffered_.remove(0, end + 1);
        auto parsed = parse_json(line.toStdString());
        if (parsed && parsed.value().find("id"))
        {
            const auto key = serialize_json(*parsed.value().find("id"));
            replies_.insert_or_assign(key, std::move(parsed).value());
        }
    }
}
Result<JsonValue> McpProcess::response(const std::int64_t id, const int timeout_ms)
{
    const auto key = std::to_string(id);
    const auto ready = wait_until(
        [&]
        {
            drain();
            return replies_.contains(key) || process_.state() == QProcess::NotRunning;
        },
        timeout_ms);
    if (!ready || !replies_.contains(key))
        return make_error(ErrorCode::kIo, "MCP subprocess did not reply",
                          {{"stderr", diagnostics().toStdString()}, {"id", key}});
    auto value = std::move(replies_.at(key));
    replies_.erase(key);
    return value;
}
Result<JsonValue> McpProcess::request(const std::string_view method, JsonValue params)
{
    auto object = *params.object_if();
    object.emplace(
        "_meta",
        JsonValue::Object{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                          {"io.modelcontextprotocol/clientCapabilities", JsonValue::Object{}}});
    const auto id = next_id_++;
    send(JsonValue::Object{{"jsonrpc", "2.0"},
                           {"id", JsonValue::number(std::to_string(id))},
                           {"method", std::string(method)},
                           {"params", std::move(object)}});
    return response(id);
}
Result<JsonValue> McpProcess::tool(const std::string_view name, JsonValue args)
{
    return request("tools/call",
                   JsonValue::Object{{"name", std::string(name)}, {"arguments", std::move(args)}});
}
bool McpProcess::received(const std::int64_t id)
{
    drain();
    return replies_.contains(std::to_string(id));
}
bool McpProcess::close(const int timeout_ms)
{
    if (process_.state() == QProcess::NotRunning)
        return true;
    process_.closeWriteChannel();
    return wait_until(
        [&]
        {
            drain();
            return process_.state() == QProcess::NotRunning;
        },
        timeout_ms);
}
QByteArray McpProcess::diagnostics()
{
    return process_.readAllStandardError();
}
Result<JsonValue> mcp_data(const JsonValue &response)
{
    const auto *result = response.find("result");
    const auto *structured = result ? result->find("structuredContent") : nullptr;
    const auto *data = structured ? structured->find("data") : nullptr;
    if (!data)
        return make_error(ErrorCode::kValidation, "MCP tool failed",
                          {{"response", serialize_json(response)}});
    return *data;
}
std::string json_string(const JsonValue &object, const std::string_view field)
{
    const auto *value = object.find(field);
    return value && value->string_if() ? *value->string_if() : std::string{};
}
JsonValue exposure_patch(const double value)
{
    return JsonValue::Array{
        JsonValue::Object{{"name", "exposure"}, {"value", json_real(value).value()}}};
}
} // namespace ravo::studio_test_support
