#ifndef UTILITY_GRADLE_CACHE_H
#define UTILITY_GRADLE_CACHE_H

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "FilePath.h"

namespace utility
{
struct GradleProjectInfo
{
	std::string fingerprint;	// build-file fingerprint it was computed for
	bool includesTests = false;
	bool mainHasLombok = false;
	bool testHasLombok = false;
	FilePath mainLombokJar;
	FilePath testLombokJar;
	std::map<std::wstring, std::vector<FilePath>> sourceDirectories;	// "main" / "test" -> original roots
};

std::vector<FilePath> gradleBuildFiles(const FilePath& projectDirectory);

std::string gradleBuildFingerprint(const FilePath& projectDirectory);

// Returns nullopt when the cache is missing or stale (build files, test inclusion or source roots changed).
std::optional<GradleProjectInfo> loadGradleProjectInfo(
	const FilePath& outputDirectory, const FilePath& projectDirectory, bool includesTests);

void saveGradleProjectInfo(const FilePath& outputDirectory, const GradleProjectInfo& info);
}	 // namespace utility

#endif	  // UTILITY_GRADLE_CACHE_H
