#pragma once

#include <functional>

#include <QString>
#include <QVariant>

#include "studio_command_controller_detail.h"

namespace ravo::command_registration
{

using AddCommand = std::function<void(
    const char *id, command_controller_detail::Condition condition,
    command_controller_detail::Validator validator, command_controller_detail::Handler handler)>;
using PresentCommand = std::function<void(const char *id, const QVariant &argument)>;
using RequestConfirmation = std::function<void(const char *request_id, const char *confirmed_id)>;
using ConfirmationValidator =
    std::function<QString(const char *confirmed_id, const QVariant &argument)>;
using RequestPresetConfirmation =
    std::function<void(const char *request_id, const char *confirmed_id, const QVariant &argument)>;
using PresetConfirmationValidator =
    std::function<QString(const char *confirmed_id, const QVariant &argument)>;
using ClearConfirmation = std::function<void()>;

struct Helpers
{
    AddCommand add;
    PresentCommand present;
    RequestConfirmation request_confirmation;
    ConfirmationValidator confirmation_validator;
    RequestPresetConfirmation request_preset_confirmation;
    PresetConfirmationValidator preset_confirmation_validator;
    ClearConfirmation clear_confirmation;
};

} // namespace ravo::command_registration
