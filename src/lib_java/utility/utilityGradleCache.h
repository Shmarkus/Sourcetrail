#ifndef UTILITY_GRADLE_CACHE_H
#define UTILITY_GRADLE_CACHE_H

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "FilePath.h"

namespace utility {
struct GradleProjectInfo {
  std::string fingerprint;                                            // build-file fingerprint it was computed for
  bool includesTests = false;
  std::map<std::wstring, FilePath> lombokJars;                        // source set -> lombok jar, for source sets using Lombok
  std::map<std::wstring, std::vector<FilePath>> sourceDirectories;    // source set ("main", "test", ...) -> original roots
};

std::vector<FilePath> gradleBuildFiles(const FilePath& projectDirectory);

std::string gradleBuildFingerprint(const FilePath& projectDirectory);

// Returns nullopt when the cache is missing or stale (build files, test inclusion or source roots changed).
std::optional<GradleProjectInfo> loadGradleProjectInfo(const FilePath& outputDirectory,
                                                       const FilePath& projectDirectory,
                                                       bool includesTests);

void saveGradleProjectInfo(const FilePath& outputDirectory, const GradleProjectInfo& info);

// The exported-jars marker is written only after a successful dependency export, so it can
// not be refreshed by anything that merely queried Gradle.
void markGradleDependenciesExported(const FilePath& outputDirectory, const std::string& fingerprint, bool includesTests);

bool gradleDependenciesUpToDate(const FilePath& outputDirectory, const FilePath& projectDirectory, bool includesTests);

struct FileStamp {
  uintmax_t size = 0;
  long long mtime = 0;    // nanoseconds

  bool operator==(const FileStamp& other) const;
};

// What a delombok output directory was last generated from.
struct DelombokManifest {
  std::string fingerprint;                                       // hash of the build-file fingerprint
  std::vector<FilePath> sourceDirectories;
  std::map<std::wstring, FileStamp> lombokConfigs;               // lombok.config files above the roots
  std::map<std::pair<size_t, std::wstring>, FileStamp> files;    // (root index, relative path) -> stamp
  // originals using @Delegate, @ExtensionMethod or lombok.val, whose output depends on other files
  std::set<std::pair<size_t, std::wstring>> crossFileOriginals;

  bool operator==(const DelombokManifest& other) const;
};

// Stamps every file in the roots and every lombok.config in the directories above them. Only
// files that changed since previous are read to find cross-file originals.
DelombokManifest scanDelombokOriginals(const std::vector<FilePath>& sourceDirectories,
                                       const std::string& fingerprint,
                                       const std::optional<DelombokManifest>& previous = std::nullopt);

// The manifest lives beside the delombok output directory, so the indexer never sees it.
std::optional<DelombokManifest> loadDelombokManifest(const FilePath& delombokOutputDirectory);

void saveDelombokManifest(const FilePath& delombokOutputDirectory, const DelombokManifest& manifest);

void removeDelombokManifest(const FilePath& delombokOutputDirectory);
}    // namespace utility

#endif    // UTILITY_GRADLE_CACHE_H
