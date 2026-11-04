#include "SourceGroupScip.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>

#include <system_error>

#include "PersistentStorage.h"
#include "ProjectSettings.h"
#include "RefreshInfo.h"
#include "SourceGroupSettings.h"
#include "SqliteIndexStorage.h"
#include "utilityString.h"

namespace fs = std::filesystem;

namespace {
fs::path directory(const FilePath& path) {
  fs::path directory = fs::path(path.wstr()).lexically_normal();
  return directory.has_filename() ? directory : directory.parent_path();
}
}    // namespace

bool SourceGroupScip::isIndexedAsWhole() const {
  return true;
}

std::string SourceGroupScip::getInputsRecord() const {
  return recordInputs(
      getScipProjectPath(),
      [this](const FilePath& file) { return isTriggerFile(file); },
      [this](const std::wstring& name) { return isUnwalkedDirectory(name); },
      [](const FilePath&) {});
}

std::string SourceGroupScip::recordInputs(const FilePath& projectPath,
                                          const std::function<bool(const FilePath& file)>& isTriggerFile,
                                          const std::function<bool(const std::wstring& folderName)>& isUnwalkedDirectory,
                                          const std::function<void(const FilePath& triggerFile)>& visit) {
  const fs::path root = directory(projectPath);
  std::vector<std::string> entries;
  std::error_code ec;
  for(fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
      it.increment(ec)) {
    if(it->is_directory(ec)) {
      if(isUnwalkedDirectory(it->path().filename().wstring())) {
        it.disable_recursion_pending();
      }
    } else if(it->is_regular_file(ec) && isTriggerFile(FilePath(it->path().wstring()))) {
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

std::set<FilePath> SourceGroupScip::filterToContainedFilePaths(const std::set<FilePath>& filePaths) const {
  const FilePath root = getScipProjectPath();
  std::set<FilePath> containedFilePaths;
  for(const FilePath& filePath : filePaths) {
    if(!isIndexedFile(filePath) || !root.contains(filePath)) {
      continue;
    }
    const fs::path relative = fs::path(filePath.wstr()).lexically_relative(directory(root));
    if(std::ranges::none_of(relative, [this](const fs::path& part) { return isExcludedDirectory(part.wstring()); })) {
      containedFilePaths.insert(filePath);
    }
  }
  return containedFilePaths;
}

std::set<FilePath> SourceGroupScip::getAllSourceFilePaths() const {
  return {getSourceFilePath()};
}

std::vector<std::shared_ptr<IndexerCommand>> SourceGroupScip::getIndexerCommands(const RefreshInfo& info) const {
  const FilePath sourceFile = getSourceFilePath();
  const bool missing = info.wholeGroupsWithoutSources.contains(getId());
  if(!info.filesToIndex.contains(sourceFile) && !missing) {
    return {};
  }

  const ProjectSettings* projectSettings = getSourceGroupSettings()->getProjectSettings();
  auto command = std::make_shared<IndexerCommandCustom>(L"node",
                                                        getIndexerArguments(),
                                                        projectSettings->getProjectFilePath(),
                                                        projectSettings->getTempDBFilePath(),
                                                        std::to_wstring(SqliteIndexStorage::getStorageVersion()),
                                                        sourceFile,
                                                        false);
  command->setSkippedOn(getLabel(), getSkippedExitCodes());
  const auto inputs = info.wholeGroupInputs.find(getId());
  const std::string inputsRecord = inputs != info.wholeGroupInputs.end() ? inputs->second : getInputsRecord();
  if(missing) {
    // Recorded like a failed run: reported on every refresh, re-run once the inputs change (the source file is back).
    command->setNotRun(sourceFile.fileName() + L" not found: " + sourceFile.wstr());
    command->setFinishedHandler([id = getId(), inputs = inputsRecord, finished = getRunFinishedHandler()](
                                    IndexerCommandCustom::Result result, const std::wstring& status, PersistentStorage& storage) {
      storage.setSourceGroupInputs(id, inputs);
      storage.setSourceGroupFailure(id, utility::encodeToUtf8(status));
      finished(result);
    });
    return {command};
  }
  // Taken before the run, so that an edit during it is seen by the next refresh. A skipped run (a missing
  // prerequisite) is retried on every refresh, a failed one only once its inputs changed.
  command->setFinishedHandler([id = getId(), inputs = inputsRecord, finished = getRunFinishedHandler()](
                                  IndexerCommandCustom::Result result, const std::wstring& status, PersistentStorage& storage) {
    storage.setSourceGroupInputs(id, result == IndexerCommandCustom::Result::Skipped ? std::string() : inputs);
    storage.setSourceGroupFailure(
        id, result == IndexerCommandCustom::Result::Failed ? utility::encodeToUtf8(status) : std::string());
    finished(result);
  });
  return {command};
}

std::function<void(IndexerCommandCustom::Result result)> SourceGroupScip::getRunFinishedHandler() const {
  return [](IndexerCommandCustom::Result) {};
}
