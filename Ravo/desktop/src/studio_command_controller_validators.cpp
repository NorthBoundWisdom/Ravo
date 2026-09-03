#include "studio_command_controller_detail.h"

#include <algorithm>
#include <cmath>

#include <QMetaType>
#include <QSet>

#include "ravo/recipe/develop.h"
#include "studio_qt.h"

namespace ravo::command_controller_detail
{
using namespace command_internal;
QString no_argument(const QVariant &argument)
{
    return argument.isValid() && !argument.isNull() ?
               QStringLiteral("This command takes no argument.") :
               QString{};
}

QString non_empty_string(const QVariant &argument)
{
    return argument.metaType().id() != QMetaType::QString ||
                   argument.toString().trimmed().isEmpty() ?
               QStringLiteral("A non-empty string is required.") :
               QString{};
}

QString map_argument(const QVariant &argument)
{
    return argument.canConvert<QVariantMap>() ? QString{} :
                                                QStringLiteral("An object argument is required.");
}

QString list_argument(const QVariant &argument)
{
    if (!argument.canConvert<QVariantList>() && !argument.canConvert<QStringList>())
        return QStringLiteral("A non-empty list of paths is required.");
    const auto values = argument.toList().isEmpty() ? QVariantList{} : argument.toList();
    if (!values.isEmpty())
    {
        for (const auto &value : values)
            if (value.metaType().id() != QMetaType::QString || value.toString().trimmed().isEmpty())
                return QStringLiteral("Every import path must be a non-empty string.");
        return {};
    }
    const auto strings = argument.toStringList();
    if (strings.isEmpty())
        return QStringLiteral("A non-empty list of paths is required.");
    for (const auto &value : strings)
        if (value.trimmed().isEmpty())
            return QStringLiteral("Every import path must be a non-empty string.");
    return {};
}

bool numeric_argument(const QVariant &argument)
{
    switch (argument.metaType().id())
    {
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Float:
    case QMetaType::Double:
        return true;
    default:
        return false;
    }
}

QString finite_number(const QVariant &argument, const QString &name)
{
    return numeric_argument(argument) && std::isfinite(argument.toDouble()) ?
               QString{} :
               tr_command(QString::fromUtf8(
                              QT_TRANSLATE_NOOP("StudioCommands", "%1 must be a finite number.")))
                   .arg(name);
}

QString required_fields(const QVariant &argument, const QStringList &fields)
{
    const auto map_error = map_argument(argument);
    if (!map_error.isEmpty())
        return map_error;
    const auto values = argument.toMap();
    for (const auto &field : fields)
        if (!values.contains(field))
            return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                                  "StudioCommands", "Missing command argument field: %1.")))
                .arg(field);
    return {};
}

QString preset_identity_argument(const QVariant &argument)
{
    const auto error = required_fields(argument, {QStringLiteral("path"), QStringLiteral("name")});
    if (!error.isEmpty())
        return error;
    const auto values = argument.toMap();
    static const QSet<QString> allowed{QStringLiteral("path"), QStringLiteral("name")};
    for (auto it = values.constBegin(); it != values.constEnd(); ++it)
    {
        if (!allowed.contains(it.key()))
            return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                                  "StudioCommands", "Unknown command argument field: %1.")))
                .arg(it.key());
    }
    const auto path = values.value(QStringLiteral("path"));
    const auto name = values.value(QStringLiteral("name"));
    return path.metaType().id() == QMetaType::QString && !path.toString().trimmed().isEmpty() &&
                   name.metaType().id() == QMetaType::QString &&
                   !name.toString().trimmed().isEmpty() ?
               QString{} :
               QStringLiteral("Preset path and name must be non-empty strings.");
}

QString one_of(const QVariant &argument, const QSet<QString> &values, const QString &name)
{
    return argument.metaType().id() == QMetaType::QString && values.contains(argument.toString()) ?
               QString{} :
               tr_command(
                   QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Unknown %1 value.")))
                   .arg(name);
}

QStringList strings_from(const QVariant &argument)
{
    auto result = argument.toStringList();
    if (!result.isEmpty())
        return result;
    for (const auto &value : argument.toList())
        result.push_back(value.toString());
    return result;
}

QString develop_parameter_fields_argument(const QVariant &argument)
{
    const bool variant_list = argument.metaType().id() == QMetaType::QVariantList;
    const bool string_list = argument.metaType().id() == QMetaType::QStringList;
    if (!variant_list && !string_list)
        return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
            "StudioCommands", "Parameter fields must be a non-empty string list.")));
    const QVariantList fields = variant_list ? argument.toList() : QVariantList{};
    const QStringList strings = string_list ? argument.toStringList() : strings_from(argument);
    if (strings.isEmpty() ||
        strings.size() > static_cast<qsizetype>(develop_selectable_field_names().size()))
        return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
            "StudioCommands", "Parameter fields must be a non-empty string list.")));
    if (variant_list && std::any_of(fields.cbegin(), fields.cend(), [](const QVariant &field)
                                    { return field.metaType().id() != QMetaType::QString; }))
        return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
            "StudioCommands", "Parameter fields must be a non-empty string list.")));
    QSet<QString> unique;
    for (const auto &field : strings)
    {
        if (field.isEmpty() || !is_develop_selectable_field(utf8_from_qstring(field)) ||
            unique.contains(field))
            return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                "StudioCommands", "Parameter fields contain an unsupported or duplicate value.")));
        unique.insert(field);
    }
    return {};
}

QString preset_save_argument(const QVariant &argument)
{
    const auto error =
        required_fields(argument, {QStringLiteral("name"), QStringLiteral("fields")});
    if (!error.isEmpty())
        return error;
    const auto values = argument.toMap();
    static const QSet<QString> allowed{QStringLiteral("name"), QStringLiteral("fields")};
    for (auto it = values.constBegin(); it != values.constEnd(); ++it)
    {
        if (!allowed.contains(it.key()))
            return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                                  "StudioCommands", "Unknown command argument field: %1.")))
                .arg(it.key());
    }
    const auto name = values.value(QStringLiteral("name"));
    if (name.metaType().id() != QMetaType::QString || name.toString().trimmed().isEmpty())
        return tr_command(QString::fromUtf8(
            QT_TRANSLATE_NOOP("StudioCommands", "Preset name must be a non-empty string.")));

    return develop_parameter_fields_argument(values.value(QStringLiteral("fields")));
}

} // namespace ravo::command_controller_detail
