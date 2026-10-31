#ifndef ST_ARCHITECTURE_H
#define ST_ARCHITECTURE_H

#include <string>
#include <vector>

#include "Edge.h"
#include "StOutput.h"
#include "types.h"

class PersistentStorage;

namespace st
{
const Edge::TypeMask PATH_EDGES = Edge::EDGE_CALL | Edge::EDGE_USAGE | Edge::EDGE_OVERRIDE;
const Edge::TypeMask IMPACT_EDGES = Edge::EDGE_CALL | Edge::EDGE_USAGE | Edge::EDGE_TYPE_USAGE | Edge::EDGE_OVERRIDE |
	Edge::EDGE_INHERITANCE | Edge::EDGE_ANNOTATION_USAGE;

// Comma-separated link names (call, usage, type-usage, override, inheritance, import, annotation);
// throws std::invalid_argument naming the first unknown or empty entry.
Edge::TypeMask parseEdges(const std::wstring& list);

enum class Group
{
	None,
	File,
	Package
};

struct ImpactResult
{
	std::vector<ResultLine> lines;
	size_t symbols = 0;
	size_t files = 0;
	size_t packages = 0;
};

// Symbols a change to `symbols` may break: users over `types` links, subtypes, and overrides both ways
// (override hops are free, every other hop costs 1; `depth` 0 = unlimited). A type stands for itself and
// all its members. Each line shows the site of the link that first reached the symbol; Group::File and
// Group::Package print one line per file or package instead, keyed by its path or name.
ImpactResult impact(PersistentStorage& storage, const std::vector<Id>& symbols, size_t depth, Edge::TypeMask types,
	Group group);
}

#endif
