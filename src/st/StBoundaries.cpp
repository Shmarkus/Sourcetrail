#include "StBoundaries.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <string>

#include "DefinitionKind.h"
#include "NodeType.h"
#include "PersistentStorage.h"
#include "SourceLocation.h"
#include "SourceLocationCollection.h"
#include "StGraphInternal.h"
#include "StorageEdge.h"

namespace st {
namespace {
enum class Boundary { None, Repository, FeignClient };

const NodeKindMask CALLABLE = NODE_METHOD | NODE_FUNCTION;

// A name element without its type parameters: `JpaRepository<T, ID>` is `JpaRepository`.
std::wstring bareName(const NameElement& element) {
  const std::wstring& name = element.getName();
  return name.substr(0, name.find(L'<'));
}

std::wstring bareQualifiedName(const NameHierarchy& name) {
  std::wstring qualified;
  for(size_t i = 0; i < name.size(); i++) {
    qualified += (i ? L"." : L"") + bareName(name[i]);
  }
  return qualified;
}

class Classifier {
public:
  explicit Classifier(PersistentStorage& storage) : m_storage(storage) {}

  Boundary of(Id type) {
    const auto known = m_kinds.find(type);
    if(known != m_kinds.end()) {
      return known->second;
    }
    // Against inheritance cycles.
    m_kinds[type] = Boundary::None;
    Boundary kind = isSpringDataRepository(m_storage.getNameHierarchyForNodeId(type)) ? Boundary::Repository : Boundary::None;
    std::vector<Id> supertypes;
    for(const StorageEdge& edge : m_storage.getEdgesForSourceIds({type})) {
      const Edge::EdgeType edgeType = Edge::intToType(edge.type);
      if(edgeType == Edge::EDGE_ANNOTATION_USAGE && kind == Boundary::None) {
        const std::wstring annotation = bareQualifiedName(m_storage.getNameHierarchyForNodeId(edge.targetNodeId));
        if(annotation == L"org.springframework.stereotype.Repository") {
          kind = Boundary::Repository;
        } else if(annotation.starts_with(L"org.springframework.cloud.") && annotation.ends_with(L".FeignClient")) {
          kind = Boundary::FeignClient;
        }
      } else if(edgeType == Edge::EDGE_INHERITANCE) {
        supertypes.push_back(edge.targetNodeId);
      }
    }
    for(const Id supertype : supertypes) {
      kind = kind == Boundary::None ? of(supertype) : kind;
    }
    m_kinds[type] = kind;
    return kind;
  }

  // The types extending `type`, transitively.
  std::set<Id> subtypes(Id type) {
    std::set<Id> found;
    std::vector<Id> level = {type};
    while(!level.empty()) {
      std::vector<Id> next;
      for(const StorageEdge& edge : m_storage.getEdgesForTargetIds(level)) {
        if(Edge::intToType(edge.type) == Edge::EDGE_INHERITANCE && edge.sourceNodeId != type &&
           found.insert(edge.sourceNodeId).second) {
          next.push_back(edge.sourceNodeId);
        }
      }
      level = std::move(next);
    }
    return found;
  }

  // The types of a field, as its type uses give them (a generic field's type arguments too).
  std::set<Id> fieldTypes(Id field) {
    std::set<Id> types;
    for(const StorageEdge& edge : m_storage.getEdgesForSourceIds({field})) {
      if(Edge::intToType(edge.type) == Edge::EDGE_TYPE_USAGE) {
        types.insert(edge.targetNodeId);
      }
    }
    return types;
  }

private:
  static bool isSpringDataRepository(const NameHierarchy& name) {
    return name.size() > 1 && bareQualifiedName(name).starts_with(L"org.springframework.data.") &&
        bareName(name.back()).ends_with(L"Repository");
  }

