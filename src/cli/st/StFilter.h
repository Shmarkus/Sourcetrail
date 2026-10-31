#ifndef ST_FILTER_H
#define ST_FILTER_H

#include <string>
#include <vector>

#include "NodeKind.h"
#include "types.h"

class PersistentStorage;

namespace st
{
struct FilterOptions
{
	NodeKindMask kinds = ~NodeKindMask(0);
	std::wstring package;
	std::wstring annotation;
	std::wstring filePattern;
	bool invert = false;
};

// All given criteria must hold; `invert` negates the result. `displayedDefinitionPaths` are what the symbol's
// definition lines print, one per declaring file (none when it has no definition); the file pattern holds when it
// matches any of them.
bool matchesFilter(PersistentStorage& storage, Id id, const FilterOptions& options,
	const std::vector<std::wstring>& displayedDefinitionPaths);
}

#endif
