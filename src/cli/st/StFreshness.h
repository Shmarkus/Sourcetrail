#ifndef ST_FRESHNESS_H
#define ST_FRESHNESS_H

#include <iostream>
#include <optional>
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

// st's last successful refresh: when it started and the index time it left behind.
struct RefreshRecord
{
	TimeStamp startedAt;
	TimeStamp indexedAt;
};

std::optional<RefreshRecord> lastRefresh(const ProjectSettings& settings);
void recordRefresh(const ProjectSettings& settings, const RefreshRecord& record, std::ostream& err = std::cerr);

// Changes at or after this time are not in the index. Sourcetrail writes the index time when indexing
// ends and keeps it when a refresh re-indexes nothing, so while the index is still the one st's refresh
// produced, that refresh's start is the baseline; after anyone else's indexing, the index time is.
TimeStamp freshnessBaseline(const TimeStamp& indexedAt, const std::optional<RefreshRecord>& last);

// The start time to record for a refresh that begins now.
TimeStamp refreshStart();
}

#endif
