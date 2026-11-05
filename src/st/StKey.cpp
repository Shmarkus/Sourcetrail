#include "StKey.h"

#include <algorithm>
#include <cwctype>
#include <optional>
#include <set>

#include "DefinitionKind.h"
#include "NodeType.h"
#include "NodeTypeSet.h"
#include "PersistentStorage.h"
#include "SearchMatch.h"

namespace st {
namespace {
struct Candidate {
  Id id;
  NodeKind kind;
  std::wstring key;
  std::wstring qualified;
  std::wstring delimiter;
  std::wstring bareKey;    // key and qualified name without type parameters
  std::wstring bareQualified;
  std::vector<std::wstring> parameters;
  std::vector<std::wstring> unscopedParameters;    // C++ only
};

// The length of the C++ operator symbol at text[i] when a name element `operator` ends there (`operator<<`, a
// `shop::operator()` followed by its parameter list), else 0. Only the symbols holding `<`, `>` or `(` matter.
size_t operatorSymbolAt(const std::wstring& text, size_t i) {
  static const std::wstring keyword = L"operator";
  if(i < keyword.size() || text.compare(i - keyword.size(), keyword.size(), keyword) != 0) {
    return 0;
  }
  const size_t start = i - keyword.size();
  if(start != 0 && (start < 2 || text.compare(start - 2, 2, L"::") != 0)) {
    return 0;
  }
  if(text.compare(i, 3, L"()(") == 0) {
    return 2;
  }
  for(const wchar_t* symbol : {L"<<=", L">>=", L"<=>", L"->*", L"<<", L">>", L"<=", L">=", L"->", L"<", L">"}) {
    const std::wstring_view view(symbol);
    if(text.compare(i, view.size(), view) == 0) {
      return view.size();
    }
  }
  return 0;
}

// Removes every balanced `<...>` group, nested ones included; a C++ operator's symbol stays.
std::wstring withoutTypeParameters(const std::wstring& text) {
  std::wstring result;
  int depth = 0;
  for(size_t i = 0; i < text.size(); i++) {
    const wchar_t c = text[i];
    if(const size_t symbol = depth == 0 ? operatorSymbolAt(text, i) : 0) {
      result += text.substr(i, symbol);
      i += symbol - 1;
    } else if(c == L'<')
      depth++;
    else if(c == L'>' && depth > 0)
      depth--;
    else if(depth == 0)
      result += c;
  }
  return result;
}

// Where the parameter list of the text's first parameterized element starts, past a C++ `operator()`; npos if none.
size_t parameterListStart(const std::wstring& text) {
  for(size_t i = text.find(L'('); i != std::wstring::npos; i = text.find(L'(', i + 1)) {
    if(const size_t symbol = operatorSymbolAt(text, i); symbol && text[i] == L'(') {
      i += symbol - 1;
      continue;
    }
    return i;
  }
  return std::wstring::npos;
}

// Text before the parameter list of the last element, which is what the search index knows.
std::wstring searchText(const std::wstring& text) {
  if(text.empty() || text.back() != L')') {
    return text;
  }
  int depth = 0;
  for(size_t i = text.size(); i-- > 0;) {
    if(text[i] == L')')
      depth++;
    else if(text[i] == L'(' && --depth == 0)
      return i ? text.substr(0, i) : text;
  }
  return text;
}

bool endsWithName(const std::wstring& full, const std::wstring& suffix, const std::wstring& delimiter) {
  if(full.size() < suffix.size() || full.compare(full.size() - suffix.size(), suffix.size(), suffix) != 0) {
    return false;
  }
  return full.size() == suffix.size() ||
      full.compare(full.size() - suffix.size() - delimiter.size(), delimiter.size(), delimiter) == 0;
}

// The text without the C++ scopes qualifying its names: `const shop::Item &` gives `const Item &`.
std::wstring withoutScopes(const std::wstring& text) {
  std::wstring result;
  size_t name = 0;    // where the name being read started in result
  for(size_t i = 0; i < text.size(); i++) {
    const wchar_t c = text[i];
    if(c == L':' && i + 1 < text.size() && text[i + 1] == L':') {
      result.erase(name);
      i++;
      continue;
    }
    if(!(std::iswalnum(c) || c == L'_')) {
      name = result.size() + 1;
    }
    result += c;
  }
  return result;
}

// Parameter types of the first `(...)` list, without type parameters or whitespace; with `unscoped`, also without the
// C++ scopes qualifying them.
std::vector<std::wstring> parameterTypes(const std::wstring& text, bool unscoped = false) {
  std::vector<std::wstring> result;
  const size_t open = parameterListStart(text);
  const size_t close = text.rfind(L')');
  if(open == std::wstring::npos || close == std::wstring::npos || close < open) {
    return result;
  }
  std::wstring list = withoutTypeParameters(text.substr(open + 1, close - open - 1));
  if(unscoped) {
    list = withoutScopes(list);
  }
  std::wstring current;
  for(const wchar_t c : list) {
    if(c == L',') {
      result.push_back(current);
      current.clear();
    } else if(!std::iswspace(c)) {
      current += c;
    }
  }
  if(!current.empty() || !result.empty()) {
    result.push_back(current);
  }
  return result;
}

// Equal counts; each wanted type equals the key's or is a dot-boundary suffix of it.
bool parametersMatch(const std::vector<std::wstring>& wanted, const std::vector<std::wstring>& actual) {
  if(wanted.size() != actual.size()) {
    return false;
  }
  for(size_t i = 0; i < wanted.size(); i++) {
    if(!endsWithName(actual[i], wanted[i], L".")) {
      return false;
    }
  }
  return true;
}

// The parameter types a query asks for, as written and without C++ scopes.
struct WantedParameters {
  explicit WantedParameters(const std::wstring& text) : types(parameterTypes(text)), unscoped(parameterTypes(text, true)) {}

