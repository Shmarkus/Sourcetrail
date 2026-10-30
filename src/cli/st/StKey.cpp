#include "StKey.h"

#include <algorithm>
#include <set>

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
};

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
			if (!seen.insert(id).second || !(storage.getNodeTypeForNodeWithId(id).getKind() & kinds))
			{
				continue;
			}
			const NameHierarchy name = storage.getNameHierarchyForNodeId(id);
			if (name.size() == 0)
			{
				continue;
			}
			result.push_back({id, symbolKey(name), name.getQualifiedName(), name.getDelimiter()});
		}
	}
	return result;
}

Resolved pick(const std::vector<Candidate>& candidates, const std::wstring& text)
{
	const bool hasParameters = text.find(L'(') != std::wstring::npos;
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
		gather([&](const Candidate& c) {
			return endsWithName(hasParameters ? c.key : c.qualified, text, c.delimiter);
		}))
	{
		return {ids.size() == 1 ? Resolution::Found : Resolution::Ambiguous, ids};
	}
	return {Resolution::NotFound, {}};
}
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
