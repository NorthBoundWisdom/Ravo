#include "ravo/control/live_control.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <map>
#include <string_view>
#include <utility>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QStandardPaths>
#include <QString>
#include <QTimer>
#include <QUuid>

namespace ravo
{
namespace
{

[[nodiscard]] std::string utf8(const QString &value)
{
    const QByteArray bytes = value.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

[[nodiscard]] QString qstring(const std::string_view value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

[[nodiscard]] Result<const JsonValue::Object *> object_at(const JsonValue &value,
                                                          const std::string_view location)
{
    const auto *object = value.object_if();
    if (object == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Live-control JSON value must be an object",
                          {{"location", std::string(location)}});
    }
    return object;
}

[[nodiscard]] Result<void>
reject_unknown_fields(const JsonValue::Object &object,
                      const std::initializer_list<std::string_view> allowed,
                      const std::string_view location)
{
    for (const auto &[key, value] : object)
    {
        static_cast<void>(value);
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
        {
            return make_error(ErrorCode::kValidation, "Unknown live-control JSON field",
                              {{"field", key}, {"location", std::string(location)}});
        }
    }
    return {};
}

[[nodiscard]] Result<std::string> required_string(const JsonValue::Object &object,
                                                  const std::string_view key,
                                                  const std::string_view location,
                                                  const std::size_t maximum = 4096U)
{
    const auto found = object.find(key);
    if (found == object.end() || found->second.string_if() == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Required live-control string is missing",
                          {{"field", std::string(key)}, {"location", std::string(location)}});
    }
    const auto &text = *found->second.string_if();
    if (text.empty() || text.size() > maximum)
    {
        return make_error(ErrorCode::kValidation, "Live-control string has an invalid size",
                          {{"field", std::string(key)}, {"location", std::string(location)}});
    }
    return text;
}

template <typename Integer>
[[nodiscard]] Result<Integer> required_integer(const JsonValue::Object &object,
                                               const std::string_view key,
                                               const std::string_view location)
{
    const auto found = object.find(key);
    if (found == object.end() || found->second.number_if() == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Required live-control integer is missing",
                          {{"field", std::string(key)}, {"location", std::string(location)}});
    }
    const auto &text = found->second.number_if()->text;
    Integer value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    {
        return make_error(ErrorCode::kValidation, "Live-control integer is invalid",
                          {{"field", std::string(key)}, {"location", std::string(location)}});
    }
    return value;
}

[[nodiscard]] std::optional<ErrorCode> error_code_from_name(const std::string_view name) noexcept
{
    for (const auto code : {ErrorCode::kInternal, ErrorCode::kInvalidArgument, ErrorCode::kNotFound,
                            ErrorCode::kValidation, ErrorCode::kUnsupported, ErrorCode::kIo,
                            ErrorCode::kCancelled, ErrorCode::kConflict})
    {
        if (error_code_name(code) == name)
        {
            return code;
        }
    }
    return std::nullopt;
}

[[nodiscard]] JsonValue error_json(const TaskError &error)
{
    JsonValue::Object context;
    for (const auto &[key, value] : error.context)
    {
        context.emplace(key, value);
    }
    return JsonValue::Object{{"code", std::string(error_code_name(error.code))},
                             {"context", std::move(context)},
                             {"message", error.message}};
}

struct ParsedTaskError
{
    TaskError value;
};

[[nodiscard]] Result<ParsedTaskError> error_from_json(const JsonValue &value)
{
    auto object = object_at(value, "response.error");
    if (!object)
    {
        return object.error();
    }
    auto known =
        reject_unknown_fields(*object.value(), {"code", "context", "message"}, "response.error");
    if (!known)
    {
        return known.error();
    }
    auto code_name = required_string(*object.value(), "code", "response.error", 64U);
    auto message = required_string(*object.value(), "message", "response.error", 16U * 1024U);
    if (!code_name)
    {
        return code_name.error();
    }
    if (!message)
    {
        return message.error();
    }
    const auto code = error_code_from_name(code_name.value());
    if (!code)
    {
        return make_error(ErrorCode::kValidation, "Live-control error code is unknown",
                          {{"code", code_name.value()}});
    }
    std::map<std::string, std::string, std::less<>> context;
    const auto found_context = object.value()->find("context");
    if (found_context == object.value()->end() || found_context->second.object_if() == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Live-control error context is missing");
    }
    for (const auto &[key, item] : *found_context->second.object_if())
    {
        if (item.string_if() == nullptr || key.size() > 256U || item.string_if()->size() > 4096U)
        {
            return make_error(ErrorCode::kValidation,
                              "Live-control error context must contain bounded strings");
        }
        context.emplace(key, *item.string_if());
    }
    return ParsedTaskError{make_error(*code, std::move(message).value(), std::move(context))};
}

[[nodiscard]] JsonValue response_json(const std::string_view request_id,
                                      const Result<JsonValue> &result)
{
    JsonValue::Object response{{"ok", static_cast<bool>(result)},
                               {"protocol", std::string(kLiveControlProtocol)},
                               {"request_id", std::string(request_id)},
                               {"type", "ravo.studio.control.response"},
                               {"version", JsonValue::number("1")}};
    if (result)
    {
        response.emplace("result", result.value());
    }
    else
    {
        response.emplace("error", error_json(result.error()));
    }
    return response;
}

[[nodiscard]] Result<JsonValue> parse_response(const JsonValue &value,
                                               const std::string_view expected_request_id)
{
    auto object = object_at(value, "response");
    if (!object)
    {
        return object.error();
    }
    auto known = reject_unknown_fields(
        *object.value(), {"error", "ok", "protocol", "request_id", "result", "type", "version"},
        "response");
    if (!known)
    {
        return known.error();
    }
    auto protocol = required_string(*object.value(), "protocol", "response", 64U);
    auto request_id = required_string(*object.value(), "request_id", "response", 128U);
    auto type = required_string(*object.value(), "type", "response", 128U);
    auto version = required_integer<std::int64_t>(*object.value(), "version", "response");
    if (!protocol)
    {
        return protocol.error();
    }
    if (!request_id)
    {
        return request_id.error();
    }
    if (!type)
    {
        return type.error();
    }
    if (!version)
    {
        return version.error();
    }
    if (protocol.value() != kLiveControlProtocol ||
        type.value() != "ravo.studio.control.response" || version.value() != 1)
    {
        return make_error(ErrorCode::kUnsupported, "Unsupported live-control response protocol");
    }
    if (request_id.value() != expected_request_id)
    {
        return make_error(
            ErrorCode::kConflict, "Live-control response request ID does not match",
            {{"expected", std::string(expected_request_id)}, {"actual", request_id.value()}});
    }
    const auto ok = object.value()->find("ok");
    if (ok == object.value()->end() || ok->second.boolean_if() == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Live-control response ok flag is missing");
    }
    if (*ok->second.boolean_if())
    {
        const auto result = object.value()->find("result");
        if (result == object.value()->end() || object.value()->contains("error"))
        {
            return make_error(ErrorCode::kValidation,
                              "Successful live-control response has invalid payload");
        }
        return result->second;
    }
    const auto error = object.value()->find("error");
    if (error == object.value()->end() || object.value()->contains("result"))
    {
        return make_error(ErrorCode::kValidation,
                          "Failed live-control response has invalid payload");
    }
    auto parsed = error_from_json(error->second);
    return parsed ? Result<JsonValue>{std::move(parsed).value().value} :
                    Result<JsonValue>{parsed.error()};
}

[[nodiscard]] QByteArray wire_bytes(const JsonValue &value)
{
    QByteArray bytes = QByteArray::fromStdString(serialize_json(value));
    bytes.push_back('\n');
    return bytes;
}

[[nodiscard]] Result<JsonValue> parse_wire_bytes(const QByteArray &bytes,
                                                 const std::string_view location)
{
    if (bytes.size() <= 0 || static_cast<std::size_t>(bytes.size()) > kLiveControlMaxMessageBytes)
    {
        return make_error(
            ErrorCode::kValidation, "Live-control message size is invalid",
            {{"location", std::string(location)}, {"bytes", std::to_string(bytes.size())}});
    }
    return parse_json(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
}

[[nodiscard]] Result<void> validate_descriptor(const LiveSessionDescriptor &descriptor)
{
    if (descriptor.schema_version != kLiveControlSchemaVersion ||
        descriptor.protocol != kLiveControlProtocol)
    {
        return make_error(ErrorCode::kUnsupported, "Unsupported live-session descriptor version");
    }
    const auto portable_name = [](const std::string_view text, const bool dot)
    {
        return !text.empty() && std::all_of(text.begin(), text.end(),
                                            [dot](const char value)
                                            {
                                                const auto byte = static_cast<unsigned char>(value);
                                                const bool alphanumeric =
                                                    (byte >= static_cast<unsigned char>('a') &&
                                                     byte <= static_cast<unsigned char>('z')) ||
                                                    (byte >= static_cast<unsigned char>('A') &&
                                                     byte <= static_cast<unsigned char>('Z')) ||
                                                    (byte >= static_cast<unsigned char>('0') &&
                                                     byte <= static_cast<unsigned char>('9'));
                                                return alphanumeric || value == '-' ||
                                                       value == '_' || (dot && value == '.');
                                            });
    };
    if (!portable_name(descriptor.session_id, false) || descriptor.session_id.size() > 128U ||
        !portable_name(descriptor.server_name, true) || descriptor.server_name.size() > 256U ||
        descriptor.process_id == 0U || descriptor.executable_path.empty() ||
        descriptor.executable_path.size() > 16U * 1024U ||
        descriptor.workspace_root.size() > 16U * 1024U)
    {
        return make_error(ErrorCode::kValidation, "Live-session descriptor is incomplete");
    }
    if (!filesystem_path_from_utf8(descriptor.executable_path).is_absolute() ||
        (!descriptor.workspace_root.empty() &&
         !filesystem_path_from_utf8(descriptor.workspace_root).is_absolute()))
    {
        return make_error(ErrorCode::kValidation, "Live-session descriptor paths must be absolute");
    }
    return {};
}

[[nodiscard]] Result<void> ensure_owner_only_directory(const QString &path)
{
    if (!QDir().mkpath(path))
    {
        return make_error(ErrorCode::kIo, "Cannot create the live-control registry directory",
                          {{"path", utf8(path)}});
    }
    const QFileInfo info(path);
    if (!info.isDir() || info.isSymLink())
    {
        return make_error(ErrorCode::kIo, "Live-control registry path is not a real directory",
                          {{"path", utf8(path)}});
    }
#if !defined(Q_OS_WIN)
    const auto permissions =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    if (!QFile::setPermissions(path, permissions))
    {
        return make_error(ErrorCode::kIo,
                          "Cannot restrict the live-control registry to the current user",
                          {{"path", utf8(path)}});
    }
#endif
    return {};
}

class LocalSocketCloseGuard final
{
public:
    LocalSocketCloseGuard(QLocalSocket &socket, const int timeout_ms) noexcept
        : socket_(&socket)
        , timeout_ms_(std::clamp(timeout_ms, 1, 1000))
    {
    }

    ~LocalSocketCloseGuard()
    {
        if (socket_->state() == QLocalSocket::UnconnectedState)
        {
            return;
        }
        socket_->disconnectFromServer();
        if (socket_->state() != QLocalSocket::UnconnectedState)
        {
            static_cast<void>(socket_->waitForDisconnected(timeout_ms_));
        }
        if (socket_->state() != QLocalSocket::UnconnectedState)
        {
            socket_->abort();
        }
    }

    LocalSocketCloseGuard(const LocalSocketCloseGuard &) = delete;
    LocalSocketCloseGuard &operator=(const LocalSocketCloseGuard &) = delete;

private:
    QLocalSocket *socket_ = nullptr;
    int timeout_ms_ = 0;
};

} // namespace

Result<JsonValue> live_session_descriptor_to_json(const LiveSessionDescriptor &descriptor)
{
    auto valid = validate_descriptor(descriptor);
    if (!valid)
    {
        return valid.error();
    }
    return JsonValue{JsonValue::Object{
        {"executable_path", descriptor.executable_path},
        {"process_id", JsonValue::number(std::to_string(descriptor.process_id))},
        {"protocol", descriptor.protocol},
        {"schema_version", JsonValue::number(std::to_string(descriptor.schema_version))},
        {"server_name", descriptor.server_name},
        {"session_id", descriptor.session_id},
        {"workspace_root", descriptor.workspace_root},
    }};
}

Result<LiveSessionDescriptor> live_session_descriptor_from_json(const JsonValue &value)
{
    auto object = object_at(value, "descriptor");
    if (!object)
    {
        return object.error();
    }
    auto known =
        reject_unknown_fields(*object.value(),
                              {"executable_path", "process_id", "protocol", "schema_version",
                               "server_name", "session_id", "workspace_root"},
                              "descriptor");
    if (!known)
    {
        return known.error();
    }
    LiveSessionDescriptor descriptor;
    auto schema_version =
        required_integer<std::int64_t>(*object.value(), "schema_version", "descriptor");
    auto process_id = required_integer<std::uint64_t>(*object.value(), "process_id", "descriptor");
    auto protocol = required_string(*object.value(), "protocol", "descriptor", 64U);
    auto session_id = required_string(*object.value(), "session_id", "descriptor", 128U);
    auto server_name = required_string(*object.value(), "server_name", "descriptor", 4096U);
    auto executable =
        required_string(*object.value(), "executable_path", "descriptor", 16U * 1024U);
    if (!schema_version)
        return schema_version.error();
    if (!process_id)
        return process_id.error();
    if (!protocol)
        return protocol.error();
    if (!session_id)
        return session_id.error();
    if (!server_name)
        return server_name.error();
    if (!executable)
        return executable.error();
    const auto workspace = object.value()->find("workspace_root");
    if (workspace == object.value()->end() || workspace->second.string_if() == nullptr ||
        workspace->second.string_if()->size() > 16U * 1024U)
    {
        return make_error(ErrorCode::kValidation,
                          "Live-session workspace root must be a bounded string");
    }
    descriptor.schema_version = schema_version.value();
    descriptor.process_id = process_id.value();
    descriptor.protocol = std::move(protocol).value();
    descriptor.session_id = std::move(session_id).value();
    descriptor.server_name = std::move(server_name).value();
    descriptor.executable_path = std::move(executable).value();
    descriptor.workspace_root = *workspace->second.string_if();
    auto valid = validate_descriptor(descriptor);
    return valid ? Result<LiveSessionDescriptor>{std::move(descriptor)} :
                   Result<LiveSessionDescriptor>{valid.error()};
}

JsonValue live_control_request_to_json(const LiveControlRequest &request)
{
    return JsonValue::Object{{"method", request.method},
                             {"params", request.params},
                             {"protocol", std::string(kLiveControlProtocol)},
                             {"request_id", request.request_id},
                             {"type", "ravo.studio.control.request"},
                             {"version", JsonValue::number("1")}};
}

Result<LiveControlRequest> live_control_request_from_json(const JsonValue &value)
{
    auto object = object_at(value, "request");
    if (!object)
    {
        return object.error();
    }
    auto known = reject_unknown_fields(
        *object.value(), {"method", "params", "protocol", "request_id", "type", "version"},
        "request");
    if (!known)
    {
        return known.error();
    }
    auto protocol = required_string(*object.value(), "protocol", "request", 64U);
    auto request_id = required_string(*object.value(), "request_id", "request", 128U);
    auto method = required_string(*object.value(), "method", "request", 128U);
    auto type = required_string(*object.value(), "type", "request", 128U);
    auto version = required_integer<std::int64_t>(*object.value(), "version", "request");
    if (!protocol)
        return protocol.error();
    if (!request_id)
        return request_id.error();
    if (!method)
        return method.error();
    if (!type)
        return type.error();
    if (!version)
        return version.error();
    if (protocol.value() != kLiveControlProtocol || type.value() != "ravo.studio.control.request" ||
        version.value() != 1)
    {
        return make_error(ErrorCode::kUnsupported, "Unsupported live-control request protocol");
    }
    const auto params = object.value()->find("params");
    if (params == object.value()->end() || params->second.object_if() == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Live-control request params must be an object");
    }
    return LiveControlRequest{std::move(request_id).value(), std::move(method).value(),
                              params->second};
}

Result<std::filesystem::path> live_control_registry_directory()
{
    QString path = qEnvironmentVariable("RAVO_LIVE_CONTROL_DIR").trimmed();
    if (path.isEmpty())
    {
        QString root = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        if (root.isEmpty())
        {
            root = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        }
        if (root.isEmpty())
        {
            return make_error(ErrorCode::kIo,
                              "No per-user runtime directory is available for live control");
        }
        path = QDir(root).filePath(QStringLiteral("ravo-live-control"));
    }
    path = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const QString temporary_root =
        QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::TempLocation));
    const QString runtime_root =
        QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation));
    if (path == QDir::rootPath() || path == QDir::homePath() ||
        (!temporary_root.isEmpty() && path == temporary_root) ||
        (!runtime_root.isEmpty() && path == runtime_root))
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Live-control registry must be a dedicated subdirectory",
                          {{"path", utf8(path)}});
    }
    auto ready = ensure_owner_only_directory(path);
    if (!ready)
    {
        return ready.error();
    }
    return filesystem_path_from_utf8(utf8(path));
}

