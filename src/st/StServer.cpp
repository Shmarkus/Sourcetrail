#include "StServer.h"

#include <algorithm>
#include <iomanip>
#include <optional>
#include <sstream>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QProcess>
#include <QSocketNotifier>
#include <QtEndian>
#include <QThread>
#include <QTimer>

#ifndef _WIN32
#  include <cerrno>
#  include <csignal>

#  include <sys/socket.h>
#  include <unistd.h>
#endif

#include "FileInfo.h"
#include "PersistentStorage.h"
#include "ProjectSettings.h"
#include "StFreshness.h"
#include "StorageStats.h"
#include "StOutput.h"
#include "StProject.h"
#include "StQueries.h"
#include "StWatch.h"

namespace {
constexpr quint32 MAX_FRAME = 256 * 1024 * 1024;
constexpr int PROBE_MS = 50;
constexpr int READ_MS = 5000;
constexpr char ACK = 0x06;
constexpr int STOP_MS = 5000;
constexpr int DETACH_MS = 30000;
constexpr int LOG_TAIL = 20;
constexpr int POLL_MS = 100;
constexpr qint64 REFRESH_MS = 600000;    // as a request's refresh
constexpr int LOCK_MS = 5000;
constexpr qint64 RETRY_MS = 5000;        // while an indexer writes, or while there is no index to watch
constexpr int PROBE_ACK_MS = 2000;
// Fixed for every protocol version: any server stops on it, a server of any build or protocol can be ended.
const QByteArray STOP_REQUEST = QByteArrayLiteral("st serve --stop");
const QByteArray STOP_REPLY = QByteArrayLiteral("st serve stopping");

QDataStream& stream(QDataStream& data) {
  data.setVersion(QDataStream::Qt_5_12);
  return data;
}

bool complete(const QDataStream& data) {
  return data.status() == QDataStream::Ok && data.atEnd();
}

QString canonicalPath(const FilePath& path) {
  return QFileInfo(QString::fromStdWString(path.wstr())).canonicalFilePath();
}

std::string seconds(qint64 ms) {
  std::ostringstream text;
  text << std::fixed << std::setprecision(1) << ms / 1000.0 << 's';
  return text.str();
}

// Refreshes the index in the background after the sources change, the way a request would: refreshStart before the
// indexer, recordRefresh after it succeeded. Never holds the project lock while the indexer runs; it takes it itself.
class BackgroundRefresh {
public:
  BackgroundRefresh(const FilePath& projectFile, st::IndexCache& cache, std::ostream& err)
      : m_project(projectFile), m_cache(cache), m_err(err), m_watcher([this]() { m_scheduler.changed(m_clock.elapsed()); }) {
    m_clock.start();
    // Sources may have changed while no server ran.
    m_scheduler.changed(m_clock.elapsed());
    QObject::connect(&m_timer, &QTimer::timeout, [this]() { poll(); });
    m_timer.start(POLL_MS);
  }

  ~BackgroundRefresh() {
    if(m_process) {
      // Killing the indexer would leave an unfinished index behind: let it finish on its own. The QProcess is
      // deliberately never deleted, since its destructor kills the process.
      m_process->disconnect();
      m_err << "st: leaving the background refresh (pid " << m_process->processId() << ") to finish" << std::endl;
      m_process = nullptr;
    }
  }

  // Waits for a running refresh; the milliseconds waited, or nullopt when none ran.
  std::optional<qint64> wait() {
    if(!m_process) {
      return std::nullopt;
    }
    QElapsedTimer timer;
    timer.start();
    if(!m_process->waitForFinished(static_cast<int>(std::max<qint64>(1, REFRESH_MS - m_running.elapsed())))) {
      stop();
    }
    return timer.elapsed();
  }

  // Writes the record of a finished refresh that could not be written yet, before a request checks freshness.
  void settle() {
    recordPending(LOCK_MS);
  }

private:
  bool load(ProjectSettings* settings) {
    return settings->load(m_project, true);
  }

  enum class Index {
    READ,
    BUSY,        // locked by another st
    INDEXING,    // being written by an indexer (or a temporary database left behind)
    NONE         // missing, empty or incompatible
  };

