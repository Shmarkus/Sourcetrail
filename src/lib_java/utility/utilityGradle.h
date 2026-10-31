#ifndef UTILITY_GRADLE_H
#define UTILITY_GRADLE_H

#include <functional>
#include <string>
#include <vector>

class FilePath;

namespace utility
{
FilePath getDelombokOutputDirectory(const FilePath& outputDirectoryPath, const std::wstring& sourceSetName);

// Delomboks the inputs into outputDirectory like Lombok's CLI does: a directory input keeps its
// relative layout, a file input is written as outputDirectory/<file name>. sourcePath may be empty.
using DelombokRunner = std::function<bool(
	const std::vector<FilePath>& inputs,
	const std::vector<FilePath>& sourcePath,
	const FilePath& outputDirectory)>;

// Brings outputDirectory up to date with the source set's roots. Delomboks only the originals
// that changed since the last successful run, or the whole tree when the manifest is missing,
// the roots, the build fingerprint, a lombok.config or any non-Java file changed.
bool delombokSourceSet(
	const std::vector<FilePath>& sourceDirectories,
	const FilePath& outputDirectory,
	const std::string& fingerprint,
	const DelombokRunner& runDelombok);

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
