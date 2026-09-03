#pragma once

#include <functional>
#include <vector>

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

#include "ravo/desktop/studio_command_controller.h"
#include "studio_command_internal.h"

namespace ravo
{
namespace command_controller_detail
{

enum class Condition
{
    kAlways,
    kCatalogOpen,
    kCatalogReady,
    kLoadedPhotos,
    kSelection,
    kReadySelection,
    kNonGrid,
    kSurveySelection,
    kDevelop,
    kDevelopSelection,
    kModifiedParameters,
    kCanUndo,
    kCanRedo,
    kCanPasteParameters,
    kCanPasteParametersToSelection,
    kCanDelete,
    kCatalogOperation,
};

using Validator = std::function<QString(const QVariant &)>;
using Handler = std::function<void(const QVariant &, const QString &)>;

[[nodiscard]] QString no_argument(const QVariant &argument);
[[nodiscard]] QString non_empty_string(const QVariant &argument);
[[nodiscard]] bool numeric_argument(const QVariant &argument);
[[nodiscard]] QString list_argument(const QVariant &argument);
[[nodiscard]] QString finite_number(const QVariant &argument, const QString &name);
[[nodiscard]] QString required_fields(const QVariant &argument, const QStringList &fields);
[[nodiscard]] QString preset_identity_argument(const QVariant &argument);
[[nodiscard]] QString one_of(const QVariant &argument, const QSet<QString> &values,
                             const QString &name);
[[nodiscard]] QStringList strings_from(const QVariant &argument);
[[nodiscard]] QString develop_parameter_fields_argument(const QVariant &argument);
[[nodiscard]] QString preset_save_argument(const QVariant &argument);

struct CommandSpec
{
    QString id;
    Condition condition = Condition::kAlways;
    Validator validator;
    Handler handler;
};

struct State
{
    bool enabled = true;
    QString reason;
};

[[nodiscard]] State resolve_state(const StudioPresenter &presenter, Condition condition,
                                  bool settings_open);
[[nodiscard]] QVariantMap accepted();
[[nodiscard]] QVariantMap rejected(const QString &code, const QString &message);

} // namespace command_controller_detail

struct StudioCommandController::Impl
{
    QHash<QString, command_controller_detail::CommandSpec> commands;
    QVector<command_internal::ActionSpec> actions = command_internal::builtin_actions();
    qulonglong confirmation_revision = 0;
    QString pending_confirmation_command;
    QString pending_confirmation_token;
    std::vector<std::string> pending_confirmation_assets;
    QVariant pending_confirmation_argument;
};

} // namespace ravo
