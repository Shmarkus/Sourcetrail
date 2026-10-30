#include "utilityDirectorySync.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>

#include "FilePath.h"
#include "FileSystem.h"

namespace utility
{
namespace
{
bool hasEqualContent(const FilePath& a, const FilePath& b)
{
	if (FileSystem::getFileByteSize(a) != FileSystem::getFileByteSize(b))
	{
		return false;
	}

	std::ifstream streamA(a.str(), std::ios::binary);
	std::ifstream streamB(b.str(), std::ios::binary);
	return std::equal(
		std::istreambuf_iterator<char>(streamA),
		std::istreambuf_iterator<char>(),
		std::istreambuf_iterator<char>(streamB));
}
}	 // namespace

DirectorySyncResult syncDirectoryContents(
	const FilePath& sourceDirectory, const FilePath& targetDirectory)
{
	DirectorySyncResult result;
	std::set<std::wstring> sourceRelativePaths;

	FileSystem::createDirectory(targetDirectory);

	for (const FilePath& sourceFile: FileSystem::getFilePathsFromDirectory(sourceDirectory))
	{
		const FilePath relativePath = sourceFile.getRelativeTo(sourceDirectory);
		sourceRelativePaths.insert(relativePath.wstr());

		const FilePath targetFile = targetDirectory.getConcatenated(relativePath);
		if (targetFile.recheckExists())
		{
			if (hasEqualContent(sourceFile, targetFile))
			{
				result.unchanged++;
				continue;
			}
			FileSystem::remove(targetFile);
		}

		FileSystem::createDirectory(targetFile.getParentDirectory());
		if (FileSystem::copyFile(sourceFile, targetFile))
		{
			result.written++;
		}
	}

	for (const FilePath& targetFile: FileSystem::getFilePathsFromDirectory(targetDirectory))
	{
		if (sourceRelativePaths.count(targetFile.getRelativeTo(targetDirectory).wstr()) == 0 &&
			FileSystem::remove(targetFile))
		{
			result.removed++;
		}
	}

	std::vector<FilePath> subDirectories = FileSystem::getRecursiveSubDirectories(targetDirectory);
	for (auto it = subDirectories.rbegin(); it != subDirectories.rend(); ++it)
	{
		// non-recursive remove fails on non-empty directories, which is what we want here
		FileSystem::remove(*it);
	}

	return result;
}
}	 // namespace utility
