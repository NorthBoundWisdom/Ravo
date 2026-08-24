#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

enum class RenderBackend
{
    kCpu,
};

struct RenderRequest
{
    AssetDescriptor asset;
    Recipe recipe;
    std::string output_uri;
    std::optional<std::uint32_t> output_width;
    std::optional<std::uint32_t> output_height;
    std::uint64_t memory_budget_bytes = 0;
    std::uint32_t worker_count = 1;
    bool deterministic = true;
    RenderBackend backend = RenderBackend::kCpu;
    CancellationToken cancellation;
    std::string correlation_id;
};

struct RenderResult
{
    std::string correlation_id;
    std::string output_uri;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct InspectionResult
{
    std::string input_uri;
    std::string format;
    std::string make;
    std::string model;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool is_raw = false;
};

struct EmbeddedPreview
{
    std::string mime_type;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> bytes;
};

struct ProgressEvent
{
    std::string correlation_id;
    std::string stage;
    std::uint64_t completed_units = 0;
    std::uint64_t total_units = 0;
};

class ProgressSink
{
public:
    virtual ~ProgressSink() = default;
    virtual void on_progress(const ProgressEvent &event) = 0;
};

class EngineFacade
{
public:
    [[nodiscard]] static Result<EngineFacade> create_phase1();

    // The facade does not retain input_uri or token after this synchronous call.
    [[nodiscard]] Result<InspectionResult> inspect(std::string_view input_uri,
                                                   const CancellationToken &cancellation) const;
    [[nodiscard]] Result<EmbeddedPreview>
    extract_embedded_preview(std::string_view input_uri,
                             const CancellationToken &cancellation) const;
    [[nodiscard]] const std::vector<OperationDescriptor> &operations() const noexcept;
    [[nodiscard]] Result<Recipe> upgrade(Recipe recipe) const;
    [[nodiscard]] Result<void> validate(const Recipe &recipe) const;

    // The sink is borrowed only for the duration of this synchronous call.
    [[nodiscard]] Result<RenderResult> render(const RenderRequest &request,
                                              ProgressSink *progress_sink = nullptr) const;

private:
    explicit EngineFacade(OperationRegistry registry);

    OperationRegistry registry_;
};

} // namespace ravo