  // Loads the index into the cache under the project lock.
  Index readIndex(const ProjectSettings& settings, int waitMs, TimeStamp* indexedAt, std::vector<FilePath>* files) {
    // Checked before the lock too, so an indexer's temporary database does not have st take the lock repeatedly.
    if(settings.getTempDBFilePath().exists()) {
      return Index::INDEXING;
    }
    QLockFile lock(st::projectLockFile(m_project));
    lock.setStaleLockTime(0);
    if(!lock.tryLock(waitMs)) {
      return Index::BUSY;
    }
    if(settings.getTempDBFilePath().exists()) {
      return Index::INDEXING;
    }
    if(!settings.getDBFilePath().exists()) {
      return Index::NONE;
    }
    PersistentStorage& storage = m_cache.storage(settings);
    if(storage.isEmpty() || storage.isIncompatible()) {
      return Index::NONE;
    }
    try {
      // What a request would otherwise compute after a reload; memoized, so cheap when nothing changed.
      m_cache.lombok(settings);
      m_cache.unresolved(storage);
    } catch(const std::exception&) {
      // The request that needs them reports the failure.
    }
    files->clear();
    for(const FileInfo& file : storage.getFileInfoForAllFiles()) {
      files->push_back(file.path);
    }
    *indexedAt = storage.getStorageStats().timestamp;
    if(m_pending.pending()) {
      if(const std::optional<st::RefreshRecord> record = m_pending.take(*indexedAt, st::lastRefresh(settings))) {
        st::recordRefresh(settings, *record, m_err);
      }
    }
    return Index::READ;
  }

  void watch(const ProjectSettings& settings, const std::vector<FilePath>& files) {
    const std::vector<QString> paths = st::watchPaths(settings, files);
    const size_t missing = m_watcher.watch(paths);
    if(!m_watching || missing) {
      m_err << "st: watching " << paths.size() - missing << " paths";
      if(missing) {
        m_err << "; cannot watch " << missing;
      }
      m_err << std::endl;
    }
    m_watching = true;
  }

  void recordPending(int waitMs) {
    ProjectSettings settings;
    TimeStamp indexedAt;
    std::vector<FilePath> files;
    if(m_pending.pending() && !m_process && load(&settings)) {
      readIndex(settings, waitMs, &indexedAt, &files);
    }
  }

  void poll() {
    if(m_process && m_running.elapsed() > REFRESH_MS) {
      stop();
    }
    m_watcher.settle();
    recordPending(0);
    const qint64 now = m_clock.elapsed();
    if(m_scheduler.due(now) && now >= m_retryAt) {
      begin();
    } else if(!m_watching && !m_process && !m_scheduler.pending() && now - m_attempted >= RETRY_MS) {
      // Started without an index: watch once one exists.
      begin();
    }
  }

