#include "StQueries.h"

#include <stdexcept>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>

namespace cli {
void prepareIndexer(QProcess& process, const QString& project, bool full) {
  QString executable = QCoreApplication::applicationDirPath() + "/Sourcetrail";
#ifdef _WIN32
  executable += ".exe";
#else
  // The Linux package installs the application as lower-case `sourcetrail`.
  if(!QFileInfo::exists(executable)) {
    executable = QCoreApplication::applicationDirPath() + "/sourcetrail";
  }
#endif
  process.setProgram(executable);
  process.setWorkingDirectory(QCoreApplication::applicationDirPath());
  QStringList args{"index", "--verbose"};
  if(full) {
    args << "--full";
  }
  args << project;
  process.setArguments(args);
}

void reindex(const QString& project, bool full, int timeoutMs, const std::function<void(const QByteArray& text, bool log)>& output) {
  QProcess process;
  prepareIndexer(process, project, full);
  process.start();
  if(!process.waitForStarted()) {
    throw std::runtime_error("Cannot start adjacent Sourcetrail executable: " + process.errorString().toStdString());
  }
  QElapsedTimer timer;
  timer.start();
  while(process.state() != QProcess::NotRunning) {
    process.waitForReadyRead(100);
    // Status lines first: what the indexer printed before a log line arrives no later than it.
    output(process.readAllStandardOutput(), false);
    output(process.readAllStandardError(), true);
    if(timer.elapsed() > timeoutMs) {
      process.terminate();
      if(!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished();
      }
      throw std::runtime_error("Indexing timed out; inspect logs before retrying");
    }
  }
  output(process.readAllStandardOutput(), false);
  output(process.readAllStandardError(), true);
  if(process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    throw std::runtime_error("Indexing failed; see stderr for Gradle, delombok or indexer diagnostics");
  }
}

QByteArray lastLines(const QByteArray& text, int count) {
  // The newline that ends the last line does not start another.
  qsizetype from = text.endsWith('\n') ? text.size() - 1 : text.size();
  for(int lines = 0; lines < count; ++lines) {
    from = from > 0 ? text.lastIndexOf('\n', from - 1) : -1;
    if(from < 0) {
      return text;
    }
  }
  return text.mid(from + 1);
}

QByteArray failureReport(const QByteArray& status, const QByteArray& log) {
  QByteArray report;
  const auto add = [&report](const QByteArray& header, const QByteArray& text) {
    if(!text.isEmpty()) {
      report += header + text + (text.endsWith('\n') ? "" : "\n");
    }
  };
  add("st: the indexer's last " + QByteArray::number(INDEXER_TAIL_LINES) + " log lines:\n", lastLines(log, INDEXER_TAIL_LINES));
  add("st: the indexer's status lines:\n", status);
  return report;
}
}    // namespace cli
