#include "StGaps.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "CppSQLite3.h"
#include "DefinitionKind.h"
#include "ErrorInfo.h"
#include "NameHierarchy.h"
#include "NodeKind.h"
#include "PersistentStorage.h"
#include "SourceLocation.h"
#include "SourceLocationCollection.h"
#include "SourceLocationFile.h"
#include "StGraphInternal.h"
#include "StKey.h"
#include "StorageEdge.h"
#include "StorageNode.h"

namespace st {
namespace {
const NodeKindMask CONTAINERS = NODE_FILE | NODE_PACKAGE | NODE_NAMESPACE | NODE_MODULE;
// Never a gap: containers, type parameters and builtin types.
const NodeKindMask NEVER_UNRESOLVED = CONTAINERS | NODE_TYPE_PARAMETER | NODE_BUILTIN_TYPE;

// What the project itself declares, read from every node once.
struct Project {
  explicit Project(PersistentStorage& storage) {
    std::vector<NameHierarchy> types;
    for(const StorageNode& node : storage.getStorageNodes()) {
      const NodeKind kind = intToNodeKind(node.type);
      const bool defined = storage.getDefinitionKindForNodeId(node.id) != DEFINITION_NONE;
      if(kind & CONTAINERS & ~NODE_FILE) {
        packages[NameHierarchy::deserialize(node.serializedName).getQualifiedName()] = defined;
      } else if((kind & (TYPES | NODE_ANNOTATION)) && defined) {
        types.push_back(NameHierarchy::deserialize(node.serializedName));
        definedTypes.insert(types.back().getQualifiedName());
      }
    }
    for(const NameHierarchy& type : types) {
      const std::wstring package = type.size() > 1 ? type.getRange(0, type.size() - 1).getQualifiedName() : L"";
      if(packages.count(package)) {
        own.insert(package);
      }
    }
  }

  // Whether the package enclosing `name` (its longest prefix naming a package) is one the project defines a type
  // in, or one it declares below such a package.
  bool owns(const NameHierarchy& name) const {
    for(size_t size = name.size() - 1; size >= 1; size--) {
      const auto package = packages.find(name.getRange(0, size).getQualifiedName());
      if(package == packages.end()) {
        continue;
      }
      if(own.count(package->first)) {
        return true;
      }
      for(size_t outer = 1; package->second && outer < size; outer++) {
        if(own.count(name.getRange(0, outer).getQualifiedName())) {
          return true;
        }
      }
      return false;
    }
    return false;
  }

  std::map<std::wstring, bool> packages;    // qualified name to whether the project declares it
  std::set<std::wstring> own;               // packages the project defines a type in
  std::set<std::wstring> definedTypes;
};

}    // namespace

struct GapInputs::Data {
  std::unique_ptr<Project> project;
  std::optional<std::set<FilePath>> incomplete;
  // Every index error's line and column, by file.
  std::optional<std::map<FilePath, std::vector<std::pair<size_t, size_t>>>> errors;
  // The symbols that have the symbol as a member, for every symbol asked about so far.
  std::map<Id, std::vector<Id>> memberOwners;
  // Whether several files declare the symbol, for every symbol asked about so far.
  std::map<Id, bool> multiDeclared;
};

GapInputs::GapInputs() : m_data(std::make_unique<Data>()) {}

GapInputs::~GapInputs() = default;

size_t GapInputs::size() const {
  return (m_data->project ? 1 : 0) + (m_data->incomplete ? 1 : 0) + (m_data->errors ? 1 : 0) + m_data->memberOwners.size() +
      m_data->multiDeclared.size();
}

GapInputs::Data& GapInputs::data() {
  return *m_data;
}

namespace {
class Unresolved {
public:
  Unresolved(PersistentStorage& storage, GapInputs::Data& inputs)
      : m_storage(storage)
      , m_inputs(inputs)
      , m_placeholder(storage.getNodeIdForNameHierarchy(NameHierarchy(L"unsolved-symbol", L"."))) {}

