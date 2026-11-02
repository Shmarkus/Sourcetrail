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
#include <QProcess>
#include <QSocketNotifier>
#include <QThread>
#include <QtEndian>

#include <algorithm>
#include <optional>
#include <sstream>

#ifndef _WIN32
#include <csignal>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "StOutput.h"

namespace
{
constexpr quint32 MAX_FRAME = 256 * 1024 * 1024;
constexpr int PROBE_MS = 50;
constexpr int READ_MS = 5000;
constexpr char ACK = 0x06;
constexpr int DETACH_MS = 30000;
constexpr int LOG_TAIL = 20;

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

// The reply to one request, or nullopt to close the connection without one.
std::optional<st::WireReply> answer(
	const st::WireRequest& wire, const FilePath& served, st::IndexCache& cache, bool* stop)
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
	const QString previous = QDir::currentPath();
	QDir::setCurrent(request.cwd);
	reply.exitCode = st::runCommand(request, {in, out, err}, cache);
	QDir::setCurrent(previous);
	reply.out = QByteArray::fromStdString(out.str());
	reply.err = QByteArray::fromStdString(err.str());
	return reply;
}

// Whether the server should stop after this connection.
bool handle(QLocalSocket& socket, const FilePath& served, st::IndexCache& cache)
{
	bool stop = false;
	QByteArray payload;
	st::WireRequest request;
	if (st::readFrame(socket, &payload, READ_MS) && st::decode(payload, &request) && st::writeAck(socket, READ_MS))
	{
		if (const std::optional<st::WireReply> reply = answer(request, served, cache, &stop))
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
	const char byte = static_cast<char>(signal);
	const ssize_t ignored = write(signalPipe[1], &byte, 1);
	(void)ignored;
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
	err << "st: server stopped\n";
	return 0;
}

int detachServer(const FilePath& projectFile, std::ostream& err)
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
	process.setArguments({"serve", "--project", project});
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
		if (deadline.hasExpired())
		{
			QFile file(log);
			QStringList lines;
			if (file.open(QIODevice::ReadOnly))
			{
				lines = QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
			}
			err << "st: server did not start within 30 s; " << log.toStdString() << ":\n";
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
	(void)options;
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
	QObject::connect(&server, &QLocalServer::newConnection, [&]() {
		while (QLocalSocket* socket = server.nextPendingConnection())
		{
			const bool stop = handle(*socket, projectFile, cache);
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
