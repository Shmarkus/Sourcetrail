#include "ApplicationSettingsPrefiller.h"

#include <range/v3/range/conversion.hpp>
#include <range/v3/view/transform.hpp>

#include "IApplicationSettings.hpp"
#include "language_packages.h"
#include "logging.h"
#include "type/MessageStatus.h"
#include "utility.h"
#include "utilityPathDetection.h"

void ApplicationSettingsPrefiller::prefillPaths(IApplicationSettings* settings) {
  bool updated = false;

  updated |= prefillCxxHeaderPaths(settings);
  updated |= prefillCxxFrameworkPaths(settings);
#if BUILD_JAVA_LANGUAGE_PACKAGE
  updated |= prefillJavaRuntimePath(settings);
  updated |= prefillJreSystemLibraryPaths(settings);
  updated |= prefillMavenExecutablePath(settings);
#endif    // BUILD_JAVA_LANGUAGE_PACKAGE

  if(updated) {
    settings->save();
  }
}

bool ApplicationSettingsPrefiller::prefillCxxHeaderPaths(IApplicationSettings* settings) {
  if(settings->getHasPrefilledHeaderSearchPaths())    // allow empty
  {
    return false;
  }

  LOG_INFO("Prefilling header search paths");
  auto cxxHeaderDetector = utility::getCxxHeaderPathDetector();
  std::vector<FilePath> paths = cxxHeaderDetector->getPaths();
  if(!paths.empty()) {
    MessageStatus(L"Ran C/C++ header path detection, found " + std::to_wstring(paths.size()) + L" path" +
                  (paths.size() == 1 ? L"" : L"s"))
        .dispatch();

    settings->setHeaderSearchPaths(utility::toStlPath(paths));
  }

  settings->setHasPrefilledHeaderSearchPaths(true);
  return true;
}

bool ApplicationSettingsPrefiller::prefillCxxFrameworkPaths(IApplicationSettings* settings) {
  if(settings->getHasPrefilledFrameworkSearchPaths())    // allow empty
  {
    return false;
  }

  LOG_INFO("Prefilling framework search paths");
  std::shared_ptr<CombinedPathDetector> cxxFrameworkDetector = utility::getCxxFrameworkPathDetector();
  std::vector<FilePath> paths = cxxFrameworkDetector->getPaths();
  if(!paths.empty()) {
    MessageStatus(L"Ran C/C++ framework path detection, found " + std::to_wstring(paths.size()) + L" path" +
                  (paths.size() == 1 ? L"" : L"s"))
        .dispatch();

    settings->setFrameworkSearchPaths(utility::toStlPath(paths));
  }

  settings->setHasPrefilledFrameworkSearchPaths(true);
  return true;
}

bool ApplicationSettingsPrefiller::prefillJavaRuntimePath(IApplicationSettings* settings) {
  if(settings->getHasPrefilledJavaPath() && !settings->getJavaPath().empty()) {
    return false;
  }

  LOG_INFO("Prefilling Java path");
  std::shared_ptr<CombinedPathDetector> javaPathDetector = utility::getJavaRuntimePathDetector();
  std::vector<FilePath> paths = javaPathDetector->getPaths();
  if(!paths.empty()) {
    MessageStatus(L"Ran Java runtime path detection, found: " + paths.front().wstr()).dispatch();

    settings->setJavaPath(paths.front());
  } else {
    MessageStatus(L"Ran Java runtime path detection, no path found.").dispatch();
  }

  settings->setHasPrefilledJavaPath(true);
  return true;
}

bool ApplicationSettingsPrefiller::prefillJreSystemLibraryPaths(IApplicationSettings* settings) {
  if(settings->getHasPrefilledJreSystemLibraryPaths())    // allow empty
  {
    return false;
  }

  LOG_INFO("Prefilling JRE system library path");
  std::shared_ptr<CombinedPathDetector> jreSystemLibraryPathsDetector = utility::getJreSystemLibraryPathsDetector();
  std::vector<FilePath> paths = jreSystemLibraryPathsDetector->getPaths();
  if(!paths.empty()) {
    MessageStatus(L"Ran JRE system library path detection, found: " + paths.front().wstr()).dispatch();

    settings->setJreSystemLibraryPaths(paths);
  } else {
    MessageStatus(L"Ran JRE system library path detection, no path found.").dispatch();
  }

  settings->setHasPrefilledJreSystemLibraryPaths(true);
  return true;
}

bool ApplicationSettingsPrefiller::prefillMavenExecutablePath(IApplicationSettings* settings) {
  if(settings->getHasPrefilledMavenPath() && !settings->getMavenPath().empty()) {
    return false;
  }

  LOG_INFO("Prefilling Maven path");
  std::shared_ptr<CombinedPathDetector> mavenPathDetector = utility::getMavenExecutablePathDetector();
  std::vector<FilePath> paths = mavenPathDetector->getPaths();
  if(!paths.empty()) {
    MessageStatus(L"Ran Maven executable path detection, found: " + paths.front().wstr()).dispatch();

    settings->setMavenPath(paths.front());
  } else {
    MessageStatus(L"Ran Maven executable path detection, no path found.").dispatch();
  }

  settings->setHasPrefilledMavenPath(true);
  return true;
}
