#include "StGaps.h"

#include <map>
#include <memory>
#include <string>

#include "DefinitionKind.h"
#include "NameHierarchy.h"
#include "NodeKind.h"
#include "PersistentStorage.h"
#include "SourceLocation.h"
#include "SourceLocationCollection.h"
#include "SourceLocationFile.h"
#include "StGraphInternal.h"
#include "StKey.h"
#include "StorageEdge.h"
#include "StorageNode.h"

namespace st
{
namespace
{
const NodeKindMask CONTAINERS = NODE_FILE | NODE_PACKAGE | NODE_NAMESPACE | NODE_MODULE;
// Never a gap: containers, type parameters and builtin types.
const NodeKindMask NEVER_UNRESOLVED = CONTAINERS | NODE_TYPE_PARAMETER | NODE_BUILTIN_TYPE;

// Packages the project defines a type in.
std::set<std::wstring> ownPackages(PersistentStorage& storage)
{
	std::set<std::wstring> packages;
	std::vector<NameHierarchy> types;
	for (const StorageNode& node: storage.getStorageNodes())
	{
		const NodeKind kind = intToNodeKind(node.type);
		if (kind & CONTAINERS & ~NODE_FILE)
		{
			packages.insert(NameHierarchy::deserialize(node.serializedName).getQualifiedName());
		}
		else if ((kind & (TYPES | NODE_ANNOTATION)) && storage.getDefinitionKindForNodeId(node.id) != DEFINITION_NONE)
		{
			types.push_back(NameHierarchy::deserialize(node.serializedName));
		}
	}
	std::set<std::wstring> own;
	for (const NameHierarchy& type: types)
	{
		const std::wstring package = type.size() > 1 ? type.getRange(0, type.size() - 1).getQualifiedName() : L"";
		if (packages.count(package))
		{
			own.insert(package);
		}
	}
	return own;
}

class Unresolved
{
public:
	explicit Unresolved(PersistentStorage& storage)
		: m_storage(storage)
		, m_placeholder(storage.getNodeIdForNameHierarchy(NameHierarchy(L"unsolved-symbol", NAME_DELIMITER_JAVA)))
	{
	}

	// The unresolved ones of `ids`, looked up together.
	std::set<Id> of(const std::vector<Id>& ids)
	{
		std::set<Id> unresolved;
		std::vector<Id> undefined;
		for (const Id id: ids)
		{
			if (id == m_placeholder)
			{
				unresolved.insert(id);
			}
			else if (m_storage.getDefinitionKindForNodeId(id) == DEFINITION_NONE)
			{
				undefined.push_back(id);
			}
		}
		for (const StorageNode& node: m_storage.getStorageNodesForIds(undefined))
		{
			if (is(node))
			{
				unresolved.insert(node.id);
			}
		}
		return unresolved;
	}

	bool is(const StorageNode& node)
	{
		if (node.id == m_placeholder)
		{
			return true;
		}
		const NodeKind kind = intToNodeKind(node.type);
		if ((kind & NEVER_UNRESOLVED) || m_storage.getDefinitionKindForNodeId(node.id) != DEFINITION_NONE)
		{
			return false;
		}
		const NameHierarchy name = NameHierarchy::deserialize(node.serializedName);
		if (isUnsolvedSymbol(name))
		{
			return true;
		}
		if (!inOwnPackage(name))
		{
			return false;
		}
		if (kind & (TYPES | NODE_ANNOTATION | NODE_SYMBOL))
		{
			return true;
		}
		// A member of a defined type without a definition is implicit: a default constructor, an enum's valueOf.
		const Id parent = m_storage.getNodeIdForNameHierarchy(name.getRange(0, name.size() - 1));
		return !parent || m_storage.getDefinitionKindForNodeId(parent) == DEFINITION_NONE;
	}

private:
	bool inOwnPackage(const NameHierarchy& name)
	{
		if (!m_ownPackages)
		{
			m_ownPackages = std::make_unique<std::set<std::wstring>>(ownPackages(m_storage));
		}
		for (size_t size = 1; size < name.size(); size++)
		{
			if (m_ownPackages->count(name.getRange(0, size).getQualifiedName()))
			{
				return true;
			}
		}
		return false;
	}

