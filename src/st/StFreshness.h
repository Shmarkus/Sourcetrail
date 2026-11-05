#ifndef ST_FRESHNESS_H
#define ST_FRESHNESS_H

#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "FilePath.h"
#include "TimeStamp.h"

class PersistentStorage;
class ProjectSettings;

namespace st {
struct Freshness {
  bool stale = false;
  size_t changedFiles = 0;
  std::string reason;
};

// The inputs record of each TypeScript and Python source group as the index keeps it, by group id: what the group was last
// indexed from; empty when its last run was skipped.
using GroupInputs = std::map<std::string, std::string>;
GroupInputs storedGroupInputs(const ProjectSettings& settings, const PersistentStorage& storage);

// Stale when a source file, source directory or Gradle build file changed at or after `indexedAt`, or when a
// TypeScript or Python group's inputs (its trigger files) differ from the ones in `inputs`; such a group without a
// stored record counts the trigger files changed at or after `indexedAt`. Other source groups without a Gradle preparation
// cache fall back to the indexed files.
Freshness checkFreshness(const ProjectSettings& settings,
                         const TimeStamp& indexedAt,
                         const std::vector<FilePath>& indexedFiles,
                         const GroupInputs& inputs = {});

// One "maven\t..." row per enabled Maven source group for `st status`: "ok", each warning of its last Maven run
// (re-shown by every refresh that reuses it) or "not prepared; st index runs Maven".
std::vector<std::string> mavenStatusRows(const ProjectSettings& settings);

// st's last successful refresh: when it started and the index time it left behind.
struct RefreshRecord {
  TimeStamp startedAt;
  TimeStamp indexedAt;
};

std::optional<RefreshRecord> lastRefresh(const ProjectSettings& settings);
void recordRefresh(const ProjectSettings& settings, const RefreshRecord& record, std::ostream& err = std::cerr);

// Changes at or after this time are not in the index. Sourcetrail writes the index time when indexing
// ends and keeps it when a refresh re-indexes nothing, so while the index is still the one st's refresh
// produced, that refresh's start is the baseline; after anyone else's indexing, the index time is.
TimeStamp freshnessBaseline(const TimeStamp& indexedAt,
                            const std::optional<RefreshRecord>& last,
                            const std::optional<TimeStamp>& inProgress = std::nullopt);

// The start of a refresh that began and has not recorded its end, kept on disk (<project>.srctrlst-start) so a
// refresh outliving its st process is still accounted for.
std::optional<TimeStamp> refreshInProgress(const ProjectSettings& settings);
void recordRefreshStart(const ProjectSettings& settings, const TimeStamp& startedAt, std::ostream& err = std::cerr);

// The start time to record for a refresh that begins now.
TimeStamp refreshStart();
}    // namespace st

#endif
