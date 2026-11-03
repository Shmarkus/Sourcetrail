#pragma once

#include "QtProjectWizardContentPath.h"

class QCheckBox;
class SourceGroupSettingsJavaMaven;

class QtProjectWizardContentPathSourceMaven : public QtProjectWizardContentPath {
  Q_OBJECT

public:
  QtProjectWizardContentPathSourceMaven(std::shared_ptr<SourceGroupSettingsJavaMaven> settings, QtProjectWizardWindow* window);

  // QtProjectWizardContent implementation
  void populate(QGridLayout* layout, int& row) override;

  void load() override;
  void save() override;

  std::vector<FilePath> getFilePaths() const override;

private:
  std::shared_ptr<SourceGroupSettings> getSourceGroupSettings() override;

  std::shared_ptr<SourceGroupSettingsJavaMaven> m_settings;
  QCheckBox* m_shouldIndexTests;
};
