#ifndef REFRESH_INFO_H
#define REFRESH_INFO_H

#include <set>

#include "FilePath.h"

enum RefreshMode
{
	REFRESH_NONE,
	REFRESH_UPDATED_FILES,
	REFRESH_UPDATED_AND_INCOMPLETE_FILES,
	REFRESH_ALL_FILES
};

struct RefreshInfo
{
	std::set<FilePath> filesToIndex;
	std::set<FilePath> filesToClear;
	std::set<FilePath> nonIndexedFilesToClear;

	// Files referencing what filesToCompareDeclarations declare. They are cleared and indexed in a second
	// pass, and only if indexing those files changed their declarations.
	std::set<FilePath> filesToCompareDeclarations;
	std::set<FilePath> dependentFilesToClear;
	std::set<FilePath> dependentFilesToIndex;

	RefreshMode mode = REFRESH_NONE;
	bool shallow = false;
};

#endif	  // REFRESH_INFO_H
