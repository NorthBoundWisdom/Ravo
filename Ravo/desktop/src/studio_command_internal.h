#pragma once

#include <QString>
#include <QStringList>

namespace ravo::command_internal
{

[[nodiscard]] QString tr_command(const QString &source);
[[nodiscard]] QString normalize_search(const QString &value);
[[nodiscard]] int token_score(const QString &source, const QString &raw_token);

} // namespace ravo::command_internal
