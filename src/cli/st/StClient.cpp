#include "StClient.h"

#include <QLocalSocket>

#include "StServer.h"

namespace
{
constexpr int CONNECT_MS = 50;
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
	// A request may refresh the index first, which can take minutes: wait for the reply as long as the server is there.
	QByteArray payload;
	WireReply reply;
	if (!writeFrame(socket, encode({PROTOCOL_VERSION, buildId(), request, stdinData}), -1) ||
		!readFrame(socket, &payload, -1) || !decode(payload, &reply))
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
