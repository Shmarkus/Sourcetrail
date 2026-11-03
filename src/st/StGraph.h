#ifndef ST_GRAPH_H
#define ST_GRAPH_H

#include <set>
#include <string>
#include <vector>

#include "Edge.h"
#include "FilePath.h"
#include "StGaps.h"
#include "StOutput.h"
#include "types.h"

class PersistentStorage;

namespace st {
enum class Direction { Incoming, Outgoing };

// Frontend: symbols declared in TypeScript files; backend: all others. Endpoint nodes are on neither side.
enum class Side { Any, Frontend, Backend };

struct QueryOptions {
  // Outgoing calls and supertypes: symbols without a definition (library methods and types) count too.
  bool library = false;
  Side side = Side::Any;
  // With Side::Frontend, symbols declared in *.spec.ts files stay.
  bool tests = false;
};

// Whether the symbol is an endpoint node, HTTP.<VERB> /<path>, that joins a frontend call to its Spring handler.
bool isEndpoint(PersistentStorage& storage, Id id);

// Lower-case link kind: call, usage, type usage, override, inheritance, annotation usage, ...
std::wstring linkKind(Edge::EdgeType type);

// Scope location when there is one, else the first token location.
// One line per file that declares the symbol (two modules may declare the same class), limited to `files` if given.
std::vector<ResultLine> definitionLines(PersistentStorage& storage,
                                        Id id,
                                        const std::wstring& key,
                                        const std::set<FilePath>* files = nullptr);

// One line per reference location, keyed by the using symbol; kind is the link kind ("call", "usage", "type usage", ...).
// `walked`, when given, receives what the query walked (for findGaps); likewise below.
std::vector<ResultLine> references(PersistentStorage& storage, const std::vector<Id>& symbols, Walked* walked = nullptr);

// Callers (Incoming) or callees (Outgoing) up to `depth` call levels (0 = unlimited); override hops are free and marked "via".
// Each line carries its symbol; an endpoint's kind is "endpoint".
std::vector<ResultLine> calls(PersistentStorage& storage,
                              const std::vector<Id>& symbols,
                              Direction direction,
                              size_t depth,
                              Walked* walked = nullptr,
                              const QueryOptions& options = {});

// Subtypes and overriding methods (Incoming) or supertypes and overridden methods (Outgoing), transitively. With
// `library`, supertypes without a definition count too, shown at the link that reaches them.
std::vector<ResultLine> hierarchy(PersistentStorage& storage,
                                  const std::vector<Id>& symbols,
                                  Direction direction,
                                  Walked* walked = nullptr,
                                  bool library = false);

struct PathHop {
  Id from;
  Id to;
  Edge::EdgeType type;
  FilePath file;
  size_t line;
  size_t column = 0;
};

// Simple chains over links of `types` from any of `from` to any of `to`, shortest first, at most `limit`.
// Vertical links (override, inheritance) are walked from the supertype down.
std::vector<std::vector<PathHop>> paths(PersistentStorage& storage,
                                        const std::vector<Id>& from,
                                        const std::vector<Id>& to,
                                        size_t depth,
                                        size_t limit,
                                        Edge::TypeMask types,
                                        Walked* walked = nullptr);

// Classes, interfaces and enums become their methods; other symbols stay.
std::vector<Id> expandClasses(PersistentStorage& storage, const std::vector<Id>& symbols);
}    // namespace st

#endif
