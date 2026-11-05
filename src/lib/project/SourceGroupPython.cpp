#include "SourceGroupPython.h"

#include "ResourcePaths.h"
#include "SourceGroupSettingsPython.h"

SourceGroupPython::SourceGroupPython(std::shared_ptr<SourceGroupSettingsPython> settings) : m_settings(std::move(settings)) {}

std::string SourceGroupPython::getInputsRecord(const FilePath& projectPath,
                                               const std::function<void(const FilePath& triggerFile)>& visit) {
  return recordInputs(
      projectPath, &SourceGroupSettingsPython::isTriggerFile, &SourceGroupSettingsPython::isUnwalkedDirectory, visit);
}

FilePath SourceGroupPython::getScipProjectPath() const {
  return m_settings->getProjectRootExpandedAndAbsolute();
}

// The root folder: a Python project need not have a project file.
FilePath SourceGroupPython::getSourceFilePath() const {
  return getScipProjectPath();
}

std::wstring SourceGroupPython::getLabel() const {
  return L"Python";
}

std::set<int> SourceGroupPython::getSkippedExitCodes() const {
  return {PrerequisiteMissingExitCode};
}

std::vector<std::wstring> SourceGroupPython::getIndexerArguments() const {
  std::vector<std::wstring> arguments{ResourcePaths::getTypeScriptIndexerFilePath().wstr(),
                                      L"--language",
                                      L"python",
                                      L"--project",
                                      getScipProjectPath().wstr(),
                                      // Into the index Sourcetrail is building: it holds the other groups' data already.
                                      L"--out",
                                      L"%{DATABASE_FILE_PATH}",
                                      L"--append"};
  if(const FilePath interpreter = m_settings->getInterpreterPathExpandedAndAbsolute(); !interpreter.empty()) {
    arguments.insert(arguments.end(), {L"--python", interpreter.wstr()});
  }
  return arguments;
}

bool SourceGroupPython::isTriggerFile(const FilePath& filePath) const {
  return SourceGroupSettingsPython::isTriggerFile(filePath);
}

bool SourceGroupPython::isUnwalkedDirectory(const std::wstring& name) const {
  return SourceGroupSettingsPython::isUnwalkedDirectory(name);
}

bool SourceGroupPython::isIndexedFile(const FilePath& filePath) const {
  return filePath.extension() == L".py";
}

// The indexer skips the same folders as the search for trigger files.
bool SourceGroupPython::isExcludedDirectory(const std::wstring& name) const {
  return SourceGroupSettingsPython::isUnwalkedDirectory(name);
}

std::shared_ptr<SourceGroupSettings> SourceGroupPython::getSourceGroupSettings() {
  return m_settings;
}

std::shared_ptr<const SourceGroupSettings> SourceGroupPython::getSourceGroupSettings() const {
  return m_settings;
}