  // The unresolved ones of `ids`, looked up together.
  std::set<Id> of(const std::vector<Id>& ids) {
    std::set<Id> unresolved;
    std::vector<Id> undefined;
    for(const Id id : ids) {
      if(id == m_placeholder) {
        unresolved.insert(id);
      } else if(m_storage.getDefinitionKindForNodeId(id) == DEFINITION_NONE) {
        undefined.push_back(id);
      }
    }
    for(const StorageNode& node : m_storage.getStorageNodesForIds(undefined)) {
      if(is(node)) {
        unresolved.insert(node.id);
      }
    }
    return unresolved;
  }

  bool is(const StorageNode& node) {
    if(node.id == m_placeholder) {
      return true;
    }
    const NodeKind kind = intToNodeKind(node.type);
    if((kind & NEVER_UNRESOLVED) || m_storage.getDefinitionKindForNodeId(node.id) != DEFINITION_NONE) {
      return false;
    }
    const NameHierarchy name = NameHierarchy::deserialize(node.serializedName);
    if(isUnsolvedSymbol(name)) {
      return true;
    }
    // A name Clang cannot resolve is an index error, so a C/C++ symbol without a definition is declared but defined
    // outside the indexed files: mostly a library's, even in a namespace the project also declares in (a std::hash
    // specialization makes std one), else a project function defined outside the compilation database.
    if(name.size() < 2 || name.getDelimiter() == L"::" || !project().owns(name)) {
      return false;
    }
    // A member of a defined type without a definition is implicit: a default constructor, an enum's valueOf.
    return (kind & (TYPES | NODE_ANNOTATION | NODE_SYMBOL)) ||
        !project().definedTypes.count(name.getRange(0, name.size() - 1).getQualifiedName());
  }

private:
  const Project& project() {
    if(!m_inputs.project) {
      m_inputs.project = std::make_unique<Project>(m_storage);
    }
    return *m_inputs.project;
  }

  PersistentStorage& m_storage;
  GapInputs::Data& m_inputs;
  const Id m_placeholder;
};

bool definesNames(const SourceLocation& location) {
  return location.isScopeLocation() || location.getType() == LOCATION_TOKEN;
}

// A symbol named in a file: whether it can own an index error (not anonymous, local or a placeholder), and its kind.
struct Named {
  bool owner = false;
  NodeKind kind = NODE_SYMBOL;
};

bool isOwner(const NameHierarchy& name) {
  if(name.size() == 0 || isUnsolvedSymbol(name) || name.back().getName().rfind(L"anonymous class", 0) == 0) {
    return false;
  }
  // Inside a method: a local class, or a member of one.
  for(size_t i = 0; i + 1 < name.size(); i++) {
    if(!name[i].getSignature().getParameterString().empty()) {
      return false;
    }
  }
  return true;
}

// The symbols of one file an index error or a result row belongs to: the innermost named scope around it; at type
// level the fields declared on the line, if any; outside every scope the file's top-level types.
class Owners {
public:
  Owners(const SourceLocationFile& file, const std::map<Id, Named>& named) {
    file.forEachStartSourceLocation([&](SourceLocation* location) {
      if(!definesNames(*location)) {
        return;
      }
      for(const Id id : location->getTokenIds()) {
        const auto it = named.find(id);
        if(it == named.end() || !it->second.owner) {
          continue;
        }
        if(location->isScopeLocation()) {
          const SourceLocation* end = location->getOtherLocation();
          m_scopes.push_back({id,
                              it->second.kind,
                              location->getLineNumber(),
                              location->getColumnNumber(),
                              end ? end->getLineNumber() : location->getLineNumber(),
                              end ? end->getColumnNumber() : 0});
        } else if(it->second.kind & (NODE_FIELD | NODE_ENUM_CONSTANT)) {
          m_fields[location->getLineNumber()].push_back(id);
        }
      }
    });
    for(const Span& span : m_scopes) {
      const bool nested = std::any_of(m_scopes.begin(), m_scopes.end(), [&](const Span& other) {
        return &other != &span && other.contains(span.firstLine, span.firstColumn) &&
            std::make_pair(other.firstLine, other.firstColumn) < std::make_pair(span.firstLine, span.firstColumn);
      });
      if(!nested && (span.kind & (TYPES | NODE_ANNOTATION))) {
        m_topLevel.push_back(span.id);
      }
    }
  }

