#include "StArchitecture.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

#include "DefinitionKind.h"
#include "NodeType.h"
#include "PersistentStorage.h"
#include "StGraphInternal.h"
#include "StKey.h"
#include "StorageEdge.h"
#include "utilityString.h"

namespace st
{
namespace
{
const NodeKindMask PACKAGES = NODE_PACKAGE | NODE_NAMESPACE | NODE_MODULE;
// Never results: files and packages are containers, type parameters and builtin types (void, int, ...) are noise.
const NodeKindMask NOT_RESULTS = NODE_FILE | PACKAGES | NODE_TYPE_PARAMETER | NODE_BUILTIN_TYPE;
const size_t UNREACHED = std::numeric_limits<size_t>::max();

struct Reach
{
	size_t cost = UNREACHED;
	std::vector<Id> edges;	// every link reaching the symbol at that cost
};

// A symbol reached by climbing from an override to the method it overrides doesn't walk back down:
// a change to one implementation doesn't affect its siblings.
struct State
{
	Id id;
	bool climbed;

	bool operator<(const State& other) const
	{
		return std::make_pair(id, climbed) < std::make_pair(other.id, other.climbed);
	}
};

class Walk
{
public:
	Walk(PersistentStorage& storage, size_t depth, Edge::TypeMask types)
		: m_storage(storage), m_depth(depth), m_types(types)
	{
	}

	std::map<Id, Reach> run(const std::set<Id>& inputs)
	{
		std::vector<State> level;
		for (const Id id: inputs)
		{
			level.push_back({id, false});
		}
		for (size_t cost = 0; !level.empty(); cost++)
		{
			std::vector<State> next;
			std::vector<State> current = startable(level);
			while (!current.empty())
			{
				current = step(current, cost, &next);
			}
			level = std::move(next);
		}
		return m_reached;
	}

private:
	// The states not walked yet, marked as walked.
	std::vector<State> startable(const std::vector<State>& states)
	{
		std::vector<State> result;
		for (const State& state: states)
		{
			const bool walked = m_walked.count({state.id, false}) || m_walked.count(state);
			if (!walked && walkable(state.id))
			{
				m_walked.insert(state);
				result.push_back(state);
			}
		}
		return result;
	}

	bool walkable(Id id)
	{
		auto it = m_walkable.find(id);
		if (it == m_walkable.end())
		{
			const bool container = m_storage.getNodeTypeForNodeWithId(id).getKind() & (NODE_FILE | PACKAGES);
			it = m_walkable.emplace(id, !container && !isUnsolvedSymbol(m_storage, id)).first;
		}
		return it->second;
	}

	void reach(Id id, Id edge, size_t cost, bool climbed, std::vector<State>* into)
	{
		Reach& known = m_reached[id];
		if (cost < known.cost)
		{
			known = {cost, {edge}};
		}
		else if (cost == known.cost)
		{
			known.edges.push_back(edge);
		}
		into->push_back({id, climbed});
	}

	// Walks every link out of `current`; returns the states reached for free, queues the others on `next`.
	std::vector<State> step(const std::vector<State>& current, size_t cost, std::vector<State>* next)
	{
		std::vector<Id> ids;
		std::set<Id> descending;
		for (const State& state: current)
		{
			ids.push_back(state.id);
			if (!state.climbed)
			{
				descending.insert(state.id);
			}
		}
		const bool deeper = !m_depth || cost < m_depth;
		std::vector<State> free;
		for (const StorageEdge& edge: m_storage.getEdgesForTargetIds(ids))
		{
			const Edge::EdgeType type = Edge::intToType(edge.type);
			if (!(type & m_types))
			{
				continue;
			}
			if (type == Edge::EDGE_OVERRIDE)
			{
				if (descending.count(edge.targetNodeId))
				{
					reach(edge.sourceNodeId, edge.id, cost, false, &free);
				}
			}
			else if (deeper)
			{
				reach(edge.sourceNodeId, edge.id, cost + 1, false, next);
			}
		}
		if (m_types & Edge::EDGE_OVERRIDE)
		{
			for (const StorageEdge& edge: m_storage.getEdgesForSourceIds(ids))
			{
				if (Edge::intToType(edge.type) == Edge::EDGE_OVERRIDE)
				{
					reach(edge.targetNodeId, edge.id, cost, true, &free);
				}
			}
		}
		return startable(free);
	}

