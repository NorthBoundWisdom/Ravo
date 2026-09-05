#pragma once

class QQmlApplicationEngine;
namespace ravo
{
// Offscreen --smoke only: validates the real import view at supported window sizes.
[[nodiscard]] bool smoke_import_layout(QQmlApplicationEngine &engine);
} // namespace ravo
