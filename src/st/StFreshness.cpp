#include "StFreshness.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iostream>
#include <set>

#include <boost/date_time/c_local_time_adjustor.hpp>

#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include "FileInfo.h"
#include "language_packages.h"
#include "PersistentStorage.h"
#include "ProjectSettings.h"
#include "SourceGroupPython.h"
#include "SourceGroupSettingsJavaGradle.h"
#include "SourceGroupSettingsJavaMaven.h"
#include "SourceGroupSettingsPython.h"
#include "SourceGroupSettingsTypeScript.h"
#include "SourceGroupTypeScript.h"
#include "utilityGradleCache.h"
#include "utilityMavenCache.h"
#include "utilityString.h"
#if BUILD_CXX_LANGUAGE_PACKAGE
#  include "SourceGroupSettingsCxxCdb.h"
#  include "SourceGroupSettingsWithSourcePaths.h"
#endif    // BUILD_CXX_LANGUAGE_PACKAGE

namespace st {
namespace {
QString refreshFile(const ProjectSettings& settings) {
  return QString::fromStdWString(settings.getFilePath().replaceExtension(L".srctrlst").wstr());
}

QString startFile(const ProjectSettings& settings) {
  return refreshFile(settings) + "-start";
}

// Sub-second, unlike FileSystem::getLastWriteTime: an edit and the refresh it triggers often share a second.
bool changedSince(const FilePath& path, const TimeStamp& indexedAt) {
  std::error_code ec;
  const auto written = std::filesystem::last_write_time(path.str(), ec);
  if(ec) {
    return true;
  }
  const auto utc = std::chrono::time_point_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now() +
      std::chrono::duration_cast<std::chrono::system_clock::duration>(written - std::filesystem::file_time_type::clock::now()));
  const auto seconds = std::chrono::floor<std::chrono::seconds>(utc);
  TimeStamp local(boost::date_time::c_local_adjustor<boost::posix_time::ptime>::utc_to_local(
      boost::posix_time::from_time_t(std::chrono::system_clock::to_time_t(seconds)) +
      boost::posix_time::microseconds((utc - seconds).count())));
  return local >= indexedAt;
}

// TimeStamp::toString drops the fraction of a second.
std::string toPreciseString(const TimeStamp& time) {
  const std::string seconds = time.toString();
  char fraction[8];
  std::snprintf(fraction, sizeof(fraction), ".%03zu", time.deltaMS(TimeStamp(seconds)) % 1000);
  return seconds + fraction;
}

// Changed source files, plus each changed directory without a changed file in it (a deletion or rename).
size_t changesBelow(const FilePath& root, const std::set<std::wstring>& extensions, const TimeStamp& indexedAt) {
  const QString rootPath = QString::fromStdWString(root.wstr());
  if(!QFileInfo(rootPath).isDir()) {
    return 0;
  }
  std::set<QString> changedDirectories;
  std::set<QString> directoriesWithChangedFiles;
  size_t changedFiles = 0;
  const auto visitDirectory = [&](const QString& path) {
    if(changedSince(FilePath(path.toStdWString()), indexedAt)) {
      changedDirectories.insert(path);
    }
  };
  visitDirectory(rootPath);
  QDirIterator it(rootPath, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
  while(it.hasNext()) {
    const QString path = it.next();
    const QFileInfo info = it.fileInfo();
    if(info.isDir()) {
      visitDirectory(path);
    } else if(extensions.count(utility::toLowerCase(FilePath(path.toStdWString()).extension())) &&
              changedSince(FilePath(path.toStdWString()), indexedAt)) {
      changedFiles++;
      directoriesWithChangedFiles.insert(info.absolutePath());
    }
  }
  for(const QString& directory : changedDirectories) {
    if(!directoriesWithChangedFiles.count(directory)) {
      changedFiles++;
    }
  }
  return changedFiles;
}

using InputsRecord = std::string (*)(const FilePath& root, const std::function<void(const FilePath& triggerFile)>& visit);

// A TypeScript or Python group is re-indexed as a whole when its inputs record changed, as the refresh decides; the
// changed trigger files are what st reports, at least one for a record that differs (a deleted file). Without a stored
// record (the last run was skipped) only the changed trigger files count. One walk over the project either way.
size_t wholeGroupChanges(const SourceGroupSettings& group,
                         const FilePath& root,
                         InputsRecord inputsRecord,
                         const TimeStamp& indexedAt,
                         const GroupInputs& inputs) {
  size_t changedFiles = 0;
  const std::string record = inputsRecord(
      root, [&](const FilePath& file) { changedFiles += changedSince(file, indexedAt) ? 1 : 0; });
  const auto stored = inputs.find(group.getId());
  if(stored == inputs.end() || stored->second.empty()) {
    return changedFiles;
  }
  return record == stored->second ? 0 : std::max<size_t>(1, changedFiles);
}
}    // namespace

GroupInputs storedGroupInputs(const ProjectSettings& settings, const PersistentStorage& storage) {
  GroupInputs inputs;
  for(const auto& group : settings.getAllSourceGroupSettings()) {
    if(std::dynamic_pointer_cast<SourceGroupSettingsTypeScript>(group) ||
       std::dynamic_pointer_cast<SourceGroupSettingsPython>(group)) {
      inputs[group->getId()] = storage.getSourceGroupInputs(group->getId());
    }
  }
  return inputs;
}

Freshness checkFreshness(const ProjectSettings& settings,
                         const TimeStamp& indexedAt,
                         const std::vector<FilePath>& indexedFiles,
                         const GroupInputs& inputs) {
  Freshness freshness;
  bool fallback = false;
  // C/C++ groups are refreshed per file, headers included: the indexed files are what they are checked by.
  bool cxx = false;
  std::vector<FilePath> typeScriptRoots;
  std::vector<FilePath> pythonRoots;
  const auto groups = settings.getAllSourceGroupSettings();
  // A refresh skips these, whatever changed in them.
  auto unsupported = SourceGroupSettingsTypeScript::getUnsupportedGroups(groups);
  for(const auto& group : SourceGroupSettingsPython::getUnsupportedGroups(groups)) {
    unsupported.push_back(group);
  }
  for(const auto& group : groups) {
    if(group->getStatus() != SOURCE_GROUP_STATUS_ENABLED || std::ranges::find(unsupported, group) != unsupported.end()) {
      continue;
    }
    if(const auto typeScript = std::dynamic_pointer_cast<SourceGroupSettingsTypeScript>(group)) {
      const FilePath root = typeScript->getTsconfigPathExpandedAndAbsolute().getParentDirectory();
      freshness.changedFiles += wholeGroupChanges(*group, root, &SourceGroupTypeScript::getInputsRecord, indexedAt, inputs);
      typeScriptRoots.push_back(root);
      continue;
    }
    if(const auto python = std::dynamic_pointer_cast<SourceGroupSettingsPython>(group)) {
      const FilePath root = python->getProjectRootExpandedAndAbsolute();
      freshness.changedFiles += wholeGroupChanges(*group, root, &SourceGroupPython::getInputsRecord, indexedAt, inputs);
      pythonRoots.push_back(root);
      continue;
    }
#if BUILD_CXX_LANGUAGE_PACKAGE
    if(group->getType() == SOURCE_GROUP_C_EMPTY || group->getType() == SOURCE_GROUP_CPP_EMPTY) {
      cxx = true;
      continue;
    }
    if(const auto cdb = std::dynamic_pointer_cast<SourceGroupSettingsCxxCdb>(group)) {
      // a refresh reads it again and indexes the source files it adds; a missing one is no change a refresh can index
      const FilePath database = cdb->getCompilationDatabasePathExpandedAndAbsolute();
      if(database.exists() && changedSince(database, indexedAt)) {
        freshness.changedFiles++;
      }
      cxx = true;
      continue;
    }
#endif    // BUILD_CXX_LANGUAGE_PACKAGE
    if(const auto maven = std::dynamic_pointer_cast<SourceGroupSettingsJavaMaven>(group)) {
      // the files the Maven cache is keyed on; a refresh runs Maven again when one changed
      const FilePath projectDirectory = maven->getMavenProjectFilePathExpandedAndAbsolute().getParentDirectory();
      for(const FilePath& buildFile :
          utility::mavenBuildFiles(projectDirectory, maven->getMavenSettingsFilePathExpandedAndAbsolute())) {
        if(changedSince(buildFile, indexedAt)) {
          freshness.changedFiles++;
        }
      }
      fallback = true;
      continue;
    }
    const auto gradle = std::dynamic_pointer_cast<SourceGroupSettingsJavaGradle>(group);
    if(!gradle) {
      fallback = true;
      continue;
    }
    const FilePath projectDirectory = gradle->getGradleProjectFilePathExpandedAndAbsolute().getParentDirectory();
    for(const FilePath& buildFile : utility::gradleBuildFiles(projectDirectory)) {
      if(changedSince(buildFile, indexedAt)) {
        freshness.changedFiles++;
      }
    }
    const auto info = utility::loadGradleProjectInfo(
        gradle->getGradleDependenciesDirectoryPath(), projectDirectory, gradle->getShouldIndexGradleTests());
    if(!info) {
      // Indexed files may be delombok copies that only change on refresh; a refresh re-queries Gradle.
      if(gradle->getGradleProjectFilePathExpandedAndAbsolute().exists()) {
        freshness.changedFiles++;
        freshness.reason = "Gradle source information missing or outdated; next query refreshes";
      } else {
        fallback = true;
      }
      continue;
    }
    std::set<std::wstring> extensions;
    for(const std::wstring& extension : gradle->getSourceExtensions()) {
      extensions.insert(utility::toLowerCase(extension));
    }
    for(const auto& sourceSet : info->sourceDirectories) {
      for(const FilePath& root : sourceSet.second) {
        freshness.changedFiles += changesBelow(root, extensions, indexedAt);
      }
    }
  }
  if(fallback || cxx) {
    if(fallback && freshness.reason.empty()) {
      freshness.reason = "no Gradle source roots cached; checked indexed files only";
    }
    for(const FilePath& file : indexedFiles) {
      // counted with their TypeScript or Python group
      const bool typeScript = SourceGroupSettingsTypeScript::isTriggerFile(file) &&
          std::ranges::any_of(typeScriptRoots, [&](const FilePath& root) { return root.contains(file); });
      const bool python = SourceGroupSettingsPython::isTriggerFile(file) &&
          std::ranges::any_of(pythonRoots, [&](const FilePath& root) { return root.contains(file); });
      if(!typeScript && !python && (!file.recheckExists() || changedSince(file, indexedAt))) {
        freshness.changedFiles++;
      }
    }
  }
  freshness.stale = freshness.changedFiles > 0;
  return freshness;
}

std::vector<std::string> mavenStatusRows(const ProjectSettings& settings) {
  std::vector<std::string> rows;
  for(const auto& group : settings.getAllSourceGroupSettings()) {
    const auto maven = std::dynamic_pointer_cast<SourceGroupSettingsJavaMaven>(group);
    if(!maven || group->getStatus() != SOURCE_GROUP_STATUS_ENABLED) {
      continue;
    }
    const std::optional<std::vector<std::wstring>> warnings = utility::loadMavenWarnings(maven->getMavenDependenciesDirectoryPath());
    if(!warnings) {
      rows.push_back("maven\tnot prepared; st index runs Maven");
    } else if(warnings->empty()) {
      rows.push_back("maven\tok");
    }
    for(const std::wstring& warning : warnings.value_or(std::vector<std::wstring>())) {
      rows.push_back("maven\t" + utility::encodeToUtf8(warning));
    }
  }
  return rows;
}

#if BUILD_CXX_LANGUAGE_PACKAGE
namespace {
// Whether a source file the compilation database names satisfies `test`, stopping at the first that does. A relative
// path counts from its entry's directory, a relative directory from the database's folder, as the indexer has them.
// Each entry costs a canonicalisation, so a large database is not walked further than needed.
bool anyDatabaseSourceFile(const FilePath& database, const std::function<bool(const FilePath&)>& test) {
  QFile file(QString::fromStdWString(database.wstr()));
  if(!file.open(QIODevice::ReadOnly)) {
    return false;
  }
  const QDir folder(QString::fromStdWString(database.getParentDirectory().wstr()));
  for(const QJsonValue& entry : QJsonDocument::fromJson(file.readAll()).array()) {
    const QDir directory(folder.absoluteFilePath(entry.toObject().value("directory").toString()));
    const QString path = QDir::cleanPath(directory.absoluteFilePath(entry.toObject().value("file").toString()));
    if(test(FilePath(path.toStdWString()).makeCanonical())) {
      return true;
    }
  }
  return false;
}

// Whether the index holds an indexed file of the group: a source file of its database, or one below its source paths.
bool hasIndexedFile(const SourceGroupSettings& group, const PersistentStorage& storage) {
  if(const auto* cdb = dynamic_cast<const SourceGroupSettingsCxxCdb*>(&group)) {
    return anyDatabaseSourceFile(cdb->getCompilationDatabasePathExpandedAndAbsolute(),
                                 [&storage](const FilePath& file) { return storage.getFilePathIndexed(file); });
  }
  const auto* sources = dynamic_cast<const SourceGroupSettingsWithSourcePaths*>(&group);
  if(!sources) {
    return false;
  }
  const std::vector<FilePath> roots = sources->getSourcePathsExpandedAndAbsolute();
  return std::ranges::any_of(storage.getFileInfoForAllFiles(), [&](const FileInfo& file) {
    return std::ranges::any_of(roots, [&file](const FilePath& root) { return root == file.path || root.contains(file.path); }) &&
        storage.getFilePathIndexed(file.path);
  });
}
}    // namespace
#endif    // BUILD_CXX_LANGUAGE_PACKAGE

std::vector<std::string> cxxStatusRows([[maybe_unused]] const ProjectSettings& settings,
                                       [[maybe_unused]] const PersistentStorage& storage) {
  std::vector<std::string> rows;
#if BUILD_CXX_LANGUAGE_PACKAGE
  for(const auto& group : settings.getAllSourceGroupSettings()) {
    const SourceGroupType type = group->getType();
    if(group->getStatus() != SOURCE_GROUP_STATUS_ENABLED ||
       (type != SOURCE_GROUP_CXX_CDB && type != SOURCE_GROUP_C_EMPTY && type != SOURCE_GROUP_CPP_EMPTY)) {
      continue;
    }
    const auto cdb = std::dynamic_pointer_cast<SourceGroupSettingsCxxCdb>(group);
    if(const FilePath database = cdb ? cdb->getCompilationDatabasePathExpandedAndAbsolute() : FilePath();
       cdb && !database.exists()) {
      rows.push_back("cxx\tbroken: compilation database not found: " + database.str());
    } else {
      rows.push_back(hasIndexedFile(*group, storage) ? "cxx\tindexed" : "cxx\tnot indexed; st index shows why");
    }
  }
#endif    // BUILD_CXX_LANGUAGE_PACKAGE
  return rows;
}

std::optional<RefreshRecord> lastRefresh(const ProjectSettings& settings) {
  QFile file(refreshFile(settings));
  if(!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return std::nullopt;
  }
  const QStringList lines = QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
  if(lines.size() != 2) {
    return std::nullopt;
  }
  try {
    RefreshRecord record{TimeStamp(lines[0].trimmed().toStdString()), TimeStamp(lines[1].trimmed().toStdString())};
    if(!record.startedAt.isValid() || !record.indexedAt.isValid()) {
      return std::nullopt;
    }
    return record;
  } catch(const std::exception&) {
    return std::nullopt;
  }
}

void recordRefresh(const ProjectSettings& settings, const RefreshRecord& record, std::ostream& err) {
  QSaveFile file(refreshFile(settings));
  const std::string text = toPreciseString(record.startedAt) + "\n" + record.indexedAt.toString() + "\n";
  if(!file.open(QIODevice::WriteOnly | QIODevice::Text) || file.write(text.c_str()) < 0 || !file.commit()) {
    err << "st: cannot write " << file.fileName().toStdString() << '\n';
    return;
  }
  // This refresh saw every edit made before it started, so an unfinished one that started no later is covered.
  if(std::optional<TimeStamp> inProgress = refreshInProgress(settings)) {
    TimeStamp startedAt = record.startedAt;
    if(*inProgress <= startedAt) {
      QFile::remove(startFile(settings));
    }
  }
}

std::optional<TimeStamp> refreshInProgress(const ProjectSettings& settings) {
  QFile file(startFile(settings));
  if(!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return std::nullopt;
  }
  try {
    const TimeStamp startedAt(QString::fromUtf8(file.readAll()).trimmed().toStdString());
    return startedAt.isValid() ? std::optional<TimeStamp>(startedAt) : std::nullopt;
  } catch(const std::exception&) {
    return std::nullopt;
  }
}

void recordRefreshStart(const ProjectSettings& settings, const TimeStamp& startedAt, std::ostream& err) {
  if(std::optional<TimeStamp> earlier = refreshInProgress(settings)) {
    TimeStamp start = startedAt;    // TimeStamp compares only non-const
    if(*earlier <= start) {
      // An earlier refresh has not recorded its end: its start stays the conservative one.
      return;
    }
  }
  QSaveFile file(startFile(settings));
  const std::string text = toPreciseString(startedAt) + "\n";
  if(!file.open(QIODevice::WriteOnly | QIODevice::Text) || file.write(text.c_str()) < 0 || !file.commit()) {
    err << "st: cannot write " << file.fileName().toStdString() << '\n';
  }
}

TimeStamp freshnessBaseline(const TimeStamp& indexedAt,
                            const std::optional<RefreshRecord>& last,
                            const std::optional<TimeStamp>& inProgress) {
  if(last && last->indexedAt.toString() == indexedAt.toString()) {
    return last->startedAt;
  }
  // A refresh started and never recorded its end, e.g. one left running by a server that stopped: the index it
  // may have written misses edits made during it.
  if(inProgress) {
    TimeStamp started = *inProgress;    // TimeStamp compares only non-const
    if(started < indexedAt) {
      return started;
    }
  }
  return indexedAt;
}

TimeStamp refreshStart() {
  // File times come from a coarse kernel clock that can trail the precise one by a tick: a file
  // written right after this call may carry an earlier time.
  return TimeStamp(boost::posix_time::microsec_clock::local_time() - boost::posix_time::milliseconds(20));
}
}    // namespace st
