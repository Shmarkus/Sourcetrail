#include "QtProjectWizardContentPathPythonRoot.h"

#include <QMessageBox>

#include "SourceGroupSettingsPython.h"
#include "utilityFile.h"

QtProjectWizardContentPathPythonRoot::QtProjectWizardContentPathPythonRoot(std::shared_ptr<SourceGroupSettingsPython> settings,
                                                                           QtProjectWizardWindow* window)
    : QtProjectWizardContentPath(window), m_settings(std::move(settings)) {
  setTitleString("Python Project Folder");
  setHelpString(
      "Enter the folder of your Python project, usually the one with its pyproject.toml, setup.py or requirements.txt. "
      "The .py files below it are indexed, except in environments, build output and caches (.venv, venv, build, "
      "dist, target, __pycache__, ...); module names come from their paths below it. Leave it empty for the folder "
      "of the project file.<br />"
      "<br />"
      "Indexing needs Node.js. Without it the Python is skipped and the rest of the project still indexes.<br />"
      "<br />"
      "You can make use of environment variables with ${ENV_VAR}.");
  setPlaceholderString("Folder of the Project File");
}

void QtProjectWizardContentPathPythonRoot::populate(QGridLayout* layout, int& row) {
  QtProjectWizardContentPath::populate(layout, row);
  m_picker->setPickDirectory(true);
}

void QtProjectWizardContentPathPythonRoot::load() {
  m_picker->setText(QString::fromStdWString(m_settings->getProjectRoot().wstr()));
}

void QtProjectWizardContentPathPythonRoot::save() {
  m_settings->setProjectRoot(FilePath(m_picker->getText().toStdWString()));
}

bool QtProjectWizardContentPathPythonRoot::check() {
  if(m_picker->getText().isEmpty()) {
    return true;
  }
  const FilePath root = utility::getExpandedAndAbsolutePath(
      FilePath(m_picker->getText().toStdWString()), m_settings->getProjectDirectoryPath());
  if(root.isDirectory()) {
    return true;
  }
  QMessageBox msgBox(m_window);
  msgBox.setText("The entered folder does not exist at \"Python Project Folder\".");
  msgBox.exec();
  return false;
}

std::shared_ptr<SourceGroupSettings> QtProjectWizardContentPathPythonRoot::getSourceGroupSettings() {
  return m_settings;
}
