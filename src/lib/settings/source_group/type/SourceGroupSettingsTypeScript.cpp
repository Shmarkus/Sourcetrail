#include "SourceGroupSettingsTypeScript.h"

#include "ConfigManager.hpp"
#include "ProjectSettings.h"
#include "utilityFile.h"

FilePath SourceGroupSettingsTypeScript::getTsconfigPathExpandedAndAbsolute() const {
  return utility::getExpandedAndAbsolutePath(m_tsconfigPath, getProjectSettings()->getProjectDirectoryPath());
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