  PersistentStorage& m_storage;
  std::map<Id, Boundary> m_kinds;
};

struct Span {
  FilePath file;
  size_t line = 0;
  size_t column = 0;
  size_t endColumn = 0;
};

// Every location of each link, with its end column when it ends on its first line.
std::map<Id, std::vector<Span>> spans(PersistentStorage& storage, const std::vector<Id>& edgeIds) {
  std::map<Id, std::vector<Span>> found;
  if(edgeIds.empty()) {
    return found;
  }
  const std::set<Id> wanted(edgeIds.begin(), edgeIds.end());
  storage.getSourceLocationsForTokenIds(edgeIds)->forEachSourceLocation([&](SourceLocation* location) {
    if(!location->isStartLocation()) {
      return;
    }
    const SourceLocation* end = location->getOtherLocation();
    const size_t endColumn = end && end->getLineNumber() == location->getLineNumber() ? end->getColumnNumber() : 0;
    for(const Id id : location->getTokenIds()) {
      if(wanted.count(id)) {
        found[id].push_back({location->getFilePath(), location->getLineNumber(), location->getColumnNumber(), endColumn});
      }
    }
  });
  return found;
}

std::wstring simpleName(PersistentStorage& storage, Id id) {
  const NameHierarchy name = storage.getNameHierarchyForNodeId(id);
  return name.size() ? bareName(name.back()) : L"";
}
}    // namespace

Boundaries boundaries(PersistentStorage& storage, const std::vector<ResultLine>& lines, const Walked* walked) {
  Classifier classifier(storage);
  Boundaries found;
  std::set<Id> seen;
  for(const ResultLine& line : lines) {
    if(!line.symbol || !seen.insert(line.symbol).second) {
      continue;
    }
    const NodeKind kind = storage.getNodeTypeForNodeWithId(line.symbol).getKind();
    const NameHierarchy name = storage.getNameHierarchyForNodeId(line.symbol);
    const Id type = (kind & TYPES)           ? line.symbol :
        (kind & CALLABLE) && name.size() > 1 ? storage.getNodeIdForNameHierarchy(name.getRange(0, name.size() - 1)) :
                                               0;
    const Boundary boundary = type ? classifier.of(type) : Boundary::None;
    if(boundary == Boundary::None) {
      continue;
    }
    std::vector<ResultLine>& group = boundary == Boundary::Repository ? found.repositories : found.feignClients;
    const std::wstring declaring = simpleName(storage, type);
    if(type == line.symbol) {
      ResultLine shown = line;
      shown.key = declaring;
      group.push_back(std::move(shown));
      continue;
    }
    const std::wstring member = L"." + bareName(name.back());

    // Each call's receiver, named only on evidence at the call site: a use of a field ending one character (the dot)
    // before the call, the field's one type a subtype of the declaring type (or, for a library method, whose own
    // inheritance the index lacks, a type of the same kind). Otherwise the declaring type.
    std::map<std::wstring, Site> firstCalls;
    if(walked) {
      const std::set<Id> subtypes = classifier.subtypes(type);
      const bool library = storage.getDefinitionKindForNodeId(line.symbol) == DEFINITION_NONE;
      const auto isReceiver = [&](Id candidate) {
        return candidate != type && (subtypes.count(candidate) || (library && classifier.of(candidate) == boundary));
      };
      for(const StorageEdge& call : storage.getEdgesForTargetIds({line.symbol})) {
        if(!(Edge::intToType(call.type) & (Edge::EDGE_CALL | Edge::EDGE_USAGE)) || !walked->passed.count(call.sourceNodeId)) {
          continue;
        }
        std::vector<Id> fieldUses;
        std::map<Id, Id> fieldOfUse;
        for(const StorageEdge& use : storage.getEdgesForSourceIds({call.sourceNodeId})) {
          if(Edge::intToType(use.type) == Edge::EDGE_USAGE &&
             storage.getNodeTypeForNodeWithId(use.targetNodeId).getKind() == NODE_FIELD) {
            fieldUses.push_back(use.id);
            fieldOfUse[use.id] = use.targetNodeId;
          }
        }
        const std::map<Id, std::vector<Span>> useSpans = spans(storage, fieldUses);
        const std::map<Id, std::vector<Span>> callSpans = spans(storage, {call.id});
        if(!callSpans.count(call.id)) {
          continue;
        }
        for(const Span& site : callSpans.at(call.id)) {
          Id receiver = 0;
          for(const auto& [use, uses] : useSpans) {
            for(const Span& span : uses) {
              if(span.file == site.file && span.line == site.line && span.endColumn && span.endColumn + 2 == site.column) {
                std::vector<Id> types;
                std::ranges::copy_if(classifier.fieldTypes(fieldOfUse.at(use)), std::back_inserter(types), isReceiver);
                receiver = types.size() == 1 ? types.front() : 0;
              }
            }
          }
          const std::wstring key = receiver ? simpleName(storage, receiver) + member + L" (" + declaring + L")" :
                                              declaring + member;
          Site& first = firstCalls[key];
          if(first.file.empty() || std::make_pair(site.file.wstr(), site.line) < std::make_pair(first.file.wstr(), first.line)) {
            first = {site.file, site.line, site.column};
          }
        }
      }
    }
    if(firstCalls.empty()) {
      ResultLine shown = line;
      shown.key = declaring + member;
      group.push_back(std::move(shown));
      continue;
    }
    for(const auto& [key, site] : firstCalls) {
      ResultLine shown = resultLine(storage, key, line.kind, site);
      shown.tier = line.tier;
      shown.symbol = line.symbol;
      group.push_back(std::move(shown));
    }
  }
  return found;
}
}    // namespace st
