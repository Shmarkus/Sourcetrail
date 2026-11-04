#include "SourceGroupTypeScript.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>

#include <system_error>

#include "FileSystem.h"
#include "IndexerCommandCustom.h"
#include "PersistentStorage.h"
#include "ProjectSettings.h"
#include "RefreshInfo.h"
#include "ResourcePaths.h"
#include "SourceGroupSettingsTypeScript.h"
#include "SqliteIndexStorage.h"
#include "utilityString.h"

namespace fs = std::filesystem;

namespace {
fs::path directory(const FilePath& path) {
  fs::path directory = fs::path(path.wstr()).lexically_normal();
  return directory.has_filename() ? directory : directory.parent_path();
}
}    // namespace

SourceGroupTypeScript::SourceGroupTypeScript(std::shared_ptr<SourceGroupSettingsTypeScript> settings)
    : m_settings(std::move(settings)) {}

bool SourceGroupTypeScript::isIndexedAsWhole() const {
  return true;
}

// Each trigger file below the TypeScript project with its size and modification time, hashed: any edit, new or
// deleted file changes it.
std::string SourceGroupTypeScript::getInputsRecord() const {
  return getInputsRecord(getTypeScriptProjectPath(), [](const FilePath&) {});
}

std::string SourceGroupTypeScript::getInputsRecord(const FilePath& projectPath,
                                                   const std::function<void(const FilePath& triggerFile)>& visit) {
  const fs::path root = directory(projectPath);
  std::vector<std::string> entries;
  std::error_code ec;
  for(fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
      it.increment(ec)) {
    if(it->is_directory(ec)) {
      if(SourceGroupSettingsTypeScript::isUnwalkedDirectory(it->path().filename().wstring())) {
        it.disable_recursion_pending();
      }
    } else if(it->is_regular_file(ec) && SourceGroupSettingsTypeScript::isTriggerFile(FilePath(it->path().wstring()))) {
      visit(FilePath(it->path().wstring()));
      const auto size = it->file_size(ec);
      const auto written = it->last_write_time(ec).time_since_epoch().count();
      entries.push_back(std::format("{}\t{}\t{}\n", it->path().lexically_relative(root).generic_string(), size, written));
    }
  }
  std::ranges::sort(entries);

  // FNV-1a
  std::uint64_t hash = 14695981039346656037ULL;
  for(const std::string& entry : entries) {
    for(const char c : entry) {
      hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
    }
  }
  return std::format("{} files {:016x}", entries.size(), hash);
}

std::set<FilePath> SourceGroupTypeScript::filterToContainedFilePaths(const std::set<FilePath>& filePaths) const {
  const FilePath root = getTypeScriptProjectPath();
  std::set<FilePath> containedFilePaths;
  for(const FilePath& filePath : filePaths) {
    const std::wstring extension = filePath.extension();
    if((extension != L".ts" && extension != L".tsx") || !root.contains(filePath)) {
      continue;
    }
    const fs::path relative = fs::path(filePath.wstr()).lexically_relative(directory(root));
    if(std::ranges::none_of(
           relative, [](const fs::path& part) { return SourceGroupSettingsTypeScript::isExcludedDirectory(part.wstring()); })) {
      containedFilePaths.insert(filePath);
    }
  }
  return containedFilePaths;
}

std::set<FilePath> SourceGroupTypeScript::getAllSourceFilePaths() const {
  return {m_settings->getTsconfigPathExpandedAndAbsolute()};
}

std::vector<std::shared_ptr<IndexerCommand>> SourceGroupTypeScript::getIndexerCommands(const RefreshInfo& info) const {
  const FilePath tsconfig = m_settings->getTsconfigPathExpandedAndAbsolute();
  const bool missing = info.wholeGroupsWithoutSources.contains(getId());
  if(!info.filesToIndex.contains(tsconfig) && !missing) {
    return {};
  }

  const ProjectSettings* projectSettings = m_settings->getProjectSettings();
  auto command = std::make_shared<IndexerCommandCustom>(
      L"node",
      std::vector<std::wstring>{ResourcePaths::getTypeScriptIndexerFilePath().wstr(),
                                L"--project",
                                getTypeScriptProjectPath().wstr(),
                                L"--tsconfig",
                                tsconfig.wstr(),
                                // Into the index Sourcetrail is building: it holds the other groups' data already.
                                L"--out",
                                L"%{DATABASE_FILE_PATH}",
                                L"--append",
                                L"--http-out",
                                getHttpCallsFilePath().wstr()},
      projectSettings->getProjectFilePath(),
      projectSettings->getTempDBFilePath(),
      std::to_wstring(SqliteIndexStorage::getStorageVersion()),
      tsconfig,
      false);
  command->setSkippedOn(L"TypeScript", {NodeModulesMissingExitCode, UnsupportedLayoutExitCode});
  const auto inputs = info.wholeGroupInputs.find(getId());
  if(missing) {
    // Recorded like a failed run: reported on every refresh, re-run once the inputs change (the tsconfig is back).
    command->setNotRun(tsconfig.fileName() + L" not found: " + tsconfig.wstr());
    command->setFinishedHandler([id = getId(),
                                 inputs = inputs != info.wholeGroupInputs.end() ? inputs->second : getInputsRecord(),
                                 httpCalls = getHttpCallsFilePath()](
                                    IndexerCommandCustom::Result, const std::wstring& status, PersistentStorage& storage) {
      storage.setSourceGroupInputs(id, inputs);
      storage.setSourceGroupFailure(id, utility::encodeToUtf8(status));
      FileSystem::remove(httpCalls);
    });
    return {command};
  }
  // Taken before the run, so that an edit during it is seen by the next refresh. A skipped run (a missing
  // prerequisite) is retried on every refresh, a failed one only once its inputs changed. Unless the run succeeded, no
  // HTTP calls sidecar is left (the indexer removes it first, but may not have started): nothing is linked from it.
  command->setFinishedHandler([id = getId(),
                               inputs = inputs != info.wholeGroupInputs.end() ? inputs->second : getInputsRecord(),
                               httpCalls = getHttpCallsFilePath()](
                                  IndexerCommandCustom::Result result, const std::wstring& status, PersistentStorage& storage) {
    storage.setSourceGroupInputs(id, result == IndexerCommandCustom::Result::Skipped ? std::string() : inputs);
    storage.setSourceGroupFailure(
        id, result == IndexerCommandCustom::Result::Failed ? utility::encodeToUtf8(status) : std::string());
    if(result != IndexerCommandCustom::Result::Succeeded) {
      FileSystem::remove(httpCalls);
    }
  });
  return {command};
}

std::shared_ptr<SourceGroupSettings> SourceGroupTypeScript::getSourceGroupSettings() {
  return m_settings;
}

std::shared_ptr<const SourceGroupSettings> SourceGroupTypeScript::getSourceGroupSettings() const {
  return m_settings;
}

FilePath SourceGroupTypeScript::getTypeScriptProjectPath() const {
  return m_settings->getTsconfigPathExpandedAndAbsolute().getParentDirectory();
}

FilePath SourceGroupTypeScript::getHttpCallsFilePath() const {
  return m_settings->getProjectSettings()->getProjectDirectoryPath().concatenate(L"ts-http-calls.json");
}
