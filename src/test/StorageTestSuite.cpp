#include "catch.hpp"

#include <chrono>
#include <fstream>
#include <future>
#include <thread>

#include "utilityString.h"

#include "Blackboard.h"
#include "DefinitionKind.h"
#include "FileSystem.h"
#include "Graph.h"
#include "IntermediateStorage.h"
#include "LocationType.h"
#include "ParseLocation.h"
#include "PersistentStorage.h"
#include "TaskIndexIfDeclarationsChanged.h"
#include "TaskLambda.h"

namespace
{
Id addJavaSymbol(PersistentStorage& storage, NodeKind kind, const std::wstring& name)
{
	const Id id = storage.addNode(StorageNodeData(nodeKindToInt(kind),
		NameHierarchy::serialize(NameHierarchy(name, NAME_DELIMITER_JAVA)))).first;
	storage.addSymbol(StorageSymbol(id, definitionKindToInt(DEFINITION_EXPLICIT)));
	return id;
}

Id addJavaFile(PersistentStorage& storage, const std::wstring& path)
{
	const Id id = storage.addNode(StorageNodeData(nodeKindToInt(NODE_FILE),
		NameHierarchy::serialize(NameHierarchy(path, NAME_DELIMITER_FILE)))).first;
	storage.addFile(StorageFile(id, path, L"java", "2026-10-30 12:00:00", true, true));
	return id;
}

void addLocation(PersistentStorage& storage, Id elementId, Id fileId, size_t line, LocationType type)
{
	const Id locationId = storage.addSourceLocation(
		StorageSourceLocationData(fileId, line, 1, line, 5, locationTypeToInt(type)));
	storage.addOccurrence(StorageOccurrence(elementId, locationId));
}

void addTokenLocation(PersistentStorage& storage, Id elementId, Id fileId, size_t line)
{
	addLocation(storage, elementId, fileId, line, LOCATION_TOKEN);
}

// Indexes data/StorageTestSuite/A.java holding class p.A on line 2 and, given parameters, a method
// "int size<parameters>" whose header is line 3. Returns the file's declarations.
std::set<std::string> getDeclarationsOfIndexedClass(
	const std::string& text, const std::wstring& methodParameters = L"")
{
	const FilePath path(L"data/StorageTestSuite/A.java");
	FileSystem::createDirectory(path.getParentDirectory());
	std::ofstream(path.str()) << text;

	PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
	storage.setup();
	const Id file = addJavaFile(storage, path.wstr());
	addTokenLocation(storage, addJavaSymbol(storage, NODE_CLASS, L"p.A"), file, 2);
	if (!methodParameters.empty())
	{
		NameHierarchy name(L"p.A", NAME_DELIMITER_JAVA);
		name.push(NameElement(L"size", L"int", methodParameters));
		const Id method = storage.addNode(StorageNodeData(nodeKindToInt(NODE_METHOD), NameHierarchy::serialize(name))).first;
		storage.addSymbol(StorageSymbol(method, definitionKindToInt(DEFINITION_EXPLICIT)));
		addTokenLocation(storage, method, file, 3);

		const std::string header = utility::splitToVector(text, "\n")[2];
		const Id signature = storage.addSourceLocation(StorageSourceLocationData(file, 3,
			header.find_first_not_of('\t') + 1, 3, header.find(')') + 1, locationTypeToInt(LOCATION_SIGNATURE)));
		storage.addOccurrence(StorageOccurrence(method, signature));
	}
	storage.buildCaches();

	const std::set<std::string> declarations = storage.getDeclarations({path});
	FileSystem::remove(path);
	return declarations;
}

class TestStorage: public PersistentStorage
{
public:
	TestStorage()
		: PersistentStorage(FilePath(L"data/test.sqlite"), FilePath(L"data/testBookmarks.sqlite"))
	{
		clear();
	}

	// const size_t getNodeCount() const
	//{
	//	return getGraph().getNodeCount();
	//}

