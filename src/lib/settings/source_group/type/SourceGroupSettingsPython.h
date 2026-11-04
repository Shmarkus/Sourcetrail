#ifndef SOURCE_GROUP_SETTINGS_PYTHON_H
#define SOURCE_GROUP_SETTINGS_PYTHON_H

#include <memory>
#include <string>
#include <vector>

#include "FilePath.h"
#include "SourceGroupSettingsScip.h"

// A Python project: its root folder, relative to the project folder (the project folder itself when empty), and
// optionally the interpreter whose installed packages library imports resolve against. Without one, the indexer takes
// the active virtualenv, a .venv or venv in the root, or python3.
class SourceGroupSettingsPython : public SourceGroupSettingsScip {
public:
  SourceGroupSettingsPython(const std::string& id, const ProjectSettings* projectSettings)
      : SourceGroupSettingsScip(SOURCE_GROUP_PYTHON, id, projectSettings) {}

  std::shared_ptr<SourceGroupSettings> createCopy() const override {
    return std::make_shared<SourceGroupSettingsPython>(*this);
  }

  // Files whose change re-runs the Python indexer: sources, the project and requirements files and lockfiles.
  static bool isTriggerFile(const FilePath& filePath) {
    const std::wstring name = filePath.fileName();
    return filePath.extension() == L".py" || name == L"pyproject.toml" || name == L"setup.cfg" ||
        (name.starts_with(L"requirements") && name.ends_with(L".txt")) || name == L"poetry.lock" || name == L"uv.lock" ||
        name == L"Pipfile.lock";
  }

  // Folders below the Python project whose files the indexer leaves out (build output, environments, caches), and
  // which the search for trigger files leaves out.
  static bool isUnwalkedDirectory(const std::wstring& name) {
    return name == L"node_modules" || name == L".sourcetrail" || name == L"build" || name == L"dist" || name == L".git" ||
        name == L".venv" || name == L"venv" || name == L"__pycache__" || name == L".tox" || name == L".mypy_cache";
  }

  // A project has one Python source group: the enabled Python groups after the first, which the wizard refuses and a
  // refresh skips.
  static std::vector<std::shared_ptr<SourceGroupSettings>> getUnsupportedGroups(
      const std::vector<std::shared_ptr<SourceGroupSettings>>& groups);
  // The status line for a skipped group.
  static std::wstring getUnsupportedGroupStatus(const SourceGroupSettings& group);

  FilePath getProjectRoot() const {
    return m_projectRoot;
  }

  void setProjectRoot(const FilePath& projectRoot) {
    m_projectRoot = projectRoot;
  }

  FilePath getProjectRootExpandedAndAbsolute() const;

  FilePath getInterpreterPath() const {
    return m_interpreterPath;
  }

  void setInterpreterPath(const FilePath& interpreterPath) {
    m_interpreterPath = interpreterPath;
  }

  // Empty when not set; a bare name ("python3.12") is left for PATH to resolve.
  FilePath getInterpreterPathExpandedAndAbsolute() const;

  void loadSettings(const ConfigManager* config) override;
  void saveSettings(ConfigManager* config) override;
  bool equalsSettings(const SourceGroupSettingsBase* other) override;

private:
  FilePath m_projectRoot;
  FilePath m_interpreterPath;
};

#endif    // SOURCE_GROUP_SETTINGS_PYTHON_H
