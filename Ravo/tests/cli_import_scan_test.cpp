#include <QCoreApplication>
#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QProcess>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include "ravo/foundation/json.h"

namespace ravo
{
TEST(CliImportScan, SubprocessScanCopyAndRescanShareContentPolicy)
{
    if (!QCoreApplication::instance())
    {
        static int argc = 1;
        static char name[] = "ravo-cli-import-test";
        static char *argv[] = {name, nullptr};
        static auto *application = new QCoreApplication(argc, argv);
        static_cast<void>(application);
    }
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto catalog = directory.filePath("library.sqlite");
    const auto source = directory.filePath("source");
    const auto destination = directory.filePath("destination");
    ASSERT_TRUE(QDir().mkpath(source));
    ASSERT_TRUE(QDir().mkpath(destination));
    QImage photo(32, 24, QImage::Format_RGB888);
    photo.setColorSpace(QColorSpace(QColorSpace::SRgb));
    photo.fill(Qt::red);
    ASSERT_TRUE(photo.save(source + "/a.png"));
    ASSERT_TRUE(QFile::copy(source + "/a.png", source + "/renamed.png"));
    const auto run = [](QStringList arguments, int expected_exit = 0)
    {
        QProcess process;
        process.start(QStringLiteral(RAVO_CLI_EXECUTABLE), arguments);
        const bool finished = process.waitForFinished(30000);
        EXPECT_TRUE(finished);
        EXPECT_EQ(process.exitStatus(), QProcess::NormalExit);
        const auto output = process.readAllStandardOutput();
        EXPECT_EQ(process.exitCode(), expected_exit)
            << output.toStdString() << process.readAllStandardError().toStdString();
        return parse_json(output.toStdString());
    };
    ASSERT_TRUE(run({"catalog", "create", "--catalog", catalog, "--json"}));
    auto scan = run({"catalog", "import-scan", "--catalog", catalog, "--input", source, "--json"});
    ASSERT_TRUE(scan);
    const auto *data = scan.value().find("data");
    ASSERT_NE(data, nullptr);
    ASSERT_NE(data->find("schema"), nullptr);
    EXPECT_EQ(*data->find("schema")->string_if(), "ravo-import-scan/v1");
    EXPECT_EQ(data->find("duplicates")->number_if()->text, "1");
    auto imported =
        run({"catalog", "import", "--catalog", catalog, "--input", source, "--mode", "copy",
             "--destination", destination, "--skip-existing", "--preview", "minimal", "--json"});
    ASSERT_TRUE(imported);
    EXPECT_TRUE(QFile::exists(destination + "/a.png"));
    EXPECT_FALSE(QFile::exists(destination + "/renamed.png"));
    EXPECT_TRUE(QFile::exists(source + "/renamed.png"));
    auto again = run({"catalog", "import-scan", "--catalog", catalog, "--input", source, "--json"});
    ASSERT_TRUE(again);
    EXPECT_EQ(again.value().find("data")->find("duplicates")->number_if()->text, "2");
    EXPECT_TRUE(run({"catalog", "import-scan", "--catalog", catalog, "--input", source, "--mode",
                     "copy", "--json"},
                    2));
    EXPECT_TRUE(run({"catalog", "list", "--catalog", catalog, "--skip-existing", "--json"}, 2));
}
} // namespace ravo
