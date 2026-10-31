#include "StFiles.h"

#include <algorithm>
#include <map>
#include <set>

#include "FileInfo.h"
#include "NodeType.h"
#include "PersistentStorage.h"
#include "SourceLocation.h"
#include "SourceLocationFile.h"
#include "StLombok.h"

namespace st
{
namespace
{
const NodeKindMask NOT_LISTED = NODE_PACKAGE | NODE_FILE | NODE_TYPE_PARAMETER;

bool listed(PersistentStorage& storage, Id id)
{
	// Edge ids share the id space but have no definition kind.
	return storage.getDefinitionKindForNodeId(id) == DEFINITION_EXPLICIT &&
		!(storage.getNodeTypeForNodeWithId(id).getKind() & NOT_LISTED);
}

bool endsWithElement(const std::wstring& path, const std::wstring& suffix)
{
	return path.size() > suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0 &&
		path[path.size() - suffix.size() - 1] == L'/';
}
}

std::vector<Id> definedInFile(PersistentStorage& storage, const FilePath& indexedFile)
{
	std::map<Id, size_t> lines;
	std::set<Id> decided;
	storage.getSourceLocationsForFile(indexedFile)->forEachStartSourceLocation([&](SourceLocation* location) {
		for (const Id id: location->getTokenIds())
		{
			if (!decided.count(id))
			{
				if (!listed(storage, id))
				{
					decided.insert(id);
					continue;
				}
				// The name's line, not the scope's: a scope starts at the first annotation.
				auto it = lines.find(id);
				if (it == lines.end())
				{
					lines.emplace(id, location->getLineNumber());
				}
				else if (!location->isScopeLocation())
				{
					it->second = location->getLineNumber();
					decided.insert(id);
				}
			}
		}
	});
	std::vector<std::pair<size_t, Id>> ordered;
	for (const auto& [id, line]: lines)
	{
		ordered.emplace_back(line, id);
	}
	std::sort(ordered.begin(), ordered.end());
	std::vector<Id> ids;
	for (const auto& entry: ordered)
	{
		ids.push_back(entry.second);
	}
	return ids;
}

Id symbolAt(PersistentStorage& storage, const FilePath& indexedFile, size_t line)
{
	Id innermost = 0;
	size_t span = 0;
	size_t start = 0;
	std::set<Id> named;
	storage.getSourceLocationsForFile(indexedFile)->forEachStartSourceLocation([&](SourceLocation* location) {
		const size_t first = location->getLineNumber();
		if (!location->isScopeLocation())
		{
			if (first == line)
			{
				for (const Id id: location->getTokenIds())
				{
					if (listed(storage, id))
					{
						named.insert(id);
					}
				}
			}
			return;
		}
		const SourceLocation* end = location->getOtherLocation();
		const size_t last = end ? end->getLineNumber() : first;
		if (first > line || last < line)
		{
			return;
		}
		for (const Id id: location->getTokenIds())
		{
			if (listed(storage, id) && (!innermost || last - first < span || (last - first == span && first > start)))
			{
				innermost = id;
				span = last - first;
				start = first;
			}
		}
	});
	if (named.empty() || named.count(innermost))
	{
		return innermost;
	}
	return *named.begin();
}

std::vector<FilePath> matchIndexedFiles(PersistentStorage& storage, const std::wstring& path, const FilePath& cwd,
	const LombokRoots* roots)
{
	std::vector<FilePath> indexed;
	for (const FileInfo& file: storage.getFileInfoForAllFiles())
	{
		indexed.push_back(file.path);
	}
	std::vector<FilePath> matches;
	const auto add = [&](const FilePath& file) {
		if (std::find(matches.begin(), matches.end(), file) == matches.end())
		{
			matches.push_back(file);
		}
	};
	const FilePath given(path);
	const FilePath resolved = given.isAbsolute() ? given : cwd.getConcatenated(path);
	if (given.isAbsolute() || resolved.exists())
	{
		const std::wstring canonical = resolved.getCanonical().wstr();
		for (const FilePath& file: indexed)
		{
			if (file.wstr() == resolved.wstr() || file.wstr() == canonical)
			{
				add(file);
			}
		}
		if (const auto generated = roots ? toGeneratedFile(resolved, *roots) : std::nullopt)
		{
			for (const FilePath& file: indexed)
			{
				if (file == *generated)
				{
					add(file);
				}
			}
		}
		return matches;
	}
	std::wstring suffix = path;
	while (suffix.compare(0, 2, L"./") == 0)
	{
		suffix.erase(0, 2);
	}
	for (const FilePath& file: indexed)
	{
		const auto original = roots ? toOriginalFile(file, *roots) : std::nullopt;
		if (endsWithElement(file.wstr(), suffix) || (original && endsWithElement(original->wstr(), suffix)))
		{
			add(file);
		}
	}
	return matches;
}
}
