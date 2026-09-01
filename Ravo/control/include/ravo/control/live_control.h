#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/foundation/error.h"
#include "ravo/foundation/json.h"

namespace ravo
{

inline constexpr std::string_view kLiveControlProtocol = "ravo-studio-control/v1";
inline constexpr std::int64_t kLiveControlSchemaVersion = 1;
inline constexpr std::size_t kLiveControlMaxMessageBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kLiveControlMaxDescriptorBytes = 64U * 1024U;

struct LiveSessionDescriptor
{
    std::int64_t schema_version = kLiveControlSchemaVersion;
    std::string protocol{std::string(kLiveControlProtocol)};
    std::string session_id;
    std::string server_name;
    std::uint64_t process_id = 0;
    std::string executable_path;
    std::string workspace_root;

    [[nodiscard]] bool operator==(const LiveSessionDescriptor &) const = default;
};

struct LiveControlRequest
{
    std::string request_id;
    std::string method;
    JsonValue params{JsonValue::Object{}};
};

[[nodiscard]] Result<JsonValue>
live_session_descriptor_to_json(const LiveSessionDescriptor &descriptor);
[[nodiscard]] Result<LiveSessionDescriptor>
live_session_descriptor_from_json(const JsonValue &value);
[[nodiscard]] JsonValue live_control_request_to_json(const LiveControlRequest &request);
[[nodiscard]] Result<LiveControlRequest> live_control_request_from_json(const JsonValue &value);

// Uses RAVO_LIVE_CONTROL_DIR when present; otherwise uses Qt's per-user runtime
// location and a per-user temporary fallback. The directory is created with
// owner-only permissions before it is returned.
[[nodiscard]] Result<std::filesystem::path> live_control_registry_directory();
[[nodiscard]] std::string filesystem_path_to_utf8(const std::filesystem::path &path);
[[nodiscard]] std::filesystem::path filesystem_path_from_utf8(std::string_view text);

// Walks upward from a file or directory until it finds a Ravo checkout root.
// An unreadable ancestor marker is treated as absent. An installed executable
// that is not inside a checkout returns nullopt.
[[nodiscard]] Result<std::optional<std::filesystem::path>>
find_ravo_workspace_root(const std::filesystem::path &start);

class LocalControlServer final
{
public:
    using Handler = std::function<Result<JsonValue>(const LiveControlRequest &)>;

    [[nodiscard]] static Result<std::unique_ptr<LocalControlServer>>
    start(LiveSessionDescriptor descriptor, Handler handler);

    ~LocalControlServer();
    LocalControlServer(LocalControlServer &&) noexcept;
    LocalControlServer &operator=(LocalControlServer &&) noexcept;
    LocalControlServer(const LocalControlServer &) = delete;
    LocalControlServer &operator=(const LocalControlServer &) = delete;

    [[nodiscard]] const LiveSessionDescriptor &descriptor() const noexcept;
    [[nodiscard]] const std::filesystem::path &descriptor_path() const noexcept;

private:
    struct Impl;
    explicit LocalControlServer(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

class LocalControlClient final
{
public:
    // Reads one validated descriptor without a separate liveness ping. The
    // caller's actual request is the liveness proof for an explicit session.
    [[nodiscard]] static Result<LiveSessionDescriptor> find_descriptor(std::string_view session_id);
    [[nodiscard]] static Result<std::vector<LiveSessionDescriptor>> discover(int timeout_ms = 500);
    [[nodiscard]] static Result<JsonValue> request(const LiveSessionDescriptor &descriptor,
                                                   std::string method,
                                                   JsonValue params = JsonValue::Object{},
                                                   int timeout_ms = 5000);
};

} // namespace ravo
