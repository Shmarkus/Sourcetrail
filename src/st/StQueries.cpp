#include "StQueries.h"

#include <iostream>
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
  process.setProcessChannelMode(QProcess::MergedChannels);
  QStringList args{"index"};
  if(full) {
    args << "--full";
  }
  args << project;
  process.setArguments(args);
}

void reindex(const QString& project, bool full, int timeoutMs, std::string* log) {
  const auto output = [&](const QByteArray& text) {
    if(log) {
      log->append(text.constData(), static_cast<size_t>(text.size()));
    } else {
      std::cerr << text.constData() << std::flush;
    }
  };
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
    output(process.readAll());
    if(timer.elapsed() > timeoutMs) {
      process.terminate();
      if(!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished();
      }
      throw std::runtime_error("Indexing timed out; inspect logs before retrying");
    }
  }
  output(process.readAll());
  if(process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    throw std::runtime_error("Indexing failed; see stderr for Gradle, delombok or indexer diagnostics");
  }
}
}    // namespace cli
