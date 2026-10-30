#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include <QFile>
#include <QTemporaryDir>

#include "CppSQLite3.h"
#include "DefinitionKind.h"
#include "FilePath.h"
#include "NameHierarchy.h"
#include "NodeKind.h"
#include "PersistentStorage.h"
#include "Query.h"
#include "StorageEdge.h"
#include "StorageNode.h"
#include "StorageSymbol.h"

namespace
{
Id addMethod(PersistentStorage& storage, const std::wstring& name)
{
	const Id id = storage.addNode(StorageNodeData(nodeKindToInt(NODE_METHOD),
		NameHierarchy::serialize(NameHierarchy(name, NAME_DELIMITER_JAVA)))).first;
	storage.addSymbol(StorageSymbol(id, definitionKindToInt(DEFINITION_EXPLICIT)));
	return id;
}
}

TEST_CASE("CLI search returns stable string ids and bounds results")
{
	PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
	storage.setup();
	const Id first = addMethod(storage, L"getGreeting");
	addMethod(storage, L"getGreetingAgain");
	storage.buildCaches();
	const auto result = cli::search(storage, "getGreeting", 1);
	REQUIRE(result["symbols"].toArray().size() == 1);
	REQUIRE(result["symbols"].toArray()[0].toObject()["id"].isString());
	REQUIRE(cli::describe(storage, first, 50)["name"].toString() == "getGreeting");
	REQUIRE_THROWS(cli::search(storage, "", 1));
	REQUIRE_THROWS(cli::describe(storage, 9999999, 50));
}

TEST_CASE("CLI references preserves call direction and kind filters")
{
	PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
	storage.setup();
	const Id caller = addMethod(storage, L"caller");
	const Id callee = addMethod(storage, L"callee");
	storage.addEdge(StorageEdgeData(Edge::EDGE_CALL, caller, callee));
	storage.buildCaches();
	const auto result = cli::references(storage, callee, "CALL", 50);
	REQUIRE(result["incoming"].toArray().size() == 1);
	REQUIRE(result["incoming"].toArray()[0].toObject()["from"].toObject()["id"].toString() == QString::number(caller));
	REQUIRE(result["outgoing"].toArray().isEmpty());
	REQUIRE(cli::references(storage, callee, "USAGE", 50)["incoming"].toArray().isEmpty());
	REQUIRE_THROWS(cli::references(storage, callee, "INVALID", 50));
}

TEST_CASE("CLI source reading rejects files not in the index")
{
	PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
	storage.setup();
	storage.buildCaches();
	REQUIRE_THROWS(cli::readSource(storage, "/etc/passwd", 1, 20));
}

TEST_CASE("Read-only storage does not create bookmarks or allow database writes")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QString dbPath = directory.path() + "/index.sqlite";
	const QString bookmarkPath = directory.path() + "/bookmarks.sqlite";
	{
		PersistentStorage storage(FilePath(dbPath.toStdWString()), FilePath(L":memory:"));
		storage.setup();
		storage.updateVersion();
		addMethod(storage, L"readOnlyMethod");
	}
	QFile before(dbPath);
	REQUIRE(before.open(QIODevice::ReadOnly));
	const auto original = before.readAll();
	before.close();
	{
		PersistentStorage storage(FilePath(dbPath.toStdWString()), FilePath(bookmarkPath.toStdWString()), true);
		storage.buildCaches();
		REQUIRE(cli::search(storage, "readOnlyMethod", 50)["symbols"].toArray().size() == 1);
	}
	CppSQLite3DB db;
	db.open(dbPath.toUtf8().constData(), true);
	REQUIRE_THROWS_AS(db.execDML("DELETE FROM node"), CppSQLite3Exception);
	db.close();
	REQUIRE_FALSE(QFile::exists(bookmarkPath));
	QFile after(dbPath);
	REQUIRE(after.open(QIODevice::ReadOnly));
	REQUIRE(after.readAll() == original);
}
