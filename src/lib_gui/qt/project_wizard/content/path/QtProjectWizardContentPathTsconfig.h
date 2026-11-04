#pragma once

#include "QtProjectWizardContentPath.h"

class SourceGroupSettingsTypeScript;

// The root tsconfig.json of a TypeScript source group.
class QtProjectWizardContentPathTsconfig : public QtProjectWizardContentPath {
  Q_OBJECT

public:
  QtProjectWizardContentPathTsconfig(std::shared_ptr<SourceGroupSettingsTypeScript> settings, QtProjectWizardWindow* window);

  // QtProjectWizardContent implementation
  void populate(QGridLayout* layout, int& row) override;

  void load() override;
  void save() override;

private:
  std::shared_ptr<SourceGroupSettings> getSourceGroupSettings() override;

  std::shared_ptr<SourceGroupSettingsTypeScript> m_settings;
};