	PersistentStorage& m_storage;
	const size_t m_depth;
	const Edge::TypeMask m_types;
	std::map<Id, Reach> m_reached;
	std::set<State> m_walked;
	std::map<Id, bool> m_walkable;
};

bool before(const Site& a, const Site& b)
{
	return std::make_pair(a.file.wstr(), a.line) < std::make_pair(b.file.wstr(), b.line);
}

// The nearest enclosing package node's name, else the qualified prefix before the top-level type.
class Packages
{
public:
	explicit Packages(PersistentStorage& storage): m_storage(storage) {}

	std::wstring of(const NameHierarchy& name)
	{
		for (size_t size = name.size(); size-- > 1;)
		{
			if (kind(name.getRange(0, size)) & PACKAGES)
			{
				return name.getRange(0, size).getQualifiedName();
			}
		}
		for (size_t size = 1; size < name.size(); size++)
		{
			if (kind(name.getRange(0, size)) & (TYPES | NODE_ANNOTATION))
			{
				return size > 1 ? name.getRange(0, size - 1).getQualifiedName() : L"";
			}
		}
		return name.size() > 1 ? name.getRange(0, name.size() - 1).getQualifiedName() : L"";
	}

	// The outermost prefix naming a type; empty when the name has none.
	NameHierarchy topLevelType(const NameHierarchy& name)
	{
		for (size_t size = 1; size <= name.size(); size++)
		{
			if (kind(name.getRange(0, size)) & (TYPES | NODE_ANNOTATION))
			{
				return name.getRange(0, size);
			}
		}
		return NameHierarchy(name.getDelimiter());
	}

private:
	NodeKind kind(const NameHierarchy& prefix)
	{
		const std::wstring serialized = NameHierarchy::serialize(prefix);
		auto it = m_kinds.find(serialized);
		if (it == m_kinds.end())
		{
			const Id id = m_storage.getNodeIdForNameHierarchy(prefix);
			it = m_kinds.emplace(serialized, id ? m_storage.getNodeTypeForNodeWithId(id).getKind() : NODE_SYMBOL).first;
		}
		return it->second;
	}

