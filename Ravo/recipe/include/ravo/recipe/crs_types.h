#pragma once

#include <string>

namespace ravo
{

// Shared CRS interchange omission record. Lives in recipe so adapters can emit
// it and services/CLI can surface it without services public headers depending
// on adapters (ADR-0120).
struct CrsOmission
{
    std::string key;
    std::string value;
    std::string reason;
};

} // namespace ravo
