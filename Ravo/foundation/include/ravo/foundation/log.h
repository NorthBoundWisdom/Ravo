#pragma once

#include <string_view>

#include <rlog/logger.h>

namespace ravo
{

void init_logging(std::string_view app_name);
void shutdown_logging();

} // namespace ravo