	// const size_t getEdgeCount() const
	//{
	//	return getGraph().getEdgeCount();
	//}
};

ParseLocation validLocation(Id locationId = 0)
{
	return ParseLocation(1, 1, locationId, 1, locationId);
}

NameHierarchy createNameHierarchy(std::wstring s)
{
	NameHierarchy nameHierarchy(NAME_DELIMITER_CXX);
	for (std::wstring element:
		 utility::splitToVector(s, nameDelimiterTypeToString(NAME_DELIMITER_CXX)))
	{
		nameHierarchy.push(element);
	}
	return nameHierarchy;
}

NameHierarchy createFunctionNameHierarchy(std::wstring ret, std::wstring name, std::wstring parameters)
{
	NameHierarchy nameHierarchy = createNameHierarchy(name);
	std::wstring lastName = nameHierarchy.back().getName();
	nameHierarchy.pop();
	nameHierarchy.push(NameElement(lastName, ret, parameters));
	return nameHierarchy;
}
}	 // namespace

TEST_CASE("storage saves file")
{
	TestStorage storage;

	std::wstring filePath = L"path/to/test.h";

	std::shared_ptr<IntermediateStorage> intermediateStorage = std::make_shared<IntermediateStorage>();
	Id id = intermediateStorage
				->addNode(StorageNodeData(
					nodeKindToInt(NODE_FILE),
					NameHierarchy::serialize(NameHierarchy(filePath, NAME_DELIMITER_FILE))))
				.first;
	intermediateStorage->addFile(StorageFile(id, filePath, L"someLanguage", "someTime", true, true));

	storage.inject(intermediateStorage.get());

	REQUIRE(storage.getNameHierarchyForNodeId(id).getQualifiedName() == filePath);
	REQUIRE(storage.getNodeTypeForNodeWithId(id).isFile());
}

TEST_CASE("storage saves node")
{
	NameHierarchy a = createNameHierarchy(L"type");

	TestStorage storage;

	std::shared_ptr<IntermediateStorage> intermediateStorage = std::make_shared<IntermediateStorage>();
	intermediateStorage->addNode(
		StorageNodeData(nodeKindToInt(NODE_TYPEDEF), NameHierarchy::serialize(a)));

	storage.inject(intermediateStorage.get());

	Id storedId = storage.getNodeIdForNameHierarchy(a);

	REQUIRE(storedId != 0);
	REQUIRE(storage.getNodeTypeForNodeWithId(storedId).getKind() == NODE_TYPEDEF);
}

TEST_CASE("storage saves field as member")
{
	NameHierarchy a = createNameHierarchy(L"Struct");
	NameHierarchy b = createNameHierarchy(L"Struct::m_field");

	TestStorage storage;

	std::shared_ptr<IntermediateStorage> intermediateStorage = std::make_shared<IntermediateStorage>();

	Id aId = intermediateStorage
				 ->addNode(StorageNodeData(nodeKindToInt(NODE_STRUCT), NameHierarchy::serialize(a)))
				 .first;
	intermediateStorage->addSymbol(StorageSymbol(aId, DEFINITION_EXPLICIT));

	Id bId = intermediateStorage
				 ->addNode(StorageNodeData(nodeKindToInt(NODE_FIELD), NameHierarchy::serialize(b)))
				 .first;
	intermediateStorage->addSymbol(StorageSymbol(bId, DEFINITION_EXPLICIT));
	intermediateStorage->addEdge(StorageEdgeData(Edge::typeToInt(Edge::EDGE_MEMBER), aId, bId));

	storage.inject(intermediateStorage.get());
	bool foundEdge = false;

	const Id sourceId = storage.getNodeIdForNameHierarchy(a);
	const Id targetId = storage.getNodeIdForNameHierarchy(b);
	for (auto edge: storage.getStorageEdges())
	{
		if (edge.sourceNodeId == sourceId && edge.targetNodeId == targetId &&
			edge.type == Edge::typeToInt(Edge::EDGE_MEMBER))
		{
			foundEdge = true;
		}
	}
	REQUIRE(foundEdge);
}

