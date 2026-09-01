#pragma once

#include <functional>

#include <QByteArray>
#include <QString>
#include <QStringList>

#include "ravo/foundation/json.h"

namespace ravo::studio_test_support
{

void ensure_qt_core();
[[nodiscard]] bool wait_until(const std::function<bool()> &ready, int timeout_ms = 15000);

class ScopedEnvironmentVariable
{
public:
    ScopedEnvironmentVariable(const char *name, const QByteArray &value);
    ~ScopedEnvironmentVariable();

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable &) = delete;
    ScopedEnvironmentVariable &operator=(const ScopedEnvironmentVariable &) = delete;

private:
    QByteArray name_;
    QByteArray old_value_;
    bool was_set_ = false;
};

struct CliProcessResult
{
    int exit_code = -1;
    QByteArray standard_output;
    QByteArray standard_error;
};

[[nodiscard]] CliProcessResult run_cli_process(const QStringList &arguments,
                                               int timeout_ms = 30000);
[[nodiscard]] Result<JsonValue> cli_data(const QByteArray &output);
[[nodiscard]] QString qml_model_entry(const QString &source, const char *field);

} // namespace ravo::studio_test_support
