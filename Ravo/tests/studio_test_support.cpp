#include "studio_test_support.h"

#include <cstddef>
#include <string_view>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QThread>

namespace ravo::studio_test_support
{

void ensure_qt_core()
{
    if (QCoreApplication::instance() != nullptr)
        return;
    static int argc = 1;
    static char executable[] = "ravo-desktop-command-tests";
    static char *argv[] = {executable, nullptr};
    static auto *application = new QCoreApplication(argc, argv);
    static_cast<void>(application);
}

[[nodiscard]] bool wait_until(const std::function<bool()> &ready, const int timeout_ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout_ms)
    {
        if (ready())
            return true;
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    return ready();
}

ScopedEnvironmentVariable::ScopedEnvironmentVariable(const char *name, const QByteArray &value)
    : name_(name)
    , old_value_(qgetenv(name))
    , was_set_(qEnvironmentVariableIsSet(name))
{
    qputenv(name, value);
}

ScopedEnvironmentVariable::~ScopedEnvironmentVariable()
{
    if (was_set_)
        qputenv(name_.constData(), old_value_);
    else
        qunsetenv(name_.constData());
}

[[nodiscard]] CliProcessResult run_cli_process(const QStringList &arguments, const int timeout_ms)
{
    QProcess process;
    process.setProgram(QStringLiteral(RAVO_CLI_EXECUTABLE));
    process.setArguments(arguments);
    process.start();
    const bool finished =
        wait_until([&] { return process.state() == QProcess::NotRunning; }, timeout_ms);
    if (!finished)
    {
        process.kill();
        process.waitForFinished(5000);
    }
    return {finished ? process.exitCode() : -1, process.readAllStandardOutput(),
            process.readAllStandardError()};
}

[[nodiscard]] Result<JsonValue> cli_data(const QByteArray &output)
{
    const QByteArray trimmed = output.trimmed();
    auto envelope =
        parse_json(std::string_view(trimmed.constData(), static_cast<std::size_t>(trimmed.size())));
    if (!envelope)
        return envelope.error();
    const auto *data = envelope.value().find("data");
    if (data == nullptr)
        return make_error(ErrorCode::kValidation, "CLI response has no data object");
    return *data;
}

[[nodiscard]] QString qml_model_entry(const QString &source, const char *field)
{
    const auto needle = QStringLiteral("\"field\": \"%1\"").arg(QString::fromLatin1(field));
    const auto field_position = source.indexOf(needle);
    if (field_position < 0)
        return {};
    const auto begin = source.lastIndexOf(QLatin1Char('{'), field_position);
    const auto end = source.indexOf(QLatin1Char('}'), field_position);
    if (begin < 0 || end < field_position)
        return {};
    return source.mid(begin, end - begin + 1);
}

} // namespace ravo::studio_test_support
