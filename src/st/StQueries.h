#ifndef ST_QUERIES_H
#define ST_QUERIES_H

#include <functional>

#include <QByteArray>
#include <QString>

class QProcess;

namespace cli {
// Runs the indexer and hands its output to `output` as it arrives.
void reindex(
    const QString& project, bool full, int timeoutMs, const std::function<void(const QByteArray&)>& output, bool verbose = false);
// Sets up `process` to run the adjacent Sourcetrail indexer on the project, output merged; start() runs it.
// `verbose` adds the indexer's INFO log lines (each file it indexes) to its status lines.
void prepareIndexer(QProcess& process, const QString& project, bool full, bool verbose = false);
}    // namespace cli

#endif