TEST_CASE("storage saves method static")
{
	// TestStorage storage;
	// Id id = storage.onMethodParsed(
	//	validLocation(1),
	//	ParseFunction(typeUsage("void"), createNameHierarchy("isMethod"), parameters("bool"), true),
	//	ParserClient::ACCESS_NONE,
	//	ParserClient::ABSTRACTION_NONE,
	//	validLocation(4)
	//);

	// Node* node = storage.getNodeWithId(id);
	// TS_ASSERT(node);
	// TS_ASSERT_EQUALS(node->getQualifiedNameWithSignature(), "isMethod");
	// TS_ASSERT_EQUALS(node->getType(), NODE_METHOD);
	// TS_ASSERT(node->getComponent<TokenComponentStatic>());
}

TEST_CASE("storage clears single file data of single file storage")
{
	/*
	m_filePath = FilePath(L"file.cpp");
	TestStorage storage;
	storage.onFunctionParsed(
		validLocation(), ParseFunction(typeUsage("bool"), createNameHierarchy("isTrue"),
		parameters("char")), validLocation()
	);

	REQUIRE(storage.getNodeCount() == 3);
	REQUIRE(storage.getEdgeCount() == 2);
	REQUIRE(storage.tokenLocationCollection().getTokenLocations().size() == 4);

	std::set<FilePath> files;
	files.insert(FilePath(m_filePath));
	storage.clearFileData(files);

	REQUIRE(storage.getNodeCount() == 0);
	REQUIRE(storage.getEdgeCount() == 0);
	REQUIRE(storage.tokenLocationCollection().getTokenLocations().size() == 0);;*/
}

TEST_CASE("storage clears unreferenced single file data of multi file storage")
{
	// m_filePath = "file.h";

	// TestStorage storage;

	// ParseFunction isTrue = ParseFunction(typeUsage("bool"), createNameHierarchy("isTrue"),
	// parameters("char")); storage.onFunctionParsed(validLocation(), isTrue, validLocation());

	// m_filePath = "file.cpp";

	// ParseFunction main = ParseFunction(typeUsage("int"), createNameHierarchy("main"),
	// parameters("void")); storage.onFunctionParsed(validLocation(), main, validLocation());

	// storage.onCallParsed(validLocation(), main, isTrue);

	// TS_ASSERT_EQUALS(storage.getNodeCount(), 6);
	// TS_ASSERT_EQUALS(storage.getEdgeCount(), 5);
	// TS_ASSERT_EQUALS(storage.tokenLocationCollection().getTokenLocations().size(), 9);

	// std::set<FilePath> files;
	// files.insert(FilePath("file.cpp"));
	// storage.clearFileData(files);

	// TS_ASSERT_EQUALS(storage.getNodeCount(), 3);
	// TS_ASSERT_EQUALS(storage.getEdgeCount(), 2);
	// TS_ASSERT_EQUALS(storage.tokenLocationCollection().getTokenLocations().size(), 4);*/
}

TEST_CASE("storage clears referenced single file data of multi file storage")
{
	// m_filePath = "file.h";

	// TestStorage storage;

	// ParseFunction isTrue = ParseFunction(typeUsage("bool"), createNameHierarchy("isTrue"),
	// parameters("void")); storage.onFunctionParsed(validLocation(), isTrue, validLocation());

	// m_filePath = "file.cpp";

	// ParseFunction main = ParseFunction(typeUsage("int"), createNameHierarchy("main"),
	// parameters("void")); storage.onFunctionParsed(validLocation(), main, validLocation());

	// storage.onCallParsed(validLocation(), main, isTrue);

	// TS_ASSERT_EQUALS(storage.getNodeCount(), 5);
	// TS_ASSERT_EQUALS(storage.getEdgeCount(), 5);
	// TS_ASSERT_EQUALS(storage.tokenLocationCollection().getTokenLocations().size(), 9);

	// std::set<FilePath> files;
	// files.insert(FilePath("file.h"));
	// storage.clearFileData(files);

	// TS_ASSERT_EQUALS(storage.getNodeCount(), 4);
	// TS_ASSERT_EQUALS(storage.getEdgeCount(), 3);
	// TS_ASSERT_EQUALS(storage.tokenLocationCollection().getTokenLocations().size(), 5);
}