std::string filesystem_path_to_utf8(const std::filesystem::path &path)
{
    const auto units = path.generic_u8string();
    return {reinterpret_cast<const char *>(units.data()), units.size()};
}

std::filesystem::path filesystem_path_from_utf8(const std::string_view text)
{
    std::u8string units;
    units.reserve(text.size());
    for (const char byte : text)
        units.push_back(static_cast<char8_t>(static_cast<unsigned char>(byte)));
    return std::filesystem::path(units);
}

Result<std::optional<std::filesystem::path>>
find_ravo_workspace_root(const std::filesystem::path &start)
{
    std::error_code error;
    auto current = std::filesystem::weakly_canonical(start, error);
    if (error)
    {
        return make_error(ErrorCode::kIo, "Cannot resolve the Ravo workspace candidate",
                          {{"path", filesystem_path_to_utf8(start)}, {"reason", error.message()}});
    }
    if (!std::filesystem::is_directory(current, error))
    {
        if (error)
        {
            return make_error(
                ErrorCode::kIo, "Cannot inspect the Ravo workspace candidate",
                {{"path", filesystem_path_to_utf8(current)}, {"reason", error.message()}});
        }
        current = current.parent_path();
    }
    while (!current.empty())
    {
        const auto regular_marker = [&](const std::filesystem::path &path) -> Result<bool>
        {
            error.clear();
            const auto status = std::filesystem::status(path, error);
            if (error == std::errc::no_such_file_or_directory)
            {
                error.clear();
                return false;
            }
            if (error)
            {
                return make_error(
                    ErrorCode::kIo, "Cannot inspect a workspace marker",
                    {{"path", filesystem_path_to_utf8(path)}, {"reason", error.message()}});
            }
            return std::filesystem::is_regular_file(status);
        };
        auto agents = regular_marker(current / "AGENTS.md");
        if (!agents)
            return agents.error();
        auto cmake = regular_marker(current / "Ravo" / "CMakeLists.txt");
        if (!cmake)
            return cmake.error();
        if (agents.value() && cmake.value())
        {
            return std::optional<std::filesystem::path>{current};
        }
        const auto parent = current.parent_path();
        if (parent == current)
        {
            break;
        }
        current = parent;
    }
    return std::optional<std::filesystem::path>{};
}

