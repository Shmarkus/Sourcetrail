#include "StClient.h"

#include <QLocalSocket>

#include "StServer.h"

namespace
{
constexpr int CONNECT_MS = 50;
constexpr int ACK_MS = 2000;
// A request may refresh the index first.
constexpr int REPLY_MS = 15 * 60 * 1000;
}

namespace st
{
std::optional<int> tryServer(const FilePath& projectFile, const Request& request, const QByteArray& stdinData,
	std::ostream& out, std::ostream& err)
{
	QLocalSocket socket;
	socket.connectToServer(serverSocketName(projectFile));
	if (!socket.waitForConnected(CONNECT_MS))
	{
		return std::nullopt;
	}
	// A server that accepts but does not acknowledge, e.g. a suspended one, is given up on quickly.
	QByteArray payload;
	WireReply reply;
	if (!writeFrame(socket, encode({PROTOCOL_VERSION, buildId(), request, stdinData}), ACK_MS) || !readAck(socket, ACK_MS) ||
		!readFrame(socket, &payload, REPLY_MS) || !decode(payload, &reply))
	{
		err << "st: server did not answer; answering without it\n";
		return std::nullopt;
	}
	if (reply.refused)
	{
		err << "st: server " << reply.refusal.toStdString() << "; answering without it\n";
		return std::nullopt;
	}
	out.write(reply.out.constData(), reply.out.size());
	err.write(reply.err.constData(), reply.err.size());
	out.flush();
	return reply.exitCode;
}
}
