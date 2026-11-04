#include "QtProjectWizardContentPathTsconfig.h"

#include "SourceGroupSettingsTypeScript.h"

QtProjectWizardContentPathTsconfig::QtProjectWizardContentPathTsconfig(std::shared_ptr<SourceGroupSettingsTypeScript> settings,
                                                                       QtProjectWizardWindow* window)
    : QtProjectWizardContentPath(window), m_settings(std::move(settings)) {
  setTitleString("TypeScript Project File (tsconfig.json)");
  setHelpString(
      "Enter the path to the root tsconfig.json of your TypeScript project. Its folder is the TypeScript project; "
      "the files it includes and its references are indexed.<br />"
      "<br />"
      "Indexing needs Node.js and the project's node_modules (run npm install first). Without them the TypeScript "
      "is skipped and the rest of the project still indexes.<br />"
      "<br />"
      "You can make use of environment variables with ${ENV_VAR}.");
  setFileEndings({L".json"});
  setIsRequired(true);
}

void QtProjectWizardContentPathTsconfig::populate(QGridLayout* layout, int& row) {
  QtProjectWizardContentPath::populate(layout, row);
  m_picker->setPickDirectory(false);
  m_picker->setFileFilter("TypeScript Project File (tsconfig*.json)");
}

void QtProjectWizardContentPathTsconfig::load() {
  m_picker->setText(QString::fromStdWString(m_settings->getTsconfigPath().wstr()));
}

void QtProjectWizardContentPathTsconfig::save() {
  m_settings->setTsconfigPath(FilePath(m_picker->getText().toStdWString()));
}

std::shared_ptr<SourceGroupSettings> QtProjectWizardContentPathTsconfig::getSourceGroupSettings() {
  return m_settings;
}
