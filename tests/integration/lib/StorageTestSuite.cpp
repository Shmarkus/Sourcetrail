#include <memory>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "AccessKind.h"
#include "DefinitionKind.h"
#include "IntermediateStorage.h"
#include "LocationType.h"
#include "MockedMessageQueue.hpp"
#include "ParseLocation.h"
#include "PersistentStorage.h"
#include "type/error/MessageErrorCountUpdate.h"
#include "utilityString.h"

namespace {
// The base has no NAME_DELIMITER_JAVA; Java names carry their delimiter as text.
const std::wstring kJavaDelimiter = L".";

Id addJavaSymbol(PersistentStorage& storage, NodeKind kind, const std::wstring& name) {
  const Id id =
      storage.addNode(StorageNodeData(nodeKindToInt(kind), NameHierarchy::serialize(NameHierarchy(name, kJavaDelimiter)))).first;
  storage.addSymbol(StorageSymbol(id, definitionKindToInt(DEFINITION_EXPLICIT)));
  return id;
}

Id addJavaFile(PersistentStorage& storage, const std::wstring& path) {
  const Id id = storage
                    .addNode(StorageNodeData(
                        nodeKindToInt(NODE_FILE), NameHierarchy::serialize(NameHierarchy(path, NAME_DELIMITER_FILE))))
                    .first;
  storage.addFile(StorageFile(id, path, L"java", "2026-10-30 12:00:00", true, true));
  return id;
}

[[maybe_unused]] void addLocation(PersistentStorage& storage, Id elementId, Id fileId, size_t line, LocationType type) {
  const Id locationId = storage.addSourceLocation(StorageSourceLocationData(fileId, line, 1, line, 5, locationTypeToInt(type)));
  storage.addOccurrence(StorageOccurrence(elementId, locationId));
}

[[maybe_unused]] void addTokenLocation(PersistentStorage& storage, Id elementId, Id fileId, size_t line) {
  addLocation(storage, elementId, fileId, line, LOCATION_TOKEN);
}

class TestStorage : public PersistentStorage {
public:
  TestStorage() : PersistentStorage(FilePath(L"data/test.sqlite"), FilePath(L"data/testBookmarks.sqlite")) {
    clear();
  }
};

[[maybe_unused]] ParseLocation validLocation(Id locationId = 0) {
  return {1, 1, locationId, 1, locationId};
}

NameHierarchy createNameHierarchy(const std::wstring& name) {
  NameHierarchy nameHierarchy(NAME_DELIMITER_CXX);
  for(const std::wstring& element : utility::splitToVector(name, nameDelimiterTypeToString(NAME_DELIMITER_CXX))) {
    nameHierarchy.push(element);
  }
  return nameHierarchy;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[maybe_unused]] NameHierarchy createFunctionNameHierarchy(std::wstring ret, const std::wstring& name, std::wstring parameters) {
  NameHierarchy nameHierarchy = createNameHierarchy(name);
  std::wstring lastName = nameHierarchy.back().getName();
  nameHierarchy.pop();
  nameHierarchy.push(NameElement(std::move(lastName), std::move(ret), std::move(parameters)));
  return nameHierarchy;
}

TEST(Storage, savesFile) {
  TestStorage storage;

  const std::wstring filePath = L"path/to/test.h";

  const std::shared_ptr<IntermediateStorage> intermediateStorage = std::make_shared<IntermediateStorage>();
  const Id nodeId = intermediateStorage
                        ->addNode(StorageNodeData(
                            nodeKindToInt(NODE_FILE), NameHierarchy::serialize(NameHierarchy(filePath, NAME_DELIMITER_FILE))))
                        .first;
  intermediateStorage->addFile(StorageFile(nodeId, filePath, L"someLanguage", "someTime", true, true));

  storage.inject(intermediateStorage.get());

  EXPECT_TRUE(storage.getNameHierarchyForNodeId(nodeId).getQualifiedName() == filePath);
  EXPECT_TRUE(storage.getNodeTypeForNodeWithId(nodeId).isFile());
}

TEST(Storage, savesNode) {
  const NameHierarchy nameHierarchy = createNameHierarchy(L"type");

  TestStorage storage;

  const std::shared_ptr<IntermediateStorage> intermediateStorage = std::make_shared<IntermediateStorage>();
  intermediateStorage->addNode(StorageNodeData(nodeKindToInt(NODE_TYPEDEF), NameHierarchy::serialize(nameHierarchy)));

  storage.inject(intermediateStorage.get());

  const Id storedId = storage.getNodeIdForNameHierarchy(nameHierarchy);

  EXPECT_TRUE(storedId != 0);
  EXPECT_TRUE(storage.getNodeTypeForNodeWithId(storedId).getKind() == NODE_TYPEDEF);
}

TEST(Storage, savesFieldAsMember) {
  const NameHierarchy nameHierarchyA = createNameHierarchy(L"Struct");
  const NameHierarchy nameHierarchyB = createNameHierarchy(L"Struct::m_field");

  TestStorage storage;

  const std::shared_ptr<IntermediateStorage> intermediateStorage = std::make_shared<IntermediateStorage>();

  const Id aId =
      intermediateStorage->addNode(StorageNodeData(nodeKindToInt(NODE_STRUCT), NameHierarchy::serialize(nameHierarchyA))).first;
  intermediateStorage->addSymbol(StorageSymbol(aId, DEFINITION_EXPLICIT));

  const Id bId =
      intermediateStorage->addNode(StorageNodeData(nodeKindToInt(NODE_FIELD), NameHierarchy::serialize(nameHierarchyB))).first;
  intermediateStorage->addSymbol(StorageSymbol(bId, DEFINITION_EXPLICIT));
  intermediateStorage->addEdge(StorageEdgeData(Edge::typeToInt(Edge::EDGE_MEMBER), aId, bId));

  storage.inject(intermediateStorage.get());
  bool foundEdge = false;

  const Id sourceId = storage.getNodeIdForNameHierarchy(nameHierarchyA);
  const Id targetId = storage.getNodeIdForNameHierarchy(nameHierarchyB);
  for(auto edge : storage.getStorageEdges()) {
    if(edge.sourceNodeId == sourceId && edge.targetNodeId == targetId && edge.type == Edge::typeToInt(Edge::EDGE_MEMBER)) {
      foundEdge = true;
    }
  }
  EXPECT_TRUE(foundEdge);
}
TEST(Storage, answersThePerIdQueriesOfSt) {
  PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
  storage.setup();
  const Id classA = addJavaSymbol(storage, NODE_CLASS, L"p.A");
  const Id method = addJavaSymbol(storage, NODE_METHOD, L"p.A.size");
  const Id list = storage
                      .addNode(StorageNodeData(
                          nodeKindToInt(NODE_CLASS), NameHierarchy::serialize(NameHierarchy(L"java.util.List", kJavaDelimiter))))
                      .first;
  const Id member = storage.addEdge(StorageEdgeData(Edge::EDGE_MEMBER, classA, method));
  const Id usage = storage.addEdge(StorageEdgeData(Edge::EDGE_TYPE_USAGE, method, list));
  storage.addComponentAccess(StorageComponentAccess(method, accessKindToInt(ACCESS_PRIVATE)));
  storage.buildCaches();

  const std::vector<StorageEdge> outgoing = storage.getEdgesForSourceIds({classA});
  ASSERT_EQ(1U, outgoing.size());
  EXPECT_EQ(member, outgoing.front().id);
  const std::vector<StorageEdge> incoming = storage.getEdgesForTargetIds({list});
  ASSERT_EQ(1U, incoming.size());
  EXPECT_EQ(usage, incoming.front().id);

  const std::vector<StorageNode> nodes = storage.getStorageNodesForIds({classA, list});
  ASSERT_EQ(2U, nodes.size());
  EXPECT_EQ(nodeKindToInt(NODE_CLASS), nodes.front().type);

  EXPECT_EQ(DEFINITION_EXPLICIT, storage.getDefinitionKindForNodeId(classA));
  EXPECT_EQ(DEFINITION_NONE, storage.getDefinitionKindForNodeId(list));
  EXPECT_EQ(ACCESS_PRIVATE, storage.getAccessKindForNodeId(method));
  EXPECT_EQ(ACCESS_NONE, storage.getAccessKindForNodeId(classA));
}

TEST(Storage, findsImportersByTheFileDeclaringTheImportedSymbol) {
  PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
  storage.setup();
  const Id fileA = addJavaFile(storage, L"/project/p/A.java");
  const Id fileB = addJavaFile(storage, L"/project/p/B.java");

  const Id classA = addJavaSymbol(storage, NODE_CLASS, L"p.A");
  const Id classB = addJavaSymbol(storage, NODE_CLASS, L"p.B");
  const Id list = addJavaSymbol(storage, NODE_CLASS, L"java.util.List");
  addTokenLocation(storage, classA, fileA, 3);
  addTokenLocation(storage, classB, fileB, 3);

  // B imports List, which no project file declares. A merely qualifies it, and does so last.
  addTokenLocation(storage, storage.addEdge(StorageEdgeData(Edge::EDGE_IMPORT, fileB, list)), fileB, 1);
  addLocation(storage, list, fileB, 1, LOCATION_QUALIFIER);
  addLocation(storage, list, fileA, 5, LOCATION_QUALIFIER);
  storage.buildCaches();

  EXPECT_EQ(0U, storage.getReferencing({FilePath(L"/project/p/A.java")}).count(FilePath(L"/project/p/B.java")));
  EXPECT_EQ(0U, storage.getReferenced({FilePath(L"/project/p/B.java")}).count(FilePath(L"/project/p/A.java")));
}

TEST(Storage, keepsTheEdgesOfASymbolAClearedFileOnlyQualifies) {
  PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
  storage.setup();
  const Id declaring = addJavaFile(storage, L"/project/p/A.java");
  const Id qualifying = addJavaFile(storage, L"/project/p/B.java");

  const Id classA = addJavaSymbol(storage, NODE_CLASS, L"p.A");
  const Id method = addJavaSymbol(storage, NODE_METHOD, L"p.A.size");
  const Id caller = addJavaSymbol(storage, NODE_METHOD, L"p.B.run");
  addTokenLocation(storage, classA, declaring, 2);
  addTokenLocation(storage, method, declaring, 3);
  addTokenLocation(storage, caller, qualifying, 3);
  const Id member = storage.addEdge(StorageEdgeData(Edge::EDGE_MEMBER, classA, method));

  // B calls A.size(), naming the class as a qualifier.
  addLocation(storage, classA, qualifying, 4, LOCATION_QUALIFIER);
  addTokenLocation(storage, storage.addEdge(StorageEdgeData(Edge::EDGE_CALL, caller, method)), qualifying, 4);
  storage.buildCaches();

  storage.clearFileElements({FilePath(L"/project/p/B.java")}, [](int) {});

  const std::vector<StorageEdge> edges = storage.getEdgesForSourceIds({classA});
  ASSERT_EQ(1U, edges.size());
  EXPECT_EQ(member, edges.front().id);
  EXPECT_TRUE(storage.getEdgesForSourceIds({caller}).empty());
}

TEST(Storage, countsErrorsInjectedAfterClearingFilesWithErrors) {
  auto messageQueue = std::make_shared<testing::NiceMock<MockedMessageQueue>>();
  std::vector<std::shared_ptr<MessageBase>> messages;
  ON_CALL(*messageQueue, pushMessage).WillByDefault([&messages](std::shared_ptr<MessageBase> message) {
    messages.push_back(std::move(message));
  });
  IMessageQueue::setInstance(messageQueue);

  PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
  storage.setup();
  const Id file = addJavaFile(storage, L"/project/p/B.java");
  for(const std::wstring& message : {L"first", L"second"}) {
    addLocation(storage, storage.addError(StorageErrorData(message, L"/project/p/B.java", false, true)), file, 1, LOCATION_ERROR);
  }
  storage.buildCaches();

  // A first pass injects no errors, then a second pass clears B.java and injects a new error.
  EXPECT_EQ(2U, storage.getErrorCount().total);
  IntermediateStorage firstPass;
  storage.inject(&firstPass);
  storage.setMode(SqliteIndexStorage::STORAGE_MODE_CLEAR);
  storage.clearFileElements({FilePath(L"/project/p/B.java")}, [](int) {});
  EXPECT_EQ(0U, storage.getErrorCount().total);
  storage.setMode(SqliteIndexStorage::STORAGE_MODE_WRITE);
  IntermediateStorage secondPass;
  const Id injectedFile = secondPass
                              .addNode(StorageNodeData(
                                  nodeKindToInt(NODE_FILE),
                                  NameHierarchy::serialize(NameHierarchy(L"/project/p/B.java", NAME_DELIMITER_FILE))))
                              .first;
  secondPass.addFile(StorageFile(injectedFile, L"/project/p/B.java", L"java", "2026-10-30 12:00:00", true, true));
  secondPass.addOccurrence(StorageOccurrence(
      secondPass.addError(StorageErrorData(L"third", L"/project/p/B.java", false, true)),
      secondPass.addSourceLocation(StorageSourceLocationData(injectedFile, 1, 1, 1, 5, locationTypeToInt(LOCATION_ERROR)))));
  storage.inject(&secondPass);

  EXPECT_EQ(1U, storage.getErrorCount().total);
  ASSERT_EQ(1U, messages.size());
  const auto* update = dynamic_cast<const MessageErrorCountUpdate*>(messages.front().get());
  ASSERT_NE(nullptr, update);
  EXPECT_EQ(1U, update->newErrors.size());
  IMessageQueue::setInstance(nullptr);
}

TEST(Storage, removesASymbolOnceTheFilesDeclaringAndReferencingItAreClearedOneAfterTheOther) {
  PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
  storage.setup();
  const Id declaring = addJavaFile(storage, L"/project/p/A.java");
  const Id referencing = addJavaFile(storage, L"/project/p/B.java");

  const Id method = addJavaSymbol(storage, NODE_METHOD, L"p.A.size");
  const Id caller = addJavaSymbol(storage, NODE_METHOD, L"p.B.run");
  addTokenLocation(storage, method, declaring, 3);
  addTokenLocation(storage, caller, referencing, 3);
  addTokenLocation(storage, storage.addEdge(StorageEdgeData(Edge::EDGE_CALL, caller, method)), referencing, 4);
  storage.buildCaches();

  storage.setMode(SqliteIndexStorage::STORAGE_MODE_CLEAR);
  storage.clearFileElements({FilePath(L"/project/p/A.java")}, [](int) {});
  EXPECT_EQ(method, storage.getNodeIdForNameHierarchy(NameHierarchy(L"p.A.size", kJavaDelimiter)));

  storage.clearFileElements({FilePath(L"/project/p/B.java")}, [](int) {});
  EXPECT_EQ(0U, storage.getNodeIdForNameHierarchy(NameHierarchy(L"p.A.size", kJavaDelimiter)));
}

TEST(Storage, keepsTheMembersASymbolDeclaredInSeveralFilesHasOutsideAClearedFile) {
  PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
  storage.setup();
  const Id fileA = addJavaFile(storage, L"/project/p/A.java");
  const Id fileB = addJavaFile(storage, L"/project/p/B.java");

  // Both files declare package p, each with its class.
  const Id package = addJavaSymbol(storage, NODE_PACKAGE, L"p");
  const Id classA = addJavaSymbol(storage, NODE_CLASS, L"p.A");
  const Id classB = addJavaSymbol(storage, NODE_CLASS, L"p.B");
  addTokenLocation(storage, package, fileA, 1);
  addTokenLocation(storage, package, fileB, 1);
  addTokenLocation(storage, classA, fileA, 2);
  addTokenLocation(storage, classB, fileB, 2);
  storage.addEdge(StorageEdgeData(Edge::EDGE_MEMBER, package, classA));
  const Id memberB = storage.addEdge(StorageEdgeData(Edge::EDGE_MEMBER, package, classB));
  storage.buildCaches();

  storage.setMode(SqliteIndexStorage::STORAGE_MODE_CLEAR);
  storage.clearFileElements({FilePath(L"/project/p/A.java")}, [](int) {});

  const std::vector<StorageEdge> members = storage.getEdgesForSourceIds({package});
  ASSERT_EQ(1U, members.size());
  EXPECT_EQ(memberB, members.front().id);
  EXPECT_EQ(0U, storage.getNodeIdForNameHierarchy(NameHierarchy(L"p.A", kJavaDelimiter)));
}

TEST(Storage, buildsTheCachesQueriesNeedWithoutTheMemberOrder) {
  PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
  storage.setup();
  const Id file = addJavaFile(storage, L"/project/p/A.java");
  const Id classA = addJavaSymbol(storage, NODE_CLASS, L"p.A");
  addTokenLocation(storage, classA, file, 2);

  storage.buildQueryCaches();

  EXPECT_EQ(file, storage.getNodeIdForFileNode(FilePath(L"/project/p/A.java")));
  const std::vector<SearchMatch> matches = storage.getAutocompletionSymbolMatches(L"p.A", NodeTypeSet::all(), 10, 10);
  ASSERT_FALSE(matches.empty());
  EXPECT_EQ(std::vector<Id>{classA}, matches.front().tokenIds);
}
}    // namespace
