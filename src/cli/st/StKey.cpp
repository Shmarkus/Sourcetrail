#include "StKey.h"

#include <algorithm>
#include <set>

#include "DefinitionKind.h"

#include "NodeType.h"
#include "NodeTypeSet.h"
#include "PersistentStorage.h"
#include "SearchMatch.h"

namespace st
{
namespace
{
struct Candidate
{
	Id id;
	std::wstring key;
	std::wstring qualified;
	std::wstring delimiter;
	std::wstring bareKey;		// key and qualified name without type parameters
	std::wstring bareQualified;
};

// Removes every balanced `<...>` group, nested ones included.
std::wstring withoutTypeParameters(const std::wstring& text)
{
	std::wstring result;
	int depth = 0;
	for (const wchar_t c: text)
	{
		if (c == L'<') depth++;
		else if (c == L'>' && depth > 0) depth--;
		else if (depth == 0) result += c;
	}
	return result;
}

// Text before the parameter list of the last element, which is what the search index knows.
std::wstring searchText(const std::wstring& text)
{
	if (text.empty() || text.back() != L')')
	{
		return text;
	}
	int depth = 0;
	for (size_t i = text.size(); i-- > 0;)
	{
		if (text[i] == L')') depth++;
		else if (text[i] == L'(' && --depth == 0) return i ? text.substr(0, i) : text;
	}
	return text;
}

bool endsWithName(const std::wstring& full, const std::wstring& suffix, const std::wstring& delimiter)
{
	if (full.size() < suffix.size() || full.compare(full.size() - suffix.size(), suffix.size(), suffix) != 0)
	{
		return false;
	}
	return full.size() == suffix.size() ||
		full.compare(full.size() - suffix.size() - delimiter.size(), delimiter.size(), delimiter) == 0;
}

std::vector<Candidate> collect(PersistentStorage& storage, const std::wstring& query, NodeKindMask kinds)
{
	std::vector<Candidate> result;
	std::set<Id> seen;
	for (const auto& match: storage.getAutocompletionSymbolMatches(query, NodeTypeSet::all(), 1000, 0))
	{
		for (Id id: match.tokenIds)
		{
			if (!seen.insert(id).second || storage.getDefinitionKindForNodeId(id) == DEFINITION_NONE ||
				!(storage.getNodeTypeForNodeWithId(id).getKind() & kinds) || isUnsolvedSymbol(storage, id))
			{
				continue;
			}
			const NameHierarchy name = storage.getNameHierarchyForNodeId(id);
			if (name.size() == 0)
			{
				continue;
			}
			const std::wstring key = symbolKey(name);
			const std::wstring qualified = name.getQualifiedName();
			result.push_back({id, key, qualified, name.getDelimiter(), withoutTypeParameters(key),
				withoutTypeParameters(qualified)});
		}
	}
	return result;
}

Resolved pick(const std::vector<Candidate>& candidates, const std::wstring& text)
{
	const bool hasParameters = text.find(L'(') != std::wstring::npos;
	const std::wstring bare = withoutTypeParameters(text);
	std::vector<Id> ids;
	auto gather = [&](auto matches) {
		for (const auto& candidate: candidates)
		{
			if (matches(candidate)) ids.push_back(candidate.id);
		}
		return !ids.empty();
	};
	if (gather([&](const Candidate& c) { return c.key == text; }) ||
		gather([&](const Candidate& c) { return c.qualified == text; }) ||
		gather([&](const Candidate& c) { return (hasParameters ? c.bareKey : c.bareQualified) == bare; }) ||
		gather([&](const Candidate& c) {
			return endsWithName(hasParameters ? c.bareKey : c.bareQualified, bare, c.delimiter);
		}))
	{
		return {ids.size() == 1 ? Resolution::Found : Resolution::Ambiguous, ids};
	}
	return {Resolution::NotFound, {}};
}
}

bool isUnsolvedSymbol(const NameHierarchy& name)
{
	return name.size() == 1 && name.back().getName() == L"unsolved-symbol";
}

bool isUnsolvedSymbol(PersistentStorage& storage, Id id)
{
	return isUnsolvedSymbol(storage.getNameHierarchyForNodeId(id));
}

std::wstring symbolKey(const NameHierarchy& name)
{
	std::wstring key = name.getQualifiedName();
	if (name.size())
	{
		key += name.back().getSignature().getParameterString();
	}
	return key;
}

Resolved resolveSymbol(PersistentStorage& storage, const std::wstring& text, NodeKindMask kinds)
{
	const std::wstring query = searchText(text);
	Resolved resolved = pick(collect(storage, query, kinds), text);
	if (resolved.status == Resolution::NotFound && query != text)
	{
		resolved = pick(collect(storage, text, kinds), text);
	}
	return resolved;
}
}

namespace st
{
std::vector<RankedSymbol> rankSymbols(PersistentStorage& storage, const std::wstring& text, NodeKindMask kinds)
{
	auto lower = [](std::wstring s) {
		std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
		return s;
	};
	if (kinds & ~NODE_TYPE_PARAMETER)
	{
		// Type parameters (`failed<T>.T`) show up only when the mask is exactly the type-parameter kind (--kind takes one kind).
		kinds &= ~NODE_TYPE_PARAMETER;
	}
	const std::wstring bare = withoutTypeParameters(text);
	const std::wstring needle = lower(bare);
	std::vector<std::pair<int, Id>> ranked;
	for (const Candidate& candidate: collect(storage, text, kinds))
	{
		if (lower(candidate.bareQualified).find(needle) == std::wstring::npos)
		{
			continue;
		}
		int tier = 2;
		if (candidate.bareQualified == bare ||
			withoutTypeParameters(storage.getNameHierarchyForNodeId(candidate.id).back().getName()) == bare)
		{
			tier = 0;
		}
		else if (endsWithName(candidate.bareQualified, bare, candidate.delimiter) ||
			endsWithName(candidate.bareKey, bare, candidate.delimiter))
		{
			tier = 1;
		}
		ranked.emplace_back(tier, candidate.id);
	}
	std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	std::vector<RankedSymbol> result;
	for (const auto& entry: ranked)
	{
		result.push_back({entry.second, entry.first});
	}
	return result;
}

std::vector<Id> findSymbols(PersistentStorage& storage, const std::wstring& text, NodeKindMask kinds)
{
	std::vector<Id> ids;
	for (const auto& symbol: rankSymbols(storage, text, kinds))
	{
		ids.push_back(symbol.id);
	}
	return ids;
}
}
