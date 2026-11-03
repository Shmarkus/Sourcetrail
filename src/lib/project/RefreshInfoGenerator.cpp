#include "RefreshInfoGenerator.h"

#include <algorithm>

#include "FileInfo.h"
#include "FileSystem.h"
#include "PersistentStorage.h"
#include "RefreshInfo.h"
#include "SourceGroup.h"
#include "SourceGroupStatusType.h"
#include "TextAccess.h"
#include "utility.h"

namespace {
std::set<FilePath> subtract(std::set<FilePath> paths, const std::set<FilePath>& removedPaths) {
  for(const FilePath& path : removedPaths) {
    paths.erase(path);
  }
  return paths;
}

// A group indexed as a whole (see SourceGroup::getInputsRecord) is cleared and indexed in full when its inputs changed
// or any of its files is to be cleared, and left alone otherwise. Its files never take part in the second pass that
// compares declarations: they are not indexed one by one.
void applyGroupsIndexedAsWhole(RefreshInfo& info,
                               const std::vector<std::shared_ptr<SourceGroup>>& sourceGroups,
                               const std::shared_ptr<const PersistentStorage>& storage) {
  std::vector<std::pair<std::shared_ptr<SourceGroup>, std::string>> wholeGroups;
  for(const std::shared_ptr<SourceGroup>& sourceGroup : sourceGroups) {
    if(sourceGroup->getStatus() == SOURCE_GROUP_STATUS_ENABLED) {
      if(std::string inputs = sourceGroup->getInputsRecord(); !inputs.empty()) {
        wholeGroups.emplace_back(sourceGroup, std::move(inputs));
      }
    }
  }
  if(wholeGroups.empty()) {
    return;
  }

  std::set<FilePath> storedFilePaths;
  for(const FileInfo& fileInfo : storage->getFileInfoForAllFiles()) {
    storedFilePaths.insert(fileInfo.path);
  }

  for(const auto& [sourceGroup, inputs] : wholeGroups) {
    const std::set<FilePath> groupFilePaths = sourceGroup->filterToContainedFilePaths(storedFilePaths);
    const std::set<FilePath> sourceFilePaths = sourceGroup->getAllSourceFilePaths();
    const auto toClear = [&info](const FilePath& path) {
      return info.filesToClear.contains(path) || info.nonIndexedFilesToClear.contains(path);
    };
    const bool changed = inputs != storage->getSourceGroupInputs(sourceGroup->getId()) ||
        std::ranges::any_of(groupFilePaths, toClear);

    info.filesToCompareDeclarations = subtract(info.filesToCompareDeclarations, groupFilePaths);
    info.dependentFilesToClear = subtract(info.dependentFilesToClear, groupFilePaths);
    info.dependentFilesToIndex = subtract(subtract(info.dependentFilesToIndex, groupFilePaths), sourceFilePaths);
    if(changed) {
      for(const FilePath& path : groupFilePaths) {
        if(storage->getFilePathIndexed(path)) {
          info.filesToClear.insert(path);
        } else {
          info.nonIndexedFilesToClear.insert(path);
        }
      }
      for(const FilePath& path : sourceFilePaths) {
        if(path.exists()) {
          info.filesToIndex.insert(path);
        }
      }
    } else {
      info.filesToClear = subtract(info.filesToClear, groupFilePaths);
      info.nonIndexedFilesToClear = subtract(info.nonIndexedFilesToClear, groupFilePaths);
      info.filesToIndex = subtract(info.filesToIndex, sourceFilePaths);
    }
  }

  if(info.dependentFilesToClear.empty()) {
    info.filesToCompareDeclarations.clear();
    info.dependentFilesToIndex.clear();
  }
}
}    // namespace

