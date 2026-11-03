#include <chrono>
#include <fstream>
#include <future>
#include <memory>
#include <thread>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "AccessKind.h"
#include "Blackboard.h"
#include "DefinitionKind.h"
#include "FileSystem.h"
#include "Graph.h"
#include "IntermediateStorage.h"
#include "LocationType.h"
#include "MockedMessageQueue.hpp"
#include "ParseLocation.h"
#include "PersistentStorage.h"
#include "TaskIndexIfDeclarationsChanged.h"
#include "TaskLambda.h"
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

// Indexes data/StorageTestSuite/A.java holding class p.A on line 2 and, given parameters, a method
// "int size<parameters>" whose header is line 3. Returns the file's declarations.
std::set<std::string> getDeclarationsOfIndexedClass(const std::string& text, const std::wstring& methodParameters = L"") {
  const FilePath path(L"data/StorageTestSuite/A.java");
  FileSystem::createDirectory(path.getParentDirectory());
  std::ofstream(path.str()) << text;

  PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
  storage.setup();
  const Id file = addJavaFile(storage, path.wstr());
  addTokenLocation(storage, addJavaSymbol(storage, NODE_CLASS, L"p.A"), file, 2);
  if(!methodParameters.empty()) {
    NameHierarchy name(L"p.A", kJavaDelimiter);
    name.push(NameElement(L"size", L"int", methodParameters));
    const Id method = storage.addNode(StorageNodeData(nodeKindToInt(NODE_METHOD), NameHierarchy::serialize(name))).first;
    storage.addSymbol(StorageSymbol(method, definitionKindToInt(DEFINITION_EXPLICIT)));
    addTokenLocation(storage, method, file, 3);

    const std::string header = utility::splitToVector(text, "\n")[2];
    const Id signature = storage.addSourceLocation(StorageSourceLocationData(
        file, 3, header.find_first_not_of('\t') + 1, 3, header.find(')') + 1, locationTypeToInt(LOCATION_SIGNATURE)));
    storage.addOccurrence(StorageOccurrence(method, signature));
  }
  storage.buildCaches();

  const std::set<std::string> declarations = storage.getDeclarations({path});
  FileSystem::remove(path);
  return declarations;
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

TEST(Storage, trailWithUnlimitedDepthTerminatesWhenItReachesAPackage) {
  auto storage = std::make_shared<PersistentStorage>(FilePath(L":memory:"), FilePath(L":memory:"));
  storage->setup();
  const Id package = addJavaSymbol(*storage, NODE_PACKAGE, L"example");
  const Id type = addJavaSymbol(*storage, NODE_CLASS, L"example.Person");
  storage->addEdge(StorageEdgeData(Edge::EDGE_MEMBER, package, type));
  storage->buildCaches();

  // Run detached so a regression fails this test instead of hanging the suite.
  auto task = std::make_shared<std::packaged_task<size_t()>>([storage, type]() {
    return storage->getGraphForTrail(0, type, ~NodeKindMask(0), Edge::EDGE_MEMBER | Edge::EDGE_INHERITANCE, false, 0, true)
        ->getNodeCount();
  });
  std::future<size_t> nodeCount = task->get_future();
  std::thread([task]() { (*task)(); }).detach();

  ASSERT_EQ(std::future_status::ready, nodeCount.wait_for(std::chrono::seconds(10)));
  EXPECT_GE(nodeCount.get(), 1U);
}

TEST(Storage, findsFilesReferencingSymbolsDeclaredInChangedFilesWithoutImports) {
  PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
  storage.setup();
  const Id declaring = addJavaFile(storage, L"/project/example/Service.java");
  const Id samePackageCaller = addJavaFile(storage, L"/project/example/ServiceTest.java");
  const Id unrelated = addJavaFile(storage, L"/project/other/Other.java");
  const Id library = addJavaFile(storage, L"/project/lib/Clock.java");

  const Id method = addJavaSymbol(storage, NODE_METHOD, L"example.Service.markFailed");
  const Id test = addJavaSymbol(storage, NODE_METHOD, L"example.ServiceTest.marks");
  const Id now = addJavaSymbol(storage, NODE_METHOD, L"lib.Clock.now");
  const Id other = addJavaSymbol(storage, NODE_METHOD, L"other.Other.run");
  addTokenLocation(storage, method, declaring, 3);
  addTokenLocation(storage, test, samePackageCaller, 3);
  addTokenLocation(storage, now, library, 3);
  addTokenLocation(storage, other, unrelated, 3);

  // ServiceTest calls the changed method; Service and Other only call a method declared elsewhere.
  addTokenLocation(storage, storage.addEdge(StorageEdgeData(Edge::EDGE_CALL, test, method)), samePackageCaller, 4);
  addTokenLocation(storage, storage.addEdge(StorageEdgeData(Edge::EDGE_CALL, method, now)), declaring, 4);
  addTokenLocation(storage, storage.addEdge(StorageEdgeData(Edge::EDGE_CALL, other, now)), unrelated, 4);
  storage.buildCaches();

  const std::set<FilePath> referencing = storage.getReferencing({FilePath(L"/project/example/Service.java")});

  EXPECT_EQ(1U, referencing.count(FilePath(L"/project/example/ServiceTest.java")));
  EXPECT_EQ(0U, referencing.count(FilePath(L"/project/other/Other.java")));
  EXPECT_EQ(0U, referencing.count(FilePath(L"/project/lib/Clock.java")));
}

TEST(Storage, findsOnlyDirectImportersOfChangedFiles) {
  PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
  storage.setup();
  const Id fileA = addJavaFile(storage, L"/project/p/A.java");
  const Id fileB = addJavaFile(storage, L"/project/p/B.java");
  const Id fileC = addJavaFile(storage, L"/project/p/C.java");

  const Id classA = addJavaSymbol(storage, NODE_CLASS, L"p.A");
  const Id classB = addJavaSymbol(storage, NODE_CLASS, L"p.B");
  const Id classC = addJavaSymbol(storage, NODE_CLASS, L"p.C");
  addTokenLocation(storage, classA, fileA, 3);
  addTokenLocation(storage, classB, fileB, 3);
  addTokenLocation(storage, classC, fileC, 3);

  // B imports A, C imports B; C does not depend on A's symbols.
  addTokenLocation(storage, storage.addEdge(StorageEdgeData(Edge::EDGE_IMPORT, fileB, classA)), fileB, 1);
  addTokenLocation(storage, storage.addEdge(StorageEdgeData(Edge::EDGE_IMPORT, fileC, classB)), fileC, 1);
  storage.buildCaches();

  const std::set<FilePath> referencing = storage.getReferencing({FilePath(L"/project/p/A.java")});

  EXPECT_EQ(1U, referencing.count(FilePath(L"/project/p/B.java")));
  EXPECT_EQ(0U, referencing.count(FilePath(L"/project/p/C.java")));
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

TEST(Storage, declarationsIgnoreAnEditInsideAMethodBody) {
  const std::set<std::string> before = getDeclarationsOfIndexedClass(
      "package p;\nclass A {\n\tint size(String s) {\n\t\treturn s.length();\n\t}\n}\n", L"(java.lang.String)");
  const std::set<std::string> after = getDeclarationsOfIndexedClass(
      "package p;\nclass A {\n\tint size(String s) {\n\t\tint unused = 1;\n\t\treturn s.length();\n\t}\n}\n",
      L"(java.lang.String)");

  EXPECT_FALSE(before.empty());
  EXPECT_EQ(before, after);
}

TEST(Storage, declarationsChangeWithAMethodSignature) {
  const std::set<std::string> before = getDeclarationsOfIndexedClass(
      "package p;\nclass A {\n\tint size(String s) {\n\t\treturn s.length();\n\t}\n}\n", L"(java.lang.String)");

  // parameter type
  EXPECT_NE(before,
            getDeclarationsOfIndexedClass("package p;\nclass A {\n\tint size(CharSequence s) {\n\t\treturn s.length();\n\t}\n}\n",
                                          L"(java.lang.CharSequence)"));
  // modifier
  EXPECT_NE(before,
            getDeclarationsOfIndexedClass(
                "package p;\nclass A {\n\tstatic int size(String s) {\n\t\treturn s.length();\n\t}\n}\n", L"(java.lang.String)"));
}

TEST(Storage, declarationsChangeWhenAMethodIsRemoved) {
  const std::set<std::string> before = getDeclarationsOfIndexedClass(
      "package p;\nclass A {\n\tint size(String s) {\n\t\treturn s.length();\n\t}\n}\n", L"(java.lang.String)");

  EXPECT_NE(before, getDeclarationsOfIndexedClass("package p;\nclass A {\n}\n"));
}

TEST(Storage, dependentFilesAreIndexedOnlyWhenDeclarationsChanged) {
  PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
  storage.setup();
  const Id file = addJavaFile(storage, L"/project/p/A.java");
  addTokenLocation(storage, addJavaSymbol(storage, NODE_CLASS, L"p.A"), file, 2);
  const std::set<FilePath> filePaths = {FilePath(L"/project/p/A.java")};
  const std::set<std::string> declarations = storage.getDeclarations(filePaths);
  const std::shared_ptr<PersistentStorage> indexed(&storage, [](PersistentStorage*) {});

  // Runs the task on a fresh blackboard and tells whether it indexed the dependent files.
  const auto indexIfChanged = [&](const std::set<std::string>& previousDeclarations, bool interrupted) {
    auto blackboard = std::make_shared<Blackboard>();
    if(interrupted) {
      blackboard->set("interrupted_indexing", true);
    }
    bool dependentFilesIndexed = false;
    std::make_shared<TaskIndexIfDeclarationsChanged>(indexed, filePaths, previousDeclarations, [&]() {
      return std::make_shared<TaskLambda>([&]() { dependentFilesIndexed = true; });
    })->update(blackboard);
    return dependentFilesIndexed;
  };

  EXPECT_FALSE(indexIfChanged(declarations, false)) << "unchanged";
  EXPECT_TRUE(indexIfChanged({}, false)) << "changed";
  EXPECT_FALSE(indexIfChanged({}, true)) << "changed but interrupted";
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

class StorageMessageFix : public testing::Test {
protected:
  void SetUp() override {
    mMessageQueue = std::make_shared<testing::NiceMock<MockedMessageQueue>>();
    ON_CALL(*mMessageQueue, pushMessage).WillByDefault([this](std::shared_ptr<MessageBase> message) {
      mMessages.push_back(std::move(message));
    });
    IMessageQueue::setInstance(mMessageQueue);
  }

  void TearDown() override {
    IMessageQueue::setInstance(nullptr);
  }

  std::shared_ptr<testing::NiceMock<MockedMessageQueue>> mMessageQueue;
  std::vector<std::shared_ptr<MessageBase>> mMessages;
};

TEST_F(StorageMessageFix, countsErrorsInjectedAfterClearingFilesWithErrors) {
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
  ASSERT_EQ(1U, mMessages.size());
  const auto* update = dynamic_cast<const MessageErrorCountUpdate*>(mMessages.front().get());
  ASSERT_NE(nullptr, update);
  EXPECT_EQ(1U, update->newErrors.size());
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
