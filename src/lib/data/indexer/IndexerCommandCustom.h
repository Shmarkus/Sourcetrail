#ifndef INDEXER_COMMAND_CUSTOM_H
#define INDEXER_COMMAND_CUSTOM_H

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "IndexerCommand.h"

class PersistentStorage;

class IndexerCommandCustom : public IndexerCommand {
public:
  static IndexerCommandType getStaticIndexerCommandType();

  IndexerCommandCustom(const std::wstring& command,
                       const std::vector<std::wstring>& arguments,
                       const FilePath& projectFilePath,
                       const FilePath& databaseFilePath,
                       const std::wstring& databaseVersion,
                       const FilePath& sourceFilePath,
                       bool runInParallel);

  IndexerCommandCustom(IndexerCommandType type,
                       const std::wstring& command,
                       const std::vector<std::wstring>& arguments,
                       const FilePath& projectFilePath,
                       const FilePath& databaseFilePath,
                       const std::wstring& databaseVersion,
                       const FilePath& sourceFilePath,
                       bool runInParallel);

  IndexerCommandType getIndexerCommandType() const override;
  size_t getByteSize(size_t stringSize) const override;

  FilePath getDatabaseFilePath() const;
  void setDatabaseFilePath(const FilePath& databaseFilePath);

  std::wstring getCommand() const;
  std::vector<std::wstring> getArguments() const;
  bool getRunInParallel() const;

  // A command that exits with one of `exitCodes`, or cannot be started, did not apply to the project (a missing
  // prerequisite, an unsupported layout): it is reported as "<label> skipped: <message>", not as a failed command.
  // Its other failures are reported as "<label> failed (exit code <n>): <its output>".
  void setSkippedOn(const std::wstring& label, const std::set<int>& exitCodes);
  bool isSkippedOn(int exitCode, bool started) const;
  const std::wstring& getLabel() const;

  enum class Result { Succeeded, Skipped, Failed };
  // Called once the command ran, with its result, the status message shown for it (empty if it succeeded) and the
  // storage it wrote to.
  using FinishedHandler = std::function<void(Result result, const std::wstring& status, PersistentStorage& storage)>;
  void setFinishedHandler(FinishedHandler handler);
  void finished(Result result, const std::wstring& status, PersistentStorage& storage) const;

protected:
  QJsonObject doSerialize() const override;

private:
  std::wstring replaceVariables(std::wstring s) const;

  IndexerCommandType m_type;
  std::wstring m_command;
  std::vector<std::wstring> m_arguments;
  FilePath m_projectFilePath;
  FilePath m_databaseFilePath;
  std::wstring m_databaseVersion;
  bool m_runInParallel;
  std::wstring m_skippedLabel;
  std::set<int> m_skippedExitCodes;
  FinishedHandler m_finishedHandler;
};

#endif    // INDEXER_COMMAND_CXXL_H