  void begin() {
    m_attempted = m_clock.elapsed();
    ProjectSettings settings;
    TimeStamp indexedAt;
    std::vector<FilePath> files;
    const Index index = load(&settings) ? readIndex(settings, 0, &indexedAt, &files) : Index::NONE;
    if(index == Index::BUSY || index == Index::INDEXING) {
      // Another st or an indexer is at work: look again after the next quiet period, or later while an indexer
      // writes.
      m_scheduler.changed(m_clock.elapsed());
      m_retryAt = index == Index::INDEXING ? m_clock.elapsed() + RETRY_MS : 0;
      return;
    }
    m_retryAt = 0;
    m_scheduler.started();
    if(index == Index::NONE) {
      // Requests report a missing index; indexing one is not a refresh.
      m_scheduler.finished();
      return;
    }
    if(!m_watching) {
      watch(settings, files);
    }
    const st::Freshness freshness = st::checkFreshness(
        settings, st::freshnessBaseline(indexedAt, st::lastRefresh(settings), st::refreshInProgress(settings)), files);
    if(!freshness.stale) {
      m_scheduler.finished();
      return;
    }
    m_started = st::refreshStart();
    // On disk before the indexer starts: a refresh this server leaves running when it stops is still accounted for.
    st::recordRefreshStart(settings, m_started, m_err);
    m_changed = freshness.changedFiles;
    m_running.start();
    m_process = new QProcess();
    cli::prepareIndexer(*m_process, QString::fromStdWString(m_project.wstr()), false);
    m_process->setStandardInputFile(QProcess::nullDevice());
    m_process->setStandardOutputFile(logFile(), QIODevice::Truncate);
    QObject::connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), [this]() { end(); });
    m_err << "st: refreshing " << m_changed << " changed files in the background" << std::endl;
    m_process->start();
    if(!m_process->waitForStarted()) {
      m_err << "st: cannot start " << m_process->program().toStdString() << ": " << m_process->errorString().toStdString()
            << std::endl;
      m_process->disconnect();
      m_process->deleteLater();
      m_process = nullptr;
      m_scheduler.finished();
    }
  }

  void end() {
    const bool succeeded = m_process->exitStatus() == QProcess::NormalExit && m_process->exitCode() == 0;
    m_process->deleteLater();
    m_process = nullptr;
    if(succeeded) {
      // Written by the first index read under the lock: this one, or a later one when the project is busy.
      m_pending.succeeded(m_started);
    }
    ProjectSettings settings;
    TimeStamp indexedAt;
    std::vector<FilePath> files;
    // Reloads the cache now, so the next request finds it warm.
    const bool read = load(&settings) && readIndex(settings, LOCK_MS, &indexedAt, &files) == Index::READ;
    if(read) {
      // New files, and files an editor replaced (an atomic save drops the watch).
      watch(settings, files);
    }
    if(succeeded) {
      m_err << "st: background refresh of " << m_changed << " files finished in " << seconds(m_running.elapsed())
            << (read ? "" : "; project busy, recording it later") << std::endl;
    } else {
      m_err << "st: background refresh failed; see " << logFile().toStdString() << std::endl;
    }
    m_scheduler.finished();
  }

  // Ends an indexer that ran out of time; finished() then reports the failure.
  void stop() {
    m_err << "st: background refresh timed out" << std::endl;
    m_process->terminate();
    if(!m_process->waitForFinished(5000)) {
      m_process->kill();
      m_process->waitForFinished();
    }
  }

  QString logFile() const {
    return QFileInfo(QString::fromStdWString(m_project.wstr())).absoluteDir().absoluteFilePath(".st-refresh.log");
  }

  const FilePath m_project;
  st::IndexCache& m_cache;
  std::ostream& m_err;
  st::RefreshScheduler m_scheduler;
  st::SourceWatcher m_watcher;
  QElapsedTimer m_clock;
  QTimer m_timer;
  QProcess* m_process = nullptr;
  QElapsedTimer m_running;
  TimeStamp m_started;
  size_t m_changed = 0;
  bool m_watching = false;
  qint64 m_retryAt = 0;
  qint64 m_attempted = 0;
  st::PendingRecord m_pending;
};

// The reply to one request, or nullopt to close the connection without one.
std::optional<st::WireReply> answer(const st::WireRequest& wire,
                                    const FilePath& served,
                                    st::IndexCache& cache,
                                    BackgroundRefresh* background) {
  st::WireReply reply{st::EXIT_INTERNAL, {}, {}};
  const auto refuse = [&reply](const char* reason) {
    reply.refused = true;
    reply.refusal = reason;
    return reply;
  };
  if(wire.version != st::PROTOCOL_VERSION) {
    return refuse("speaks another protocol version");
  }
  if(wire.buildId != st::buildId()) {
    return refuse("is from another build");
  }
  const st::Route route = st::route(wire.request);
  if(route.command == "serve") {
    return std::nullopt;
  }
  const QString project = canonicalPath(route.project);
  if(project.isEmpty() || project != canonicalPath(served)) {
    return refuse("serves another project");
  }
  st::Request request = wire.request;
  request.served = true;
  std::istringstream in(wire.stdinData.toStdString());
  std::ostringstream out;
  std::ostringstream err;
  // A refresh already running is waited for, not started a second time; the freshness check then runs as usual.
  if(const std::optional<qint64> waited = background ? background->wait() : std::nullopt) {
    err << "st: waited " << seconds(*waited) << " for the background refresh\n";
  }
  if(background) {
    background->settle();
  }
  const QString previous = QDir::currentPath();
  QDir::setCurrent(request.cwd);
  reply.exitCode = st::runCommand(request, {in, out, err}, cache);
  QDir::setCurrent(previous);
  reply.out = QByteArray::fromStdString(out.str());
  reply.err = QByteArray::fromStdString(err.str());
  return reply;
}

