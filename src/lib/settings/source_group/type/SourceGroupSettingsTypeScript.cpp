#include "SourceGroupSettingsTypeScript.h"

#include "ConfigManager.hpp"
#include "ProjectSettings.h"
#include "SourceGroupStatusType.h"
#include "utilityFile.h"
#include "utilityString.h"

FilePath SourceGroupSettingsTypeScript::getTsconfigPathExpandedAndAbsolute() const {
  return utility::getExpandedAndAbsolutePath(m_tsconfigPath, getProjectSettings()->getProjectDirectoryPath());
}

std::vector<std::shared_ptr<SourceGroupSettings>> SourceGroupSettingsTypeScript::getUnsupportedGroups(
    const std::vector<std::shared_ptr<SourceGroupSettings>>& groups) {
  std::vector<std::shared_ptr<SourceGroupSettings>> unsupported;
  bool first = true;
  for(const std::shared_ptr<SourceGroupSettings>& group : groups) {
    if(group->getStatus() == SOURCE_GROUP_STATUS_ENABLED && std::dynamic_pointer_cast<SourceGroupSettingsTypeScript>(group)) {
      if(!first) {
        unsupported.push_back(group);
      }
      first = false;
    }
  }
  return unsupported;
}

std::wstring SourceGroupSettingsTypeScript::getUnsupportedGroupStatus(const SourceGroupSettings& group) {
  return L"TypeScript: only one TypeScript source group per project is supported; skipping " +
      utility::decodeFromUtf8(group.getName());
}

void SourceGroupSettingsTypeScript::loadSettings(const ConfigManager* config) {
  SourceGroupSettingsWithComponents::loadSettings(config);
  setTsconfigPath(config->getValueOrDefault(s_keyPrefix + getId() + "/typescript/tsconfig_path", FilePath(L"")));
}

void SourceGroupSettingsTypeScript::saveSettings(ConfigManager* config) {
  SourceGroupSettingsWithComponents::saveSettings(config);
  config->setValue(s_keyPrefix + getId() + "/typescript/tsconfig_path", m_tsconfigPath.wstr());
}

bool SourceGroupSettingsTypeScript::equalsSettings(const SourceGroupSettingsBase* other) {
  const auto* otherTypeScript = dynamic_cast<const SourceGroupSettingsTypeScript*>(other);
  return otherTypeScript && SourceGroupSettingsWithComponents::equalsSettings(other) &&
      m_tsconfigPath == otherTypeScript->m_tsconfigPath;
}
