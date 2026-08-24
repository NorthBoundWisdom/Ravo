#include <iostream>
#include <span>
#include <string_view>
#include <vector>

#include <QCoreApplication>

#include "ravo/cli/application.h"
#include "ravo/foundation/log.h"

int main(int argc, char *argv[])
{
    QCoreApplication qt(argc, argv);
    ravo::init_logging("ravo");
    auto engine = ravo::EngineFacade::create_phase1();
    if (!engine)
    {
        std::cerr << "ravo: " << ravo::error_code_name(engine.error().code) << ": "
                  << engine.error().message << '\n';
        ravo::shutdown_logging();
        return ravo::cli_exit_code(engine.error().code);
    }

    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index)
    {
        arguments.emplace_back(argv[index]);
    }
    const ravo::CliApplication application(engine.value(), std::cout, std::cerr);
    const int exit_code = application.run(std::span{arguments});
    ravo::shutdown_logging();
    return exit_code;
}
