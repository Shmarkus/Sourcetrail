#pragma once

#include "QtProjectWizardContentPath.h"

class SourceGroupSettingsPython;

// The interpreter of a Python source group, whose installed packages library imports resolve against; optional.
class QtProjectWizardContentPathPythonInterpreter : public QtProjectWizardContentPath {
  Q_OBJECT

public:
  QtProjectWizardContentPathPythonInterpreter(std::shared_ptr<SourceGroupSettingsPython> settings, QtProjectWizardWindow* window);

  // QtProjectWizardContent implementation
  void populate(QGridLayout* layout, int& row) override;

  void load() override;
  void save() override;
  bool check() override;

private:
  std::shared_ptr<SourceGroupSettings> getSourceGroupSettings() override;

  std::shared_ptr<SourceGroupSettingsPython> m_settings;
};
