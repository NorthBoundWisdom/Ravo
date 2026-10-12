#pragma once
class QQmlApplicationEngine;
namespace ravo
{
class StudioCommandController;
[[nodiscard]] bool smoke_photo_merge_dialog(QQmlApplicationEngine &engine,
                                            StudioCommandController &commands);
} // namespace ravo
