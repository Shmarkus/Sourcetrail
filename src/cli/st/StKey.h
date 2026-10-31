#ifndef ST_KEY_H
#define ST_KEY_H

#include <string>
#include <vector>

#include "NameHierarchy.h"
#include "NodeKind.h"
#include "types.h"

class PersistentStorage;

namespace st
{
enum class Resolution
{
	Found,
	Ambiguous,
	NotFound
};

struct Resolved
{
	Resolution status;
	std::vector<Id> ids;
};

// The Java indexer's placeholder for a reference it could not resolve; never a real symbol.
bool isUnsolvedSymbol(const NameHierarchy& name);
bool isUnsolvedSymbol(PersistentStorage& storage, Id id);

// Qualified name plus the last element's parameter string, without return type.
std::wstring symbolKey(const NameHierarchy& name);

// Exact key, then exact qualified name (overloads are ambiguous), then suffix match.
Resolved resolveSymbol(PersistentStorage& storage, const std::wstring& text, NodeKindMask kinds);

struct RankedSymbol
{
	Id id;
	int tier;	// 0 exact, 1 suffix, 2 other substring
};

// Defined (non-external) symbols whose qualified name contains `text` case-insensitively,
// ordered exact, then suffix, then other substring matches.
std::vector<RankedSymbol> rankSymbols(PersistentStorage& storage, const std::wstring& text, NodeKindMask kinds);
std::vector<Id> findSymbols(PersistentStorage& storage, const std::wstring& text, NodeKindMask kinds);

}

#endif
