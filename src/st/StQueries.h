#ifndef ST_QUERIES_H
#define ST_QUERIES_H

#include <functional>

#include <QByteArray>
#include <QString>

class QProcess;

namespace cli {
// Runs the indexer and hands its output to `output` as it arrives: its status lines (stdout, `log` false) and its log
// lines (stderr, `log` true; each file it indexes, Gradle and delombok output, errors).
void reindex(const QString& project, bool full, int timeoutMs, const std::function<void(const QByteArray& text, bool log)>& output);
// Sets up `process` to run the adjacent Sourcetrail indexer on the project with --verbose, so that its log lines are
// there to show when it fails; start() runs it.
void prepareIndexer(QProcess& process, const QString& project, bool full);
// How many of a failed indexer's log lines st shows.
constexpr int INDEXER_TAIL_LINES = 50;
// What st shows of a failed indexer's output: its last log lines, then its status lines in full (they hold the errors
// it reported, such as a failed Gradle command, and are short); a part left empty is left out.
QByteArray failureReport(const QByteArray& status, const QByteArray& log);
// The last `count` lines of `text`.
QByteArray lastLines(const QByteArray& text, int count);
}    // namespace cli

#endif