	PersistentStorage& m_storage;
	std::map<std::wstring, NodeKind> m_kinds;
};

// The input symbols plus what they own through member links; a package owns its sub-packages too.
std::set<Id> owned(PersistentStorage& storage, const std::vector<Id>& symbols)
{
	std::set<Id> all(symbols.begin(), symbols.end());
	std::vector<Id> owners;
	for (const Id id: symbols)
	{
		if (storage.getNodeTypeForNodeWithId(id).getKind() & (TYPES | NODE_ANNOTATION | NODE_PACKAGE))
		{
			owners.push_back(id);
		}
	}
	while (!owners.empty())
	{
		std::vector<Id> members;
		for (const StorageEdge& edge: storage.getEdgesForSourceIds(owners))
		{
			if (Edge::intToType(edge.type) == Edge::EDGE_MEMBER && all.insert(edge.targetNodeId).second)
			{
				members.push_back(edge.targetNodeId);
			}
		}
		owners = std::move(members);
	}
	return all;
}

// A package and its sub-packages; other symbols stand for themselves.
std::vector<Id> withSubPackages(PersistentStorage& storage, Id input)
{
	std::vector<Id> roots = {input};
	if (storage.getNodeTypeForNodeWithId(input).getKind() != NODE_PACKAGE)
	{
		return roots;
	}
	// The index lacks some package-to-sub-package member links, so sub-packages are found by name.
	const NameHierarchy name = storage.getNameHierarchyForNodeId(input);
	const std::wstring prefix = name.getQualifiedName() + name.getDelimiter();
	for (const StorageNode& node: storage.getStorageNodes())
	{
		if (intToNodeKind(node.type) == NODE_PACKAGE && node.id != input &&
			storage.getNameHierarchyForNodeId(node.id).getQualifiedName().compare(0, prefix.size(), prefix) == 0)
		{
			roots.push_back(node.id);
		}
	}
	return roots;
}

struct Affected
{
	Id id;
	NameHierarchy name;
	Site site;			// of the reaching link
	Site definition;	// empty when the symbol has no location
};

std::vector<ResultLine> grouped(PersistentStorage& storage, const std::map<std::wstring, std::vector<const Affected*>>& groups,
	const std::wstring& label)
{
	std::vector<ResultLine> lines;
	for (const auto& [key, members]: groups)
	{
		Site first;
		for (const Affected* affected: members)
		{
			if (!affected->definition.file.empty() && (first.file.empty() || before(affected->definition, first)))
			{
				first = affected->definition;
			}
		}
		lines.push_back(resultLine(storage, key, label + L", " + std::to_wstring(members.size()) + L" symbols", first));
	}
	return lines;
}
}

Edge::TypeMask parseEdges(const std::wstring& list)
{
	static const std::map<std::wstring, Edge::EdgeType> names = {
		{L"call", Edge::EDGE_CALL},
		{L"usage", Edge::EDGE_USAGE},
		{L"type-usage", Edge::EDGE_TYPE_USAGE},
		{L"override", Edge::EDGE_OVERRIDE},
		{L"inheritance", Edge::EDGE_INHERITANCE},
		{L"import", Edge::EDGE_IMPORT},
		{L"annotation", Edge::EDGE_ANNOTATION_USAGE}};
	Edge::TypeMask types = 0;
	for (const std::wstring& entry: utility::splitToVector(list, L","))
	{
		const std::wstring name = utility::trim(entry);
		const auto it = names.find(name);
		if (it == names.end())
		{
			throw std::invalid_argument(name.empty()
				? "empty --edges entry"
				: "unknown --edges entry '" + utility::encodeToUtf8(name) +
					"' (call, usage, type-usage, override, inheritance, import, annotation)");
		}
		types |= it->second;
	}
	return types;
}

ImpactResult impact(PersistentStorage& storage, const std::vector<Id>& symbols, size_t depth, Edge::TypeMask types,
	Group group)
{
	std::vector<Id> roots;
	for (const Id id: symbols)
	{
		const std::vector<Id> expanded = withSubPackages(storage, id);
		roots.insert(roots.end(), expanded.begin(), expanded.end());
	}
	const std::set<Id> inputs = owned(storage, roots);
	const std::map<Id, Reach> reached = Walk(storage, depth, types).run(inputs);

	std::vector<Affected> affected;
	std::vector<Id> edges;
	for (const auto& [id, reach]: reached)
	{
		if (inputs.count(id) || storage.getDefinitionKindForNodeId(id) == DEFINITION_NONE ||
			(storage.getNodeTypeForNodeWithId(id).getKind() & NOT_RESULTS))
		{
			continue;
		}
		NameHierarchy name = storage.getNameHierarchyForNodeId(id);
		if (isUnsolvedSymbol(name))
		{
			continue;
		}
		affected.push_back({id, std::move(name), {}, {}});
		edges.insert(edges.end(), reach.edges.begin(), reach.edges.end());
	}
	std::vector<Id> ids;
	for (const Affected& symbol: affected)
	{
		ids.push_back(symbol.id);
	}
	const std::map<Id, Site> linkSiteById = linkSites(storage, edges);
	const std::map<Id, Site> definitionById = definitionSites(storage, ids);

	ImpactResult result;
	std::set<std::wstring> files;
	std::map<std::wstring, std::vector<const Affected*>> byFile;
	std::map<std::wstring, std::vector<const Affected*>> byPackage;
	Packages packages(storage);
	for (Affected& symbol: affected)
	{
		// Ties between links reaching a symbol at the same cost go to the first site by path and line.
		for (const Id edge: reached.at(symbol.id).edges)
		{
			const auto site = linkSiteById.find(edge);
			if (site != linkSiteById.end() && (symbol.site.file.empty() || before(site->second, symbol.site)))
			{
				symbol.site = site->second;
			}
		}
		const auto definition = definitionById.find(symbol.id);
		if (definition != definitionById.end())
		{
			symbol.definition = definition->second;
		}
		if (symbol.site.file.empty())
		{
			symbol.site = symbol.definition;
		}
		const FilePath& file = symbol.definition.file.empty() ? symbol.site.file : symbol.definition.file;
		if (!file.empty())
		{
			files.insert(file.wstr());
			byFile[file.wstr()].push_back(&symbol);
		}
		byPackage[packages.of(symbol.name)].push_back(&symbol);
	}
	result.symbols = affected.size();
	result.files = files.size();
	result.packages = byPackage.size();

	if (group == Group::File)
	{
		result.lines = grouped(storage, byFile, L"file");
	}
	else if (group == Group::Package)
	{
		result.lines = grouped(storage, byPackage, L"package");
	}
	else
	{
		for (const Affected& symbol: affected)
		{
			if (symbol.site.file.empty())
			{
				continue;
			}
			const size_t depth = reached.at(symbol.id).cost;
			ResultLine line = resultLine(storage, symbolKey(symbol.name),
				storage.getNodeTypeForNodeWithId(symbol.id).getReadableTypeWString() + L", depth " + std::to_wstring(depth),
				symbol.site);
			// Under --limit the nearest rows stay.
			line.tier = static_cast<int>(depth);
			result.lines.push_back(std::move(line));
		}
	}
	return result;
}

std::vector<ResultLine> deps(PersistentStorage& storage, const std::vector<Id>& symbols, Direction direction)
{
	const Edge::TypeMask counted = ~Edge::TypeMask(Edge::EDGE_MEMBER | Edge::EDGE_IMPORT | Edge::EDGE_INCLUDE);
	Packages packages(storage);

	struct Row
	{
		std::wstring label;
		std::set<Id> edges;
	};
	std::map<std::wstring, Row> rows;
	for (const Id input: symbols)
	{
		const bool packageLevel = storage.getNodeTypeForNodeWithId(input).getKind() == NODE_PACKAGE;
		// The row an endpoint rolls up to; empty for the endpoints that are no dependency.
		const auto rowKey = [&](const NameHierarchy& name) {
			if (packageLevel)
			{
				return packages.of(name);
			}
			const NameHierarchy top = packages.topLevelType(name);
			return symbolKey(top.size() ? top : name);
		};
		const NameHierarchy inputName = storage.getNameHierarchyForNodeId(input);
		const std::wstring home = packageLevel ? inputName.getQualifiedName() : rowKey(inputName);

		// A package input covers its sub-packages.
		const auto inside = [&](const std::wstring& key) {
			return key == home ||
				(packageLevel && key.compare(0, home.size() + 1, home + inputName.getDelimiter()) == 0);
		};

		const std::set<Id> own = owned(storage, withSubPackages(storage, input));
		const std::vector<Id> ids(own.begin(), own.end());
		const std::vector<StorageEdge> edges = direction == Direction::Outgoing ? storage.getEdgesForSourceIds(ids)
																				: storage.getEdgesForTargetIds(ids);
		std::map<Id, std::wstring> keyById;
		for (const StorageEdge& edge: edges)
		{
			const Id other = direction == Direction::Outgoing ? edge.targetNodeId : edge.sourceNodeId;
			if (!(Edge::intToType(edge.type) & counted) || own.count(other))
			{
				continue;
			}
			auto it = keyById.find(other);
			if (it == keyById.end())
			{
				std::wstring key;
				const NameHierarchy name = storage.getNameHierarchyForNodeId(other);
				if (storage.getDefinitionKindForNodeId(other) != DEFINITION_NONE &&
					!(storage.getNodeTypeForNodeWithId(other).getKind() & (NOT_RESULTS & ~NODE_TYPE_PARAMETER)) && !isUnsolvedSymbol(name))
				{
					key = rowKey(name);
				}
				it = keyById.emplace(other, key).first;
			}
			if (!it->second.empty() && !inside(it->second))
			{
				Row& row = rows[it->second];
				row.label = packageLevel ? L"package" : L"class";
				row.edges.insert(edge.id);
			}
		}
	}

	std::vector<Id> edgeIds;
	for (const auto& [key, row]: rows)
	{
		edgeIds.insert(edgeIds.end(), row.edges.begin(), row.edges.end());
	}
	const std::map<Id, std::vector<Site>> sitesByEdge = allLinkSites(storage, edgeIds);

	std::vector<ResultLine> lines;
	for (const auto& [key, row]: rows)
	{
		size_t count = 0;
		Site first;
		for (const Id edge: row.edges)
		{
			const auto it = sitesByEdge.find(edge);
			count += it == sitesByEdge.end() ? 1 : it->second.size();
			if (it != sitesByEdge.end())
			{
				for (const Site& site: it->second)
				{
					if (first.file.empty() || before(site, first))
					{
						first = site;
					}
				}
			}
		}
		lines.push_back(resultLine(storage, key, row.label + L", " + std::to_wstring(count) + L" links", first));
	}
	return lines;
}
}
