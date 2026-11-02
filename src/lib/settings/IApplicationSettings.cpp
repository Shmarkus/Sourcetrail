#include "IApplicationSettings.hpp"

#include "utility.h"
#include "utilityFile.h"

IApplicationSettings::Ptr IApplicationSettings::mInstance;

IApplicationSettings::IApplicationSettings() noexcept = default;
IApplicationSettings::~IApplicationSettings() noexcept = default;

bool IApplicationSettings::operator==(const IApplicationSettings& other) const noexcept {
  return utility::isPermutation(getHeaderSearchPaths(), other.getHeaderSearchPaths()) &&
      utility::isPermutation(getFrameworkSearchPaths(), other.getFrameworkSearchPaths());
}


FilePath IApplicationSettings::getJavaPath() const {
  return FilePath(getValue<std::wstring>("indexing/java/java_path", L""));
}

void IApplicationSettings::setJavaPath(const FilePath& path) {
  setValue<std::wstring>("indexing/java/java_path", path.wstr());
}

bool IApplicationSettings::getHasPrefilledJavaPath() const {
  return getValue<bool>("indexing/java/has_prefilled_java_path", false);
}

void IApplicationSettings::setHasPrefilledJavaPath(bool v) {
  setValue<bool>("indexing/java/has_prefilled_java_path", v);
}

int IApplicationSettings::getJavaMaximumMemory() const {
  return getValue<int>("indexing/java/java_maximum_memory", -1);
}

void IApplicationSettings::setJavaMaximumMemory(int size) {
  setValue<int>("indexing/java/java_maximum_memory", size);
}

std::vector<FilePath> IApplicationSettings::getJreSystemLibraryPaths() const {
  return getPathValues("indexing/java/jre_system_library_paths/jre_system_library_path");
}

std::vector<FilePath> IApplicationSettings::getJreSystemLibraryPathsExpanded() const {
  return utility::getExpandedPaths(getJreSystemLibraryPaths());
}

bool IApplicationSettings::setJreSystemLibraryPaths(const std::vector<FilePath>& paths) {
  return setPathValues("indexing/java/jre_system_library_paths/jre_system_library_path", paths);
}

bool IApplicationSettings::getHasPrefilledJreSystemLibraryPaths() const {
  return getValue<bool>("indexing/java/has_prefilled_jre_system_library_paths", false);
}

void IApplicationSettings::setHasPrefilledJreSystemLibraryPaths(bool v) {
  setValue<bool>("indexing/java/has_prefilled_jre_system_library_paths", v);
}

FilePath IApplicationSettings::getMavenPath() const {
  return FilePath(getValue<std::wstring>("indexing/java/maven_path", L""));
}

void IApplicationSettings::setMavenPath(const FilePath& path) {
  setValue<std::wstring>("indexing/java/maven_path", path.wstr());
}

bool IApplicationSettings::getHasPrefilledMavenPath() const {
  return getValue<bool>("indexing/java/has_prefilled_maven_path", false);
}

void IApplicationSettings::setHasPrefilledMavenPath(bool v) {
  setValue<bool>("indexing/java/has_prefilled_maven_path", v);
}
