#pragma once
#include <future>
#include "ravo/desktop/studio_presenter.h"
namespace ravo::testing
{
class StudioAutomationTestControl
{
public:
    static std::uint64_t generation(StudioPresenter &);
    static Result<LibraryPage> query(StudioPresenter &, LibraryPageRequest);
    static Result<Recipe> saved(StudioPresenter &);
    static bool block(StudioPresenter &, std::shared_ptr<std::promise<void>>,
                      std::shared_future<void>);
};
} // namespace ravo::testing
