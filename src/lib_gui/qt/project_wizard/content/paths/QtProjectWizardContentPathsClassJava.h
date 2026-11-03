#pragma once

#include "QtProjectWizardContentPaths.h"

class QCheckBox;

class QtProjectWizardContentPathsClassJava : public QtProjectWizardContentPaths {
  Q_OBJECT
public:
  QtProjectWizardContentPathsClassJava(std::shared_ptr<SourceGroupSettings> settings, QtProjectWizardWindow* window);

  // QtProjectWizardContent implementation
  virtual void populate(QGridLayout* layout, int& row) override;
  virtual void load() override;
  virtual void save() override;

private:
  QCheckBox* m_useJreSystemLibraryCheckBox;
};
