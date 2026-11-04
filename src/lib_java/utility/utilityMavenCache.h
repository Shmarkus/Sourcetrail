#ifndef UTILITY_MAVEN_CACHE_H
#define UTILITY_MAVEN_CACHE_H

#include <optional>
#include <string>
#include <vector>

#include "FilePath.h"

namespace utility {
// Every pom.xml in the project (not under target, node_modules, .git or .sourcetrail), every file under .mvn, the
// configured settings file and ~/.m2/settings.xml.
std::vector<FilePath> mavenBuildFiles(const FilePath& projectDirectory, const FilePath& settingsFilePath);

// The cache key: size and modification time of each build file, the Maven executable, the settings file path and
// whether tests are included.
std::string mavenBuildFingerprint(const FilePath& mavenPath,
                                  const FilePath& settingsFilePath,
                                  const FilePath& projectDirectory,
                                  bool includesTests);

// Every source directory the effective POM names, existing or not, for the fingerprint it was read with.
// Returns nullopt when the cache is missing, unreadable or of another fingerprint.
std::optional<std::vector<FilePath>> loadMavenSourceDirectories(const FilePath& outputDirectory, const std::string& fingerprint);

void saveMavenSourceDirectories(const FilePath& outputDirectory,
                                const std::string& fingerprint,
                                const std::vector<FilePath>& sourceDirectories);

// Written only after generate-sources and copy-dependencies both succeeded, with the jars the copy left behind.
void markMavenDependenciesExported(const FilePath& outputDirectory, const std::string& fingerprint);

// True when the last export was for this fingerprint and every jar it recorded is still there.
bool mavenDependenciesUpToDate(const FilePath& outputDirectory, const std::string& fingerprint);

void removeMavenDependenciesExported(const FilePath& outputDirectory);

// Removes the export marker and the cached source directories.
void removeMavenCache(const FilePath& outputDirectory);
}    // namespace utility

#endif    // UTILITY_MAVEN_CACHE_H
