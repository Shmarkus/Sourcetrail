#include "StServer.h"

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
#include <QThread>
#include <QTimer>
#include <QtEndian>

#include <algorithm>
#include <iomanip>
#include <optional>
#include <sstream>

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "FileInfo.h"
#include "PersistentStorage.h"
#include "ProjectSettings.h"
#include "Query.h"
#include "StFreshness.h"
#include "StOutput.h"
#include "StProject.h"
#include "StWatch.h"
#include "StorageStats.h"

namespace
{
constexpr quint32 MAX_FRAME = 256 * 1024 * 1024;
constexpr int PROBE_MS = 50;
constexpr int READ_MS = 5000;
constexpr char ACK = 0x06;
constexpr int STOP_MS = 5000;
constexpr int DETACH_MS = 30000;
constexpr int LOG_TAIL = 20;
constexpr int POLL_MS = 100;
constexpr qint64 REFRESH_MS = 600000;	// as a request's refresh
constexpr int LOCK_MS = 5000;

QDataStream& stream(QDataStream& data)
{
	data.setVersion(QDataStream::Qt_5_12);
	return data;
}

bool complete(const QDataStream& data)
{
	return data.status() == QDataStream::Ok && data.atEnd();
}

QString canonicalPath(const FilePath& path)
{
	return QFileInfo(QString::fromStdWString(path.wstr())).canonicalFilePath();
}

std::string seconds(qint64 ms)
{
	std::ostringstream text;
	text << std::fixed << std::setprecision(1) << ms / 1000.0 << 's';
	return text.str();
}

// Refreshes the index in the background after the sources change, the way a request would: refreshStart before the
// indexer, recordRefresh after it succeeded. Never holds the project lock while the indexer runs; it takes it itself.
class BackgroundRefresh
{
public:
	BackgroundRefresh(const FilePath& projectFile, st::IndexCache& cache, std::ostream& err)
		: m_project(projectFile)
		, m_cache(cache)
		, m_err(err)
		, m_watcher([this]() { m_scheduler.changed(m_clock.elapsed()); })
	{
		m_clock.start();
		// Sources may have changed while no server ran.
		m_scheduler.changed(m_clock.elapsed());
		QObject::connect(&m_timer, &QTimer::timeout, [this]() { poll(); });
		m_timer.start(POLL_MS);
	}

	~BackgroundRefresh()
	{
		if (m_process)
		{
			// Killing the indexer would leave an unfinished index behind: let it finish on its own.
			m_process->disconnect();
			m_err << "st: leaving the background refresh (pid " << m_process->processId() << ") to finish" << std::endl;
			m_process = nullptr;
		}
	}

	// Waits for a running refresh; the milliseconds waited, or nullopt when none ran.
	std::optional<qint64> wait()
	{
		if (!m_process)
		{
			return std::nullopt;
		}
		QElapsedTimer timer;
		timer.start();
		if (!m_process->waitForFinished(static_cast<int>(std::max<qint64>(1, REFRESH_MS - m_running.elapsed()))))
		{
			stop();
		}
		return timer.elapsed();
	}

private:
	bool load(ProjectSettings* settings)
	{
		return settings->load(m_project, true);
	}

	enum class Index
	{
		READ,
		BUSY,	// locked, or being written by an indexer
		NONE	// missing, empty or incompatible
	};

	// Loads the index into the cache under the project lock.
	Index readIndex(const ProjectSettings& settings, int waitMs, TimeStamp* indexedAt, std::vector<FilePath>* files)
	{
		QLockFile lock(st::projectLockFile(m_project));
		lock.setStaleLockTime(0);
		if (!lock.tryLock(waitMs) || settings.getTempDBFilePath().exists())
		{
			return Index::BUSY;
		}
		if (!settings.getDBFilePath().exists())
		{
			return Index::NONE;
		}
		PersistentStorage& storage = m_cache.storage(settings);
		if (storage.isEmpty() || storage.isIncompatible())
		{
			return Index::NONE;
		}
		try
		{
			// What a request would otherwise compute after a reload; memoized, so cheap when nothing changed.
			m_cache.lombok(settings);
			m_cache.unresolved(storage);
		}
		catch (const std::exception&)
		{
			// The request that needs them reports the failure.
		}
		files->clear();
		for (const FileInfo& file: storage.getFileInfoForAllFiles())
		{
			files->push_back(file.path);
		}
		*indexedAt = storage.getStorageStats().timestamp;
		return Index::READ;
	}

