#pragma once

#include <functional>

#include "ravo/services/catalog_service.h"

namespace ravo::testing
{

class CatalogServiceTestControl
{
public:
    static void set_before_import_publication(CatalogService &service,
                                              std::function<void()> callback);
};

} // namespace ravo::testing
