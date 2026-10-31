#ifndef UTILITY_DIRECTORY_SYNC_H
#define UTILITY_DIRECTORY_SYNC_H

#include <cstddef>
#include <utility>
#include <vector>

class FilePath;

namespace utility
{
struct DirectorySyncResult
{
	size_t written = 0;
	size_t unchanged = 0;
	size_t removed = 0;
	size_t failed = 0;
};

// Makes targetDirectory mirror sourceDirectory. Files with equal bytes are left untouched
// (mtime kept), changed or new files are copied, files missing from the source are removed
// together with directories left empty. Never throws; failures are logged and counted.
DirectorySyncResult syncDirectoryContents(
	const FilePath& sourceDirectory, const FilePath& targetDirectory);

// Copies each source file over its target file the same way, touching no other file. A missing
// source file counts as failed.
DirectorySyncResult syncFiles(const std::vector<std::pair<FilePath, FilePath>>& sourceAndTargetFiles);
}	 // namespace utility

#endif	  // UTILITY_DIRECTORY_SYNC_H
