#pragma once
// STL
#include <memory>
#include <set>
#include <string>
#include <vector>
// internal
#include "LanguageType.h"
#include "SourceGroupStatusType.h"
#include "SourceGroupType.h"

class DialogView;
class FilePath;
class FilePathFilter;
class IndexerCommand;
class IndexerCommandProvider;
class SourceGroupSettings;
class StorageProvider;
class Task;

struct RefreshInfo;

class SourceGroup {
public:
  virtual ~SourceGroup();

  virtual bool prepareIndexing(bool fullRefresh);
  virtual bool allowsPartialClearing() const;
  virtual bool allowsShallowIndexing() const;

  // A group indexed by one command over all of its files at once, the TypeScript group, returns a record of the inputs
  // that command reads; the index stores it once the command succeeded. A refresh clears and re-imports the whole group
  // when the record differs from the stored one or the refresh clears any of the group's files, and touches none of
  // its files otherwise. Empty for groups indexed file by file.
  virtual std::string getInputsRecord() const;

  virtual std::set<FilePath> filterToContainedFilePaths(const std::set<FilePath>& filePaths) const = 0;
  virtual std::set<FilePath> getAllSourceFilePaths() const = 0;
  virtual std::shared_ptr<IndexerCommandProvider> getIndexerCommandProvider(const RefreshInfo& info) const;
  virtual std::vector<std::shared_ptr<IndexerCommand>> getIndexerCommands(const RefreshInfo& info) const = 0;
  virtual std::shared_ptr<Task> getPreIndexTask(std::shared_ptr<StorageProvider> storageProvider,
                                                std::shared_ptr<DialogView> dialogView) const;

  std::string getId() const;
  SourceGroupType getType() const;
  LanguageType getLanguage() const;
  SourceGroupStatusType getStatus() const;
  std::set<FilePath> filterToContainedSourceFilePath(const std::set<FilePath>& staticSourceFilePaths) const;
  bool containsSourceFilePath(const FilePath& sourceFilePath) const;

protected:
  virtual std::shared_ptr<SourceGroupSettings> getSourceGroupSettings() = 0;
  virtual std::shared_ptr<const SourceGroupSettings> getSourceGroupSettings() const = 0;

  std::set<FilePath> filterToContainedFilePaths(const std::set<FilePath>& filePaths,
                                                const std::set<FilePath>& indexedFilePaths,
                                                const std::set<FilePath>& indexedFileOrDirectoryPaths,
                                                const std::vector<FilePathFilter>& excludeFilters) const;
};