struct LocalControlServer::Impl
{
    LiveSessionDescriptor descriptor;
    std::filesystem::path descriptor_path;
    Handler handler;
    std::unique_ptr<QLocalServer> server;
    QHash<QLocalSocket *, QByteArray> buffers;
    bool descriptor_owned = false;

    void send(QLocalSocket *socket, const std::string_view request_id,
              const Result<JsonValue> &result)
    {
        if (socket == nullptr)
        {
            return;
        }
        QByteArray response = wire_bytes(response_json(request_id, result));
        if (static_cast<std::size_t>(response.size()) > kLiveControlMaxMessageBytes)
        {
            response = wire_bytes(response_json(
                request_id,
                make_error(ErrorCode::kValidation, "Live-control response exceeds the byte limit",
                           {{"maximum", std::to_string(kLiveControlMaxMessageBytes)}})));
        }
        socket->write(response);
        socket->flush();
        socket->disconnectFromServer();
    }

    void consume(QLocalSocket *socket)
    {
        auto found = buffers.find(socket);
        if (found == buffers.end())
        {
            return;
        }
        found.value().append(socket->readAll());
        if (static_cast<std::size_t>(found.value().size()) > kLiveControlMaxMessageBytes)
        {
            send(socket, "invalid",
                 make_error(ErrorCode::kValidation, "Live-control request exceeds the byte limit",
                            {{"maximum", std::to_string(kLiveControlMaxMessageBytes)}}));
            return;
        }
        const auto newline = found.value().indexOf('\n');
        if (newline < 0)
        {
            return;
        }
        const QByteArray complete = found.value();
        buffers.erase(found);
        const QByteArray request_bytes = complete.left(newline);
        if (!complete.mid(newline + 1).trimmed().isEmpty())
        {
            send(socket, "invalid",
                 make_error(ErrorCode::kValidation,
                            "A live-control connection accepts exactly one request"));
            return;
        }
        auto json = parse_wire_bytes(request_bytes, "request");
        if (!json)
        {
            send(socket, "invalid", json.error());
            return;
        }
        auto request = live_control_request_from_json(json.value());
        if (!request)
        {
            send(socket, "invalid", request.error());
            return;
        }
        if (request.value().method == "ping")
        {
            send(socket, request.value().request_id, live_session_descriptor_to_json(descriptor));
            return;
        }
        send(socket, request.value().request_id, handler(request.value()));
    }