// Whether the server should stop after this connection.
bool handle(QLocalSocket& socket, const FilePath& served, st::IndexCache& cache, BackgroundRefresh* background) {
  bool stop = false;
  QByteArray payload;
  st::WireRequest request;
  if(!st::readFrame(socket, &payload, READ_MS)) {
    // nothing to answer
  } else if(payload == STOP_REQUEST) {
    // Obeyed before any version or build check, so a server left over from a rebuild can still be ended.
    stop = true;
    if(st::writeAck(socket, READ_MS)) {
      st::writeFrame(socket, STOP_REPLY, READ_MS);
    }
  } else if(st::decode(payload, &request) && st::writeAck(socket, READ_MS)) {
    if(const std::optional<st::WireReply> reply = answer(request, served, cache, background)) {
      st::writeFrame(socket, st::encode(*reply), READ_MS);
    }
  }
  socket.disconnectFromServer();
  return stop;
}

#ifndef _WIN32
int signalPipe[2] = {-1, -1};

extern "C" void onSignal(int signal) {
  const int saved = errno;
  const char byte = static_cast<char>(signal);
  const ssize_t ignored = write(signalPipe[1], &byte, 1);
  (void)ignored;
  errno = saved;
}

bool processAlive(qint64 pid) {
  return kill(static_cast<pid_t>(pid), 0) == 0;
}

// Quits the event loop on SIGTERM, SIGINT and SIGHUP, so the server removes its socket file on the way out.
bool quitOnSignals() {
  if(socketpair(AF_UNIX, SOCK_STREAM, 0, signalPipe) != 0) {
    return false;
  }
  auto* notifier = new QSocketNotifier(signalPipe[0], QSocketNotifier::Read, QCoreApplication::instance());
  QObject::connect(notifier, &QSocketNotifier::activated, []() { QCoreApplication::quit(); });
  struct sigaction action = {};
  action.sa_handler = onSignal;
  sigemptyset(&action.sa_mask);
  for(const int signal : {SIGTERM, SIGINT, SIGHUP}) {
    sigaction(signal, &action, nullptr);
  }
  return true;
}
#else
bool quitOnSignals() {
  return true;
}

bool processAlive(qint64) {
  return true;
}
#endif

bool readExactly(QLocalSocket& socket, qint64 size, QByteArray* data, const QDeadlineTimer& deadline) {
  data->clear();
  while(data->size() < size) {
    if(socket.bytesAvailable() == 0 && (deadline.hasExpired() || !socket.waitForReadyRead(deadline.remainingTime()))) {
      return false;
    }
    data->append(socket.read(size - data->size()));
  }
  return true;
}

bool writeAll(QLocalSocket& socket, const QByteArray& data, const QDeadlineTimer& deadline) {
  if(socket.write(data) != data.size()) {
    return false;
  }
  while(socket.bytesToWrite() > 0) {
    if(deadline.hasExpired() || !socket.waitForBytesWritten(deadline.remainingTime())) {
      return false;
    }
  }
  return true;
}
}    // namespace

