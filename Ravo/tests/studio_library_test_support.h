#pragma once
#include <QTemporaryDir>
namespace ravo::studio_test_support
{
void make_paged_catalog(const QTemporaryDir &root, int count = 205,
                        bool include_unloaded_video = false);
}
