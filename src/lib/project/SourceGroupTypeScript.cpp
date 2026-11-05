#include "SourceGroupTypeScript.h"

#include "FileSystem.h"
#include "ProjectSettings.h"
#include "ResourcePaths.h"
#include "SourceGroupSettingsTypeScript.h"

SourceGroupTypeScript::SourceGroupTypeScript(std::shared_ptr<SourceGroupSettingsTypeScript> settings)
    : m_settings(std::move(settings)) {}

std::string SourceGroupTypeScript::getInputsRecord(const FilePath& projectPath,
                                                   const std::function<void(const FilePath& triggerFile)>& visit) {
  return recordInputs(
      projectPath, &SourceGroupSettingsTypeScript::isTriggerFile, &SourceGroupSettingsTypeScript::isUnwalkedDirectory, visit);
}

FilePath SourceGroupTypeScript::getScipProjectPath() const {
  return getTypeScriptProjectPath();
}

FilePath SourceGroupTypeScript::getTypeScriptProjectPath() const {
  return m_settings->getTsconfigPathExpandedAndAbsolute().getParentDirectory();
}

FilePath SourceGroupTypeScript::getHttpCallsFilePath() const {
  return m_settings->getProjectSettings()->getProjectDirectoryPath().concatenate(L"ts-http-calls.json");
}

FilePath SourceGroupTypeScript::getSourceFilePath() const {
  return m_settings->getTsconfigPathExpandedAndAbsolute();
}

std::wstring SourceGroupTypeScript::getLabel() const {
  return L"TypeScript";
}

std::set<int> SourceGroupTypeScript::getSkippedExitCodes() const {
  return {NodeModulesMissingExitCode, UnsupportedLayoutExitCode};
}

std::vector<std::wstring> SourceGroupTypeScript::getIndexerArguments() const {
  return {ResourcePaths::getTypeScriptIndexerFilePath().wstr(),
          L"--project",
          getTypeScriptProjectPath().wstr(),
          L"--tsconfig",
          getSourceFilePath().wstr(),
          // Into the index Sourcetrail is building: it holds the other groups' data already.
          L"--out",
          L"%{DATABASE_FILE_PATH}",
          L"--append",
          L"--http-out",
          getHttpCallsFilePath().wstr()};
}

bool SourceGroupTypeScript::isTriggerFile(const FilePath& filePath) const {
  return SourceGroupSettingsTypeScript::isTriggerFile(filePath);
}

bool SourceGroupTypeScript::isUnwalkedDirectory(const std::wstring& name) const {
  return SourceGroupSettingsTypeScript::isUnwalkedDirectory(name);
}

bool SourceGroupTypeScript::isIndexedFile(const FilePath& filePath) const {
  const std::wstring extension = filePath.extension();
  return extension == L".ts" || extension == L".tsx";
}

bool SourceGroupTypeScript::isExcludedDirectory(const std::wstring& name) const {
  return SourceGroupSettingsTypeScript::isExcludedDirectory(name);
}

std::function<void(IndexerCommandCustom::Result result)> SourceGroupTypeScript::getRunFinishedHandler() const {
  return [httpCalls = getHttpCallsFilePath()](IndexerCommandCustom::Result result) {
    if(result != IndexerCommandCustom::Result::Succeeded) {
      FileSystem::remove(httpCalls);
    }
  };
}

std::shared_ptr<SourceGroupSettings> SourceGroupTypeScript::getSourceGroupSettings() {
  return m_settings;
}

std::shared_ptr<const SourceGroupSettings> SourceGroupTypeScript::getSourceGroupSettings() const {
  return m_settings;
}
