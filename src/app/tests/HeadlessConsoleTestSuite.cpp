#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <gtest/gtest.h>

namespace {
struct RunResult {
  bool finished = false;
  QProcess::ExitStatus exitStatus = QProcess::CrashExit;
  int exitCode = -1;
  QString output;
  QString errors;
};

// Runs the built Sourcetrail binary headless, with HOME pointed at a fresh temporary directory so the
// user's ~/.config/sourcetrail is never read or written.
RunResult runSourcetrail(const QStringList& arguments) {
  constexpr int TimeoutMs = 30000;

  QTemporaryDir home;
  EXPECT_TRUE(home.isValid());

  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  environment.insert(QStringLiteral("HOME"), home.path());
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
}    // namespace

TEST(HeadlessConsole, configPrintsHelpAndExitsZero) {
  const RunResult result = runSourcetrail({QStringLiteral("config"), QStringLiteral("--help")});

  ASSERT_TRUE(result.finished) << result.output.toStdString() << result.errors.toStdString();
  EXPECT_EQ(QProcess::NormalExit, result.exitStatus) << result.errors.toStdString();
  EXPECT_EQ(0, result.exitCode) << result.errors.toStdString();
  EXPECT_TRUE(result.output.contains(QStringLiteral("Sourcetrail config"))) << result.output.toStdString();
}

TEST(HeadlessConsole, argumentsArePassedInOrder) {
  const RunResult result = runSourcetrail({QStringLiteral("index"), QStringLiteral("--help"), QStringLiteral("--full")});

  ASSERT_TRUE(result.finished) << result.output.toStdString() << result.errors.toStdString();
  EXPECT_EQ(QProcess::NormalExit, result.exitStatus) << result.errors.toStdString();
  EXPECT_EQ(0, result.exitCode) << result.errors.toStdString();
  // With the arguments mixed up the parser sees "index index index" and reports too many positional options.
  EXPECT_TRUE(result.output.contains(QStringLiteral("Sourcetrail index"))) << result.output.toStdString();
  EXPECT_FALSE(result.errors.contains(QStringLiteral("ERROR"))) << result.errors.toStdString();
}
