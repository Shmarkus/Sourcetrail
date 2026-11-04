#include "SourceGroupSettingsPython.h"

#include <filesystem>

#include <system_error>

#include "ConfigManager.hpp"
#include "ProjectSettings.h"
#include "utilityFile.h"

namespace fs = std::filesystem;

namespace {
fs::path toPath(const FilePath& path) {
  fs::path normal = fs::path{path.wstr()}.lexically_normal();
  if(!normal.has_filename() && normal.has_parent_path()) {
    normal = normal.parent_path();    // "app/" -> "app"
  }
  return normal;
}

bool isDirectory(const fs::path& path) {
  std::error_code error;
  return fs::is_directory(path, error);
}

bool isFile(const fs::path& path) {
  std::error_code error;
  return fs::is_regular_file(path, error);
}

// The deepest folder both lie in.
fs::path commonFolder(const fs::path& first, const fs::path& second) {
  fs::path common;
  for(auto a = first.begin(), b = second.begin(); a != first.end() && b != second.end() && *a == *b; ++a, ++b) {
    common /= *a;
  }
  return common;
}
}    // namespace

FilePath SourceGroupSettingsPython::getProjectRootExpandedAndAbsolute() const {
  const FilePath projectDirectory = getProjectSettings()->getProjectDirectoryPath();
  if(m_projectRoot.empty()) {
    return projectDirectory;
  }
  return utility::getExpandedAndAbsolutePath(m_projectRoot, projectDirectory);
}

FilePath SourceGroupSettingsPython::getInterpreterPathExpandedAndAbsolute() const {
  if(m_interpreterPath.wstr().find_first_of(L"/\\") == std::wstring::npos) {
    return m_interpreterPath;
  }
  return utility::getExpandedAndAbsolutePath(m_interpreterPath, getProjectSettings()->getProjectDirectoryPath());
}

FilePath SourceGroupSettingsPython::findEnvironmentInterpreter(const FilePath& root) {
  for(const wchar_t* environment : {L".venv", L"venv"}) {
    const fs::path interpreter = toPath(root) / environment / "bin" / "python";
    if(fs::exists(interpreter)) {
      return FilePath{interpreter.wstring()};
    }
  }
  return {};
}

std::vector<std::shared_ptr<SourceGroupSettings>> SourceGroupSettingsPython::getUnsupportedGroups(
    const std::vector<std::shared_ptr<SourceGroupSettings>>& groups) {
  return SourceGroupSettingsScip::getUnsupportedGroups(groups, SOURCE_GROUP_PYTHON);
}

std::wstring SourceGroupSettingsPython::getUnsupportedGroupStatus(const SourceGroupSettings& group) {
  return SourceGroupSettingsScip::getUnsupportedGroupStatus(group, L"Python");
}

void SourceGroupSettingsPython::loadSettings(const ConfigManager* config) {
  SourceGroupSettingsScip::loadSettings(config);
  const std::string key = s_keyPrefix + getId() + "/python/";
  if(!config->isValueDefined(key + "project_root") && !config->isValueDefined(key + "interpreter_path")) {
    loadUpstreamSettings(config);
    return;
  }
  setProjectRoot(config->getValueOrDefault(key + "project_root", FilePath(L"")));
  setInterpreterPath(config->getValueOrDefault(key + "interpreter_path", FilePath(L"")));
}

// The project root is the folder holding the source paths, above any package (a folder with an __init__.py) so that
// modules keep their package names; the interpreter is the environment's, or the environment when it is one. Upstream's
// source extensions and exclude filters have no counterpart: the indexer reads every .py file below the root.
void SourceGroupSettingsPython::loadUpstreamSettings(const ConfigManager* config) {
  const std::string key = s_keyPrefix + getId();
  const FilePath projectDirectory = getProjectSettings()->getProjectDirectoryPath();
  const fs::path projectFolder = toPath(projectDirectory);

  const auto sourcePaths = config->getValuesOrDefaults(key + "/source_paths/source_path", std::vector<FilePath>());
  if(!sourcePaths.empty()) {
    bool relative = true;
    fs::path root;
    for(const FilePath& sourcePath : sourcePaths) {
      relative = relative && !sourcePath.isAbsolute();
      fs::path folder = toPath(utility::getExpandedAndAbsolutePath(sourcePath, projectDirectory));
      if(!isDirectory(folder)) {
        folder = folder.parent_path();
      }
      root = root.empty() ? folder : commonFolder(root, folder);
    }
    while(isFile(root / "__init__.py") && root.has_relative_path()) {
      root = root.parent_path();
    }
    if(root != projectFolder) {
      setProjectRoot(FilePath{(relative ? root.lexically_relative(projectFolder) : root).wstring()});
    }
  }

  const FilePath environment = config->getValueOrDefault(key + "/python_environment_path", FilePath(L""));
  if(!environment.empty()) {
    const fs::path absolute = toPath(utility::getExpandedAndAbsolutePath(environment, projectDirectory));
    if(!isDirectory(absolute)) {
      setInterpreterPath(environment);
    } else if(isFile(absolute / "Scripts" / "python.exe")) {
      setInterpreterPath(FilePath{(toPath(environment) / "Scripts" / "python.exe").wstring()});
    } else {
      setInterpreterPath(FilePath{(toPath(environment) / "bin" / "python").wstring()});
    }
  }
}

void SourceGroupSettingsPython::saveSettings(ConfigManager* config) {
  SourceGroupSettingsScip::saveSettings(config);
  const std::string key = s_keyPrefix + getId() + "/python/";
  config->setValue(key + "project_root", m_projectRoot.wstr());
  config->setValue(key + "interpreter_path", m_interpreterPath.wstr());
}

bool SourceGroupSettingsPython::equalsSettings(const SourceGroupSettingsBase* other) {
  const auto* otherPython = dynamic_cast<const SourceGroupSettingsPython*>(other);
  return otherPython && SourceGroupSettingsScip::equalsSettings(other) && m_projectRoot == otherPython->m_projectRoot &&
      m_interpreterPath == otherPython->m_interpreterPath;
}
