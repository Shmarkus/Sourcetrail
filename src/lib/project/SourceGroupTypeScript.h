#ifndef SOURCE_GROUP_TYPESCRIPT_H
#define SOURCE_GROUP_TYPESCRIPT_H

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "SourceGroup.h"

class SourceGroupSettingsTypeScript;

// A TypeScript project, indexed by the bundled ts-indexer (Node.js) in one run over all of its files: the run appends
// to the index, so a refresh that re-runs it first clears everything the last run wrote (see getInputsRecord). Its one
// source file is the tsconfig; its files in the index are the .ts/.tsx files below the tsconfig's folder.
class SourceGroupTypeScript : public SourceGroup {
public:
  // ts-indexer's exit code for a project whose node_modules are missing, and for a layout it does not support.
  static constexpr int NodeModulesMissingExitCode = 3;
  static constexpr int UnsupportedLayoutExitCode = 4;

  explicit SourceGroupTypeScript(std::shared_ptr<SourceGroupSettingsTypeScript> settings);

  bool isIndexedAsWhole() const override;
  std::string getInputsRecord() const override;

  std::set<FilePath> filterToContainedFilePaths(const std::set<FilePath>& filePaths) const override;
  std::set<FilePath> getAllSourceFilePaths() const override;
  std::vector<std::shared_ptr<IndexerCommand>> getIndexerCommands(const RefreshInfo& info) const override;

private:
  std::shared_ptr<SourceGroupSettings> getSourceGroupSettings() override;
  std::shared_ptr<const SourceGroupSettings> getSourceGroupSettings() const override;

  FilePath getTypeScriptProjectPath() const;

  std::shared_ptr<SourceGroupSettingsTypeScript> m_settings;
};

#endif    // SOURCE_GROUP_TYPESCRIPT_H