  // Column 0 stands for the whole line.
  std::vector<Id> at(size_t line, size_t column) const {
    const Span* innermost = nullptr;
    for(const Span& span : m_scopes) {
      if(span.contains(line, column) &&
         (!innermost ||
          std::make_pair(span.firstLine, span.firstColumn) > std::make_pair(innermost->firstLine, innermost->firstColumn))) {
        innermost = &span;
      }
    }
    if(!innermost) {
      return m_topLevel;
    }
    const auto fields = m_fields.find(line);
    if((innermost->kind & (TYPES | NODE_ANNOTATION)) && fields != m_fields.end()) {
      return fields->second;
    }
    return {innermost->id};
  }

private:
  struct Span {
    Id id;
    NodeKind kind;
    size_t firstLine;
    size_t firstColumn;
    size_t lastLine;
    size_t lastColumn;

    bool contains(size_t line, size_t column) const {
      if(!column) {
        return firstLine <= line && line <= lastLine;
      }
      return std::make_pair(firstLine, firstColumn) <= std::make_pair(line, column) &&
          std::make_pair(line, column) <= std::make_pair(lastLine, lastColumn);
    }
  };

  std::vector<Span> m_scopes;
  std::map<size_t, std::vector<Id>> m_fields;
  std::vector<Id> m_topLevel;
};

// Index errors belonging to a walked symbol or to the symbol enclosing a result row. Only the files with errors that
// hold a result row or a walked symbol's definition are read; the errors themselves only when there are such files.
size_t indexErrors(PersistentStorage& storage,
                   GapInputs::Data& inputs,
                   const std::set<Id>& nodes,
                   const std::vector<ResultLine>& lines) {
  // The indexer marks every file it reported errors in incomplete.
  if(!inputs.incomplete) {
    inputs.incomplete = storage.getIncompleteFiles();
  }
  const std::set<FilePath>& incomplete = *inputs.incomplete;
  if(incomplete.empty()) {
    return 0;
  }
  std::set<FilePath> files;
  for(const ResultLine& line : lines) {
    if(incomplete.count(line.file)) {
      files.insert(line.file);
    }
  }
  std::vector<Id> defined;
  for(const Id id : nodes) {
    if(storage.getDefinitionKindForNodeId(id) != DEFINITION_NONE) {
      defined.push_back(id);
    }
  }
  // Reading the locations of thousands of symbols costs more than reading every file with errors.
  if(defined.size() >= 10 * incomplete.size()) {
    files = incomplete;
  } else if(!defined.empty()) {
    storage.getSourceLocationsForTokenIds(defined)->forEachSourceLocation([&](SourceLocation* location) {
      if(location->isStartLocation() && definesNames(*location) && incomplete.count(location->getFilePath())) {
        files.insert(location->getFilePath());
      }
    });
  }
  if(files.empty()) {
    return 0;
  }
  if(!inputs.errors) {
    std::map<FilePath, std::vector<std::pair<size_t, size_t>>> read;
    for(const ErrorInfo& error : storage.getErrorInfos()) {
      read[FilePath(error.filePath)].push_back({error.lineNumber, error.columnNumber});
    }
    inputs.errors = std::move(read);
  }
  std::map<FilePath, std::vector<std::pair<size_t, size_t>>> errors;
  for(const FilePath& file : files) {
    const auto found = inputs.errors->find(file);
    if(found != inputs.errors->end()) {
      errors[file] = found->second;
    }
  }

  std::map<FilePath, std::shared_ptr<SourceLocationFile>> locations;
  std::set<Id> ids;
  for(const auto& [file, fileErrors] : errors) {
    locations[file] = storage.getSourceLocationsForFile(file);
    locations[file]->forEachStartSourceLocation([&](SourceLocation* location) {
      for(const Id id : location->getTokenIds()) {
        if(definesNames(*location) && storage.getDefinitionKindForNodeId(id) != DEFINITION_NONE) {
          ids.insert(id);
        }
      }
    });
  }
  std::map<Id, Named> named;
  for(const StorageNode& node : storage.getStorageNodesForIds(std::vector<Id>(ids.begin(), ids.end()))) {
    const NodeKind kind = intToNodeKind(node.type);
    named[node.id] = {!(kind & CONTAINERS) && isOwner(NameHierarchy::deserialize(node.serializedName)), kind};
  }

  std::map<FilePath, Owners> owners;
  for(const auto& [file, located] : locations) {
    owners.emplace(file, Owners(*located, named));
  }
  std::set<Id> wanted = nodes;
  for(const ResultLine& line : lines) {
    const auto it = owners.find(line.file);
    if(it != owners.end()) {
      for(const Id id : it->second.at(line.line, line.column)) {
        wanted.insert(id);
      }
    }
  }
  size_t count = 0;
  for(const auto& [file, fileOwners] : owners) {
    for(const auto& [line, column] : errors.at(file)) {
      const std::vector<Id> belongs = fileOwners.at(line, column);
      count += std::any_of(belongs.begin(), belongs.end(), [&](Id id) { return wanted.count(id) > 0; });
    }
  }
  return count;
}
}    // namespace

// Passed symbols and the types enclosing them that more than one file declares: a class two modules both have,
// whose other copy's users may be bound to this one or missing.
size_t multiDeclared(PersistentStorage& storage, GapInputs::Data& inputs, const std::set<Id>& nodes) {
  std::set<Id> symbols = nodes;
  for(std::vector<Id> level(nodes.begin(), nodes.end()); !level.empty();) {
    // A type's incoming links are all its uses; only the symbols not seen before are read.
    std::vector<Id> unknown;
    for(const Id id : level) {
      if(!inputs.memberOwners.count(id)) {
        unknown.push_back(id);
      }
    }
    if(!unknown.empty()) {
      // Kept only once the read succeeded, so a throw leaves nothing half-filled behind.
      std::map<Id, std::vector<Id>> read;
      for(const Id id : unknown) {
        read[id];
      }
      for(const StorageEdge& edge : storage.getEdgesForTargetIds(unknown)) {
        if(Edge::intToType(edge.type) == Edge::EDGE_MEMBER) {
          read[edge.targetNodeId].push_back(edge.sourceNodeId);
        }
      }
      inputs.memberOwners.insert(read.begin(), read.end());
    }
    std::vector<Id> owners;
    for(const Id id : level) {
      for(const Id owner : inputs.memberOwners[id]) {
        if(symbols.insert(owner).second) {
          owners.push_back(owner);
        }
      }
    }
    level = owners;
  }
  // Every file of a package declares it; a C++ symbol declared in a header and defined in a source file is no second
  // copy.
  std::vector<Id> candidates;
  for(const StorageNode& node : storage.getStorageNodesForIds(std::vector<Id>(symbols.begin(), symbols.end()))) {
    if(!(intToNodeKind(node.type) & (NODE_PACKAGE | NODE_MODULE | NODE_NAMESPACE | NODE_FILE)) &&
       NameHierarchy::deserialize(node.serializedName).getDelimiter() != L"::") {
      candidates.push_back(node.id);
    }
  }
  // Reading a symbol's declarations walks all its occurrences, so only the symbols not seen before are read.
  std::vector<Id> unknown;
  size_t count = 0;
  for(const Id id : candidates) {
    const auto known = inputs.multiDeclared.find(id);
    if(known == inputs.multiDeclared.end()) {
      unknown.push_back(id);
    } else {
      count += known->second;
    }
  }
  const auto sites = declarationSites(storage, unknown);
  for(const Id id : unknown) {
    const auto found = sites.find(id);
    const bool several = found != sites.end() && found->second.size() > 1;
    inputs.multiDeclared[id] = several;
    count += several;
  }
  return count;
}

Gaps findGaps(PersistentStorage& storage, const Walked& walked, const std::vector<ResultLine>& lines, GapInputs* memo) {
  GapInputs local;
  GapInputs::Data& inputs = (memo ? *memo : local).data();
  std::map<Id, Id> links = walked.links;
  for(const Scope& scope : walked.scopes) {
    const std::vector<Id> from(scope.from.begin(), scope.from.end());
    if(from.empty()) {
      continue;
    }
    if(scope.into) {
      for(const StorageEdge& edge : storage.getEdgesForTargetIds(from)) {
        if(Edge::intToType(edge.type) & scope.into) {
          links.emplace(edge.id, edge.sourceNodeId);
        }
      }
    }
    if(scope.outOf) {
      for(const StorageEdge& edge : storage.getEdgesForSourceIds(from)) {
        if(Edge::intToType(edge.type) & scope.outOf) {
          links.emplace(edge.id, edge.targetNodeId);
        }
      }
    }
  }
  std::set<Id> ends;
  for(const auto& [edge, end] : links) {
    ends.insert(end);
  }
  const std::set<Id> unresolved = Unresolved(storage, inputs).of(std::vector<Id>(ends.begin(), ends.end()));
  Gaps gaps;
  for(const auto& [edge, end] : links) {
    gaps.unresolvedLinks += unresolved.count(end);
  }
  gaps.indexErrors = indexErrors(storage, inputs, walked.nodes, lines);
  gaps.multiDeclared = multiDeclared(storage, inputs, walked.passed);
  return gaps;
}

size_t countUnresolved(PersistentStorage& storage) {
  GapInputs inputs;
  Unresolved unresolved(storage, inputs.data());
  size_t count = 0;
  // A copy: classifying may read all nodes again, which replaces the vector getStorageNodes returns.
  const std::vector<StorageNode> nodes = storage.getStorageNodes();
  for(const StorageNode& node : nodes) {
    count += unresolved.is(node);
  }
  return count;
}

void printGaps(const Gaps& gaps, std::ostream& out) {
  std::vector<std::string> parts;
  if(gaps.multiDeclared) {
    parts.push_back(std::to_string(gaps.multiDeclared) + " symbols declared in several modules");
  }
  if(gaps.unresolvedLinks) {
    parts.push_back(std::to_string(gaps.unresolvedLinks) + " links to unresolved symbols");
  }
  if(gaps.indexErrors) {
    parts.push_back(std::to_string(gaps.indexErrors) + " index errors");
  }
  if(parts.empty()) {
    return;
  }
  out << "st: incomplete: ";
  for(size_t i = 0; i < parts.size(); i++) {
    out << (i ? ", " : "") << parts[i];
  }
  // st status totals the unresolved symbols and index errors, not the symbols several modules declare
  out << (gaps.unresolvedLinks || gaps.indexErrors ? " (see st status)\n" : "\n");
}

void printGaps(const std::function<Gaps()>& findGaps, std::ostream& out) {
  std::string failure;
  try {
    printGaps(findGaps(), out);
    return;
  } catch(CppSQLite3Exception& error) {
    failure = error.errorMessage();
  } catch(const std::exception& error) {
    failure = error.what();
  } catch(...) {
    failure = "unknown error";
  }
  out << "st: incomplete: gap check failed (" << failure << ")\n";
}
}    // namespace st
