#ifndef SOURCE_GROUP_PYTHON_H
#define SOURCE_GROUP_PYTHON_H

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "SourceGroupScip.h"

class SourceGroupSettingsPython;

// A Python project, indexed as a whole by the bundled SCIP indexer with scip-python (see SourceGroupScip). Its one
// source file is its root folder; its files in the index are the .py files below it, outside environments, build
// output and caches.
class SourceGroupPython : public SourceGroupScip {
public:
  // The indexer's exit code when Node is too old.
  static constexpr int PrerequisiteMissingExitCode = 3;

  explicit SourceGroupPython(std::shared_ptr<SourceGroupSettingsPython> settings);

  using SourceGroupScip::getInputsRecord;
  // The inputs record of the Python project in `root`, handing each trigger file to `visit` on the way.
  static std::string getInputsRecord(const FilePath& root, const std::function<void(const FilePath& triggerFile)>& visit);

  FilePath getScipProjectPath() const override;

protected:
  FilePath getSourceFilePath() const override;
  std::wstring getLabel() const override;
  std::set<int> getSkippedExitCodes() const override;
  std::vector<std::wstring> getIndexerArguments() const override;
  bool isTriggerFile(const FilePath& filePath) const override;
  bool isUnwalkedDirectory(const std::wstring& name) const override;
  bool isIndexedFile(const FilePath& filePath) const override;
  bool isExcludedDirectory(const std::wstring& name) const override;

private:
  std::shared_ptr<SourceGroupSettings> getSourceGroupSettings() override;
  std::shared_ptr<const SourceGroupSettings> getSourceGroupSettings() const override;

  std::shared_ptr<SourceGroupSettingsPython> m_settings;
};

#endif    // SOURCE_GROUP_PYTHON_H