    ~Impl()
    {
        if (server)
        {
            server->close();
            QLocalServer::removeServer(qstring(descriptor.server_name));
        }
        if (descriptor_owned && !descriptor_path.empty())
        {
            QFile::remove(qstring(filesystem_path_to_utf8(descriptor_path)));
        }
    }
};

Result<std::unique_ptr<LocalControlServer>>
LocalControlServer::start(LiveSessionDescriptor descriptor, Handler handler)
{
    if (!handler)
    {
        return make_error(ErrorCode::kInvalidArgument, "Live-control server requires a handler");
    }
    if (descriptor.server_name.empty() && !descriptor.session_id.empty())
    {
        descriptor.server_name = "ravo-studio-" + descriptor.session_id;
    }
    auto valid = validate_descriptor(descriptor);
    if (!valid)
    {
        return valid.error();
    }
    auto registry = live_control_registry_directory();
    if (!registry)
    {
        return registry.error();
    }
    auto impl = std::make_unique<Impl>();
    impl->descriptor = std::move(descriptor);
    impl->descriptor_path = registry.value() / (impl->descriptor.session_id + ".json");
    impl->handler = std::move(handler);
    impl->server = std::make_unique<QLocalServer>();
    impl->server->setSocketOptions(QLocalServer::UserAccessOption);
    impl->server->setMaxPendingConnections(8);
    if (!impl->server->listen(qstring(impl->descriptor.server_name)))
    {
        return make_error(ErrorCode::kIo, "Cannot start the Studio live-control endpoint",
                          {{"reason", utf8(impl->server->errorString())},
                           {"server_name", impl->descriptor.server_name}});
    }
    QObject::connect(
        impl->server.get(), &QLocalServer::newConnection, impl->server.get(),
        [owner = impl.get()]()
        {
            while (owner->server->hasPendingConnections())
            {
                QLocalSocket *socket = owner->server->nextPendingConnection();
                if (socket == nullptr)
                {
                    continue;
                }
                socket->setReadBufferSize(static_cast<qint64>(kLiveControlMaxMessageBytes + 1U));
                owner->buffers.insert(socket, {});
                QObject::connect(socket, &QLocalSocket::readyRead, socket,
                                 [owner, socket]() { owner->consume(socket); });
                QObject::connect(socket, &QLocalSocket::disconnected, socket,
                                 [owner, socket]()
                                 {
                                     owner->buffers.remove(socket);
                                     socket->deleteLater();
                                 });
                QTimer::singleShot(5000, socket,
                                   [socket]()
                                   {
                                       if (socket->state() != QLocalSocket::UnconnectedState)
                                       {
                                           socket->disconnectFromServer();
                                       }
                                   });
            }
        });

    auto descriptor_json = live_session_descriptor_to_json(impl->descriptor);
    if (!descriptor_json)
    {
        return descriptor_json.error();
    }
    const QByteArray bytes = wire_bytes(descriptor_json.value());
    if (static_cast<std::size_t>(bytes.size()) > kLiveControlMaxDescriptorBytes)
    {
        return make_error(ErrorCode::kValidation, "Live-session descriptor exceeds the byte limit");
    }
    QFile file(qstring(filesystem_path_to_utf8(impl->descriptor_path)));
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
        return make_error(ErrorCode::kConflict, "Live-session descriptor already exists",
                          {{"path", filesystem_path_to_utf8(impl->descriptor_path)},
                           {"reason", utf8(file.errorString())}});
    }
    impl->descriptor_owned = true;
