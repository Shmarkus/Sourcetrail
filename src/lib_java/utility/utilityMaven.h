#ifndef UTILITY_MAVEN_H
#define UTILITY_MAVEN_H

#include <functional>
#include <string>
#include <vector>

class FilePath;

namespace utility {
std::wstring mavenGenerateSources(const FilePath& mavenPath, const FilePath& settingsFilePath, const FilePath& projectDirectoryPath);
bool mavenCopyDependencies(const FilePath& mavenPath,
                           const FilePath& settingsFilePath,
                           const FilePath& projectDirectoryPath,
                           const FilePath& outputDirectoryPath);
std::vector<FilePath> mavenGetAllDirectoriesFromEffectivePom(const FilePath& mavenPath,
                                                             const FilePath& settingsFilePath,
                                                             const FilePath& projectDirectoryPath,
                                                             const FilePath& outputDirectoryPath,
                                                             bool addTestDirectories);

// Runs generate-sources and copy-dependencies into outputDirectoryPath, unless an earlier run for the same cache key
// (see mavenBuildFingerprint) exported every dependency it still finds there. A full refresh always runs them and
// drops the cached source directories. onStep is called before each Maven command with its description.
bool mavenPrepareProject(const FilePath& mavenPath,
                         const FilePath& settingsFilePath,
                         const FilePath& projectDirectoryPath,
                         const FilePath& outputDirectoryPath,
                         bool addTestDirectories,
                         bool fullRefresh,
                         const std::function<void(const std::wstring&)>& onStep);

// The existing source directories of the effective POM, read from the cache when its key still matches.
std::vector<FilePath> mavenGetSourceDirectories(const FilePath& mavenPath,
                                                const FilePath& settingsFilePath,
                                                const FilePath& projectDirectoryPath,
                                                const FilePath& outputDirectoryPath,
                                                bool addTestDirectories);
}    // namespace utility

#endif    // UTILITY_MAVEN_H
