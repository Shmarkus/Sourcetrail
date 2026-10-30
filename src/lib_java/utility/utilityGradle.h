#ifndef UTILITY_GRADLE_H
#define UTILITY_GRADLE_H

#include <string>
#include <vector>

class FilePath;

namespace utility
{
bool gradleCopyDependencies(
	const FilePath& projectDirectoryPath,
	const FilePath& outputDirectoryPath,
	bool addTestDependencies);

bool gradlePrepareLombokSources(
	const FilePath& projectDirectoryPath,
	const FilePath& outputDirectoryPath,
	bool addTestSources);

std::vector<FilePath> gradleGetAllSourceDirectories(
	const FilePath& projectDirectoryPath,
	const FilePath& outputDirectoryPath,
	bool addTestDirectories);
}	 // namespace utility

#endif	  // UTILITY_GRADLE_H