#if !defined(Q_OS_WIN)
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
    {
        return make_error(ErrorCode::kIo, "Cannot restrict the live-session descriptor",
                          {{"path", filesystem_path_to_utf8(impl->descriptor_path)}});
    }
#endif
    if (file.write(bytes) != bytes.size() || !file.flush())
    {
        return make_error(ErrorCode::kIo, "Cannot publish the live-session descriptor",
                          {{"path", filesystem_path_to_utf8(impl->descriptor_path)},
                           {"reason", utf8(file.errorString())}});
    }
    file.close();
    return std::unique_ptr<LocalControlServer>{new LocalControlServer(std::move(impl))};
}

LocalControlServer::LocalControlServer(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

LocalControlServer::~LocalControlServer() = default;
LocalControlServer::LocalControlServer(LocalControlServer &&) noexcept = default;
LocalControlServer &LocalControlServer::operator=(LocalControlServer &&) noexcept = default;

const LiveSessionDescriptor &LocalControlServer::descriptor() const noexcept
{
    return impl_->descriptor;
}

const std::filesystem::path &LocalControlServer::descriptor_path() const noexcept
{
    return impl_->descriptor_path;
}

Result<JsonValue> LocalControlClient::request(const LiveSessionDescriptor &descriptor,
                                              std::string method, JsonValue params,
                                              const int timeout_ms)
{
    auto valid = validate_descriptor(descriptor);
    if (!valid)
    {
        return valid.error();
    }
    if (method.empty() || method.size() > 128U || params.object_if() == nullptr ||
        timeout_ms <= 0 || timeout_ms > 120000)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Live-control request arguments are invalid");
    }
    const std::string request_id = utf8(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const LiveControlRequest request{request_id, std::move(method), std::move(params)};
    const QByteArray outgoing = wire_bytes(live_control_request_to_json(request));
    if (static_cast<std::size_t>(outgoing.size()) > kLiveControlMaxMessageBytes)
    {
        return make_error(ErrorCode::kValidation, "Live-control request exceeds the byte limit",
                          {{"maximum", std::to_string(kLiveControlMaxMessageBytes)}});
    }

    QLocalSocket socket;
    socket.setReadBufferSize(static_cast<qint64>(kLiveControlMaxMessageBytes + 1U));
    socket.connectToServer(qstring(descriptor.server_name), QIODevice::ReadWrite);
    if (!socket.waitForConnected(timeout_ms))
    {
        return make_error(
            ErrorCode::kNotFound, "Studio live session is unavailable",
            {{"session_id", descriptor.session_id}, {"reason", utf8(socket.errorString())}});
    }
    // A Windows named-pipe server cannot reliably accept the next request until
    // the prior client handle has completed disconnect. Bound that cleanup on
    // every post-connect return so discovery's ping can be followed immediately
    // by a state or mutation request without a transient not-found result.
    const LocalSocketCloseGuard close_guard(socket, timeout_ms);
    if (socket.write(outgoing) != outgoing.size() || !socket.waitForBytesWritten(timeout_ms))
    {
        return make_error(
            ErrorCode::kIo, "Cannot write the Studio live-control request",
            {{"session_id", descriptor.session_id}, {"reason", utf8(socket.errorString())}});
    }

    QByteArray incoming;
    QElapsedTimer timer;
    timer.start();
    while (incoming.indexOf('\n') < 0)
    {
        incoming.append(socket.readAll());
        if (static_cast<std::size_t>(incoming.size()) > kLiveControlMaxMessageBytes)
        {
            return make_error(ErrorCode::kValidation,
                              "Live-control response exceeds the byte limit",
                              {{"maximum", std::to_string(kLiveControlMaxMessageBytes)}});
        }
        if (incoming.indexOf('\n') >= 0)
        {
            break;
        }
        const int remaining = timeout_ms - static_cast<int>(timer.elapsed());
        if (remaining <= 0 || !socket.waitForReadyRead(remaining))
        {
            if (socket.state() == QLocalSocket::UnconnectedState &&
                socket.error() != QLocalSocket::SocketTimeoutError)
            {
                return make_error(ErrorCode::kIo,
                                  "Studio closed the live-control connection without a response",
                                  {{"session_id", descriptor.session_id},
                                   {"reason", utf8(socket.errorString())}});
            }
            return make_error(ErrorCode::kCancelled, "Timed out waiting for the Studio response",
                              {{"session_id", descriptor.session_id},
                               {"timeout_ms", std::to_string(timeout_ms)}});
        }
    }
    const auto newline = incoming.indexOf('\n');
    if (!incoming.mid(newline + 1).trimmed().isEmpty())
    {
        return make_error(ErrorCode::kValidation,
                          "Live-control response contains trailing messages");
    }
    auto json = parse_wire_bytes(incoming.left(newline), "response");
    if (!json)
    {
        return json.error();
    }
    return parse_response(json.value(), request_id);
}

Result<std::vector<LiveSessionDescriptor>> LocalControlClient::discover(const int timeout_ms)
{
    if (timeout_ms <= 0 || timeout_ms > 10000)
    {
        return make_error(ErrorCode::kInvalidArgument, "Discovery timeout is invalid");
    }
    auto registry = live_control_registry_directory();
    if (!registry)
    {
        return registry.error();
    }
    QDir directory(qstring(filesystem_path_to_utf8(registry.value())));
    const auto files = directory.entryInfoList({QStringLiteral("*.json")}, QDir::Files,
                                               QDir::Name | QDir::IgnoreCase);
    std::vector<LiveSessionDescriptor> sessions;
    sessions.reserve(static_cast<std::size_t>(files.size()));
    for (const QFileInfo &info : files)
    {
        if (info.isSymLink() || info.size() <= 0 ||
            static_cast<std::size_t>(info.size()) > kLiveControlMaxDescriptorBytes)
        {
            continue;
        }
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly))
        {
            continue;
        }
        const QByteArray bytes = file.readAll().trimmed();
        auto json =
            parse_json(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
        if (!json)
        {
            continue;
        }
        auto descriptor = live_session_descriptor_from_json(json.value());
        if (!descriptor)
        {
            continue;
        }
        auto ping = request(descriptor.value(), "ping", JsonValue::Object{}, timeout_ms);
        if (!ping)
        {
            continue;
        }
        auto live = live_session_descriptor_from_json(ping.value());
        if (!live || live.value() != descriptor.value())
        {
            continue;
        }
        sessions.push_back(std::move(descriptor).value());
    }
    std::sort(sessions.begin(), sessions.end(),
              [](const LiveSessionDescriptor &left, const LiveSessionDescriptor &right)
              { return left.session_id < right.session_id; });
    return sessions;
}

} // namespace ravo
