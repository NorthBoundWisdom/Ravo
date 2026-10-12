#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/json.h"

namespace ravo
{
// GUI-owned, bounded receipts for one Studio session. IDs must first be
// reserved by this owner: an evicted/expired ID can never become a new write.
class StudioOperationStore
{
public:
    [[nodiscard]] Result<JsonValue> reserve(std::string_view session_id);
    [[nodiscard]] Result<JsonValue> get(std::string_view id) const;
    [[nodiscard]] Result<bool> begin(std::string_view id, const JsonValue &request);
    [[nodiscard]] Result<CancellationToken> token(std::string_view id) const;
    [[nodiscard]] Result<JsonValue> cancel(std::string_view id);
    void finish(std::string_view id, bool committed, std::int64_t revision,
                std::int64_t history_head, std::string error_code = {}, std::string message = {});
    void stage(std::string_view id, const JsonValue &state, bool changed);
    void unresolved(std::string_view id, const TaskError &error);
    void progress(std::string_view id, JsonValue result);
    void cancel_all();

private:
    struct Entry
    {
        std::string request;
        JsonValue::Object receipt;
        CancellationSource cancellation;
        bool active = false;
    };
    std::map<std::string, Entry, std::less<>> entries_;
    std::deque<std::string> order_;
    std::uint64_t sequence_ = 0;
};
} // namespace ravo
