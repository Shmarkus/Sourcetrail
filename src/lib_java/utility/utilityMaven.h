#ifndef UTILITY_MAVEN_H
#define UTILITY_MAVEN_H

#include <functional>
#include <string>
#include <vector>

class FilePath;

namespace utility {
std::wstring mavenGenerateSources(const FilePath& mavenPath, const FilePath& settingsFilePath, const FilePath& projectDirectoryPath);
std::vector<FilePath> mavenGetAllDirectoriesFromEffectivePom(const FilePath& mavenPath,
                                                             const FilePath& settingsFilePath,
                                                             const FilePath& projectDirectoryPath,
                                                             const FilePath& outputDirectoryPath,
                                                             bool addTestDirectories);

// Runs generate-sources and copy-dependencies into outputDirectoryPath and reads the source directories of the
// effective POM, unless an earlier run for the same cache key (see mavenBuildFingerprint) left every dependency and
// source directory it recorded in place. A full refresh always runs them. onStep is called before each Maven command
// with its description. Only a Maven that cannot run and a failing effective POM are errors, and leave no marker
// behind; a failing generate-sources or copy-dependencies is a warning, stored with the marker and returned again by
// every refresh that reuses it.
struct MavenPreparation {
  std::wstring errorMessage;             // empty on success
  std::vector<std::wstring> warnings;    // goals that failed, this run or the cached one; indexed with partial results
};

MavenPreparation mavenPrepareProject(const FilePath& mavenPath,
                                     const FilePath& settingsFilePath,
                                     const FilePath& projectDirectoryPath,
                                     const FilePath& outputDirectoryPath,
                                     bool addTestDirectories,
                                     bool fullRefresh,
                                     const std::function<void(const std::wstring&)>& onStep);

// The existing source directories of the effective POM, read from the cache when its key still matches. Sets
// errorMessage when Maven failed.
std::vector<FilePath> mavenGetSourceDirectories(const FilePath& mavenPath,
                                                const FilePath& settingsFilePath,
                                                const FilePath& projectDirectoryPath,
                                                const FilePath& outputDirectoryPath,
                                                bool addTestDirectories,
                                                std::wstring& errorMessage);
}    // namespace utility

#endif    // UTILITY_MAVEN_H
