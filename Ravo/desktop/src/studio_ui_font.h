#pragma once

#include <QFont>
#include <QString>
#include <QStringList>

namespace ravo::desktop_internal
{
// GUI-thread-only application bootstrap. Uses the installed font inventory;
// no font downloads, cache, settings or asynchronous work are owned here.
[[nodiscard]] QStringList studio_ui_font_families(const QFont &system_font,
                                                  const QString &language);
void apply_studio_ui_font(const QString &language);
} // namespace ravo::desktop_internal
