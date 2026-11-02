#include "StServer.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QtEndian>

#include <optional>
#include <sstream>

#include "StOutput.h"

namespace
{
constexpr quint32 MAX_FRAME = 256 * 1024 * 1024;
constexpr int PROBE_MS = 50;
constexpr int READ_MS = 5000;

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
std::optional<st::WireReply> answer(const st::WireRequest& wire, const FilePath& served, st::IndexCache& cache)
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

void handle(QLocalSocket& socket, const FilePath& served, st::IndexCache& cache)
{
	QByteArray payload;
	st::WireRequest request;
	if (st::readFrame(socket, &payload, READ_MS) && st::decode(payload, &request))
	{
		if (const std::optional<st::WireReply> reply = answer(request, served, cache))
		{
			st::writeFrame(socket, st::encode(*reply), READ_MS);
		}
	}
	socket.disconnectFromServer();
}

bool readExactly(QLocalSocket& socket, qint64 size, QByteArray* data, int timeoutMs)
{
	data->clear();
	while (data->size() < size)
	{
		if (socket.bytesAvailable() == 0 && !socket.waitForReadyRead(timeoutMs))
		{
			return false;
		}
		data->append(socket.read(size - data->size()));
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
	const QFileInfo binary(QCoreApplication::applicationFilePath());
	return binary.absoluteFilePath() + '|' + QString::number(binary.size()) + '|' +
		QString::number(binary.lastModified().toMSecsSinceEpoch());
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
	char header[4];
	qToBigEndian<quint32>(static_cast<quint32>(payload.size()), header);
	if (socket.write(header, 4) != 4 || socket.write(payload) != payload.size())
	{
		return false;
	}
	while (socket.bytesToWrite() > 0)
	{
		if (!socket.waitForBytesWritten(timeoutMs))
		{
			return false;
		}
	}
	return true;
}

bool readFrame(QLocalSocket& socket, QByteArray* payload, int timeoutMs)
{
	QByteArray header;
	if (!readExactly(socket, 4, &header, timeoutMs))
	{
		return false;
	}
	const quint32 size = qFromBigEndian<quint32>(header.constData());
	return size <= MAX_FRAME && readExactly(socket, size, payload, timeoutMs);
}

bool serverRunning(const FilePath& projectFile)
{
	QLocalSocket socket;
	socket.connectToServer(serverSocketName(projectFile));
	return socket.waitForConnected(PROBE_MS);
}

int serve(const FilePath& projectFile, const ServeOptions& options, std::ostream& err)
{
	(void)options;
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
	IndexCache cache;
	QObject::connect(&server, &QLocalServer::newConnection, [&]() {
		while (QLocalSocket* socket = server.nextPendingConnection())
		{
			handle(*socket, projectFile, cache);
			socket->deleteLater();
		}
	});
	err << "st: serving " << project.toStdString() << " on " << name.toStdString() << std::endl;
	return QCoreApplication::exec();
}
}
