#include "SourceGroupSettingsTypeScript.h"

#include "ConfigManager.hpp"
#include "ProjectSettings.h"
#include "utilityFile.h"

FilePath SourceGroupSettingsTypeScript::getTsconfigPathExpandedAndAbsolute() const {
  return utility::getExpandedAndAbsolutePath(m_tsconfigPath, getProjectSettings()->getProjectDirectoryPath());
}

std::vector<std::shared_ptr<SourceGroupSettings>> SourceGroupSettingsTypeScript::getUnsupportedGroups(
    const std::vector<std::shared_ptr<SourceGroupSettings>>& groups) {
  return SourceGroupSettingsScip::getUnsupportedGroups(groups, SOURCE_GROUP_TYPESCRIPT);
}

std::wstring SourceGroupSettingsTypeScript::getUnsupportedGroupStatus(const SourceGroupSettings& group) {
  return SourceGroupSettingsScip::getUnsupportedGroupStatus(group, L"TypeScript");
}

void SourceGroupSettingsTypeScript::loadSettings(const ConfigManager* config) {
  SourceGroupSettingsScip::loadSettings(config);
  setTsconfigPath(config->getValueOrDefault(s_keyPrefix + getId() + "/typescript/tsconfig_path", FilePath(L"")));
}

void SourceGroupSettingsTypeScript::saveSettings(ConfigManager* config) {
  SourceGroupSettingsScip::saveSettings(config);
  config->setValue(s_keyPrefix + getId() + "/typescript/tsconfig_path", m_tsconfigPath.wstr());
}

bool SourceGroupSettingsTypeScript::equalsSettings(const SourceGroupSettingsBase* other) {
  const auto* otherTypeScript = dynamic_cast<const SourceGroupSettingsTypeScript*>(other);
  return otherTypeScript && SourceGroupSettingsScip::equalsSettings(other) &&
      m_tsconfigPath == otherTypeScript->m_tsconfigPath;
}
