#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "IApplicationSettings.hpp"

namespace {
struct RunResult {
  bool finished = false;
  QProcess::ExitStatus exitStatus = QProcess::CrashExit;
  int exitCode = -1;
  QString output;
  QString errors;
};

// Runs the built Sourcetrail binary headless with HOME pointed at `home`, keeping the Gradle cache of the caller.
RunResult runSourcetrail(const QString& home, const QStringList& arguments) {
  constexpr int TimeoutMs = 300000;

  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  if(!environment.contains(QStringLiteral("GRADLE_USER_HOME"))) {
    environment.insert(QStringLiteral("GRADLE_USER_HOME"), environment.value(QStringLiteral("HOME")) + QStringLiteral("/.gradle"));
  }
  environment.insert(QStringLiteral("HOME"), home);
  environment.insert(QStringLiteral("SOURCETRAIL_VIA_SCRIPT"), QStringLiteral("1"));

  QProcess process;
  process.setProcessEnvironment(environment);
  process.start(QStringLiteral(SOURCETRAIL_APP_PATH), arguments);

  RunResult result;
  result.finished = process.waitForFinished(TimeoutMs);
  if(!result.finished) {
    process.kill();
    process.waitForFinished();
  }
  result.exitStatus = process.exitStatus();
  result.exitCode = process.exitCode();
  result.output = QString::fromLocal8Bit(process.readAllStandardOutput());
  result.errors = QString::fromLocal8Bit(process.readAllStandardError());
  return result;
}

// Application settings for the child process: single-process indexing with the JVM the test settings name.
void writeApplicationSettings(const std::filesystem::path& home) {
  const std::filesystem::path directory = home / ".config" / "sourcetrail";
  std::filesystem::create_directories(directory);
  std::ofstream(directory / "ApplicationSettings.xml")
      << "<config><indexing><indexer_thread_count>2</indexer_thread_count><multi_process_indexing>0</multi_process_indexing>"
      << "<java><has_prefilled_java_path>1</has_prefilled_java_path><java_path>"
      << IApplicationSettings::getInstance()->getJavaPath().str() << "</java_path></java></indexing></config>\n";
}
}    // namespace

// Review Focus 1: a path containing '{' or '}' reaches the log calls of the Java package (and the Gradle output
// quoting it). fmt treats the first log argument as a format string, so such text must be logged, never thrown.
TEST(JavaIndexing, bracesInPathsAndOutputAreLoggedNotThrown) {
  ASSERT_FALSE(IApplicationSettings::getInstance()->getJavaPath().empty()) << "no JVM configured for the Java tests";

  QTemporaryDir scratch;
  ASSERT_TRUE(scratch.isValid());
  const std::filesystem::path root = scratch.path().toStdString();
  const std::filesystem::path project = root / "with{braces}";
  std::filesystem::copy(SOURCETRAIL_LOMBOK_FIXTURE, project, std::filesystem::copy_options::recursive);
  writeApplicationSettings(root / "home");

  const RunResult result = runSourcetrail(
      QString::fromStdString((root / "home").string()),
      {QStringLiteral("index"), QStringLiteral("--full"), QString::fromStdString((project / "project.srctrlprj").string())});

  const std::string log = result.output.toStdString() + result.errors.toStdString();
  ASSERT_TRUE(result.finished) << log;
  EXPECT_EQ(QProcess::NormalExit, result.exitStatus) << log;
  EXPECT_EQ(0, result.exitCode) << log;
  EXPECT_TRUE(result.output.contains(QStringLiteral("9/9 source files indexed"))) << log;
  EXPECT_TRUE(result.output.contains(QStringLiteral("; 0 errors"))) << log;
  EXPECT_FALSE(result.errors.contains(QStringLiteral("LOG ERROR"))) << log;
}

// Headless indexing prints only its status lines by default: no INFO log line, such as each file it indexes, reaches
// the console unless --verbose asks for them.
TEST(JavaIndexing, headlessIndexLogsEachSourceFileOnlyWhenVerbose) {
  ASSERT_FALSE(IApplicationSettings::getInstance()->getJavaPath().empty()) << "no JVM configured for the Java tests";

  QTemporaryDir scratch;
  ASSERT_TRUE(scratch.isValid());
  const std::filesystem::path root = scratch.path().toStdString();
  const std::filesystem::path project = root / "lombok";
  std::filesystem::copy(SOURCETRAIL_LOMBOK_FIXTURE, project, std::filesystem::copy_options::recursive);
  writeApplicationSettings(root / "home");
  const QString home = QString::fromStdString((root / "home").string());
  const QString projectFile = QString::fromStdString((project / "project.srctrlprj").string());

  const RunResult quiet = runSourcetrail(home, {QStringLiteral("index"), QStringLiteral("--full"), projectFile});

  std::string log = quiet.output.toStdString() + quiet.errors.toStdString();
  ASSERT_TRUE(quiet.finished) << log;
  EXPECT_EQ(0, quiet.exitCode) << log;
  EXPECT_TRUE(quiet.output.contains(QStringLiteral("9/9 source files indexed"))) << log;
  EXPECT_FALSE((quiet.output + quiet.errors).contains(QStringLiteral("indexing source file: "))) << log;

  const RunResult verbose = runSourcetrail(
      home, {QStringLiteral("index"), QStringLiteral("--full"), QStringLiteral("--verbose"), projectFile});

  log = verbose.output.toStdString() + verbose.errors.toStdString();
  ASSERT_TRUE(verbose.finished) << log;
  EXPECT_EQ(0, verbose.exitCode) << log;
  EXPECT_TRUE(verbose.output.contains(QStringLiteral("9/9 source files indexed"))) << log;
  EXPECT_EQ(9, verbose.output.count(QStringLiteral("indexing source file: "))) << log;
  // Listeners unregistering at shutdown is no error.
  EXPECT_FALSE((verbose.output + verbose.errors).contains(QStringLiteral("Listener was not found"))) << log;
}