  // They match the candidate's, for a C++ candidate also with the scopes left out on both sides.
  bool match(const Candidate& candidate) const {
    return parametersMatch(types, candidate.parameters) ||
        (candidate.delimiter == L"::" && unscoped == candidate.unscopedParameters);
  }

  std::vector<std::wstring> types;
  std::vector<std::wstring> unscoped;
};

// The last name element of a query, which every symbol that can resolve from it contains.
std::wstring lastElement(const std::wstring& query) {
  const std::wstring bare = withoutTypeParameters(query.substr(0, parameterListStart(query)));
  const size_t split = bare.find_last_of(L".:");
  return split == std::wstring::npos ? bare : bare.substr(split + 1);
}

// Project symbols only, or with `library` only the symbols without a definition in the project (library types).
std::vector<Candidate> collect(PersistentStorage& storage,
                               const std::wstring& query,
                               const std::vector<SearchMatch>& matches,
                               NodeKindMask kinds,
                               bool library = false) {
  std::vector<Candidate> result;
  std::set<Id> seen;
  const std::wstring needle = lastElement(query);
  for(const auto& match : matches) {
    for(size_t i = 0; i < match.tokenIds.size(); i++) {
      const Id id = match.tokenIds[i];
      // Resolution is exact, but the search is fuzzy: skip matches not containing the name before any storage call.
      if(library && i < match.tokenNames.size() && match.tokenNames[i].getQualifiedName().find(needle) == std::wstring::npos) {
        continue;
      }
      if(!seen.insert(id).second || (storage.getDefinitionKindForNodeId(id) == DEFINITION_NONE) != library ||
         !(storage.getNodeTypeForNodeWithId(id).getKind() & kinds) || isUnsolvedSymbol(storage, id)) {
        continue;
      }
      const NameHierarchy name = storage.getNameHierarchyForNodeId(id);
      if(name.size() == 0) {
        continue;
      }
      const std::wstring key = symbolKey(name);
      const std::wstring qualified = name.getQualifiedName();
      result.push_back({id,
                        storage.getNodeTypeForNodeWithId(id).getKind(),
                        key,
                        qualified,
                        name.getDelimiter(),
                        withoutTypeParameters(key),
                        withoutTypeParameters(qualified),
                        parameterTypes(key),
                        name.getDelimiter() == L"::" ? parameterTypes(key, true) : std::vector<std::wstring>()});
    }
  }
  return result;
}

std::vector<SearchMatch> search(PersistentStorage& storage, const std::wstring& query) {
  return storage.getAutocompletionSymbolMatches(query, NodeTypeSet::all(), 1000, 100);
}

// A type together with its own constructors (methods named like it, directly inside it) stands for the type.
std::optional<Id> typeWithConstructors(const std::vector<Candidate>& candidates, const std::vector<Id>& ids) {
  const Candidate* type = nullptr;
  for(const Candidate& candidate : candidates) {
    if(std::find(ids.begin(), ids.end(), candidate.id) == ids.end() ||
       !(candidate.kind & (NODE_CLASS | NODE_INTERFACE | NODE_STRUCT | NODE_ENUM))) {
      continue;
    }
    if(type) {
      return std::nullopt;
    }
    type = &candidate;
  }
  if(!type) {
    return std::nullopt;
  }
  const size_t split = type->bareQualified.rfind(type->delimiter);
  const std::wstring name = split == std::wstring::npos ? type->bareQualified :
                                                          type->bareQualified.substr(split + type->delimiter.size());
  for(const Candidate& candidate : candidates) {
    if(candidate.id != type->id && std::find(ids.begin(), ids.end(), candidate.id) != ids.end() &&
       (candidate.kind != NODE_METHOD || candidate.bareQualified != type->bareQualified + type->delimiter + name)) {
      return std::nullopt;
    }
  }
  return type->id;
}

Resolved pick(const std::vector<Candidate>& candidates, const std::wstring& text) {
  const size_t parameters = parameterListStart(text);
  const bool hasParameters = parameters != std::wstring::npos;
  const std::wstring bare = withoutTypeParameters(text);
  const std::wstring bareName = hasParameters ? withoutTypeParameters(text.substr(0, parameters)) : bare;
  const WantedParameters wanted(text);
  std::vector<Id> ids;
  auto gather = [&](auto matches) {
    for(const auto& candidate : candidates) {
      if(matches(candidate))
        ids.push_back(candidate.id);
    }
    return !ids.empty();
  };
  if(gather([&](const Candidate& c) { return c.key == text; }) || gather([&](const Candidate& c) { return c.qualified == text; }) ||
     gather([&](const Candidate& c) { return c.bareQualified == bareName && (!hasParameters || wanted.match(c)); }) ||
     gather([&](const Candidate& c) {
       return endsWithName(c.bareQualified, bareName, c.delimiter) && (!hasParameters || wanted.match(c));
     })) {
    if(ids.size() > 1) {
      if(const std::optional<Id> type = typeWithConstructors(candidates, ids)) {
        return {Resolution::Found, {*type}};
      }
    }
    return {ids.size() == 1 ? Resolution::Found : Resolution::Ambiguous, ids};
  }
  return {Resolution::NotFound, {}};
}
}    // namespace

bool isUnsolvedSymbol(const NameHierarchy& name) {
  return name.size() == 1 && name.back().getName() == L"unsolved-symbol";
}

bool isUnsolvedSymbol(PersistentStorage& storage, Id id) {
  return isUnsolvedSymbol(storage.getNameHierarchyForNodeId(id));
}

std::wstring symbolKey(const NameHierarchy& name) {
  std::wstring key = name.getQualifiedName();
  if(name.size()) {
    key += name.back().getSignature().getParameterString();
  }
  return key;
}

Resolved resolveSymbol(PersistentStorage& storage, const std::wstring& text, NodeKindMask kinds) {
  const std::wstring query = searchText(text);
  Resolved resolved;
  // Both passes share one fuzzy search.
  const std::vector<SearchMatch> matches = search(storage, query);
  std::optional<std::vector<SearchMatch>> textMatches;
  // A project symbol wins; a library symbol (an annotation such as `Scheduled`) answers only when none has the name.
  for(const bool library : {false, true}) {
    resolved = pick(collect(storage, query, matches, kinds, library), text);
    if(resolved.status == Resolution::NotFound && query != text) {
      if(!textMatches) {
        textMatches = search(storage, text);
      }
      resolved = pick(collect(storage, text, *textMatches, kinds, library), text);
    }
    if(resolved.status != Resolution::NotFound) {
      break;
    }
  }
  return resolved;
}
}    // namespace st