TEST_CASE("storage clears multi file data of multi file storage")
{
	// m_filePath = "file.h";

	// TestStorage storage;

	// ParseFunction isTrue = ParseFunction(typeUsage("bool"), createNameHierarchy("isTrue"),
	// parameters("void")); storage.onFunctionParsed(validLocation(), isTrue, validLocation());

	// m_filePath = "file.cpp";

	// ParseFunction main = ParseFunction(typeUsage("int"), createNameHierarchy("main"),
	// parameters("void")); storage.onFunctionParsed(validLocation(), main, validLocation());

	// storage.onCallParsed(validLocation(), main, isTrue);

	// TS_ASSERT_EQUALS(storage.getNodeCount(), 5);
	// TS_ASSERT_EQUALS(storage.getEdgeCount(), 5);
	// TS_ASSERT_EQUALS(storage.tokenLocationCollection().getTokenLocations().size(), 9);

	// std::set<FilePath> filePaths;
	// filePaths.insert(FilePath("file.cpp"));
	// filePaths.insert(FilePath("file.h"));
	// storage.clearFileData(filePaths);

	// TS_ASSERT_EQUALS(storage.getNodeCount(), 0);
	// TS_ASSERT_EQUALS(storage.getEdgeCount(), 0);
	// TS_ASSERT_EQUALS(storage.tokenLocationCollection().getTokenLocations().size(), 0);
}

TEST_CASE("storage finds and removes depending file nodes")
{
	// TestStorage storage;

	// Id id1 = storage.onFileParsed("f.h");
	// Id id2 = storage.onFileParsed("file.h");
	// Id id3 = storage.onFileParsed("file.cpp");
	// Id id4 = storage.onFileIncludeParsed(validLocation(), "file.h", "f.h");
	// Id id5 = storage.onFileIncludeParsed(validLocation(), "file.cpp", "file.h");

	// std::string name1 = storage.getNodeWithId(id2)->getQualifiedNameWithSignature();
	// std::string name2 = storage.getNodeWithId(id3)->getQualifiedNameWithSignature();

	// std::set<FilePath> filePaths;
	// filePaths.insert(FilePath(name1));
	// std::set<FilePath> dependingFilePaths = storage.getDependingFilePathsAndRemoveFileNodes(filePaths);

	// TS_ASSERT_EQUALS(dependingFilePaths.size(), 1);
	// TS_ASSERT_EQUALS(dependingFilePaths.begin()->str(), name2);

	// TS_ASSERT(storage.getNodeWithId(id1));
	// TS_ASSERT(!storage.getNodeWithId(id2));
	// TS_ASSERT(!storage.getNodeWithId(id3));
	// TS_ASSERT(!storage.getEdgeWithId(id4));
	// TS_ASSERT(!storage.getEdgeWithId(id5));
}

