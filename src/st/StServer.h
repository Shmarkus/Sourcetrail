#ifndef ST_SERVER_H
#define ST_SERVER_H

#include <iostream>

#include <QByteArray>
#include <QString>

#include "FilePath.h"
#include "StCommand.h"
#include "StSocket.h"

class QLocalSocket;

namespace st {
constexpr quint32 PROTOCOL_VERSION = 2;

// The running binary's identity: absolute path, size and mtime of QCoreApplication::applicationFilePath(), taken at the
// first call.
QString buildId();

struct WireRequest {
  quint32 version;
  QString buildId;
  Request request;
  QByteArray stdinData;
};

struct WireReply {
  qint32 exitCode;
  QByteArray out;
  QByteArray err;
  bool refused = false;
  QString refusal;
};

// A request of another protocol version decodes to its version alone.
QByteArray encode(const WireRequest& request);
bool decode(const QByteArray& data, WireRequest* request);
QByteArray encode(const WireReply& reply);
bool decode(const QByteArray& data, WireReply* reply);

// Frames are a big-endian quint32 length, then the payload; frames above 256 MB are refused. timeoutMs bounds the
// whole frame.
bool writeFrame(QLocalSocket& socket, const QByteArray& payload, int timeoutMs);
bool readFrame(QLocalSocket& socket, QByteArray* payload, int timeoutMs);

// The server acknowledges a request it has read and decoded with one byte, then replies.
bool writeAck(QLocalSocket& socket, int timeoutMs);
bool readAck(QLocalSocket& socket, int timeoutMs);

// Whether a server for the project accepts a connection within 50 ms, in a private socket directory.
bool serverRunning(const FilePath& projectFile);

struct ServeOptions {
  bool watch = true;    // watch the sources and refresh in the background; --no-watch clears it
};

// Asks the server for the project to stop with a fixed frame every protocol version recognises before any version
// check ("st serve --stop"; the server acknowledges, replies "st serve stopping" and stops). 0 once it has stopped,
// EXIT_EMPTY when none listens, EXIT_INTERNAL with the reason when it refuses or does not answer.
int stopServer(const FilePath& projectFile, std::ostream& err);

// Starts `st serve` for the project in the background, logging to <project dir>/.st-serve.log, and waits up to 30 s
// for it to answer; 0 once it does, else EXIT_INTERNAL with the log's tail on err. A server of this build already
// running is 0; one of another build is EXIT_INTERNAL.
int detachServer(const FilePath& projectFile, const ServeOptions& options, std::ostream& err);

// Answers requests for the project until the process ends. Needs a QCoreApplication. Creates the socket's directory
// 0700 and refuses one that is not private (EXIT_INTERNAL). Holds <socket>.lock while it runs: another server for the
// project is "already running" (0), or of another build (EXIT_INTERNAL).
// With options.watch it watches the sources and, 500 ms after the last change, refreshes a stale index in the
// background as a request would (indexer status lines in <project dir>/.st-refresh.log, its log lines in .st-refresh.log.verbose).
// A request arriving meanwhile waits for that refresh first. A refresh still running when the server stops is left to finish; its
// start, recorded before it began (recordRefreshStart), keeps the edits made during it from counting as indexed.
int serve(const FilePath& projectFile, const ServeOptions& options, std::ostream& err);
}    // namespace st

#endif
