#include "mcp_internal.h"

#include <algorithm>
#include <charconv>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include "ravo/foundation/executor.h"

namespace ravo::cli_internal
{
namespace
{
JsonValue error_json(std::string code, std::string message, JsonValue context = JsonValue::Object{})
{
    return JsonValue::Object{{"code", std::move(code)},
                             {"message", std::move(message)},
                             {"context", std::move(context)}};
}
JsonValue task_error_json(const TaskError &error)
{
    JsonValue::Object context;
    for (const auto &[key, value] : error.context)
        context.emplace(key, value);
    return error_json(std::string(error_code_name(error.code)), error.message,
                      JsonValue{std::move(context)});
}
JsonValue capabilities()
{
    return JsonValue::Object{{"tools", JsonValue::Object{{"listChanged", false}}},
                             {"resources", JsonValue::Object{{"listChanged", false}}}};
}
bool known_parameters(const JsonValue *params, std::initializer_list<std::string_view> names)
{
    if (!params)
        return true;
    if (!params->object_if())
        return false;
    for (const auto &[key, value] : *params->object_if())
    {
        static_cast<void>(value);
        if (key != "_meta" && std::find(names.begin(), names.end(), key) == names.end())
            return false;
    }
    return true;
}
constexpr auto instructions =
    "Use an explicit Studio session. Read state.binding and editing_scope before evaluating. "
    "Evaluate is read-only; commit only the chosen candidate. Preserve operation_id and query its receipt after "
    "timeouts. Never auto-retarget a stale binding or replay an unknown write. Staged mask changes are not saved. "
    "Batch photos commit independently; inspect every partial result. Original images are never modified.";
struct Job
{
    JsonValue id;
    bool modern = false;
    std::string tool;
    McpInvocation invocation;
    std::unique_ptr<QTemporaryDir> directory;
    QPointer<QProcess> process;
    QPointer<QProcess> cancellation_process;
    QPointer<QTimer> deadline_timer;
    QPointer<QTimer> cancellation_timer;
    QElapsedTimer elapsed;
    QByteArray output;
    QByteArray diagnostics;
    bool reserving = false;
    bool cancelling = false;
    bool suppressed = false;
    bool done = false;
    std::optional<JsonValue> cancellation_error;
};
struct Resource
{
    std::string uri;
    JsonValue metadata;
    QByteArray bytes;
};

class McpServer
{
public:
    McpServer(std::ostream &output, std::ostream &diagnostics)
        : output_(output)
        , diagnostics_(diagnostics)
    {
    }
    int run()
    {
        const bool posted = reader_.post(
            [this]
            {
                std::string line;
                bool oversized = false;
                for (int character = std::cin.get(); character != EOF; character = std::cin.get())
                {
                    if (character == '\n')
                    {
                        if (!line.empty() && line.back() == '\r')
                            line.pop_back();
                        pending_input_.acquire();
                        QMetaObject::invokeMethod(
                            &context_,
                            [this, line = std::move(line), oversized]() mutable
                            {
                                pending_input_.release();
                                if (oversized)
                                    protocol_error(JsonValue{nullptr}, -32600,
                                                   "MCP request exceeds the byte limit");
                                else
                                    receive(std::move(line));
                            },
                            Qt::QueuedConnection);
                        line.clear();
                        oversized = false;
                    }
                    else if (!oversized)
                    {
                        if (line.size() == kMcpMaximumInputBytes)
                        {
                            line.clear();
                            oversized = true;
                        }
                        else
                            line.push_back(static_cast<char>(character));
                    }
                }
                QMetaObject::invokeMethod(
                    &context_,
                    [this, incomplete = !line.empty() || oversized]
                    {
                        if (incomplete)
                            protocol_error(JsonValue{nullptr}, -32700, "Unterminated MCP message");
                        closing_ = true;
                        std::vector<std::shared_ptr<Job>> pending;
                        for (const auto &[id, job] : jobs_)
                        {
                            static_cast<void>(id);
                            pending.push_back(job);
                        }
                        for (const auto &job : pending)
                            stop(job, true, error_json("cancelled", "MCP input closed"));
                        if (jobs_.empty())
                            loop_.quit();
                        QTimer::singleShot(3000, &context_,
                                           [this]
                                           {
                                               if (closing_)
                                                   loop_.quit();
                                           });
                    },
                    Qt::QueuedConnection);
            });
        if (!posted)
            return 1;
        loop_.exec();
        reader_.request_stop();
        reader_.wait(); // EOF was posted only after the blocking reader completed.
        for (auto *process : context_.findChildren<QProcess *>())
        {
            if (process->state() != QProcess::NotRunning)
            {
                process->kill();
                static_cast<void>(process->waitForFinished(250));
            }
        }
        jobs_.clear();
        resources_.clear();
        return 0;
    }

private:
    void send(const JsonValue &message)
    {
        if (closing_)
            return;
        const auto encoded = serialize_json(message);
        constexpr std::size_t maximum = 32U * 1024U * 1024U;
        if (encoded.size() > maximum)
        {
            const auto *id = message.find("id");
            protocol_error(id ? *id : JsonValue{nullptr}, -32000,
                           "MCP response exceeds the byte limit");
            return;
        }
        output_ << encoded << '\n';
        output_.flush();
        if (!output_)
            diagnostics_ << "Ravo MCP output stream failed\n";
    }
    void protocol_error(const JsonValue &id, const int code, const std::string &message,
                        JsonValue data = JsonValue::Object{})
    {
        send(JsonValue::Object{
            {"jsonrpc", "2.0"},
            {"id", id},
            {"error", JsonValue::Object{{"code", JsonValue::number(std::to_string(code))},
                                        {"message", message},
                                        {"data", std::move(data)}}}});
    }
    void reply(const JsonValue &id, JsonValue result, const bool modern)
    {
        if (modern)
        {
            auto object = *result.object_if();
            object.insert_or_assign("resultType", "complete");
            result = JsonValue{std::move(object)};
        }
        send(JsonValue::Object{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
    }
    void finish(const std::shared_ptr<Job> &job, JsonValue data, const bool error,
                JsonValue::Array extra = {})
    {
        if (job->done)
            return;
        job->done = true;
        for (auto timer : {job->deadline_timer, job->cancellation_timer})
            if (timer)
            {
                timer->stop();
                timer->deleteLater();
            }
        if (job->process)
        {
            job->process->disconnect(&context_);
            if (job->process->state() != QProcess::NotRunning)
            {
                job->process->kill();
                static_cast<void>(job->process->waitForFinished(100));
            }
            job->process->deleteLater();
        }
        if (job->cancellation_process)
        {
            job->cancellation_process->disconnect(&context_);
            if (job->cancellation_process->state() != QProcess::NotRunning)
            {
                job->cancellation_process->kill();
                static_cast<void>(job->cancellation_process->waitForFinished(100));
            }
            job->cancellation_process->deleteLater();
        }
        if (!job->suppressed && !closing_)
        {
            JsonValue::Object structured{{"type", "ravo.mcp.tool_result"},
                                         {"version", JsonValue::number("1")},
                                         {"tool", job->tool}};
            structured.emplace(error ? "error" : "data", std::move(data));
            JsonValue::Array content{JsonValue::Object{
                {"type", "text"}, {"text", serialize_json(JsonValue{structured})}}};
            for (auto &item : extra)
                content.push_back(std::move(item));
            reply(job->id,
                  JsonValue::Object{{"structuredContent", std::move(structured)},
                                    {"content", std::move(content)},
                                    {"isError", error}},
                  job->modern);
        }
        jobs_.erase(serialize_json(job->id));
        if (closing_ && jobs_.empty())
            loop_.quit();
    }
    Result<Resource> image_resource(const std::shared_ptr<Job> &job, const JsonValue &data)
    {
        const auto *artifact = data.find("artifact");
        if (!artifact || !artifact->object_if())
            return make_error(ErrorCode::kValidation, "CLI image artifact is missing");
        const auto *type = artifact->find("type"), *version = artifact->find("version");
        const auto *path = artifact->find("path"), *digest = artifact->find("content_sha256"),
                   *count = artifact->find("byte_count");
        const auto *mime = artifact->find("mime_type"),
                   *profile = artifact->find("color_profile_fingerprint");
        const auto expected = job->directory->filePath("image.png");
        if (!type || !type->string_if() || *type->string_if() != "ravo.image_artifact" ||
            !version || !version->number_if() || version->number_if()->text != "1" || !path ||
            !path->string_if() || QString::fromStdString(*path->string_if()) != expected ||
            !digest || !digest->string_if() || digest->string_if()->size() != 64 || !count ||
            !count->number_if() || !mime || !mime->string_if() ||
            *mime->string_if() != "image/png" || !profile || !profile->string_if() ||
            profile->string_if()->size() != 16)
            return make_error(ErrorCode::kValidation, "CLI image artifact contract is invalid");
        QFile file(expected);
        if (!file.open(QIODevice::ReadOnly) || file.size() < 1 ||
            file.size() > static_cast<qint64>(kMcpMaximumImageBytes))
            return make_error(ErrorCode::kIo,
                              "MCP image artifact is absent or exceeds its byte bound");
        auto bytes = file.read(static_cast<qint64>(kMcpMaximumImageBytes + 1));
        if (file.error() != QFileDevice::NoError ||
            bytes.size() > static_cast<qsizetype>(kMcpMaximumImageBytes) ||
            std::to_string(bytes.size()) != count->number_if()->text ||
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString() !=
                *digest->string_if())
            return make_error(ErrorCode::kConflict, "Image artifact content changed");
        Resource resource;
        resource.uri =
            "ravo://artifact/" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        auto metadata = *artifact->object_if();
        metadata.erase("path");
        metadata.insert_or_assign("resource_uri", resource.uri);
        metadata.insert_or_assign("lifecycle", "mcp_process_bounded");
        resource.metadata = JsonValue{std::move(metadata)};
        resource.bytes = std::move(bytes);
        return resource;
    }
    void complete_backend(const std::shared_ptr<Job> &job)
    {
        if (job->done || job->cancelling)
            return;
        auto json = parse_json(std::string_view(job->output.constData(),
                                                static_cast<std::size_t>(job->output.size())));
        if (!json || !json.value().object_if() || !json.value().find("ok") ||
            !json.value().find("ok")->boolean_if())
        {
            auto error = error_json("io", "CLI returned an invalid JSON envelope");
            if (job->invocation.write && !job->invocation.operation_id.empty())
                stop(job, false, std::move(error));
            else
                finish(job, std::move(error), true);
            return;
        }
        if (!*json.value().find("ok")->boolean_if())
        {
            const auto *error = json.value().find("error");
            if (job->invocation.write && !job->invocation.operation_id.empty() && error &&
                error->find("code") && error->find("code")->string_if() &&
                *error->find("code")->string_if() == "cancelled")
                stop(job, false, *error);
            else
                finish(job,
                       error && error->object_if() ?
                           *error :
                           error_json("io", "CLI failed without a structured error"),
                       true);
            return;
        }
        const auto *data = json.value().find("data");
        if (!data || !data->object_if())
        {
            finish(job, error_json("validation", "CLI data is not an object"), true);
            return;
        }
        if (job->reserving)
        {
            const auto *id = data->find("operation_id");
            if (!id || !id->string_if() || id->string_if()->empty() ||
                id->string_if()->size() > 200)
            {
                finish(job, error_json("validation", "CLI operation reservation is invalid"), true);
                return;
            }
            job->reserving = false;
            job->invocation.operation_id = *id->string_if();
            job->invocation.arguments << "--operation-id"
                                      << QString::fromStdString(job->invocation.operation_id);
            start_child(job, job->invocation.arguments);
            return;
        }
        bool failed = false;
        if (const auto *phase = data->find("phase"); phase && phase->string_if())
            failed = *phase->string_if() == "failed" || *phase->string_if() == "cancelled" ||
                     *phase->string_if() == "partially_committed";
        if (!job->invocation.image)
        {
            // A receipt remains the result even for a partial/failed operation;
            // never discard its committed IDs behind a generic transport error.
            if (failed && job->invocation.write)
            {
                auto value = error_json(
                    "operation_incomplete", "Inspect the operation receipt before retrying",
                    JsonValue::Object{{"operation_id", job->invocation.operation_id}});
                auto object = *value.object_if();
                object.emplace("receipt", *data);
                finish(job, JsonValue{std::move(object)}, true);
            }
            else
                finish(job, *data, false);
            return;
        }
        auto resource = image_resource(job, *data);
        if (!resource)
        {
            finish(job, task_error_json(resource.error()), true);
            return;
        }
        constexpr qsizetype budget = 64 * 1024 * 1024;
        while (
            !resource_order_.empty() &&
            (resources_.size() >= 64 || resource_bytes_ + resource.value().bytes.size() > budget))
        {
            const auto oldest = resource_order_.front();
            resource_order_.pop_front();
            resource_bytes_ -= resources_.at(oldest).bytes.size();
            resources_.erase(oldest);
        }
        const auto uri = resource.value().uri;
        resource_bytes_ += resource.value().bytes.size();
        resource_order_.push_back(uri);
        resources_.emplace(uri, std::move(resource).value());
        const auto &stored = resources_.at(uri);
        auto object = *data->object_if();
        object.insert_or_assign("artifact", stored.metadata);
        JsonValue::Array content{JsonValue::Object{{"type", "image"},
                                                   {"mimeType", "image/png"},
                                                   {"data", stored.bytes.toBase64().toStdString()}},
                                 JsonValue::Object{{"type", "resource_link"},
                                                   {"uri", uri},
                                                   {"name", "Ravo preview"},
                                                   {"mimeType", "image/png"}}};
        finish(job, JsonValue{std::move(object)}, false, std::move(content));
    }
    void start_child(const std::shared_ptr<Job> &job, QStringList arguments)
    {
        if (job->done || job->cancelling)
            return;
        const int remaining = job->invocation.timeout_ms - static_cast<int>(job->elapsed.elapsed());
        if (remaining < 100)
        {
            stop(job, false, error_json("cancelled", "Tool deadline expired"));
            return;
        }
        const auto timeout = arguments.indexOf("--timeout-ms");
        if (timeout >= 0 && timeout + 1 < arguments.size())
            arguments[timeout + 1] = QString::number(remaining);
        if (job->process)
        {
            job->process->disconnect(&context_);
            job->process->deleteLater();
        }
        auto *process = new QProcess(&context_);
        job->process = process;
        job->output.clear();
        job->diagnostics.clear();
        process->setProgram(QCoreApplication::applicationFilePath());
        process->setArguments(arguments);
        QObject::connect(
            process, &QProcess::readyReadStandardOutput, &context_,
            [this, job, process]
            {
                job->output += process->readAllStandardOutput();
                if (job->output.size() > static_cast<qsizetype>(kMcpMaximumInputBytes))
                    stop(job, false,
                         error_json("validation", "CLI response exceeds its byte limit"));
            });
        QObject::connect(process, &QProcess::readyReadStandardError, &context_,
                         [job, process]
                         {
                             auto bytes = process->readAllStandardError();
                             if (job->diagnostics.size() < 4096)
                                 job->diagnostics += bytes.left(4096 - job->diagnostics.size());
                         });
        QObject::connect(process, &QProcess::errorOccurred, &context_,
                         [this, job](QProcess::ProcessError error)
                         {
                             if (error == QProcess::FailedToStart)
                                 finish(job, error_json("io", "Cannot start the Ravo CLI client"),
                                        true);
                         });
        QObject::connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                         &context_,
                         [this, job, process](int, QProcess::ExitStatus)
                         {
                             job->output += process->readAllStandardOutput();
                             if (!job->done && !job->cancelling)
                                 complete_backend(job);
                         });
        process->start();
    }
    void stop(const std::shared_ptr<Job> &job, const bool suppress, JsonValue error)
    {
        if (job->done)
            return;
        job->suppressed = job->suppressed || suppress;
        if (job->cancelling)
            return;
        if (!job->invocation.write || job->invocation.operation_id.empty())
        {
            if (job->process)
                job->process->kill();
            finish(job, std::move(error), true);
            return;
        }
        job->cancelling = true;
        job->cancellation_error = std::move(error);
        auto *process = new QProcess(&context_);
        job->cancellation_process = process;
        process->setProgram(QCoreApplication::applicationFilePath());
        process->setArguments({"studio", "operation", "--session-id",
                               QString::fromStdString(job->invocation.session_id), "--operation-id",
                               QString::fromStdString(job->invocation.operation_id), "--action",
                               "cancel", "--timeout-ms", "1000", "--json"});
        auto finish_cancel = [this, job, process]
        {
            if (job->done)
                return;
            if (job->process)
                job->process->kill();
            auto result = parse_json(process->readAllStandardOutput().toStdString());
            const JsonValue *receipt = result && result.value().find("ok") &&
                                               result.value().find("ok")->boolean_if() &&
                                               *result.value().find("ok")->boolean_if() ?
                                           result.value().find("data") :
                                           nullptr;
            if (receipt && receipt->object_if())
            {
                const auto *committed = receipt->find("mutation_applied");
                const auto *phase = receipt->find("phase");
                if (committed && committed->boolean_if() && *committed->boolean_if() && phase &&
                    phase->string_if() && *phase->string_if() != "pending")
                {
                    if (*phase->string_if() == "partially_committed")
                    {
                        auto partial =
                            *error_json(
                                 "operation_incomplete", "Batch committed only some photos",
                                 JsonValue::Object{{"operation_id", job->invocation.operation_id},
                                                   {"mutation_applied", "true"}})
                                 .object_if();
                        partial.emplace("receipt", *receipt);
                        finish(job, JsonValue{std::move(partial)}, true);
                    }
                    else
                        finish(job, *receipt, false);
                    return;
                }
            }
            auto object = *job->cancellation_error->object_if();
            auto context = JsonValue::Object{{"operation_id", job->invocation.operation_id},
                                             {"mutation_applied", "unknown"}};
            if (receipt && receipt->object_if())
            {
                object.emplace("receipt", *receipt);
                if (const auto *applied = receipt->find("mutation_applied");
                    applied && applied->boolean_if())
                    context.insert_or_assign("mutation_applied",
                                             *applied->boolean_if() ? "true" : "false");
            }
            object.insert_or_assign("context", std::move(context));
            finish(job, JsonValue{std::move(object)}, true);
        };
        QObject::connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                         &context_,
                         [finish_cancel](int, QProcess::ExitStatus) { finish_cancel(); });
        QObject::connect(process, &QProcess::errorOccurred, &context_,
                         [finish_cancel](QProcess::ProcessError error)
                         {
                             if (error == QProcess::FailedToStart)
                                 finish_cancel();
                         });
        auto *cancel_timer = new QTimer(&context_);
        job->cancellation_timer = cancel_timer;
        cancel_timer->setSingleShot(true);
        QObject::connect(cancel_timer, &QTimer::timeout, &context_,
                         [job, finish_cancel]
                         {
                             if (!job->done)
                             {
                                 if (job->cancellation_process)
                                     job->cancellation_process->kill();
                                 finish_cancel();
                             }
                         });
        cancel_timer->start(2500);
        process->start();
    }
    void receive(std::string line)
    {
        if (closing_)
            return;
        auto json = parse_json(line);
        if (!json)
        {
            protocol_error(JsonValue{nullptr}, -32700, "Invalid MCP JSON");
            return;
        }
        const auto *root = json.value().object_if();
        const auto *method = json.value().find("method"), *rpc = json.value().find("jsonrpc"),
                   *id = json.value().find("id");
        const auto *params = json.value().find("params");
        const auto response_id = id ? *id : JsonValue{nullptr};
        bool valid_id = !id || id->string_if();
        if (id && id->string_if() && id->string_if()->size() > 128)
            valid_id = false;
        if (id && id->number_if())
        {
            std::int64_t integer = 0;
            const auto &value = id->number_if()->text;
            auto parsed = std::from_chars(value.data(), value.data() + value.size(), integer);
            valid_id = parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
        }
        if (!root || !method || !method->string_if() || !rpc || !rpc->string_if() ||
            *rpc->string_if() != "2.0" || !valid_id || (params && !params->object_if()))
        {
            protocol_error(JsonValue{nullptr}, -32600, "Invalid MCP request");
            return;
        }
        for (const auto &[key, value] : *root)
        {
            static_cast<void>(value);
            if (key != "jsonrpc" && key != "method" && key != "id" && key != "params")
            {
                protocol_error(response_id, -32600, "Unknown JSON-RPC field");
                return;
            }
        }
        const auto name = *method->string_if();
        if (!id)
        {
            if (name == "notifications/initialized")
            {
                if (legacy_initialized_)
                    legacy_ready_ = true;
                return;
            }
            if (name == "notifications/cancelled")
            {
                const auto *target = params ? params->find("requestId") : nullptr;
                if (target)
                {
                    const auto found = jobs_.find(serialize_json(*target));
                    if (found != jobs_.end())
                        stop(found->second, true,
                             error_json("cancelled", "Client cancelled the request"));
                }
            }
            return;
        }
        if (name == "initialize")
        {
            if (!known_parameters(params, {"protocolVersion", "capabilities", "clientInfo"}))
            {
                protocol_error(response_id, -32602, "Unknown initialize parameter");
                return;
            }
            const auto *version = params ? params->find("protocolVersion") : nullptr;
            const auto *caps = params ? params->find("capabilities") : nullptr;
            const auto *client = params ? params->find("clientInfo") : nullptr;
            if (legacy_initialized_ || !version || !version->string_if() || !caps ||
                !caps->object_if() || !client || !client->object_if() || !client->find("name") ||
                !client->find("name")->string_if() || !client->find("version") ||
                !client->find("version")->string_if())
            {
                protocol_error(response_id, -32602, "Invalid initialize request");
                return;
            }
            if (*version->string_if() == "2026-07-28")
            {
                protocol_error(response_id, -32602,
                               "2026-07-28 uses per-request metadata and server/discover");
                return;
            }
            legacy_initialized_ = true;
            reply(response_id,
                  JsonValue::Object{
                      {"protocolVersion", "2025-11-25"},
                      {"capabilities", capabilities()},
                      {"serverInfo", JsonValue::Object{{"name", "ravo"}, {"version", "1"}}},
                      {"instructions", instructions}},
                  false);
            return;
        }
        bool modern = false;
        const auto *meta = params ? params->find("_meta") : nullptr;
        if (meta && !meta->object_if())
        {
            protocol_error(response_id, -32602, "Request metadata must be an object");
            return;
        }
        const auto *version =
            meta ? meta->find("io.modelcontextprotocol/protocolVersion") : nullptr;
        if (version)
        {
            if (version->string_if() && *version->string_if() == "2025-11-25")
            {
                if (!legacy_ready_)
                {
                    protocol_error(response_id, -32002, "Legacy initialization is required");
                    return;
                }
            }
            else if (!version->string_if() || *version->string_if() != "2026-07-28")
            {
                protocol_error(
                    response_id, -32022, "Unsupported MCP protocol version",
                    JsonValue::Object{{"supported", JsonValue::Array{"2026-07-28", "2025-11-25"}},
                                      {"requested", version->string_if() ? *version->string_if() :
                                                                           std::string{}}});
                return;
            }
            else
            {
                const auto *caps = meta->find("io.modelcontextprotocol/clientCapabilities");
                const auto *client = meta->find("io.modelcontextprotocol/clientInfo");
                if (!caps || !caps->object_if() ||
                    (client && (!client->object_if() || !client->find("name") ||
                                !client->find("name")->string_if() || !client->find("version") ||
                                !client->find("version")->string_if())))
                {
                    protocol_error(response_id, -32602, "Modern client metadata is invalid");
                    return;
                }
                modern = true;
            }
        }
        else if (!legacy_ready_ && name != "ping")
        {
            protocol_error(response_id, -32002,
                           "Initialize the legacy protocol or declare modern request metadata");
            return;
        }
        if (name == "server/discover")
        {
            if (!known_parameters(params, {}))
            {
                protocol_error(response_id, -32602, "Unknown discovery parameter");
                return;
            }
            if (!modern)
            {
                protocol_error(response_id, -32602, "server/discover requires modern metadata");
                return;
            }
            reply(response_id,
                  JsonValue::Object{
                      {"supportedVersions", JsonValue::Array{"2026-07-28", "2025-11-25"}},
                      {"capabilities", capabilities()},
                      {"instructions", instructions},
                      {"ttlMs", JsonValue::number("60000")},
                      {"cacheScope", "public"}},
                  true);
            return;
        }
        if (name == "ping")
        {
            if (!known_parameters(params, {}))
                protocol_error(response_id, -32602, "Unknown ping parameter");
            else
                reply(response_id, JsonValue::Object{}, modern);
            return;
        }
        if (name == "tools/list")
        {
            if (!known_parameters(params, {"cursor"}))
            {
                protocol_error(response_id, -32602, "Unknown tool-list parameter");
                return;
            }
            if (params && params->find("cursor"))
            {
                protocol_error(response_id, -32602, "Tool cursor is invalid");
                return;
            }
            auto object = JsonValue::Object{{"tools", mcp_tool_definitions()}};
            if (modern)
            {
                object.emplace("ttlMs", JsonValue::number("60000"));
                object.emplace("cacheScope", "public");
            }
            reply(response_id, std::move(object), modern);
            return;
        }
        if (name == "resources/templates/list")
        {
            if (!known_parameters(params, {}) || (params && params->find("cursor")))
                protocol_error(response_id, -32602, "Invalid template-list parameter");
            else
                reply(response_id, JsonValue::Object{{"resourceTemplates", JsonValue::Array{}}},
                      modern);
            return;
        }
        if (name == "resources/list")
        {
            if (!known_parameters(params, {"cursor"}))
            {
                protocol_error(response_id, -32602, "Unknown resource-list parameter");
                return;
            }
            if (params && params->find("cursor"))
            {
                protocol_error(response_id, -32602, "Resource cursor is invalid");
                return;
            }
            JsonValue::Array list;
            for (const auto &[uri, resource] : resources_)
                list.push_back(JsonValue::Object{
                    {"uri", uri},
                    {"name", "Ravo preview"},
                    {"mimeType", "image/png"},
                    {"size", JsonValue::number(std::to_string(resource.bytes.size()))}});
            auto object = JsonValue::Object{{"resources", std::move(list)}};
            if (modern)
            {
                object.emplace("ttlMs", JsonValue::number("0"));
                object.emplace("cacheScope", "private");
            }
            reply(response_id, std::move(object), modern);
            return;
        }
        if (name == "resources/read")
        {
            if (!known_parameters(params, {"uri"}))
            {
                protocol_error(response_id, -32602, "Unknown resource-read parameter");
                return;
            }
            const auto *uri = params ? params->find("uri") : nullptr;
            if (!uri || !uri->string_if())
            {
                protocol_error(response_id, -32602, "Resource URI is required");
                return;
            }
            const auto found = resources_.find(*uri->string_if());
            if (found == resources_.end())
            {
                protocol_error(response_id, -32002, "Resource is unknown or expired");
                return;
            }
            reply(response_id,
                  JsonValue::Object{
                      {"contents", JsonValue::Array{JsonValue::Object{
                                       {"uri", found->first},
                                       {"mimeType", "image/png"},
                                       {"blob", found->second.bytes.toBase64().toStdString()}}}}},
                  modern);
            return;
        }
        if (name != "tools/call")
        {
            protocol_error(response_id, -32601, "MCP method is not supported");
            return;
        }
        if (!known_parameters(params, {"name", "arguments"}))
        {
            protocol_error(response_id, -32602, "Unknown tool-call parameter");
            return;
        }
        const auto *tool = params ? params->find("name") : nullptr;
        const auto *args = params ? params->find("arguments") : nullptr;
        if (!tool || !tool->string_if() || !args || !args->object_if())
        {
            protocol_error(response_id, -32602, "Tool name and arguments object are required");
            return;
        }
        const auto definitions = mcp_tool_definitions();
        if (std::none_of(definitions.array_if()->begin(), definitions.array_if()->end(),
                         [&](const auto &definition)
                         { return *definition.find("name")->string_if() == *tool->string_if(); }))
        {
            protocol_error(response_id, -32602, "Unknown MCP tool");
            return;
        }
        const auto key = serialize_json(response_id);
        if (jobs_.contains(key) || jobs_.size() >= 8)
        {
            protocol_error(response_id, -32000, "Duplicate request ID or request capacity is busy");
            return;
        }
        auto job = std::make_shared<Job>();
        job->id = response_id;
        job->modern = modern;
        job->tool = *tool->string_if();
        job->directory = std::make_unique<QTemporaryDir>();
        if (!job->directory->isValid())
        {
            protocol_error(response_id, -32000, "Cannot create private request directory");
            return;
        }
        auto invocation = mcp_invocation(job->tool, *args, job->directory->filePath("request.json"),
                                         job->directory->filePath("image.png"));
        jobs_.emplace(key, job);
        if (!invocation)
        {
            finish(job, task_error_json(invocation.error()), true);
            return;
        }
        job->invocation = std::move(invocation).value();
        job->elapsed.start();
        if (job->invocation.request)
        {
            QFile file(job->directory->filePath("request.json"));
            const auto bytes = QByteArray::fromStdString(serialize_json(*job->invocation.request));
            if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
                file.write(bytes) != bytes.size() || !file.flush())
            {
                finish(job, error_json("io", "Cannot write the bounded CLI request"), true);
                return;
            }
        }
        auto *deadline_timer = new QTimer(&context_);
        job->deadline_timer = deadline_timer;
        deadline_timer->setSingleShot(true);
        QObject::connect(deadline_timer, &QTimer::timeout, &context_,
                         [this, job]
                         {
                             if (!job->done)
                                 stop(job, false,
                                      error_json("cancelled", "MCP tool deadline expired"));
                         });
        deadline_timer->start(job->invocation.timeout_ms);
        if (job->invocation.write && job->invocation.operation_id.empty())
        {
            job->reserving = true;
            start_child(job, {"studio", "operation", "--action", "reserve", "--session-id",
                              QString::fromStdString(job->invocation.session_id), "--timeout-ms",
                              QString::number(job->invocation.timeout_ms), "--json"});
        }
        else
        {
            if (job->invocation.write)
                job->invocation.arguments << "--operation-id"
                                          << QString::fromStdString(job->invocation.operation_id);
            start_child(job, job->invocation.arguments);
        }
    }
    std::ostream &output_;
    std::ostream &diagnostics_;
    QObject context_;
    QEventLoop loop_;
    SerialExecutor reader_;
    QSemaphore pending_input_{16};
    bool legacy_initialized_ = false;
    bool legacy_ready_ = false;
    bool closing_ = false;
    std::map<std::string, std::shared_ptr<Job>> jobs_;
    std::map<std::string, Resource> resources_;
    std::deque<std::string> resource_order_;
    qsizetype resource_bytes_ = 0;
};
} // namespace
int run_mcp_server(std::ostream &output, std::ostream &diagnostics)
{
    McpServer server(output, diagnostics);
    return server.run();
}
} // namespace ravo::cli_internal