TEST_CASE("storage trail with unlimited depth terminates when it reaches a package")
{
	auto storage = std::make_shared<PersistentStorage>(FilePath(L":memory:"), FilePath(L":memory:"));
	storage->setup();
	const Id package = addJavaSymbol(*storage, NODE_PACKAGE, L"example");
	const Id type = addJavaSymbol(*storage, NODE_CLASS, L"example.Person");
	storage->addEdge(StorageEdgeData(Edge::EDGE_MEMBER, package, type));
	storage->buildCaches();

	// Run detached so a regression fails this test instead of hanging the suite.
	auto task = std::make_shared<std::packaged_task<size_t()>>([storage, type]() {
		return storage
			->getGraphForTrail(0, type, ~NodeKindMask(0), Edge::EDGE_MEMBER | Edge::EDGE_INHERITANCE, false, 0, true)
			->getNodeCount();
	});
	std::future<size_t> nodeCount = task->get_future();
	std::thread([task]() { (*task)(); }).detach();

	REQUIRE(nodeCount.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
	REQUIRE(nodeCount.get() >= 1);
}

TEST_CASE("storage finds files referencing symbols declared in changed files without imports")
{
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

	REQUIRE(referencing.count(FilePath(L"/project/example/ServiceTest.java")) == 1);
	REQUIRE(referencing.count(FilePath(L"/project/other/Other.java")) == 0);
	REQUIRE(referencing.count(FilePath(L"/project/lib/Clock.java")) == 0);
}

TEST_CASE("storage finds only direct importers of changed files")
{
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

	REQUIRE(referencing.count(FilePath(L"/project/p/B.java")) == 1);
	REQUIRE(referencing.count(FilePath(L"/project/p/C.java")) == 0);
}

TEST_CASE("storage finds importers by the file declaring the imported symbol")
{
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

	REQUIRE(storage.getReferencing({FilePath(L"/project/p/A.java")}).count(FilePath(L"/project/p/B.java")) == 0);
	REQUIRE(storage.getReferenced({FilePath(L"/project/p/B.java")}).count(FilePath(L"/project/p/A.java")) == 0);
}

TEST_CASE("storage declarations ignore an edit inside a method body")
{
	const std::set<std::string> before = getDeclarationsOfIndexedClass(
		"package p;\nclass A {\n\tint size(String s) {\n\t\treturn s.length();\n\t}\n}\n", L"(java.lang.String)");
	const std::set<std::string> after = getDeclarationsOfIndexedClass(
		"package p;\nclass A {\n\tint size(String s) {\n\t\tint unused = 1;\n\t\treturn s.length();\n\t}\n}\n",
		L"(java.lang.String)");

	REQUIRE(!before.empty());
	REQUIRE(before == after);
}

TEST_CASE("storage declarations change with a method signature")
{
	const std::set<std::string> before = getDeclarationsOfIndexedClass(
		"package p;\nclass A {\n\tint size(String s) {\n\t\treturn s.length();\n\t}\n}\n", L"(java.lang.String)");

	SECTION("parameter type")
	{
		REQUIRE(before != getDeclarationsOfIndexedClass(
			"package p;\nclass A {\n\tint size(CharSequence s) {\n\t\treturn s.length();\n\t}\n}\n",
			L"(java.lang.CharSequence)"));
	}
	SECTION("modifier")
	{
		REQUIRE(before != getDeclarationsOfIndexedClass(
			"package p;\nclass A {\n\tstatic int size(String s) {\n\t\treturn s.length();\n\t}\n}\n",
			L"(java.lang.String)"));
	}
}

TEST_CASE("storage declarations change when a method is removed")
{
	const std::set<std::string> before = getDeclarationsOfIndexedClass(
		"package p;\nclass A {\n\tint size(String s) {\n\t\treturn s.length();\n\t}\n}\n", L"(java.lang.String)");

	REQUIRE(before != getDeclarationsOfIndexedClass("package p;\nclass A {\n}\n"));
}

TEST_CASE("dependent files are indexed only when declarations changed")
{
	PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
	storage.setup();
	const Id file = addJavaFile(storage, L"/project/p/A.java");
	addTokenLocation(storage, addJavaSymbol(storage, NODE_CLASS, L"p.A"), file, 2);
	const std::set<FilePath> filePaths = {FilePath(L"/project/p/A.java")};
	const std::set<std::string> declarations = storage.getDeclarations(filePaths);
	std::shared_ptr<PersistentStorage> indexed(&storage, [](PersistentStorage*) {});
	std::shared_ptr<Blackboard> blackboard = std::make_shared<Blackboard>();

	bool dependentFilesIndexed = false;
	const auto indexIfChanged = [&](const std::set<std::string>& previousDeclarations) {
		std::make_shared<TaskIndexIfDeclarationsChanged>(indexed, filePaths, previousDeclarations, [&]() {
			return std::make_shared<TaskLambda>([&]() { dependentFilesIndexed = true; });
		})->update(blackboard);
	};

	SECTION("unchanged")
	{
		indexIfChanged(declarations);
		REQUIRE(!dependentFilesIndexed);
	}
	SECTION("changed")
	{
		indexIfChanged({});
		REQUIRE(dependentFilesIndexed);
	}
	SECTION("changed but interrupted")
	{
		blackboard->set("interrupted_indexing", true);
		indexIfChanged({});
		REQUIRE(!dependentFilesIndexed);
	}
}

TEST_CASE("storage keeps the edges of a symbol a cleared file only qualifies")
{
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
	REQUIRE(edges.size() == 1);
	REQUIRE(edges.front().id == member);
	REQUIRE(storage.getEdgesForSourceIds({caller}).empty());
}

TEST_CASE("storage counts errors injected after clearing files with errors")
{
	PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
	storage.setup();
	const Id file = addJavaFile(storage, L"/project/p/B.java");
	for (const std::wstring& message: {L"first", L"second"})
	{
		addLocation(storage, storage.addError(StorageErrorData(message, L"/project/p/B.java", false, true)), file, 1, LOCATION_ERROR);
	}
	storage.buildCaches();

	// A first pass injects no errors, then a second pass clears B.java and injects a new error.
	CHECK(storage.getErrorCount().total == 2);
	IntermediateStorage firstPass;
	storage.inject(&firstPass);
	storage.setMode(SqliteIndexStorage::STORAGE_MODE_CLEAR);
	storage.clearFileElements({FilePath(L"/project/p/B.java")}, [](int) {});
	CHECK(storage.getErrorCount().total == 0);
	storage.setMode(SqliteIndexStorage::STORAGE_MODE_WRITE);
	IntermediateStorage secondPass;
	const Id injectedFile = secondPass.addNode(StorageNodeData(nodeKindToInt(NODE_FILE),
		NameHierarchy::serialize(NameHierarchy(L"/project/p/B.java", NAME_DELIMITER_FILE)))).first;
	secondPass.addFile(StorageFile(injectedFile, L"/project/p/B.java", L"java", "2026-10-30 12:00:00", true, true));
	secondPass.addOccurrence(StorageOccurrence(
		secondPass.addError(StorageErrorData(L"third", L"/project/p/B.java", false, true)),
		secondPass.addSourceLocation(StorageSourceLocationData(injectedFile, 1, 1, 1, 5, locationTypeToInt(LOCATION_ERROR)))));
	storage.inject(&secondPass);

	REQUIRE(storage.getErrorCount().total == 1);
}

TEST_CASE("storage removes a symbol once the files declaring and referencing it are cleared one after the other")
{
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
	REQUIRE(storage.getNodeIdForNameHierarchy(NameHierarchy(L"p.A.size", NAME_DELIMITER_JAVA)) == method);

	storage.clearFileElements({FilePath(L"/project/p/B.java")}, [](int) {});
	REQUIRE(storage.getNodeIdForNameHierarchy(NameHierarchy(L"p.A.size", NAME_DELIMITER_JAVA)) == 0);
}

TEST_CASE("storage keeps the members a symbol declared in several files has outside a cleared file")
{
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
	REQUIRE(members.size() == 1);
	REQUIRE(members.front().id == memberB);
	REQUIRE(storage.getNodeIdForNameHierarchy(NameHierarchy(L"p.A", NAME_DELIMITER_JAVA)) == 0);
}
