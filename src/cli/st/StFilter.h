#ifndef ST_FILTER_H
#define ST_FILTER_H

#include <string>

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

// All given criteria must hold; `invert` negates the result. `displayedDefinitionPath` is what a
// definition line prints for the symbol ("" when it has none).
bool matchesFilter(PersistentStorage& storage, Id id, const FilterOptions& options,
	const std::wstring& displayedDefinitionPath);
}

#endif
