#pragma once

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

namespace ravo::command_internal
{

struct ShortcutSpec
{
    QString sequence;
    bool requires_non_text_input = false;
};

struct ActionSpec
{
    QString id;
    QString command_id;
    QString title;
    QString category;
    QStringList keywords;
    QString menu_path;
    int order = 0;
    bool palette_visible = true;
    bool has_argument = false;
    QVariant argument;
    QVector<ShortcutSpec> shortcuts;
};

[[nodiscard]] QString tr_command(const QString &source);
[[nodiscard]] QString primary_key(const QString &key, bool shift = false, bool alt = false);
[[nodiscard]] QString native_key(const QString &portable);
[[nodiscard]] QString normalize_search(const QString &value);
[[nodiscard]] int token_score(const QString &source, const QString &raw_token);
[[nodiscard]] QStringList command_ids();
[[nodiscard]] QVector<ActionSpec> builtin_actions();

} // namespace ravo::command_internal