	void watch(const ProjectSettings& settings, const std::vector<FilePath>& files)
	{
		const std::vector<QString> paths = st::watchPaths(settings, files);
		const size_t missing = m_watcher.watch(paths);
		if (!m_watching || missing)
		{
			m_err << "st: watching " << paths.size() - missing << " paths";
			if (missing)
			{
				m_err << "; cannot watch " << missing;
			}
			m_err << std::endl;
		}
		m_watching = true;
	}

	void poll()
	{
		if (m_process && m_running.elapsed() > REFRESH_MS)
		{
			stop();
		}
		if (m_scheduler.due(m_clock.elapsed()))
		{
			begin();
		}
	}

	void begin()
	{
		ProjectSettings settings;
		TimeStamp indexedAt;
		std::vector<FilePath> files;
		const Index index = load(&settings) ? readIndex(settings, 0, &indexedAt, &files) : Index::NONE;
		if (index == Index::BUSY)
		{
			// Another st or an indexer is at work: look again after the next quiet period.
			m_scheduler.changed(m_clock.elapsed());
			return;
		}
		m_scheduler.started();
		if (index == Index::NONE)
		{
			// Requests report a missing index; indexing one is not a refresh.
			m_scheduler.finished();
			return;
		}
		if (!m_watching)
		{
			watch(settings, files);
		}
		const st::Freshness freshness =
			st::checkFreshness(settings, st::freshnessBaseline(indexedAt, st::lastRefresh(settings)), files);
		if (!freshness.stale)
		{
			m_watcher.rewatch();
			m_scheduler.finished();
			return;
		}
		m_started = st::refreshStart();
		m_changed = freshness.changedFiles;
		m_running.start();
		m_process = new QProcess();
		cli::prepareIndexer(*m_process, QString::fromStdWString(m_project.wstr()), false);
		m_process->setStandardInputFile(QProcess::nullDevice());
		m_process->setStandardOutputFile(logFile(), QIODevice::Truncate);
		QObject::connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), [this]() { end(); });
		m_err << "st: refreshing " << m_changed << " changed files in the background" << std::endl;
		m_process->start();
		if (!m_process->waitForStarted())
		{
			m_err << "st: cannot start " << m_process->program().toStdString() << ": "
				  << m_process->errorString().toStdString() << std::endl;
			m_process->disconnect();
			m_process->deleteLater();
			m_process = nullptr;
			m_scheduler.finished();
		}
	}

	void end()
	{
		const bool succeeded = m_process->exitStatus() == QProcess::NormalExit && m_process->exitCode() == 0;
		m_process->deleteLater();
		m_process = nullptr;
		ProjectSettings settings;
		TimeStamp indexedAt;
		std::vector<FilePath> files;
		// Reloads the cache now, so the next request finds it warm.
		const bool read = load(&settings) && readIndex(settings, LOCK_MS, &indexedAt, &files) == Index::READ;
		if (succeeded && read)
		{
			st::recordRefresh(settings, {m_started, indexedAt}, m_err);
		}
		if (read)
		{
			// New files, and files an editor replaced (an atomic save drops the watch).
			watch(settings, files);
		}
		if (succeeded)
		{
			m_err << "st: background refresh of " << m_changed << " files finished in " << seconds(m_running.elapsed())
				  << (read ? "" : "; cannot read the index afterwards") << std::endl;
		}
		else
		{
			m_err << "st: background refresh failed; see " << logFile().toStdString() << std::endl;
		}
		m_scheduler.finished();
	}

	// Ends an indexer that ran out of time; finished() then reports the failure.
	void stop()
	{
		m_err << "st: background refresh timed out" << std::endl;
		m_process->terminate();
		if (!m_process->waitForFinished(5000))
		{
			m_process->kill();
			m_process->waitForFinished();
		}
	}

	QString logFile() const
	{
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
};

