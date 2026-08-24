#pragma once

#include <string_view>

#include "quill/LogMacros.h"
#include "quill/Logger.h"

namespace ravo
{

void init_logging(std::string_view app_name);
void shutdown_logging();
[[nodiscard]] quill::Logger *logger();

} // namespace ravo
