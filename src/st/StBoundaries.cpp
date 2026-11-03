#include "StBoundaries.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <string>

#include "DefinitionKind.h"
#include "NodeType.h"
#include "PersistentStorage.h"
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

  // The types a caller refers to: in its own type uses and in the types of the fields it uses.
  std::set<Id> referencedTypes(Id caller) {
    std::set<Id> types;
    std::vector<Id> fields;
    for(const StorageEdge& edge : m_storage.getEdgesForSourceIds({caller})) {
      const Edge::EdgeType type = Edge::intToType(edge.type);
      if(type == Edge::EDGE_TYPE_USAGE) {
        types.insert(edge.targetNodeId);
      } else if(type == Edge::EDGE_USAGE && m_storage.getNodeTypeForNodeWithId(edge.targetNodeId).getKind() == NODE_FIELD) {
        fields.push_back(edge.targetNodeId);
      }
    }
    if(!fields.empty()) {
      for(const StorageEdge& edge : m_storage.getEdgesForSourceIds(fields)) {
        if(Edge::intToType(edge.type) == Edge::EDGE_TYPE_USAGE) {
          types.insert(edge.targetNodeId);
        }
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

    // Each caller's receiver: the declaring type or the one subtype of it the caller refers to.
    std::map<std::wstring, std::vector<Id>> callsByKey;
    if(walked) {
      const std::set<Id> subtypes = classifier.subtypes(type);
      const bool library = storage.getDefinitionKindForNodeId(line.symbol) == DEFINITION_NONE;
      for(const StorageEdge& edge : storage.getEdgesForTargetIds({line.symbol})) {
        if(!(Edge::intToType(edge.type) & (Edge::EDGE_CALL | Edge::EDGE_USAGE)) || !walked->passed.count(edge.sourceNodeId)) {
          continue;
        }
        const std::set<Id> referenced = classifier.referencedTypes(edge.sourceNodeId);
        std::vector<Id> receivers;
        std::set_intersection(
            subtypes.begin(), subtypes.end(), referenced.begin(), referenced.end(), std::back_inserter(receivers));
        // The index holds none of a library's own inheritance (JpaRepository extends ListCrudRepository): a library
        // method's receiver is the one boundary of its kind the caller refers to.
        if(receivers.empty() && library) {
          std::copy_if(referenced.begin(), referenced.end(), std::back_inserter(receivers), [&](Id other) {
            return other != type && classifier.of(other) == boundary;
          });
        }
        const std::wstring key = !referenced.count(type) && receivers.size() == 1 ?
            simpleName(storage, receivers.front()) + member + L" (" + declaring + L")" :
            declaring + member;
        callsByKey[key].push_back(edge.id);
      }
    }
    if(callsByKey.empty()) {
      ResultLine shown = line;
      shown.key = declaring + member;
      group.push_back(std::move(shown));
      continue;
    }
    for(const auto& [key, edges] : callsByKey) {
      Site first;
      for(const auto& [edge, site] : linkSites(storage, edges)) {
        if(first.file.empty() || std::make_pair(site.file.wstr(), site.line) < std::make_pair(first.file.wstr(), first.line)) {
          first = site;
        }
      }
      ResultLine shown = first.file.empty() ? line : resultLine(storage, key, line.kind, first);
      shown.key = key;
      shown.tier = line.tier;
      shown.symbol = line.symbol;
      group.push_back(std::move(shown));
    }
  }
  return found;
}
}    // namespace st