// The reply to one request, or nullopt to close the connection without one.
std::optional<st::WireReply> answer(const st::WireRequest& wire, const FilePath& served, st::IndexCache& cache,
	BackgroundRefresh* background, bool* stop)
{
	st::WireReply reply{st::EXIT_INTERNAL, {}, {}};
	const auto refuse = [&reply](const char* reason) {
		reply.refused = true;
		reply.refusal = reason;
		return reply;
	};
	if (wire.version != st::PROTOCOL_VERSION)
	{
		return refuse("speaks another protocol version");
	}
	// A stop is obeyed whatever the build, so a server left over from a rebuild can still be ended.
	if (wire.request.arguments == QStringList{"st", "serve", "--stop"})
	{
		*stop = true;
		reply.exitCode = st::EXIT_RESULTS;
		return reply;
	}
	if (wire.buildId != st::buildId())
	{
		return refuse("is from another build");
	}
	const st::Route route = st::route(wire.request);
	if (route.command == "serve")
	{
		return std::nullopt;
	}
	const QString project = canonicalPath(route.project);
	if (project.isEmpty() || project != canonicalPath(served))
	{
		return refuse("serves another project");
	}
	st::Request request = wire.request;
	request.served = true;
	std::istringstream in(wire.stdinData.toStdString());
	std::ostringstream out;
	std::ostringstream err;
	// A refresh already running is waited for, not started a second time; the freshness check then runs as usual.
	if (const std::optional<qint64> waited = background ? background->wait() : std::nullopt)
	{
		err << "st: waited " << seconds(*waited) << " for the background refresh\n";
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
bool handle(QLocalSocket& socket, const FilePath& served, st::IndexCache& cache, BackgroundRefresh* background)
{
	bool stop = false;
	QByteArray payload;
	st::WireRequest request;
	if (st::readFrame(socket, &payload, READ_MS) && st::decode(payload, &request) && st::writeAck(socket, READ_MS))
	{
		if (const std::optional<st::WireReply> reply = answer(request, served, cache, background, &stop))
		{
			st::writeFrame(socket, st::encode(*reply), READ_MS);
		}
	}
	socket.disconnectFromServer();
	return stop;
}

#ifndef _WIN32
int signalPipe[2] = {-1, -1};

extern "C" void onSignal(int signal)
{
	const int saved = errno;
	const char byte = static_cast<char>(signal);
	const ssize_t ignored = write(signalPipe[1], &byte, 1);
	(void)ignored;
	errno = saved;
}

bool processAlive(qint64 pid)
{
	return kill(static_cast<pid_t>(pid), 0) == 0;
}

// Quits the event loop on SIGTERM, SIGINT and SIGHUP, so the server removes its socket file on the way out.
bool quitOnSignals()
{
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, signalPipe) != 0)
	{
		return false;
	}
	auto* notifier = new QSocketNotifier(signalPipe[0], QSocketNotifier::Read, QCoreApplication::instance());
	QObject::connect(notifier, &QSocketNotifier::activated, []() { QCoreApplication::quit(); });
	struct sigaction action = {};
	action.sa_handler = onSignal;
	sigemptyset(&action.sa_mask);
	for (const int signal : {SIGTERM, SIGINT, SIGHUP})
	{
		sigaction(signal, &action, nullptr);
	}
	return true;
}
#else
bool quitOnSignals()
{
	return true;
}

bool processAlive(qint64)
{
	return true;
}
#endif

bool readExactly(QLocalSocket& socket, qint64 size, QByteArray* data, const QDeadlineTimer& deadline)
{
	data->clear();
	while (data->size() < size)
	{
		if (socket.bytesAvailable() == 0 && (deadline.hasExpired() || !socket.waitForReadyRead(deadline.remainingTime())))
		{
			return false;
		}
		data->append(socket.read(size - data->size()));
	}
	return true;
}