RefreshInfo RefreshInfoGenerator::getRefreshInfoForUpdatedFiles(const std::vector<std::shared_ptr<SourceGroup>>& sourceGroups,
                                                                std::shared_ptr<const PersistentStorage> storage) {
  // 1) Divide filepaths that are already known by the storage to "unchanged and indexed",
  // "unchanged and non-indexed" and "changed"
  std::set<FilePath> unchangedIndexedFilePaths;
  std::set<FilePath> unchangedNonindexedFilePaths;
  std::set<FilePath> changedFilePaths;
  // indexed files whose content changed and that are still part of the project
  std::set<FilePath> editedFilePaths;

  {
    const std::vector<FileInfo> fileInfosFromStorage = storage->getFileInfoForAllFiles();

    std::set<FilePath> alreadyKnownPaths;
    {
      const std::set<FilePath> filePathsFromStorage = utility::toSet(
          utility::convert<FileInfo, FilePath>(fileInfosFromStorage, [](const FileInfo& info) { return info.path; }));

      for(const std::shared_ptr<SourceGroup>& sourceGroup : sourceGroups) {
        if(sourceGroup->getStatus() == SOURCE_GROUP_STATUS_ENABLED) {
          utility::append(alreadyKnownPaths, sourceGroup->filterToContainedFilePaths(filePathsFromStorage));
        }
      }
    }

    // checking source and header files
    for(const FileInfo& info : fileInfosFromStorage) {
      if(alreadyKnownPaths.find(info.path) != alreadyKnownPaths.end() && info.path.exists()) {
        if(storage->getFilePathIndexed(info.path)) {
          if(didFileChange(info, storage)) {
            changedFilePaths.insert(info.path);
            editedFilePaths.insert(info.path);
          } else {
            unchangedIndexedFilePaths.insert(info.path);
          }
        } else {
          changedFilePaths.insert(info.path);
        }
      } else if(!storage->getFilePathIndexed(info.path) && !didFileChange(info, storage)) {
        unchangedNonindexedFilePaths.insert(info.path);
      } else    // file has been removed
      {
        changedFilePaths.insert(info.path);
      }
    }
  }

  const std::set<FilePath> allSourceFilePathsFromSourcegroups = getAllSourceFilePaths(sourceGroups);

  // 2) Figure out which files need to be cleared
  // 2.1) Add all changed files
  std::set<FilePath> filesToClear = changedFilePaths;

  // 2.2) Add files that are reference the changed files. Files that only resolve against what an
  // edited file declares are deferred: they need re-indexing only if those declarations change.
  utility::append(filesToClear, storage->getReferencingByIncludes(changedFilePaths));
  utility::append(filesToClear, storage->getReferencingByDeclarations(subtract(changedFilePaths, editedFilePaths)));
  const std::set<FilePath> referencingEditedFilePaths = storage->getReferencingByDeclarations(editedFilePaths);

  // 2.3) Handle files that are referenced by the files that will be cleared. These will be
  // re-indexed on the fly. However, we do not
  //		need to clear files that are also referenced by unchanged source files, because
  // otherwise we will lose these connections.
  // 2.3.1) Get all source file paths that will not be cleared.
  // - Initially this list contains all source file paths the project would index right now.
  // - Then we remove all source files that will be cleared
  // - NOTE: Source files that are new to the project will part of this list, but won't result in
  // any referenced
  //   paths because they are not part of the DB. Source files that are new to the project but are
  //   already in the DB will be removed from this list if they have changed or reference changed
  //   files.
  std::set<FilePath> staticSourceFiles = allSourceFilePathsFromSourcegroups;
  for(const FilePath& path : filesToClear) {
    staticSourceFiles.erase(path);
  }

  // 2.3.2) Get sets of referenced files
  const std::set<FilePath> staticReferencedFilePaths = storage->getReferenced(staticSourceFiles);
  const std::set<FilePath> dynamicReferencedFilePaths = storage->getReferenced(filesToClear);

  // 2.3.3) Add "dynamicReferencedFilePaths" to "filesToClear" that are not referenced by static
  // paths, because these files may not be
  //        referenced anymore. If they still are, they will be re-added when encountered during
  //        re-indexing.
  for(const FilePath& path : dynamicReferencedFilePaths) {
    if(staticReferencedFilePaths.find(path) == staticReferencedFilePaths.end() &&
       staticSourceFiles.find(path) == staticSourceFiles.end()) {
      filesToClear.insert(path);
    }
  }

  // 3) Figure out which files need to be indexed
  std::set<FilePath> filesToIndex;
  for(const FilePath& path : allSourceFilePathsFromSourcegroups) {
    if(filesToClear.find(path) != filesToClear.end() ||                            // file will be cleared
       unchangedIndexedFilePaths.find(path) == unchangedIndexedFilePaths.end())    // file has been changed or added
    {
      filesToIndex.insert(path);
    }
  }

  // 4) Store and return this information
  RefreshInfo info;
  info.mode = RefreshMode::UpdatedFiles;
  info.filesToIndex = filesToIndex;
  const std::set<FilePath> dependentFilePaths = subtract(referencingEditedFilePaths, filesToClear);
  if(!dependentFilePaths.empty()) {
    // Unlike in 2.3, files referenced by the dependent files need no clearing: dependent files are
    // unchanged, so they still reference them.
    info.filesToCompareDeclarations = editedFilePaths;
    info.dependentFilesToClear = dependentFilePaths;
    for(const FilePath& path : dependentFilePaths) {
      if(allSourceFilePathsFromSourcegroups.find(path) != allSourceFilePathsFromSourcegroups.end()) {
        info.dependentFilesToIndex.insert(path);
      }
    }
  }
  for(const FilePath& fileToClear : filesToClear) {
    if(storage->getFilePathIndexed(fileToClear)) {
      info.filesToClear.insert(fileToClear);
    } else {
      info.nonIndexedFilesToClear.insert(fileToClear);
    }
  }

  applyGroupsIndexedAsWhole(info, sourceGroups, storage);
  return info;
}

