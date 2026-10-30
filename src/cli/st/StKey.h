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

// Qualified name plus the last element's parameter string, without return type.
std::wstring symbolKey(const NameHierarchy& name);

// Exact key, then exact qualified name (overloads are ambiguous), then suffix match.
Resolved resolveSymbol(PersistentStorage& storage, const std::wstring& text, NodeKindMask kinds);
}

#endif
