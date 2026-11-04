#include "SourceGroupSettingsPython.h"

#include "ConfigManager.hpp"
#include "ProjectSettings.h"
#include "utilityFile.h"

FilePath SourceGroupSettingsPython::getProjectRootExpandedAndAbsolute() const {
  const FilePath projectDirectory = getProjectSettings()->getProjectDirectoryPath();
  if(m_projectRoot.empty()) {
    return projectDirectory;
  }
  return utility::getExpandedAndAbsolutePath(m_projectRoot, projectDirectory);
}

FilePath SourceGroupSettingsPython::getInterpreterPathExpandedAndAbsolute() const {
  if(m_interpreterPath.wstr().find_first_of(L"/\\") == std::wstring::npos) {
    return m_interpreterPath;
  }
  return utility::getExpandedAndAbsolutePath(m_interpreterPath, getProjectSettings()->getProjectDirectoryPath());
}

std::vector<std::shared_ptr<SourceGroupSettings>> SourceGroupSettingsPython::getUnsupportedGroups(
    const std::vector<std::shared_ptr<SourceGroupSettings>>& groups) {
  return SourceGroupSettingsScip::getUnsupportedGroups(groups, SOURCE_GROUP_PYTHON);
}

std::wstring SourceGroupSettingsPython::getUnsupportedGroupStatus(const SourceGroupSettings& group) {
  return SourceGroupSettingsScip::getUnsupportedGroupStatus(group, L"Python");
}

void SourceGroupSettingsPython::loadSettings(const ConfigManager* config) {
  SourceGroupSettingsScip::loadSettings(config);
  const std::string key = s_keyPrefix + getId() + "/python/";
  setProjectRoot(config->getValueOrDefault(key + "project_root", FilePath(L"")));
  setInterpreterPath(config->getValueOrDefault(key + "interpreter_path", FilePath(L"")));
}

void SourceGroupSettingsPython::saveSettings(ConfigManager* config) {
  SourceGroupSettingsScip::saveSettings(config);
  const std::string key = s_keyPrefix + getId() + "/python/";
  config->setValue(key + "project_root", m_projectRoot.wstr());
  config->setValue(key + "interpreter_path", m_interpreterPath.wstr());
}

bool SourceGroupSettingsPython::equalsSettings(const SourceGroupSettingsBase* other) {
  const auto* otherPython = dynamic_cast<const SourceGroupSettingsPython*>(other);
  return otherPython && SourceGroupSettingsScip::equalsSettings(other) && m_projectRoot == otherPython->m_projectRoot &&
      m_interpreterPath == otherPython->m_interpreterPath;
}
