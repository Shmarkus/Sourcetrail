#ifndef SOURCE_GROUP_SCIP_H
#define SOURCE_GROUP_SCIP_H

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "IndexerCommandCustom.h"
#include "SourceGroup.h"

// A project of one language, indexed by the bundled SCIP indexer (Node.js) in one run over all of its files: the run
// appends to the index, so a refresh that re-runs it first clears everything the last run wrote (see getInputsRecord).
// Its one source file keys the run; its files in the index are the language's files below the project folder.
class SourceGroupScip : public SourceGroup {
public:
  bool isIndexedAsWhole() const override;
  std::string getInputsRecord() const override;

  std::set<FilePath> filterToContainedFilePaths(const std::set<FilePath>& filePaths) const override;
  std::set<FilePath> getAllSourceFilePaths() const override;
  std::vector<std::shared_ptr<IndexerCommand>> getIndexerCommands(const RefreshInfo& info) const override;

  // The folder the indexer reads, which the indexed files are below.
  virtual FilePath getScipProjectPath() const = 0;

protected:
  // Each trigger file below `projectPath`, outside the unwalked folders, with its size and modification time, hashed:
  // any edit, new or deleted file changes it. Each trigger file is handed to `visit` on the way.
  static std::string recordInputs(const FilePath& projectPath,
                                  const std::function<bool(const FilePath& file)>& isTriggerFile,
                                  const std::function<bool(const std::wstring& folderName)>& isUnwalkedDirectory,
                                  const std::function<void(const FilePath& triggerFile)>& visit);

  // The file the run is keyed on; while it is missing, the run is not started and reported like a failed one.
  virtual FilePath getSourceFilePath() const = 0;
  // The language in the status lines: "<label> skipped: …", "<label> failed (exit code <n>): …".
  virtual std::wstring getLabel() const = 0;
  // The indexer's exit codes for a project it does not apply to (a missing prerequisite).
  virtual std::set<int> getSkippedExitCodes() const = 0;
  // The arguments to node: the indexer, then its options; the output is %{DATABASE_FILE_PATH}, appended to.
  virtual std::vector<std::wstring> getIndexerArguments() const = 0;
  virtual bool isTriggerFile(const FilePath& filePath) const = 0;
  virtual bool isUnwalkedDirectory(const std::wstring& name) const = 0;
  // Whether the indexer writes this file to the index, if it is below the project folder and outside the excluded ones.
  virtual bool isIndexedFile(const FilePath& filePath) const = 0;
  virtual bool isExcludedDirectory(const std::wstring& name) const = 0;
  // Called once the run finished or was not started, after its inputs and failure were recorded.
  virtual std::function<void(IndexerCommandCustom::Result result)> getRunFinishedHandler() const;
};

#endif    // SOURCE_GROUP_SCIP_H