	PersistentStorage& m_storage;
	const Id m_placeholder;
	std::unique_ptr<std::set<std::wstring>> m_ownPackages;
};

bool definesNames(const SourceLocation& location)
{
	return location.isScopeLocation() || location.getType() == LOCATION_TOKEN;
}

size_t errorFiles(PersistentStorage& storage, const std::set<Id>& nodes, const std::vector<ResultLine>& lines)
{
	const std::set<FilePath> incomplete = storage.getIncompleteFiles();
	if (incomplete.empty())
	{
		return 0;
	}
	std::set<FilePath> touched;
	for (const ResultLine& line: lines)
	{
		touched.insert(line.file);
	}
	std::vector<Id> defined;
	for (const Id id: nodes)
	{
		if (storage.getDefinitionKindForNodeId(id) != DEFINITION_NONE)
		{
			defined.push_back(id);
		}
	}
	// The definitions of a few symbols, or what the incomplete files define for a wide walk: reading the
	// locations of thousands of symbols costs more than reading a few dozen files.
	if (defined.size() >= 10 * incomplete.size())
	{
		for (const FilePath& file: incomplete)
		{
			storage.getSourceLocationsForFile(file)->forEachStartSourceLocation([&](SourceLocation* location) {
				for (const Id id: location->getTokenIds())
				{
					if (definesNames(*location) && nodes.count(id))
					{
						touched.insert(file);
					}
				}
			});
		}
	}
	else if (!defined.empty())
	{
		storage.getSourceLocationsForTokenIds(defined)->forEachSourceLocation([&](SourceLocation* location) {
			if (location->isStartLocation() && definesNames(*location))
			{
				touched.insert(location->getFilePath());
			}
		});
	}
	size_t count = 0;
	for (const FilePath& file: touched)
	{
		count += incomplete.count(file);
	}
	return count;
}
}

Gaps findGaps(PersistentStorage& storage, const Walked& walked, const std::vector<ResultLine>& lines)
{
	std::map<Id, Id> links = walked.links;
	for (const Scope& scope: walked.scopes)
	{
		const std::vector<Id> from(scope.from.begin(), scope.from.end());
		if (from.empty())
		{
			continue;
		}
		if (scope.into)
		{
			for (const StorageEdge& edge: storage.getEdgesForTargetIds(from))
			{
				if (Edge::intToType(edge.type) & scope.into)
				{
					links.emplace(edge.id, edge.sourceNodeId);
				}
			}
		}
		if (scope.outOf)
		{
			for (const StorageEdge& edge: storage.getEdgesForSourceIds(from))
			{
				if (Edge::intToType(edge.type) & scope.outOf)
				{
					links.emplace(edge.id, edge.targetNodeId);
				}
			}
		}
	}
	std::set<Id> ends;
	for (const auto& [edge, end]: links)
	{
		ends.insert(end);
	}
	const std::set<Id> unresolved = Unresolved(storage).of(std::vector<Id>(ends.begin(), ends.end()));
	Gaps gaps;
	for (const auto& [edge, end]: links)
	{
		gaps.unresolvedLinks += unresolved.count(end);
	}
	gaps.errorFiles = errorFiles(storage, walked.nodes, lines);
	return gaps;
}

size_t countUnresolved(PersistentStorage& storage)
{
	Unresolved unresolved(storage);
	size_t count = 0;
	// A copy: classifying may read all nodes again, which replaces the vector getStorageNodes returns.
	const std::vector<StorageNode> nodes = storage.getStorageNodes();
	for (const StorageNode& node: nodes)
	{
		count += unresolved.is(node);
	}
	return count;
}

void printGaps(const Gaps& gaps, std::ostream& out)
{
	if (!gaps.unresolvedLinks && !gaps.errorFiles)
	{
		return;
	}
	out << "st: incomplete: ";
	if (gaps.unresolvedLinks)
	{
		out << gaps.unresolvedLinks << " links to unresolved symbols" << (gaps.errorFiles ? ", " : "");
	}
	if (gaps.errorFiles)
	{
		out << gaps.errorFiles << " files with index errors";
	}
	out << " (see st status)\n";
}
}
