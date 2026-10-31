#ifndef ST_FILES_H
#define ST_FILES_H

#include <string>
#include <vector>

#include "FilePath.h"
#include "types.h"

class PersistentStorage;

namespace st
{
struct LombokRoots;

// Explicitly defined symbols whose definition is in the file, in line order; packages, files and type parameters are left out.
std::vector<Id> definedInFile(PersistentStorage& storage, const FilePath& indexedFile);

// The innermost defined symbol whose scope contains the line, unless another symbol is defined by a name on that
// line (a field in a class); 0 when there is none.
Id symbolAt(PersistentStorage& storage, const FilePath& indexedFile, size_t line);

// Indexed files a user-typed path denotes. An existing file (or an absolute path) must be indexed itself, or be
// the original of an indexed delombok file; any other path is a suffix match on a whole path element, against
// the indexed path and against the original path of delombok files.
std::vector<FilePath> matchIndexedFiles(PersistentStorage& storage, const std::wstring& path, const FilePath& cwd,
	const LombokRoots* roots = nullptr);
}

#endif
