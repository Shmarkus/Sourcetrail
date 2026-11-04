#ifndef SOURCE_GROUP_TYPESCRIPT_H
#define SOURCE_GROUP_TYPESCRIPT_H

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "SourceGroupScip.h"

class SourceGroupSettingsTypeScript;

// A TypeScript project, indexed as a whole by the bundled ts-indexer (see SourceGroupScip). Its one source file is the
// tsconfig; its files in the index are the .ts/.tsx files below the tsconfig's folder.
class SourceGroupTypeScript : public SourceGroupScip {
public:
  // ts-indexer's exit code for a project whose node_modules are missing, and for a layout it does not support.
  static constexpr int NodeModulesMissingExitCode = 3;
  static constexpr int UnsupportedLayoutExitCode = 4;

  explicit SourceGroupTypeScript(std::shared_ptr<SourceGroupSettingsTypeScript> settings);

  using SourceGroupScip::getInputsRecord;
  // The inputs record of the TypeScript project in `root`, handing each trigger file to `visit` on the way.
  static std::string getInputsRecord(const FilePath& root, const std::function<void(const FilePath& triggerFile)>& visit);

  FilePath getScipProjectPath() const override;
  // The folder of the tsconfig, which the sidecar's file paths are relative to.
  FilePath getTypeScriptProjectPath() const;
  // The HttpClient calls of the last successful run, next to the project file, for the endpoint linking.
  FilePath getHttpCallsFilePath() const;

protected:
  FilePath getSourceFilePath() const override;
  std::wstring getLabel() const override;
  std::set<int> getSkippedExitCodes() const override;
  std::vector<std::wstring> getIndexerArguments() const override;
  bool isTriggerFile(const FilePath& filePath) const override;
  bool isUnwalkedDirectory(const std::wstring& name) const override;
  bool isIndexedFile(const FilePath& filePath) const override;
  bool isExcludedDirectory(const std::wstring& name) const override;
  // Unless the run succeeded, no HTTP calls sidecar is left (the indexer removes it first, but may not have started):
  // nothing is linked from it.
  std::function<void(IndexerCommandCustom::Result result)> getRunFinishedHandler() const override;

private:
  std::shared_ptr<SourceGroupSettings> getSourceGroupSettings() override;
  std::shared_ptr<const SourceGroupSettings> getSourceGroupSettings() const override;

  std::shared_ptr<SourceGroupSettingsTypeScript> m_settings;
};

#endif    // SOURCE_GROUP_TYPESCRIPT_H
