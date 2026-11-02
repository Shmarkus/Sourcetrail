#ifndef ST_SERVER_H
#define ST_SERVER_H

#include <QByteArray>
#include <QString>

#include <iostream>

#include "FilePath.h"
#include "StCommand.h"

class QLocalSocket;

namespace st
{
constexpr quint32 PROTOCOL_VERSION = 2;

// $XDG_RUNTIME_DIR, else QDir::tempPath(), + "/st-" + first 16 hex of SHA-1(canonical project path) + ".sock".
QString serverSocketName(const FilePath& projectFile);

// The running binary's identity: absolute path, size and mtime of QCoreApplication::applicationFilePath(), taken at the
// first call.
QString buildId();

struct WireRequest
{
	quint32 version;
	QString buildId;
	Request request;
	QByteArray stdinData;
};

struct WireReply
{
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

// Whether a server for the project accepts a connection within 50 ms.
bool serverRunning(const FilePath& projectFile);

struct ServeOptions
{
	bool watch = true;	// Task 5 reads watch; --no-watch clears it
};

// Answers requests for the project until the process ends. Needs a QCoreApplication.
int serve(const FilePath& projectFile, const ServeOptions& options, std::ostream& err);
}

#endif