namespace st {
std::vector<Id> overloadsOf(PersistentStorage& storage, const std::wstring& text, NodeKindMask kinds) {
  const std::wstring name = searchText(text);
  return name == text ? std::vector<Id>() : resolveSymbol(storage, name, kinds).ids;
}

std::vector<RankedSymbol> rankSymbols(PersistentStorage& storage, const std::wstring& text, NodeKindMask kinds) {
  auto lower = [](std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
  };
  if(kinds & ~NODE_TYPE_PARAMETER) {
    // Type parameters (`failed<T>.T`) show up only when the mask is exactly the type-parameter kind (--kind takes one kind).
    kinds &= ~NODE_TYPE_PARAMETER;
  }
  // A parameter list narrows the name's matches to the overloads it fits, as name resolution does.
  const std::wstring name = searchText(text);
  const bool hasParameters = name != text;
  const WantedParameters wanted(text);
  const std::wstring bare = withoutTypeParameters(name);
  const std::wstring needle = lower(bare);
  std::vector<std::pair<int, Id>> ranked;
  for(const Candidate& candidate : collect(storage, name, search(storage, name), kinds)) {
    if(lower(candidate.bareQualified).find(needle) == std::wstring::npos || (hasParameters && !wanted.match(candidate))) {
      continue;
    }
    int tier = 2;
    if(candidate.bareQualified == bare ||
       withoutTypeParameters(storage.getNameHierarchyForNodeId(candidate.id).back().getName()) == bare) {
      tier = 0;
    } else if(endsWithName(candidate.bareQualified, bare, candidate.delimiter) ||
              endsWithName(candidate.bareKey, bare, candidate.delimiter)) {
      tier = 1;
    }
    ranked.emplace_back(tier, candidate.id);
  }
  std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<RankedSymbol> result;
  for(const auto& entry : ranked) {
    result.push_back({entry.second, entry.first});
  }
  return result;
}

std::vector<Id> findSymbols(PersistentStorage& storage, const std::wstring& text, NodeKindMask kinds) {
  std::vector<Id> ids;
  for(const auto& symbol : rankSymbols(storage, text, kinds)) {
    ids.push_back(symbol.id);
  }
  return ids;
}
}    // namespace st
