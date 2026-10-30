#ifndef ST_FRESHNESS_H
#define ST_FRESHNESS_H

#include <string>
#include <vector>

#include "FilePath.h"
#include "TimeStamp.h"

class ProjectSettings;

namespace st
{
struct Freshness
{
	bool stale = false;
	size_t changedFiles = 0;
	std::string reason;
};

// Stale when a source file, source directory or Gradle build file changed at or after `indexedAt`.
// Source groups without a Gradle preparation cache fall back to the indexed files.
Freshness checkFreshness(const ProjectSettings& settings, const TimeStamp& indexedAt, const std::vector<FilePath>& indexedFiles);

// Start of st's last successful refresh. Sourcetrail keeps the index time when a refresh finds
// nothing to re-index (e.g. a directory changed by an editor's temporary file), so st remembers it.
TimeStamp lastRefresh(const ProjectSettings& settings);
void recordRefresh(const ProjectSettings& settings, const TimeStamp& startedAt);
}

#endif
