#include "StGraph.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <set>

#include "Graph.h"
#include "NodeType.h"
#include "PersistentStorage.h"
#include "SourceLocation.h"
#include "SourceLocationCollection.h"
#include "StGraphInternal.h"
#include "StKey.h"
#include "TextAccess.h"
#include "utilityString.h"

namespace st
{
namespace
{
const Edge::TypeMask CALL_EDGES = Edge::EDGE_CALL | Edge::EDGE_USAGE | Edge::EDGE_OVERRIDE;
const NodeKindMask CALLABLE = NODE_METHOD | NODE_FUNCTION;
const size_t UNREACHED = std::numeric_limits<size_t>::max();
const size_t MAX_PATH_PAIRS = 200;
const size_t MAX_PATH_EXPANSIONS = 200000;

std::wstring key(PersistentStorage& storage, Id id)
{
	return symbolKey(storage.getNameHierarchyForNodeId(id));
}

bool isKind(const Node* node, NodeKindMask kinds)
{
	return node && (node->getType().getKind() & kinds);
}

struct Hop
{
	Id to;
	const Edge* edge;
};

// Neighbours in the walking direction: Outgoing follows calls forward and overrides from the
// overridden method down to its overrides; Incoming is the reverse.
std::map<Id, std::vector<Hop>> adjacency(const Graph& graph, Edge::TypeMask types, Direction direction)
{
	std::map<Id, std::vector<Hop>> hops;
	graph.forEachEdge([&](Edge* edge) {
		if (!edge->isType(types))
		{
			return;
		}
		const bool against = (direction == Direction::Incoming) != edge->isType(Edge::LAYOUT_VERTICAL);
		const Id from = against ? edge->getTo()->getId() : edge->getFrom()->getId();
		const Id to = against ? edge->getFrom()->getId() : edge->getTo()->getId();
		hops[from].push_back({to, edge});
	});
	return hops;
}

struct Reached
{
	size_t cost = UNREACHED;
	Id via = 0;
	Id siteEdge = 0;
};

// 0-1 BFS over one trail graph: call and usage hops cost 1, override hops are free.
std::map<Id, Reached> walkCalls(const Graph& graph, Id start, Direction direction, size_t depth)
{
	const auto hops = adjacency(graph, CALL_EDGES, direction);
	std::map<Id, Reached> best;
	std::map<Id, Reached> printable;
	std::deque<Id> queue = {start};
	best[start].cost = 0;
	while (!queue.empty())
	{
		const Id current = queue.front();
		queue.pop_front();
		if (current != start && !isKind(graph.getNodeById(current), CALLABLE))
		{
			continue;
		}
		const auto it = hops.find(current);
		if (it == hops.end())
		{
			continue;
		}
		const Reached here = best[current];
		for (const Hop& hop: it->second)
		{
			const bool isOverride = hop.edge->isType(Edge::EDGE_OVERRIDE);
			Reached next;
			next.cost = here.cost + (isOverride ? 0 : 1);
			if (depth && next.cost > depth)
			{
				continue;
			}
			if (direction == Direction::Incoming)
			{
				next.via = isOverride ? hop.to : here.via;
				next.siteEdge = isOverride ? here.siteEdge : hop.edge->getId();
			}
			else
			{
				next.via = isOverride ? current : 0;
				next.siteEdge = isOverride ? here.siteEdge : hop.edge->getId();
			}
			if (!(direction == Direction::Incoming && isOverride) && next.cost < printable[hop.to].cost)
			{
				printable[hop.to] = next;
			}
			Reached& known = best[hop.to];
			if (next.cost < known.cost)
			{
				known = next;
				if (isOverride)
				{
					queue.push_front(hop.to);
				}
				else
				{
					queue.push_back(hop.to);
				}
			}
		}
	}
	for (auto it = printable.begin(); it != printable.end();)
	{
		// A field whose initializer calls the symbol is a caller; it is reported but never walked further.
		const NodeKindMask reported = direction == Direction::Incoming ? CALLABLE | NODE_FIELD : CALLABLE;
		it = isKind(graph.getNodeById(it->first), reported) ? std::next(it) : printable.erase(it);
	}
	return printable;
}

// Simple paths from origin to target inside a trail graph, breadth-first.
void collectPaths(const Graph& graph, Id origin, Id target, size_t depth, size_t limit, Edge::TypeMask types,
	std::vector<std::vector<const Edge*>>* found)
{
	const auto hops = adjacency(graph, types, Direction::Outgoing);
	struct Partial
	{
		Id at;
		std::vector<const Edge*> edges;
		std::set<Id> visited;
	};
	std::deque<Partial> queue = {{origin, {}, {origin}}};
	size_t expansions = 0;
	while (!queue.empty() && found->size() < limit && expansions++ < MAX_PATH_EXPANSIONS)
	{
		Partial partial = std::move(queue.front());
		queue.pop_front();
		const auto it = hops.find(partial.at);
		if (it == hops.end() || (depth && partial.edges.size() >= depth))
		{
			continue;
		}
		for (const Hop& hop: it->second)
		{
			if (partial.visited.count(hop.to))
			{
				continue;
			}
			Partial next{hop.to, partial.edges, partial.visited};
			next.edges.push_back(hop.edge);
			next.visited.insert(hop.to);
			if (hop.to == target)
			{
				found->push_back(next.edges);
				if (found->size() >= limit)
				{
					return;
				}
			}
			else
			{
				queue.push_back(std::move(next));
			}
		}
	}
}
}

std::wstring linkKind(Edge::EdgeType type)
{
	std::wstring kind = Edge::getReadableTypeString(type);
	if (utility::isPostfix<std::wstring>(L"use", kind))
	{
		kind.replace(kind.size() - 3, 3, L"usage");
	}
	return kind;
}

std::string sourceText(PersistentStorage& storage, const Site& site)
{
	const auto content = storage.getFileContent(site.file, false);
	if (content && site.line >= 1 && site.line <= content->getLineCount())
	{
		return content->getLine(static_cast<unsigned int>(site.line));
	}
	return "";
}

ResultLine resultLine(PersistentStorage& storage, const std::wstring& key, const std::wstring& kind, const Site& site)
{
	ResultLine line{key, kind, site.file, site.line, sourceText(storage, site)};
	line.column = site.column;
	return line;
}

Site linkSite(PersistentStorage& storage, Id edgeId)
{
	const auto sites = linkSites(storage, {edgeId});
	return sites.empty() ? Site() : sites.begin()->second;
}

std::map<Id, std::vector<Site>> allLinkSites(PersistentStorage& storage, const std::vector<Id>& edgeIds)
{
	std::map<Id, std::vector<Site>> sites;
	if (edgeIds.empty())
	{
		return sites;
	}
	const std::set<Id> wanted(edgeIds.begin(), edgeIds.end());
	storage.getSourceLocationsForTokenIds(edgeIds)->forEachSourceLocation([&](SourceLocation* location) {
		if (!location->isStartLocation())
		{
			return;
		}
		for (const Id id: location->getTokenIds())
		{
			if (wanted.count(id))
			{
				sites[id].push_back({location->getFilePath(), location->getLineNumber(), location->getColumnNumber()});
			}
		}
	});
	return sites;
}

std::map<Id, Site> linkSites(PersistentStorage& storage, const std::vector<Id>& edgeIds)
{
	std::map<Id, Site> first;
	for (const auto& [id, sites]: allLinkSites(storage, edgeIds))
	{
		first[id] = *std::min_element(sites.begin(), sites.end(), [](const Site& a, const Site& b) {
			return std::make_pair(a.file.wstr(), a.line) < std::make_pair(b.file.wstr(), b.line);
		});
	}
	return first;
}

std::map<Id, Site> definitionSites(PersistentStorage& storage, const std::vector<Id>& ids)
{
	std::map<Id, Site> sites;
	if (ids.empty())
	{
		return sites;
	}
	const std::set<Id> wanted(ids.begin(), ids.end());
	// The name's line, not the scope's: a scope starts at the first annotation.
	std::map<Id, bool> isScope;
	storage.getSourceLocationsForTokenIds(ids)->forEachSourceLocation([&](SourceLocation* location) {
		if (!location->isStartLocation())
		{
			return;
		}
		for (const Id id: location->getTokenIds())
		{
			if (wanted.count(id) && (!sites.count(id) || (!location->isScopeLocation() && isScope[id])))
			{
				sites[id] = {location->getFilePath(), location->getLineNumber(), location->getColumnNumber()};
				isScope[id] = location->isScopeLocation();
			}
		}
	});
	return sites;
}

std::vector<ResultLine> definitionLines(PersistentStorage& storage, Id id, const std::wstring& key)
{
	const auto sites = definitionSites(storage, {id});
	if (sites.empty())
	{
		return {};
	}
	return {resultLine(storage, key, storage.getNodeTypeForNodeWithId(id).getReadableTypeWString(), sites.begin()->second)};
}

std::vector<ResultLine> references(PersistentStorage& storage, const std::vector<Id>& symbols)
{
	std::vector<ResultLine> lines;
	for (const Id id: symbols)
	{
		const Edge::TypeMask types = ~Edge::TypeMask(Edge::EDGE_MEMBER | Edge::EDGE_IMPORT);
		// A directed trail walks vertical links (inheritance, override) against the search direction.
		std::map<Id, Edge*> edges;
		const auto incoming = storage.getGraphForTrail(0, id, ~NodeKindMask(0), types, true, 1, true);
		const auto vertical = storage.getGraphForTrail(id, 0, ~NodeKindMask(0), types & Edge::LAYOUT_VERTICAL, true, 1, true);
		for (const auto& graph: {incoming, vertical})
		{
			graph->forEachEdge([&](Edge* edge) { edges.emplace(edge->getId(), edge); });
		}
		for (const auto& [edgeId, edge]: edges)
		{
			if (edge->getTo()->getId() != id || edge->isType(Edge::EDGE_MEMBER | Edge::EDGE_IMPORT))
			{
				continue;
			}
			if (isUnsolvedSymbol(edge->getFrom()->getNameHierarchy()))
			{
				continue;
			}
			const std::wstring user = symbolKey(edge->getFrom()->getNameHierarchy());
			storage.getSourceLocationsForTokenIds({edge->getId()})->forEachSourceLocation([&](SourceLocation* location) {
				if (location->isStartLocation())
				{
					const Site site{location->getFilePath(), location->getLineNumber(), location->getColumnNumber()};
					lines.push_back(resultLine(storage, user, linkKind(edge->getType()), site));
				}
			});
		}
	}
	return lines;
}

std::vector<ResultLine> calls(PersistentStorage& storage, const std::vector<Id>& symbols, Direction direction, size_t depth)
{
	const std::set<Id> inputs(symbols.begin(), symbols.end());
	std::map<Id, Reached> results;
	for (const Id id: symbols)
	{
		const auto graph = direction == Direction::Incoming
			? storage.getGraphForTrail(0, id, ~NodeKindMask(0), CALL_EDGES, false, depth * 3, true)
			: storage.getGraphForTrail(id, 0, ~NodeKindMask(0), CALL_EDGES, false, depth * 3, true);
		for (const auto& [node, reached]: walkCalls(*graph, id, direction, depth))
		{
			if (!inputs.count(node) && reached.cost < results[node].cost)
			{
				results[node] = reached;
			}
		}
	}
	std::vector<ResultLine> lines;
	for (const auto& [node, reached]: results)
	{
		if (isUnsolvedSymbol(storage, node))
		{
			continue;
		}
		std::wstring kind = storage.getNodeTypeForNodeWithId(node).getReadableTypeWString();
		if (reached.via)
		{
			kind += L" via " + key(storage, reached.via);
		}
		if (reached.siteEdge)
		{
			lines.push_back(resultLine(storage, key(storage, node), kind, linkSite(storage, reached.siteEdge)));
		}
		else
		{
			for (ResultLine& line: definitionLines(storage, node, key(storage, node)))
			{
				line.kind = kind;
				lines.push_back(std::move(line));
			}
		}
	}
	return lines;
}

std::vector<ResultLine> hierarchy(PersistentStorage& storage, const std::vector<Id>& symbols, Direction direction)
{
	const std::set<Id> inputs(symbols.begin(), symbols.end());
	std::set<Id> results;
	for (const Id id: symbols)
	{
		const Edge::TypeMask types = (storage.getNodeTypeForNodeWithId(id).getKind() & TYPES)
			? Edge::EDGE_INHERITANCE
			: Edge::EDGE_OVERRIDE;
		const auto graph = direction == Direction::Incoming
			? storage.getGraphForTrail(id, 0, ~NodeKindMask(0), types, false, 0, true)
			: storage.getGraphForTrail(0, id, ~NodeKindMask(0), types, false, 0, true);
		// Walking down means from supertype to subtype, i.e. against the stored link.
		const auto hops = adjacency(*graph, types, direction == Direction::Incoming ? Direction::Outgoing : Direction::Incoming);
		std::set<Id> seen = {id};
		std::deque<Id> queue = {id};
		while (!queue.empty())
		{
			const auto it = hops.find(queue.front());
			queue.pop_front();
			if (it == hops.end())
			{
				continue;
			}
			for (const Hop& hop: it->second)
			{
				if (seen.insert(hop.to).second)
				{
					queue.push_back(hop.to);
					if (!inputs.count(hop.to))
					{
						results.insert(hop.to);
					}
				}
			}
		}
	}
	std::vector<ResultLine> lines;
	for (const Id id: results)
	{
		if (isUnsolvedSymbol(storage, id))
		{
			continue;
		}
		for (ResultLine& line: definitionLines(storage, id, key(storage, id)))
		{
			lines.push_back(std::move(line));
		}
	}
	return lines;
}

std::vector<std::vector<PathHop>> paths(PersistentStorage& storage, const std::vector<Id>& from,
	const std::vector<Id>& to, size_t depth, size_t limit, Edge::TypeMask types)
{
	std::vector<std::vector<PathHop>> chains;
	size_t pairs = 0;
	for (const Id origin: from)
	{
		for (const Id target: to)
		{
			if (origin == target || pairs++ >= MAX_PATH_PAIRS)
			{
				continue;
			}
			const auto graph = storage.getGraphForTrail(origin, target, ~NodeKindMask(0), types, false, depth, true);
			std::vector<std::vector<const Edge*>> found;
			collectPaths(*graph, origin, target, depth, limit, types, &found);
			for (const auto& edges: found)
			{
				std::vector<PathHop> chain;
				bool solved = true;
				for (const Edge* edge: edges)
				{
					const bool down = edge->isType(Edge::LAYOUT_VERTICAL);
					const Id hopFrom = chain.empty() ? origin : chain.back().to;
					const Id hopTo = down ? edge->getFrom()->getId() : edge->getTo()->getId();
					Site site = linkSite(storage, edge->getId());
					if (site.file.empty())
					{
						for (const ResultLine& line: definitionLines(storage, hopTo, L""))
						{
							site = {line.file, line.line, line.column};
						}
					}
					solved = solved && !isUnsolvedSymbol(storage, hopTo);
					chain.push_back({hopFrom, hopTo, edge->getType(), site.file, site.line, site.column});
				}
				if (solved)
				{
					chains.push_back(std::move(chain));
				}
			}
		}
	}
	std::stable_sort(chains.begin(), chains.end(), [](const auto& a, const auto& b) { return a.size() < b.size(); });
	if (chains.size() > limit)
	{
		chains.resize(limit);
	}
	return chains;
}

std::vector<Id> expandClasses(PersistentStorage& storage, const std::vector<Id>& symbols)
{
	std::vector<Id> expanded;
	for (const Id id: symbols)
	{
		if (!(storage.getNodeTypeForNodeWithId(id).getKind() & TYPES))
		{
			expanded.push_back(id);
			continue;
		}
		storage.getGraphForChildrenOfNodeId(id)->forEachNode([&](Node* node) {
			if (node->getId() != id && isKind(node, CALLABLE) && !isUnsolvedSymbol(node->getNameHierarchy()))
			{
				expanded.push_back(node->getId());
			}
		});
	}
	return expanded;
}
}
