#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <QObject>

#include "ravo/control/live_control.h"
#include "ravo/foundation/error.h"
#include "ravo/foundation/json.h"

namespace ravo
{

class StudioCommandController;
class StudioPresenter;

class StudioLiveSessionController final : public QObject
{
public:
    [[nodiscard]] static Result<std::unique_ptr<StudioLiveSessionController>>
    create(StudioPresenter &presenter, StudioCommandController &commands,
           QObject *parent = nullptr);

    ~StudioLiveSessionController() override;

    [[nodiscard]] const LiveSessionDescriptor &descriptor() const noexcept;
    [[nodiscard]] const std::filesystem::path &descriptorPath() const noexcept;
    [[nodiscard]] JsonValue snapshot() const;

private:
    StudioLiveSessionController(StudioPresenter &presenter, StudioCommandController &commands,
                                QObject *parent);

    [[nodiscard]] Result<void> start();
    void refresh();
    [[nodiscard]] Result<JsonValue> handle(const LiveControlRequest &request);

    StudioPresenter &presenter_;
    StudioCommandController &commands_;
    LiveSessionDescriptor descriptor_;
    std::unique_ptr<LocalControlServer> server_;
    std::uint64_t session_revision_ = 1;
    std::uint64_t selection_revision_ = 1;
    std::uint64_t recipe_revision_ = 0;
    std::uint64_t saved_recipe_revision_ = 0;
    std::uint64_t preview_state_revision_ = 0;
    std::string selection_identity_;
    std::string current_recipe_json_;
    std::string saved_recipe_json_;
    std::string baseline_recipe_json_;
    std::string recipe_error_;
    std::string preview_identity_;
};

} // namespace ravo
