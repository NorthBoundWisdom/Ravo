#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "ravo/control/live_control.h"
#include "ravo/engine/engine.h"
#include "ravo/recipe/recipe.h"
namespace ravo::cli_internal
{
struct StudioCliArguments
{
    // Views borrow the caller's argv span for this synchronous command only.
    std::string_view session_id;
    std::string_view workspace_root;
    std::string_view asset_id;
    std::optional<std::uint64_t> expected_session_revision;
    std::optional<std::uint64_t> expected_selection_revision;
    std::optional<std::uint64_t> expected_recipe_revision;
    std::string_view mask_action;
    std::string_view mask_arguments;
    std::string_view video_value;
    std::optional<std::uint64_t> expected_video_generation;
    JsonValue::Array fields;
    std::string_view request_file;
    std::string_view operation_id;
    std::string_view local_id;
    bool wait = false;
    std::optional<std::uint64_t> offset;
    std::optional<std::uint64_t> limit;
    std::optional<std::int64_t> expected_catalog_revision;
    std::string_view output;
    std::optional<std::uint32_t> max_edge;
    int timeout_ms = 120000;
};
struct ObservedStudioState
{
    JsonValue json;
    std::string session_id;
    std::uint64_t session_revision = 0;
    std::string asset_id;
    std::uint64_t selection_revision = 0;
    std::uint64_t recipe_revision = 0;
    std::string recipe_state;
    std::string current_recipe_json;
    std::string preview_state;
    bool preview_matches_recipe = false;
    bool busy = false;
    std::string catalog_path;
    std::int64_t catalog_revision = 0;
    std::string operation_id;
    std::string error_text;
};
[[nodiscard]] Result<StudioCliArguments> parse_studio_flags(std::span<const std::string_view>);
[[nodiscard]] Result<LiveSessionDescriptor> resolve_live_session(const StudioCliArguments &);
[[nodiscard]] Result<ObservedStudioState> observed_studio_state(JsonValue);
[[nodiscard]] Result<ObservedStudioState> read_studio_state(const LiveSessionDescriptor &,
                                                            int timeout_ms, std::size_t offset = 0,
                                                            std::size_t limit = 0);
[[nodiscard]] Result<ObservedStudioState>
wait_for_studio_state(const LiveSessionDescriptor &, std::string_view asset_id,
                      std::optional<std::string_view> expected_recipe, bool require_saved,
                      bool require_preview, int timeout_ms,
                      std::optional<std::uint64_t> selection_revision = {},
                      std::optional<std::uint64_t> recipe_revision = {});
[[nodiscard]] Result<JsonValue>
render_studio_artifact(const EngineFacade &, const LiveSessionDescriptor &,
                       const ObservedStudioState &, std::string_view output, std::uint32_t max_edge,
                       int timeout_ms, const Recipe *trial = nullptr);
[[nodiscard]] Result<JsonValue>
run_studio_automation(const EngineFacade &, std::string_view command, const StudioCliArguments &);
} // namespace ravo::cli_internal
