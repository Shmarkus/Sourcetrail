#include "QtProjectWizardContentPathPythonInterpreter.h"

#include <QMessageBox>

#include "SourceGroupSettingsPython.h"
#include "utilityFile.h"

QtProjectWizardContentPathPythonInterpreter::QtProjectWizardContentPathPythonInterpreter(
    std::shared_ptr<SourceGroupSettingsPython> settings, QtProjectWizardWindow* window)
    : QtProjectWizardContentPath(window), m_settings(std::move(settings)) {
  setTitleString("Python Interpreter");
  setHelpString(
      "Library imports (requests, django, ...) resolve against the packages installed for this interpreter, e.g. "
      ".venv/bin/python, or a name on the PATH such as python3.12.<br />"
      "<br />"
      "Leave it empty to use the active virtualenv, else a .venv or venv in the Python project folder, else python3. "
      "Without an interpreter the project's own code is still indexed; library symbols stay unresolved.<br />"
      "<br />"
      "You can make use of environment variables with ${ENV_VAR}.");
}

void QtProjectWizardContentPathPythonInterpreter::populate(QGridLayout* layout, int& row) {
  // the environment the indexer would find, shown while the field is empty
  const FilePath found = SourceGroupSettingsPython::findEnvironmentInterpreter(m_settings->getProjectRootExpandedAndAbsolute());
  setPlaceholderString(found.empty() ? QStringLiteral("Active virtualenv, .venv or venv, else python3") :
                                       QString::fromStdWString(L"Found: " + found.wstr()));
  QtProjectWizardContentPath::populate(layout, row);
  m_picker->setPickDirectory(false);
}

void QtProjectWizardContentPathPythonInterpreter::load() {
  m_picker->setText(QString::fromStdWString(m_settings->getInterpreterPath().wstr()));
}

void QtProjectWizardContentPathPythonInterpreter::save() {
  m_settings->setInterpreterPath(FilePath(m_picker->getText().toStdWString()));
}

// Empty, a name for the PATH, or an existing file.
bool QtProjectWizardContentPathPythonInterpreter::check() {
  const std::wstring text = m_picker->getText().toStdWString();
  if(text.empty() || text.find_first_of(L"/\\") == std::wstring::npos) {
    return true;
  }
  if(utility::getExpandedAndAbsolutePath(FilePath(text), m_settings->getProjectDirectoryPath()).exists()) {
    return true;
  }
  QMessageBox msgBox(m_window);
  msgBox.setText("The entered path does not exist at \"Python Interpreter\".");
  msgBox.exec();
  return false;
}

std::shared_ptr<SourceGroupSettings> QtProjectWizardContentPathPythonInterpreter::getSourceGroupSettings() {
  return m_settings;
}
