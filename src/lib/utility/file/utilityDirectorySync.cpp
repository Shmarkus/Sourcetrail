#include "utilityDirectorySync.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

#include "FilePath.h"
#include "FileSystem.h"
#include "logging.h"

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

// the old target file stays in place until the new content is completely written
bool replaceFile(const FilePath& sourceFile, const FilePath& targetFile)
{
	namespace fs = std::filesystem;

	const fs::path target(targetFile.str());
	const fs::path temporary = fs::path(target).concat(".sync.tmp");
	std::error_code ec;

	fs::create_directories(target.parent_path(), ec);
	if (!ec)
	{
		fs::copy_file(fs::path(sourceFile.str()), temporary, fs::copy_options::overwrite_existing, ec);
	}
	if (!ec)
	{
		fs::rename(temporary, target, ec);
	}
	if (ec)
	{
		std::error_code ignored;
		fs::remove(temporary, ignored);
		LOG_ERROR_STREAM(<< "Failed to write \"" << targetFile.str() << "\": " << ec.message());
		return false;
	}

	return true;
}

void syncFile(const FilePath& sourceFile, const FilePath& targetFile, DirectorySyncResult& result)
{
	if (targetFile.recheckExists() && hasEqualContent(sourceFile, targetFile))
	{
		result.unchanged++;
	}
	else if (replaceFile(sourceFile, targetFile))
	{
		result.written++;
	}
	else
	{
		result.failed++;
	}
}
}	 // namespace

DirectorySyncResult syncFiles(const std::vector<std::pair<FilePath, FilePath>>& sourceAndTargetFiles)
{
	DirectorySyncResult result;
	for (const auto& [sourceFile, targetFile]: sourceAndTargetFiles)
	{
		if (sourceFile.recheckExists())
		{
			syncFile(sourceFile, targetFile, result);
		}
		else
		{
			LOG_ERROR_STREAM(<< "Missing \"" << sourceFile.str() << "\"");
			result.failed++;
		}
	}
	return result;
}

DirectorySyncResult syncDirectoryContents(
	const FilePath& sourceDirectory, const FilePath& targetDirectory)
{
	DirectorySyncResult result;
	std::set<std::wstring> sourceRelativePaths;

	std::error_code ec;
	std::filesystem::create_directories(targetDirectory.str(), ec);
	// relative paths are taken between canonical paths, which need an up to date existence check
	targetDirectory.recheckExists();

	try
	{
		for (const FilePath& sourceFile: FileSystem::getFilePathsFromDirectory(sourceDirectory))
		{
			const FilePath relativePath = sourceFile.getRelativeTo(sourceDirectory);
			sourceRelativePaths.insert(relativePath.wstr());

			syncFile(sourceFile, targetDirectory.getConcatenated(relativePath), result);
		}

		for (const FilePath& targetFile: FileSystem::getFilePathsFromDirectory(targetDirectory))
		{
			if (sourceRelativePaths.count(targetFile.getRelativeTo(targetDirectory).wstr()) == 0)
			{
				if (FileSystem::remove(targetFile))
				{
					result.removed++;
				}
				else
				{
					LOG_ERROR_STREAM(<< "Failed to remove \"" << targetFile.str() << "\"");
					result.failed++;
				}
			}
		}
	}
	catch (const std::exception& e)
	{
		LOG_ERROR_STREAM(<< "Directory sync failed: " << e.what());
		result.failed++;
	}

	try
	{
		std::vector<FilePath> subDirectories = FileSystem::getRecursiveSubDirectories(targetDirectory);
		for (auto it = subDirectories.rbegin(); it != subDirectories.rend(); ++it)
		{
			// non-recursive remove fails on non-empty directories, which is what we want here
			FileSystem::remove(*it);
		}
	}
	catch (const std::exception& e)
	{
		LOG_ERROR_STREAM(<< "Directory cleanup failed: " << e.what());
		result.failed++;
	}

	return result;
}
}	 // namespace utility
