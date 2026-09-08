#include "ravo/desktop/studio_command_controller.h"

#include "studio_command_controller_detail.h"
#include "studio_command_registration.h"
#include "studio_command_ids.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QCoreApplication>
#include <QMetaType>
#include <QRegularExpression>
#include <QSet>
#include <Qt>

#include "ravo/desktop/studio_presenter.h"
#include "studio_qt.h"

namespace ravo
{
using namespace command_internal;
using namespace command_controller_detail;

namespace
{
template <typename Range>
QSet<QString> string_set(const Range &values)
{
    QSet<QString> result;
    for (const auto &value : values)
        result.insert(value);
    return result;
}

} // namespace

StudioCommandController::StudioCommandController(StudioPresenter &presenter, QObject *parent)
    : QObject(parent)
    , presenter_(presenter)
    , impl_(std::make_unique<Impl>())
{
    const auto add =
        [this](const char *id, const Condition condition, Validator validator, Handler handler)
    {
        const auto key = QLatin1String(id);
        if (impl_->commands.contains(key))
            qFatal("Duplicate Studio command: %s", id);
        impl_->commands.insert(key, {key, condition, std::move(validator), std::move(handler)});
    };
    const auto present = [this](const char *id, const QVariant &argument)
    { emit presentationCommandRequested(QLatin1String(id), argument); };
    const auto request_confirmation = [this](const char *request_id, const char *confirmed_id)
    {
        ++impl_->confirmation_revision;
        impl_->pending_confirmation_command = QLatin1String(confirmed_id);
        impl_->pending_confirmation_token = QStringLiteral("%1:%2")
                                                .arg(QLatin1String(confirmed_id))
                                                .arg(impl_->confirmation_revision);
        impl_->pending_confirmation_assets = presenter_.selected_asset_ids();
        impl_->pending_confirmation_argument.clear();
        emit presentationCommandRequested(QLatin1String(request_id),
                                          impl_->pending_confirmation_token);
    };
    const auto confirmation_validator = [this](const char *confirmed_id, const QVariant &argument)
    {
        if (argument.metaType().id() != QMetaType::QString ||
            impl_->pending_confirmation_command != QLatin1String(confirmed_id) ||
            impl_->pending_confirmation_token != argument.toString())
            return QStringLiteral("A current confirmation token is required.");
        if (impl_->pending_confirmation_assets != presenter_.selected_asset_ids())
            return QStringLiteral("The photo selection changed after confirmation was requested.");
        return QString{};
    };
    const auto request_preset_confirmation =
        [this](const char *request_id, const char *confirmed_id, const QVariant &argument)
    {
        ++impl_->confirmation_revision;
        impl_->pending_confirmation_command = QLatin1String(confirmed_id);
        impl_->pending_confirmation_token = QStringLiteral("%1:%2")
                                                .arg(QLatin1String(confirmed_id))
                                                .arg(impl_->confirmation_revision);
        impl_->pending_confirmation_assets.clear();
        impl_->pending_confirmation_argument = argument;
        auto presentation = argument.toMap();
        presentation.insert(QStringLiteral("token"), impl_->pending_confirmation_token);
        emit presentationCommandRequested(QLatin1String(request_id), presentation);
    };
    const auto preset_confirmation_validator =
        [this](const char *confirmed_id, const QVariant &argument)
    {
        const auto field_error =
            required_fields(argument, {QStringLiteral("token"), QStringLiteral("path")});
        if (!field_error.isEmpty())
            return field_error;
        const auto fields = argument.toMap();
        static const QSet<QString> allowed{QStringLiteral("token"), QStringLiteral("path")};
        for (auto it = fields.constBegin(); it != fields.constEnd(); ++it)
        {
            if (!allowed.contains(it.key()))
                return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                                      "StudioCommands", "Unknown command argument field: %1.")))
                    .arg(it.key());
        }
        const auto token = fields.value(QStringLiteral("token"));
        const auto path = fields.value(QStringLiteral("path"));
        if (token.metaType().id() != QMetaType::QString ||
            path.metaType().id() != QMetaType::QString ||
            impl_->pending_confirmation_command != QLatin1String(confirmed_id) ||
            impl_->pending_confirmation_token != token.toString())
            return QStringLiteral("A current confirmation token is required.");
        const auto pending = impl_->pending_confirmation_argument.toMap();
        if (pending.value(QStringLiteral("path")).toString() != path.toString())
            return QStringLiteral("The preset changed after confirmation was requested.");
        return QString{};
    };
    const auto clear_confirmation = [this]()
    {
        impl_->pending_confirmation_command.clear();
        impl_->pending_confirmation_token.clear();
        impl_->pending_confirmation_assets.clear();
        impl_->pending_confirmation_argument.clear();
    };

    const command_registration::Helpers helpers{add,
                                                present,
                                                request_confirmation,
                                                confirmation_validator,
                                                request_preset_confirmation,
                                                preset_confirmation_validator,
                                                clear_confirmation};
    registerLibraryCommands(helpers);
    registerImportCommands(helpers);
    registerExportCommands(helpers);
    registerRecoveryCommands(helpers);
    registerDevelopCommands(helpers);
    registerViewCommands(helpers);

    const auto errors = validateBuiltinDefinitions();
    if (!errors.isEmpty())
        qFatal("Studio command registry validation failed: %s",
               errors.join(QLatin1String("; ")).toUtf8().constData());
    if (string_set(impl_->commands.keys()) != string_set(command_ids()))
        qFatal("Studio command handlers do not cover the builtin command registry");
    const auto changed = [this]() { refresh(); };
    connect(&presenter_, &StudioPresenter::catalogChanged, this, changed);
    connect(&presenter_, &StudioPresenter::busyChanged, this, changed);
    connect(&presenter_, &StudioPresenter::libraryWorkChanged, this, changed);
    connect(&presenter_, &StudioPresenter::importPageChanged, this, changed);
    connect(&presenter_, &StudioPresenter::selectionChanged, this, changed);
    connect(&presenter_, &StudioPresenter::browseModeChanged, this, changed);
    connect(&presenter_, &StudioPresenter::zoomChanged, this, changed);
    connect(&presenter_, &StudioPresenter::editChanged, this, changed);
    connect(&presenter_, &StudioPresenter::copiedParametersChanged, this, changed);
}

StudioCommandController::~StudioCommandController() = default;

} // namespace ravo