bool writeAll(QLocalSocket& socket, const QByteArray& data, const QDeadlineTimer& deadline)
{
	if (socket.write(data) != data.size())
	{
		return false;
	}
	while (socket.bytesToWrite() > 0)
	{
		if (deadline.hasExpired() || !socket.waitForBytesWritten(deadline.remainingTime()))
		{
			return false;
		}
	}
	return true;
}
}

namespace st
{
QString serverSocketName(const FilePath& projectFile)
{
	const QString path = QString::fromStdWString(projectFile.wstr());
	const QString canonical = QFileInfo(path).canonicalFilePath();
	const QByteArray hash = QCryptographicHash::hash(
		(canonical.isEmpty() ? QDir::cleanPath(QFileInfo(path).absoluteFilePath()) : canonical).toUtf8(),
		QCryptographicHash::Sha1);
	const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
	return (runtime.isEmpty() ? QDir::tempPath() : runtime) + "/st-" + QString::fromLatin1(hash.toHex().left(16)) + ".sock";
}

QString buildId()
{
	// Taken once: a rebuild replaces the file under a running server, which must keep its own identity.
	static const QString id = []() {
		const QFileInfo binary(QCoreApplication::applicationFilePath());
		return binary.absoluteFilePath() + '|' + QString::number(binary.size()) + '|' +
			QString::number(binary.lastModified().toMSecsSinceEpoch());
	}();
	return id;
}

QByteArray encode(const WireRequest& request)
{
	QByteArray data;
	QDataStream out(&data, QIODevice::WriteOnly);
	stream(out) << request.version << request.buildId << request.request.arguments << request.request.cwd
				<< request.request.stdinIsTerminal << request.stdinData;
	return data;
}

bool decode(const QByteArray& data, WireRequest* request)
{
	QDataStream in(data);
	stream(in) >> request->version;
	if (in.status() == QDataStream::Ok && request->version != PROTOCOL_VERSION)
	{
		return true;
	}
	in >> request->buildId >> request->request.arguments >> request->request.cwd >> request->request.stdinIsTerminal >>
		request->stdinData;
	return complete(in);
}

QByteArray encode(const WireReply& reply)
{
	QByteArray data;
	QDataStream out(&data, QIODevice::WriteOnly);
	stream(out) << reply.exitCode << reply.out << reply.err << reply.refused << reply.refusal;
	return data;
}

bool decode(const QByteArray& data, WireReply* reply)
{
	QDataStream in(data);
	stream(in) >> reply->exitCode >> reply->out >> reply->err >> reply->refused >> reply->refusal;
	return complete(in);
}

bool writeFrame(QLocalSocket& socket, const QByteArray& payload, int timeoutMs)
{
	QByteArray frame(4, '\0');
	qToBigEndian<quint32>(static_cast<quint32>(payload.size()), frame.data());
	return writeAll(socket, frame + payload, QDeadlineTimer(timeoutMs));
}

bool readFrame(QLocalSocket& socket, QByteArray* payload, int timeoutMs)
{
	const QDeadlineTimer deadline(timeoutMs);
	QByteArray header;
	if (!readExactly(socket, 4, &header, deadline))
	{
		return false;
	}
	const quint32 size = qFromBigEndian<quint32>(header.constData());
	return size <= MAX_FRAME && readExactly(socket, size, payload, deadline);
}

bool writeAck(QLocalSocket& socket, int timeoutMs)
{
	return writeAll(socket, QByteArray(1, ACK), QDeadlineTimer(timeoutMs));
}

bool readAck(QLocalSocket& socket, int timeoutMs)
{
	QByteArray ack;
	return readExactly(socket, 1, &ack, QDeadlineTimer(timeoutMs)) && ack[0] == ACK;
}

bool serverRunning(const FilePath& projectFile)
{
	QLocalSocket socket;
	socket.connectToServer(serverSocketName(projectFile));
	return socket.waitForConnected(PROBE_MS);
}

int stopServer(const FilePath& projectFile, std::ostream& err)
{
	QLocalSocket socket;
	socket.connectToServer(serverSocketName(projectFile));
	QByteArray payload;
	WireReply reply;
	if (!socket.waitForConnected(PROBE_MS) ||
		!writeFrame(socket, encode({PROTOCOL_VERSION, buildId(), {{"st", "serve", "--stop"}, {}, false}, {}}), READ_MS) ||
		!readAck(socket, READ_MS) || !readFrame(socket, &payload, READ_MS) || !decode(payload, &reply) || reply.refused)
	{
		err << "st: no server running\n";
		return EXIT_EMPTY;
	}
	// The server answers until its event loop has ended and the socket is closed.
	const QDeadlineTimer deadline(STOP_MS);
	while (serverRunning(projectFile))
	{
		if (deadline.hasExpired())
		{
			err << "st: server did not stop\n";
			return EXIT_INTERNAL;
		}
		QThread::msleep(20);
	}
	err << "st: server stopped\n";
	return 0;
}

int detachServer(const FilePath& projectFile, const ServeOptions& options, std::ostream& err)
{
	const QString project = QString::fromStdWString(projectFile.wstr());
	if (serverRunning(projectFile))
	{
		err << "st: server already running for " << project.toStdString() << '\n';
		return 0;
	}
	const QString log = QFileInfo(project).absoluteDir().absoluteFilePath(".st-serve.log");
	QProcess process;
	process.setProgram(QCoreApplication::applicationFilePath());
	process.setArguments(options.watch ? QStringList{"serve", "--project", project}
									   : QStringList{"serve", "--no-watch", "--project", project});
	process.setStandardInputFile(QProcess::nullDevice());
	process.setStandardOutputFile(log, QIODevice::Truncate);
	process.setStandardErrorFile(log, QIODevice::Append);
	qint64 pid = 0;
	if (!process.startDetached(&pid))
	{
		err << "st: cannot start " << process.program().toStdString() << ": " << process.errorString().toStdString() << '\n';
		return EXIT_INTERNAL;
	}
	QDeadlineTimer deadline(DETACH_MS);
	while (!serverRunning(projectFile))
	{
		if (deadline.hasExpired() || !processAlive(pid))
		{
			QFile file(log);
			QStringList lines;
			if (file.open(QIODevice::ReadOnly))
			{
				lines = QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
			}
			err << (deadline.hasExpired() ? "st: server did not start within 30 s; " : "st: server exited; ") << log.toStdString() << ":\n";
			for (int i = std::max(0, static_cast<int>(lines.size()) - LOG_TAIL); i < lines.size(); i++)
			{
				err << lines[i].toStdString() << '\n';
			}
			return EXIT_INTERNAL;
		}
		QThread::msleep(100);
	}
	err << "st: server running for " << project.toStdString() << " (pid " << pid << ")\n";
	return 0;
}

int serve(const FilePath& projectFile, const ServeOptions& options, std::ostream& err)
{
	buildId();
	const QString project = QString::fromStdWString(projectFile.wstr());
	const QString name = serverSocketName(projectFile);
	if (serverRunning(projectFile))
	{
		err << "st: server already running for " << project.toStdString() << '\n';
		return EXIT_RESULTS;
	}
	QLocalServer::removeServer(name);
	QLocalServer server;
	server.setSocketOptions(QLocalServer::UserAccessOption);
	if (!server.listen(name))
	{
		err << "st: cannot listen on " << name.toStdString() << ": " << server.errorString().toStdString() << '\n';
		return EXIT_INTERNAL;
	}
	quitOnSignals();
	IndexCache cache;
	std::optional<BackgroundRefresh> background;
	if (options.watch)
	{
		background.emplace(projectFile, cache, err);
	}
	QObject::connect(&server, &QLocalServer::newConnection, [&]() {
		while (QLocalSocket* socket = server.nextPendingConnection())
		{
			const bool stop = handle(*socket, projectFile, cache, background ? &*background : nullptr);
			socket->deleteLater();
			if (stop)
			{
				QCoreApplication::quit();
				return;
			}
		}
	});
	err << "st: serving " << project.toStdString() << " on " << name.toStdString() << std::endl;
	return QCoreApplication::exec();
}
}
