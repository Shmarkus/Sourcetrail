#ifndef ST_QUERIES_H
#define ST_QUERIES_H

#include <string>

#include <QString>

class QProcess;

namespace cli {
// Streams the indexer's output to stderr, or collects it into `log` when given.
void reindex(const QString& project, bool full, int timeoutMs, std::string* log = nullptr);
// Sets up `process` to run the adjacent Sourcetrail indexer on the project, output merged; start() runs it.
void prepareIndexer(QProcess& process, const QString& project, bool full);
}    // namespace cli

#endif
