#ifndef ST_GAPS_H
#define ST_GAPS_H

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
	std::vector<Scope> scopes;
	std::map<Id, Id> links;
};

struct Gaps
{
	size_t unresolvedLinks = 0;
	size_t errorFiles = 0;
};

// Followed links whose other end the index could not resolve, and incomplete files (files with index errors)
// holding a walked node's definition or a result location. A symbol is unresolved when it is the
// unsolved-symbol placeholder, or has no definition and is named inside a package the project defines types in
// (a Java record, a build-generated class), unless it is an implicit member of a defined type (a default
// constructor, an enum's valueOf).
Gaps findGaps(PersistentStorage& storage, const Walked& walked, const std::vector<ResultLine>& lines);

// Unresolved symbols in the whole index, by the rule of findGaps.
size_t countUnresolved(PersistentStorage& storage);

// `st: incomplete: <n> links to unresolved symbols, <m> files with index errors (see st status)` on `out`,
// leaving out a zero part; nothing when both are zero.
void printGaps(const Gaps& gaps, std::ostream& out);
}

#endif
