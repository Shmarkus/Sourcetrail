#ifndef CLI_QUERY_H
#define CLI_QUERY_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <string>

#include "types.h"

class PersistentStorage;
class QProcess;

namespace cli
{
QJsonObject symbol(PersistentStorage& storage, Id id);
QJsonObject search(PersistentStorage& storage, const QString& query, int limit);
QJsonObject describe(PersistentStorage& storage, Id id, int limit);
QJsonObject references(PersistentStorage& storage, Id id, const QString& kind, int limit);
QJsonObject readSource(PersistentStorage& storage, const QString& path, int start, int end);
QJsonObject status(PersistentStorage& storage);
// Streams the indexer's output to stderr, or collects it into `log` when given.
void reindex(const QString& project, bool full, int timeoutMs, std::string* log = nullptr);
// Sets up `process` to run the adjacent Sourcetrail indexer on the project, output merged; start() runs it.
void prepareIndexer(QProcess& process, const QString& project, bool full);
QJsonArray locations(PersistentStorage& storage, Id id, int limit);
}

#endif