namespace st {
QString buildId() {
  // Taken once: a rebuild replaces the file under a running server, which must keep its own identity.
  static const QString id = []() {
    const QFileInfo binary(QCoreApplication::applicationFilePath());
    return binary.absoluteFilePath() + '|' + QString::number(binary.size()) + '|' +
        QString::number(binary.lastModified().toMSecsSinceEpoch());
  }();
  return id;
}

QByteArray encode(const WireRequest& request) {
  QByteArray data;
  QDataStream out(&data, QIODevice::WriteOnly);
  stream(out) << request.version << request.buildId << request.request.arguments << request.request.cwd
              << request.request.stdinIsTerminal << request.stdinData;
  return data;
}

bool decode(const QByteArray& data, WireRequest* request) {
  QDataStream in(data);
  stream(in) >> request->version;
  if(in.status() == QDataStream::Ok && request->version != PROTOCOL_VERSION) {
    return true;
  }
  in >> request->buildId >> request->request.arguments >> request->request.cwd >> request->request.stdinIsTerminal >>
      request->stdinData;
  return complete(in);
}

QByteArray encode(const WireReply& reply) {
  QByteArray data;
  QDataStream out(&data, QIODevice::WriteOnly);
  stream(out) << reply.exitCode << reply.out << reply.err << reply.refused << reply.refusal;
  return data;
}

bool decode(const QByteArray& data, WireReply* reply) {
  QDataStream in(data);
  stream(in) >> reply->exitCode >> reply->out >> reply->err >> reply->refused >> reply->refusal;
  return complete(in);
}

bool writeFrame(QLocalSocket& socket, const QByteArray& payload, int timeoutMs) {
  QByteArray frame(4, '\0');
  qToBigEndian<quint32>(static_cast<quint32>(payload.size()), frame.data());
  return writeAll(socket, frame + payload, QDeadlineTimer(timeoutMs));
}

bool readFrame(QLocalSocket& socket, QByteArray* payload, int timeoutMs) {
  const QDeadlineTimer deadline(timeoutMs);
  QByteArray header;
  if(!readExactly(socket, 4, &header, deadline)) {
    return false;
  }
  const quint32 size = qFromBigEndian<quint32>(header.constData());
  return size <= MAX_FRAME && readExactly(socket, size, payload, deadline);
}

bool writeAck(QLocalSocket& socket, int timeoutMs) {
  return writeAll(socket, QByteArray(1, ACK), QDeadlineTimer(timeoutMs));
}

bool readAck(QLocalSocket& socket, int timeoutMs) {
  QByteArray ack;
  return readExactly(socket, 1, &ack, QDeadlineTimer(timeoutMs)) && ack[0] == ACK;
}

bool serverRunning(const FilePath& projectFile) {
  const QString name = serverSocketName(projectFile);
  if(privateDirectory(QFileInfo(name).path(), false) != DirectoryState::PRIVATE) {
    return false;
  }
  QLocalSocket socket;
  socket.connectToServer(name);
  return socket.waitForConnected(PROBE_MS);
}

int stopServer(const FilePath& projectFile, std::ostream& err) {
  const QString name = serverSocketName(projectFile);
  const QString directory = QFileInfo(name).path();
  if(privateDirectory(directory, false) == DirectoryState::UNSAFE) {
    err << "st: unsafe server directory " << directory.toStdString() << '\n';
    return EXIT_INTERNAL;
  }
  QLocalSocket socket;
  socket.connectToServer(name);
  if(!socket.waitForConnected(PROBE_MS)) {
    err << "st: no server running\n";
    return EXIT_EMPTY;
  }
  QByteArray payload;
  if(!writeFrame(socket, STOP_REQUEST, READ_MS) || !readAck(socket, READ_MS) || !readFrame(socket, &payload, READ_MS)) {
    err << "st: server did not answer\n";
    return EXIT_INTERNAL;
  }
  if(payload != STOP_REPLY) {
    WireReply reply;
    err << "st: server " << (decode(payload, &reply) && reply.refused ? reply.refusal.toStdString() : "did not stop") << '\n';
    return EXIT_INTERNAL;
  }
  // The server answers until its event loop has ended and the socket is closed.
  const QDeadlineTimer deadline(STOP_MS);
  while(serverRunning(projectFile)) {
    if(deadline.hasExpired()) {
      err << "st: server did not stop\n";
      return EXIT_INTERNAL;
    }
    QThread::msleep(20);
  }
  err << "st: server stopped\n";
  return 0;
}

namespace {
enum class Running { NO, THIS_BUILD, OTHER_BUILD };

// Whether a server for the project answers, and whether it is of this build: a server of this build closes the
// connection on `st serve` without a reply, one of another build or protocol refuses it.
Running runningServer(const FilePath& projectFile) {
  if(!serverRunning(projectFile)) {
    return Running::NO;
  }
  QLocalSocket socket;
  socket.connectToServer(serverSocketName(projectFile));
  if(!socket.waitForConnected(PROBE_MS)) {
    return Running::NO;
  }
  const QString project = QString::fromStdWString(projectFile.wstr());
  const Request probe{{"st", "serve", "--project", project}, QDir::currentPath(), true};
  QByteArray payload;
  WireReply reply;
  if(writeFrame(socket, encode({PROTOCOL_VERSION, buildId(), probe, {}}), READ_MS) && readAck(socket, PROBE_ACK_MS) &&
     readFrame(socket, &payload, READ_MS) && decode(payload, &reply) && reply.refused) {
    return Running::OTHER_BUILD;
  }
  // Busy or silent: it is running all the same.
  return Running::THIS_BUILD;
}

// The answer to `st serve` when a server already runs.
int alreadyRunning(Running running, const QString& project, std::ostream& err) {
  if(running == Running::OTHER_BUILD) {
    err << "st: a server from another build is running; run st serve --stop first\n";
    return EXIT_INTERNAL;
  }
  err << "st: server already running for " << project.toStdString() << '\n';
  return EXIT_RESULTS;
}
}    // namespace

int detachServer(const FilePath& projectFile, const ServeOptions& options, std::ostream& err) {
  const QString project = QString::fromStdWString(projectFile.wstr());
  if(const Running running = runningServer(projectFile); running != Running::NO) {
    return alreadyRunning(running, project, err);
  }
  const QString log = QFileInfo(project).absoluteDir().absoluteFilePath(".st-serve.log");
  QProcess process;
  process.setProgram(QCoreApplication::applicationFilePath());
  process.setArguments(options.watch ? QStringList{"serve", "--project", project} :
                                       QStringList{"serve", "--no-watch", "--project", project});
  process.setStandardInputFile(QProcess::nullDevice());
  process.setStandardOutputFile(log, QIODevice::Truncate);
  process.setStandardErrorFile(log, QIODevice::Append);
  qint64 pid = 0;
  if(!process.startDetached(&pid)) {
    err << "st: cannot start " << process.program().toStdString() << ": " << process.errorString().toStdString() << '\n';
    return EXIT_INTERNAL;
  }
  QDeadlineTimer deadline(DETACH_MS);
  while(!serverRunning(projectFile)) {
    if(deadline.hasExpired() || !processAlive(pid)) {
      QFile file(log);
      QStringList lines;
      if(file.open(QIODevice::ReadOnly)) {
        lines = QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
      }
      err << (deadline.hasExpired() ? "st: server did not start within 30 s; " : "st: server exited; ") << log.toStdString()
          << ":\n";
      for(int i = std::max(0, static_cast<int>(lines.size()) - LOG_TAIL); i < lines.size(); i++) {
        err << lines[i].toStdString() << '\n';
      }
      return EXIT_INTERNAL;
    }
    QThread::msleep(100);
  }
  err << "st: server running for " << project.toStdString() << " (pid " << pid << ")\n";
  return 0;
}

int serve(const FilePath& projectFile, const ServeOptions& options, std::ostream& err) {
  buildId();
  const QString project = QString::fromStdWString(projectFile.wstr());
  const QString name = serverSocketName(projectFile);
  const QString directory = QFileInfo(name).path();
  if(const DirectoryState state = privateDirectory(directory, true); state != DirectoryState::PRIVATE) {
    err << "st: " << (state == DirectoryState::UNSAFE ? "unsafe server directory " : "cannot create server directory ")
        << directory.toStdString() << '\n';
    return EXIT_INTERNAL;
  }
  // Held for the server's lifetime, so of two servers starting at once only one removes and claims the socket.
  const QString lockName = name + ".lock";
  QLockFile lock(lockName);
  lock.setStaleLockTime(0);
  if(!lock.tryLock(0)) {
    if(lock.error() != QLockFile::LockFailedError) {
      err << "st: cannot lock " << lockName.toStdString() << '\n';
      return EXIT_INTERNAL;
    }
    return alreadyRunning(runningServer(projectFile), project, err);
  }
  if(const Running running = runningServer(projectFile); running != Running::NO) {
    return alreadyRunning(running, project, err);
  }
  QLocalServer::removeServer(name);
  QLocalServer server;
  server.setSocketOptions(QLocalServer::UserAccessOption);
  if(!server.listen(name)) {
    err << "st: cannot listen on " << name.toStdString() << ": " << server.errorString().toStdString() << '\n';
    return EXIT_INTERNAL;
  }
  quitOnSignals();
  IndexCache cache;
  std::optional<BackgroundRefresh> background;
  if(options.watch) {
    background.emplace(projectFile, cache, err);
  }
  QObject::connect(&server, &QLocalServer::newConnection, [&]() {
    while(QLocalSocket* socket = server.nextPendingConnection()) {
      const bool stop = handle(*socket, projectFile, cache, background ? &*background : nullptr);
      socket->deleteLater();
      if(stop) {
        QCoreApplication::quit();
        return;
      }
    }
  });
  err << "st: serving " << project.toStdString() << " on " << name.toStdString() << std::endl;
  return QCoreApplication::exec();
}
}    // namespace st
