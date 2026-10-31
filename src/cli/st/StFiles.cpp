#include "StFiles.h"

#include <algorithm>
#include <map>
#include <set>

#include "AccessKind.h"
#include "FileInfo.h"
#include "Graph.h"
#include "Node.h"
#include "NodeType.h"
#include "PersistentStorage.h"
#include "SourceLocation.h"
#include "SourceLocationCollection.h"
#include "SourceLocationFile.h"
#include "StGraph.h"
#include "StKey.h"
#include "StLombok.h"
#include "utilityString.h"

namespace st
{
namespace
{
const NodeKindMask TYPES = NODE_CLASS | NODE_INTERFACE | NODE_ENUM | NODE_STRUCT | NODE_UNION;
const NodeKindMask NOT_LISTED = NODE_PACKAGE | NODE_FILE | NODE_TYPE_PARAMETER;

bool listed(PersistentStorage& storage, Id id)
{
	// Edge ids share the id space but have no definition kind.
	return storage.getDefinitionKindForNodeId(id) == DEFINITION_EXPLICIT &&
		!(storage.getNodeTypeForNodeWithId(id).getKind() & NOT_LISTED) && !isUnsolvedSymbol(storage, id);
}

// The file that defines the symbol: its scope's file, else the only file with a token for it, else (when
// references are recorded as tokens too) the file of its enclosing symbol.
FilePath definitionFile(PersistentStorage& storage, Id id)
{
	std::set<FilePath> scopes;
	std::set<FilePath> tokens;
	storage.getSourceLocationsForTokenIds({id})->forEachSourceLocation([&](SourceLocation* location) {
		(location->isScopeLocation() ? scopes : tokens).insert(location->getFilePath());
	});
	if (!scopes.empty())
	{
		return *scopes.begin();
	}
	if (tokens.size() == 1)
	{
		return *tokens.begin();
	}
	NameHierarchy name = storage.getNameHierarchyForNodeId(id);
	name.pop();
	const Id parent = name.size() ? storage.getNodeIdForNameHierarchy(name) : 0;
	return parent ? definitionFile(storage, parent) : FilePath();
}

// Scopes and name tokens; qualifiers (`Util` in `Util.format()`) carry the node id of a symbol that is only used.
bool definesNames(const SourceLocation& location)
{
	return location.isScopeLocation() || location.getType() == LOCATION_TOKEN;
}

bool endsWithElement(const std::wstring& path, const std::wstring& suffix)
{
	return path.size() > suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0 &&
		path[path.size() - suffix.size() - 1] == L'/';
}

// The number of trailing path elements two paths share.
size_t sharedTail(const std::wstring& a, const std::wstring& b)
{
	const std::vector<std::wstring> x = utility::splitToVector(a, L"/");
	const std::vector<std::wstring> y = utility::splitToVector(b, L"/");
	size_t shared = 0;
	while (shared < x.size() && shared < y.size() && !x[x.size() - 1 - shared].empty() &&
		   x[x.size() - 1 - shared] == y[y.size() - 1 - shared])
	{
		shared++;
	}
	return shared;
}
}

std::vector<Id> definedInFile(PersistentStorage& storage, const FilePath& indexedFile)
{
	std::map<Id, size_t> lines;
	std::set<Id> decided;
	storage.getSourceLocationsForFile(indexedFile)->forEachStartSourceLocation([&](SourceLocation* location) {
		if (!definesNames(*location))
		{
			return;
		}
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

std::vector<ResultLine> outline(
	PersistentStorage& storage, const std::vector<Id>& symbols, const std::map<Id, std::set<FilePath>>& inFiles)
{
	std::vector<ResultLine> lines;
	for (const Id id: symbols)
	{
		AccessKind access = storage.getAccessKindForNodeId(id);
		// Interface and annotation members are implicitly public, which the indexer records as default access.
		NameHierarchy owner = storage.getNameHierarchyForNodeId(id);
		owner.pop();
		const Id ownerId = owner.size() ? storage.getNodeIdForNameHierarchy(owner) : 0;
		if (ownerId && (storage.getNodeTypeForNodeWithId(ownerId).getKind() & (NODE_INTERFACE | NODE_ANNOTATION)))
		{
			access = ACCESS_NONE;
		}
		const auto restricted = inFiles.find(id);
		for (ResultLine& line: definitionLines(storage, id, symbolKey(storage.getNameHierarchyForNodeId(id)),
				 restricted == inFiles.end() ? nullptr : &restricted->second))
		{
			if (access != ACCESS_NONE)
			{
				line.kind = (access == ACCESS_DEFAULT ? L"package" : accessKindToString(access)) + L" " + line.kind;
			}
			storage.getSourceLocationsForTokenIds({id})->forEachSourceLocation([&](SourceLocation* location) {
				const SourceLocation* end = location->getOtherLocation();
				if (location->isScopeLocation() && location->isStartLocation() && end && location->getFilePath() == line.file)
				{
					line.endLine = end->getLineNumber();
				}
			});
			lines.push_back(std::move(line));
		}
	}
	return lines;
}

bool isTypeSymbol(PersistentStorage& storage, Id id)
{
	return storage.getNodeTypeForNodeWithId(id).getKind() & TYPES;
}

std::vector<Id> directMembers(PersistentStorage& storage, Id type)
{
	std::vector<std::pair<size_t, Id>> ordered;
	storage.getGraphForChildrenOfNodeId(type)->forEachNode([&](Node* node) {
		if (node->getId() != type && listed(storage, node->getId()))
		{
			const auto lines = definitionLines(storage, node->getId(), L"");
			ordered.emplace_back(lines.empty() ? 0 : lines.front().line, node->getId());
		}
	});
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
	std::set<Id> referenced;
	std::set<Id> referencing;
	storage.getSourceLocationsForFile(indexedFile)->forEachStartSourceLocation([&](SourceLocation* location) {
		const size_t first = location->getLineNumber();
		if (!definesNames(*location))
		{
			return;
		}
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
					else if (const StorageEdge edge = storage.getEdgeById(id); edge.id)
					{
						referenced.insert(edge.targetNodeId);
						referencing.insert(edge.sourceNodeId);
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
	// A reference to a symbol defined elsewhere is not a name on this line.
	for (auto it = named.begin(); it != named.end();)
	{
		// A name that a link on the line points to is a use, unless the line's own symbol is what uses it.
		const bool used = referenced.count(*it) && !referencing.count(*it);
		it = !used && definitionFile(storage, *it) == indexedFile ? std::next(it) : named.erase(it);
	}
	if (named.empty() || named.count(innermost))
	{
		return innermost;
	}
	return *named.begin();
}

std::optional<Id> symbolAtOriginal(PersistentStorage& storage, const FilePath& indexedFile, const FilePath& original,
	size_t line, const std::function<std::vector<std::string>(const FilePath&)>& readLines, const LombokRoots* cache)
{
	const std::vector<std::optional<size_t>> generated = originalToGenerated(original, indexedFile, readLines, cache);
	if (line == 0)
	{
		return std::nullopt;
	}
	if (line <= generated.size() && generated[line - 1])
	{
		return symbolAt(storage, indexedFile, *generated[line - 1] + 1);
	}
	// The alignment keeps the order of both files, so the line lies between the counterparts of its neighbours, and
	// belongs to their symbol when every generated line from one to the other does.
	std::optional<size_t> above;
	std::optional<size_t> below;
	for (size_t i = std::min(line - 1, generated.size()); i-- > 0 && !above;)
	{
		above = generated[i];
	}
	for (size_t i = line; i < generated.size() && !below; i++)
	{
		below = generated[i];
	}
	if (!above || !below)
	{
		return std::nullopt;
	}
	const Id id = symbolAt(storage, indexedFile, *above + 1);
	for (size_t g = *above + 1; id && g <= *below; g++)
	{
		if (symbolAt(storage, indexedFile, g + 1) != id)
		{
			return std::nullopt;
		}
	}
	return id ? std::optional<Id>(id) : std::nullopt;
}

IndexedFiles indexedFiles(PersistentStorage& storage, const LombokRoots* roots)
{
	IndexedFiles result;
	for (const FileInfo& file: storage.getFileInfoForAllFiles())
	{
		result.files.push_back(file.path);
		result.originals.push_back(roots ? toOriginalFile(file.path, *roots) : std::nullopt);
	}
	return result;
}

std::vector<FilePath> matchIndexedFiles(const IndexedFiles& indexed, const std::wstring& path, const FilePath& cwd,
	const LombokRoots* roots)
{
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
		const auto generated = roots ? toGeneratedFile(resolved, *roots) : std::nullopt;
		for (const FilePath& file: indexed.files)
		{
			if (file.wstr() == resolved.wstr() || file.wstr() == canonical || (generated && file == *generated))
			{
				add(file);
			}
		}
		if (!matches.empty() || resolved.exists())
		{
			return matches;
		}
		// A path from another machine (a CI or container stack trace): the files sharing the longest tail with it.
		size_t longest = 0;
		for (size_t i = 0; i < indexed.files.size(); i++)
		{
			const auto& original = indexed.originals[i];
			const size_t shared =
				std::max(sharedTail(indexed.files[i].wstr(), path), original ? sharedTail(original->wstr(), path) : 0);
			if (shared > longest)
			{
				longest = shared;
				matches.clear();
			}
			if (shared && shared == longest)
			{
				add(indexed.files[i]);
			}
		}
		return matches;
	}
	std::wstring suffix = path;
	while (suffix.compare(0, 2, L"./") == 0)
	{
		suffix.erase(0, 2);
	}
	for (size_t i = 0; i < indexed.files.size(); i++)
	{
		const auto& original = indexed.originals[i];
		if (endsWithElement(indexed.files[i].wstr(), suffix) || (original && endsWithElement(original->wstr(), suffix)))
		{
			add(indexed.files[i]);
		}
	}
	return matches;
}
}
