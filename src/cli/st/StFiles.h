#ifndef ST_FILES_H
#define ST_FILES_H

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "FilePath.h"
#include "StOutput.h"
#include "types.h"

class PersistentStorage;

namespace st
{
struct LombokRoots;

// Explicitly defined symbols whose definition is in the file, in line order; packages, files and type parameters are left out.
std::vector<Id> definedInFile(PersistentStorage& storage, const FilePath& indexedFile);

// One line per symbol: kind is "<access> <kind>" (just the kind without stored access), endLine the end of the scope.
std::vector<ResultLine> outline(PersistentStorage& storage, const std::vector<Id>& symbols);

// Classes, interfaces, enums, structs and unions.
bool isTypeSymbol(PersistentStorage& storage, Id id);

// Explicitly defined members one level below the type (nested types included, their members not), in line order.
std::vector<Id> directMembers(PersistentStorage& storage, Id type);

// The innermost defined symbol whose scope contains the line, unless another symbol is defined by a name on that
// line (a field in a class); 0 when there is none.
Id symbolAt(PersistentStorage& storage, const FilePath& indexedFile, size_t line);

// symbolAt for a 1-based line of the original of a delombok file. A line without a counterpart takes the symbol of
// the nearest lines above and below that have one, when both give the same; nullopt when nothing settles it.
std::optional<Id> symbolAtOriginal(PersistentStorage& storage, const FilePath& indexedFile, const FilePath& original,
	size_t line, const std::function<std::vector<std::string>(const FilePath&)>& readLines,
	const LombokRoots* cache = nullptr);

// The indexed files, with the original path of each delombok file, read once per invocation.
struct IndexedFiles
{
	std::vector<FilePath> files;
	std::vector<std::optional<FilePath>> originals;
};

IndexedFiles indexedFiles(PersistentStorage& storage, const LombokRoots* roots = nullptr);

// Indexed files a user-typed path denotes. An existing file (or an absolute path) must be indexed itself, or be
// the original of an indexed delombok file; any other path is a suffix match on a whole path element, against
// the indexed path and against the original path of delombok files.
std::vector<FilePath> matchIndexedFiles(const IndexedFiles& indexed, const std::wstring& path, const FilePath& cwd,
	const LombokRoots* roots = nullptr);
}

#endif
