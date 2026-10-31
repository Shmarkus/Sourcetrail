#ifndef ST_GAPS_H
#define ST_GAPS_H

#include <functional>
#include <map>
#include <ostream>
#include <set>
#include <vector>

#include "Edge.h"
#include "StOutput.h"
#include "types.h"

class PersistentStorage;

namespace st
{
// Links a command followed from some of the nodes it walked: links of `into` kinds ending at one of `from`,
// and links of `outOf` kinds starting at one.
struct Scope
{
	std::set<Id> from;
	Edge::TypeMask into = 0;
	Edge::TypeMask outOf = 0;
};

// What a command walked: its inputs plus every node reached, and the links it followed, either as scopes or,
// when the command fetched them already, one by one (link id to its end outside the walk).
struct Walked
{
	std::set<Id> nodes;
	// The inputs and the nodes the walk went on from; result rows it stopped at are only in `nodes`.
	std::set<Id> passed;
	std::vector<Scope> scopes;
	std::map<Id, Id> links;
};

struct Gaps
{
	size_t unresolvedLinks = 0;
	size_t indexErrors = 0;
	size_t multiDeclared = 0;	// walked symbols, or types enclosing one, that several files declare
};

// Followed links whose other end the index could not resolve, and index errors belonging to a walked symbol or to
// the symbol enclosing a result row. A symbol is unresolved when it is the unsolved-symbol placeholder, or has no
// definition and its package is one the project defines types in or declares below one (a Java record, a
// build-generated class), unless it is an implicit member of a defined type (a default constructor, an enum's
// valueOf). An error belongs to the innermost named symbol around it (anonymous and local classes skipped); at type
// level to the fields declared on its line, if any; outside every scope (an import) to the file's top-level types.
// A passed symbol or a type enclosing it that several files declare (a class two modules both have) is a gap too:
// users of the other copy may be bound to this one or missing.
Gaps findGaps(PersistentStorage& storage, const Walked& walked, const std::vector<ResultLine>& lines);

// Unresolved symbols in the whole index, by the rule of findGaps.
size_t countUnresolved(PersistentStorage& storage);

// `st: incomplete: <n> links to unresolved symbols, <m> index errors, <k> symbols declared in several modules
// (see st status)` on `out`, leaving out a zero part; nothing when all are zero.
void printGaps(const Gaps& gaps, std::ostream& out);

// printGaps of what `findGaps` returns; when it throws, `st: incomplete: gap check failed (<what>)` instead, since
// no line on stderr means the answer is complete.
void printGaps(const std::function<Gaps()>& findGaps, std::ostream& out);
}

#endif
