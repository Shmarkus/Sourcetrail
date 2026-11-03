#include "StClient.h"

#include <QFileInfo>
#include <QLocalSocket>

#include "StServer.h"

namespace {
constexpr int CONNECT_MS = 50;
constexpr int ACK_MS = 2000;
// A request may refresh the index first; st index waits for its indexer however long it runs.
constexpr int REPLY_MS = 15 * 60 * 1000;
constexpr int INDEX_REPLY_MS = -1;
}    // namespace

namespace st {
std::optional<int> tryServer(
    const FilePath& projectFile, const Request& request, const QByteArray& stdinData, std::ostream& out, std::ostream& err) {
  const QString name = serverSocketName(projectFile);
  const QString directory = QFileInfo(name).path();
  switch(privateDirectory(directory, false)) {
  case DirectoryState::MISSING:
    return std::nullopt;
  case DirectoryState::UNSAFE:
    // Someone else could listen there and be sent this request.
    err << "st: unsafe server directory " << directory.toStdString() << "; answering without it\n";
    return std::nullopt;
  case DirectoryState::PRIVATE:
    break;
  }
  QLocalSocket socket;
  socket.connectToServer(name);
  if(!socket.waitForConnected(CONNECT_MS)) {
    return std::nullopt;
  }
  // A server that accepts but does not acknowledge, e.g. a suspended one, is given up on quickly.
  QByteArray payload;
  WireReply reply;
  if(!writeFrame(socket, encode({PROTOCOL_VERSION, buildId(), request, stdinData}), ACK_MS) || !readAck(socket, ACK_MS) ||
     !readFrame(socket, &payload, route(request).command == "index" ? INDEX_REPLY_MS : REPLY_MS) || !decode(payload, &reply)) {
    err << "st: server did not answer; answering without it\n";
    return std::nullopt;
  }
  if(reply.refused) {
    err << "st: server " << reply.refusal.toStdString() << "; answering without it\n";
    return std::nullopt;
  }
  // Notes first, as a cold run piped to a file shows them.
  err.write(reply.err.constData(), reply.err.size());
  err.flush();
  out.write(reply.out.constData(), reply.out.size());
  out.flush();
  return reply.exitCode;
}
}    // namespace st
