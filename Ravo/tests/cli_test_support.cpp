#include "cli_test_support.h"

#include <utility>

#include <QCoreApplication>

#include "ravo/foundation/log.h"

namespace ravo
{
namespace
{

void ensure_cli_test_qt_core()
{
    static const bool logging = []
    {
        ravo::init_logging("ravo-cli-tests");
        return true;
    }();
    static_cast<void>(logging);
    if (QCoreApplication::instance() != nullptr)
    {
        return;
    }
    static int argc = 1;
    static char dummy[] = "ravo-cli-tests";
    static char *argv[] = {dummy, nullptr};
    static auto *app = new QCoreApplication(argc, argv);
    static_cast<void>(app);
}

} // namespace

CliTest::CliTest()
    : engine([]
             {
                 auto created = EngineFacade::create_phase1();
                 return std::move(created).value();
             }())
{
}

CliTest::~CliTest() = default;

void CliTest::SetUp()
{
    ensure_cli_test_qt_core();
    const auto created = EngineFacade::create_phase1();
    ASSERT_TRUE(created) << created.error().message;
    engine = std::move(created).value();
}

} // namespace ravo
