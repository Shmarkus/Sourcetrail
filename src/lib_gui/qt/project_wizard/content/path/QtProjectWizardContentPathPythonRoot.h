#pragma once

#include "QtProjectWizardContentPath.h"

class SourceGroupSettingsPython;

// The root folder of a Python source group.
class QtProjectWizardContentPathPythonRoot : public QtProjectWizardContentPath {
  Q_OBJECT

public:
  QtProjectWizardContentPathPythonRoot(std::shared_ptr<SourceGroupSettingsPython> settings, QtProjectWizardWindow* window);

  // QtProjectWizardContent implementation
  void populate(QGridLayout* layout, int& row) override;

  void load() override;
  void save() override;
  bool check() override;

private:
  std::shared_ptr<SourceGroupSettings> getSourceGroupSettings() override;

  std::shared_ptr<SourceGroupSettingsPython> m_settings;
};
