#ifndef ST_WATCH_H
#define ST_WATCH_H

#include <QFileSystemWatcher>
#include <QString>

#include <functional>
#include <optional>
#include <set>
#include <vector>

#include "FilePath.h"
#include "StFreshness.h"

class ProjectSettings;

namespace st
{
// Pure debounce state: no timers, no Qt, so it can be tested without an event loop.
class RefreshScheduler
{
public:
	explicit RefreshScheduler(qint64 quietMs = 500);
	void changed(qint64 nowMs);	// a watched path changed
	bool due(qint64 nowMs) const;	// changes pending, none for quietMs, no refresh running
	void started();	// pending changes are now being refreshed
	void finished();	// the running refresh ended (successfully or not)
	bool pending() const;
	bool running() const;

private:
	qint64 m_quietMs;
	qint64 m_lastChange = 0;
	bool m_pending = false;
	bool m_running = false;
};

// The record of a successful background refresh, kept until the index can be read under the project lock. Recorded
// against the index time read then; if that index is a later one, the baseline is earlier than needed (one extra
// refresh at worst, never a missed edit). Dropped when a refresh that started later has recorded itself.
class PendingRecord
{
public:
	void succeeded(const TimeStamp& startedAt);
	// The record to write now that the index time is known, or nullopt; clears the pending record either way.
	std::optional<RefreshRecord> take(const TimeStamp& indexedAt, const std::optional<RefreshRecord>& last);
	bool pending() const;

private:
	std::optional<TimeStamp> m_startedAt;
};

// Directories and files to watch: every source root, every directory holding an indexed file, every
// indexed file, and the Gradle build files; never anything under build/, .gradle/, .git/ or
// sourcetrail_dependencies/. Above 20000 paths, files are dropped and only directories are watched.
std::vector<QString> watchPaths(const ProjectSettings& settings, const std::vector<FilePath>& indexedFiles);

// Watches a set of paths and calls back on every change. Keeps watching a path an editor replaced (an atomic save
// drops the watch) once it exists again.
class SourceWatcher
{
public:
	explicit SourceWatcher(std::function<void()> onChange);

	// Watches exactly these paths from now on; returns how many could not be watched yet.
	size_t watch(const std::vector<QString>& paths);
	// Watches again the wanted paths that dropped out and exist again; returns how many are still missing.
	size_t rewatch();
	// rewatch(), once, if a change was reported since the last one; cheap otherwise.
	void settle();

private:
	QFileSystemWatcher m_watcher;
	std::set<QString> m_wanted;
	std::function<void()> m_onChange;
	bool m_dirty = false;
};
}

#endif
