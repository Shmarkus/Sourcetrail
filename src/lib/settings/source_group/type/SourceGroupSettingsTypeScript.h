#ifndef SOURCE_GROUP_SETTINGS_TYPESCRIPT_H
#define SOURCE_GROUP_SETTINGS_TYPESCRIPT_H

#include <memory>
#include <string>
#include <vector>

#include "FilePath.h"
#include "SourceGroupSettingsScip.h"

// A TypeScript project: its root tsconfig.json, relative to the project folder. The folder holding the tsconfig is the
// TypeScript project the indexer reads; its node_modules are the user's own `npm install`.
class SourceGroupSettingsTypeScript : public SourceGroupSettingsScip {
public:
  SourceGroupSettingsTypeScript(const std::string& id, const ProjectSettings* projectSettings)
      : SourceGroupSettingsScip(SOURCE_GROUP_TYPESCRIPT, id, projectSettings) {}

  std::shared_ptr<SourceGroupSettings> createCopy() const override {
    return std::make_shared<SourceGroupSettingsTypeScript>(*this);
  }

  // Files whose change re-runs the TypeScript indexer: sources, tsconfigs, the package manifest and lockfiles.
  static bool isTriggerFile(const FilePath& filePath) {
    const std::wstring name = filePath.fileName();
    const std::wstring extension = filePath.extension();
    return extension == L".ts" || extension == L".tsx" || (name.starts_with(L"tsconfig") && name.ends_with(L".json")) ||
        name == L"package.json" || name == L"package-lock.json" || name == L"yarn.lock" || name == L"pnpm-lock.yaml";
  }

  // Folders below the TypeScript project that hold no sources of it.
  static bool isExcludedDirectory(const std::wstring& name) {
    return name == L"node_modules" || name == L".sourcetrail";
  }

  // Folders the search for trigger files leaves out: the excluded ones, and version control, build output and caches.
  static bool isUnwalkedDirectory(const std::wstring& name) {
    return isExcludedDirectory(name) || name == L".git" || name == L"build" || name == L"target" || name == L"dist" ||
        name == L".angular" || name == L"coverage";
  }

  // A project has one TypeScript source group (the groups would share the HTTP calls sidecar and the endpoint layer):
  // the enabled TypeScript groups after the first, which the wizard refuses and a refresh skips.
  static std::vector<std::shared_ptr<SourceGroupSettings>> getUnsupportedGroups(
      const std::vector<std::shared_ptr<SourceGroupSettings>>& groups);
  // The status line for a skipped group.
  static std::wstring getUnsupportedGroupStatus(const SourceGroupSettings& group);

  FilePath getTsconfigPath() const {
    return m_tsconfigPath;
  }

  void setTsconfigPath(const FilePath& tsconfigPath) {
    m_tsconfigPath = tsconfigPath;
  }

  FilePath getTsconfigPathExpandedAndAbsolute() const;

  void loadSettings(const ConfigManager* config) override;
  void saveSettings(ConfigManager* config) override;
  bool equalsSettings(const SourceGroupSettingsBase* other) override;

private:
  FilePath m_tsconfigPath;
};

#endif    // SOURCE_GROUP_SETTINGS_TYPESCRIPT_H