RefreshInfo RefreshInfoGenerator::getRefreshInfoForIncompleteFiles(const std::vector<std::shared_ptr<SourceGroup>>& sourceGroups,
                                                                   std::shared_ptr<const PersistentStorage> storage) {
  RefreshInfo info = getRefreshInfoForUpdatedFiles(sourceGroups, storage);
  info.mode = RefreshMode::UpdatedAndIncompleteFiles;

  std::set<FilePath> incompleteFiles;
  {
    const std::set<FilePath> filesToClear = utility::concat(info.filesToClear, info.nonIndexedFilesToClear);
    for(const FilePath& path : storage->getIncompleteFiles()) {
      if(filesToClear.find(path) == filesToClear.end()) {
        incompleteFiles.insert(path);
      }
    }
  }

  if(!incompleteFiles.empty()) {
    utility::append(incompleteFiles, storage->getReferencing(incompleteFiles));

    std::set<FilePath> staticSourceFilePaths = getAllSourceFilePaths(sourceGroups);
    for(const FilePath& path : incompleteFiles) {
      staticSourceFilePaths.erase(path);

      if(storage->getFilePathIndexed(path)) {
        info.filesToClear.insert(path);
      } else {
        info.nonIndexedFilesToClear.insert(path);
      }
    }

    for(const auto& sourceGroup : sourceGroups) {
      if(sourceGroup->getStatus() == SOURCE_GROUP_STATUS_ENABLED) {
        utility::append(info.filesToIndex, sourceGroup->filterToContainedSourceFilePath(staticSourceFilePaths));
      }
    }

    info.dependentFilesToClear = subtract(info.dependentFilesToClear, incompleteFiles);
    info.dependentFilesToIndex = subtract(info.dependentFilesToIndex, info.filesToIndex);
    if(info.dependentFilesToClear.empty()) {
      info.filesToCompareDeclarations.clear();
    }
  }

  applyGroupsIndexedAsWhole(info, sourceGroups, storage);
  return info;
}

RefreshInfo RefreshInfoGenerator::getRefreshInfoForAllFiles(const std::vector<std::shared_ptr<SourceGroup>>& sourceGroups) {
  RefreshInfo info;
  info.mode = RefreshMode::AllFiles;
  info.filesToIndex = getAllSourceFilePaths(sourceGroups);
  return info;
}

std::set<FilePath> RefreshInfoGenerator::getAllSourceFilePaths(const std::vector<std::shared_ptr<SourceGroup>>& sourceGroups) {
  std::set<FilePath> allSourceFilePaths;

  for(const auto& sourceGroup : sourceGroups) {
    if(sourceGroup->getStatus() == SOURCE_GROUP_STATUS_ENABLED) {
      for(const FilePath& sourceFilePath : sourceGroup->getAllSourceFilePaths()) {
        if(sourceFilePath.exists()) {
          allSourceFilePaths.insert(sourceFilePath);
        }
      }
    }
  }

  return allSourceFilePaths;
}

bool RefreshInfoGenerator::didFileChange(const FileInfo& info, std::shared_ptr<const PersistentStorage> storage) {
  FileInfo diskFileInfo = FileSystem::getFileInfoForPath(info.path);
  if(diskFileInfo.lastWriteTime > info.lastWriteTime) {
    if(!storage->hasContentForFile(info.path)) {
      return true;
    }

    std::shared_ptr<TextAccess> storedFileContent = storage->getFileContent(info.path, false);
    std::shared_ptr<TextAccess> diskFileContent = TextAccess::createFromFile(diskFileInfo.path);

    const std::vector<std::string>& diskFileLines = diskFileContent->getAllLines();
    const std::vector<std::string>& storedFileLines = storedFileContent->getAllLines();

    if(diskFileLines.size() != storedFileLines.size()) {
      return true;
    }

    for(size_t i = 0; i < diskFileLines.size(); i++) {
      if(diskFileLines[i] != storedFileLines[i]) {
        return true;
      }
    }
    return false;
  }
  return false;
}