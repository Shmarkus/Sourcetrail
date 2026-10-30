#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <set>
#include <sstream>

#include "DefinitionKind.h"
#include "FilePath.h"
#include "NameHierarchy.h"
#include "NodeKind.h"
#include "PersistentStorage.h"
#include "StInput.h"
#include "StKey.h"
#include "StOutput.h"
#include "StProject.h"
#include "StorageNode.h"
#include "StorageSymbol.h"

namespace
{
const NodeKindMask ALL_KINDS = (NODE_MAX_VALUE << 1) - 1;

NameHierarchy method(const std::vector<std::wstring>& path, const std::wstring& name,
	const std::wstring& returnType, const std::wstring& parameters)
{
	NameHierarchy hierarchy(NAME_DELIMITER_JAVA);
	for (const auto& element: path)
	{
		hierarchy.push(element);
	}
	hierarchy.push(NameElement(name, returnType, parameters));
	return hierarchy;
}

Id add(PersistentStorage& storage, const NameHierarchy& name, NodeKind kind = NODE_METHOD)
{
	const Id id = storage.addNode(StorageNodeData(nodeKindToInt(kind), NameHierarchy::serialize(name))).first;
	storage.addSymbol(StorageSymbol(id, definitionKindToInt(DEFINITION_EXPLICIT)));
	return id;
}

void write(const QString& path, const QByteArray& content = "")
{
	QDir().mkpath(QFileInfo(path).absolutePath());
	QFile file(path);
	REQUIRE(file.open(QIODevice::WriteOnly));
	file.write(content);
}
}

TEST_CASE("st key omits return type and keeps parameters")
{
	REQUIRE(st::symbolKey(method({L"example", L"Person"}, L"getName", L"java.lang.String", L"()")) ==
		L"example.Person.getName()");
	NameHierarchy person(NAME_DELIMITER_JAVA);
	person.push(L"example");
	person.push(L"Person");
	REQUIRE(st::symbolKey(person) == L"example.Person");
}

TEST_CASE("st keys round-trip generic and anonymous names")
{
	PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
	storage.setup();
	const auto generic = method({L"dummy", L"Mocks"}, L"verifyAndCaptureSave<E>", L"void", L"(java.util.List<E>, int)");
	const auto anonymous = method({L"dummy", L"anonymous class (A.java<80:44>)"}, L"consumeAll", L"void", L"(java.util.Collection<E>)");
	const Id genericId = add(storage, generic);
	const Id anonymousId = add(storage, anonymous);
	storage.buildCaches();
	for (const auto& [name, id]: {std::make_pair(generic, genericId), std::make_pair(anonymous, anonymousId)})
	{
		const auto resolved = st::resolveSymbol(storage, st::symbolKey(name), ALL_KINDS);
		REQUIRE(resolved.status == st::Resolution::Found);
		REQUIRE(resolved.ids == std::vector<Id>{id});
	}
}

TEST_CASE("st resolves overloads as ambiguous and exact keys as found")
{
	PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
	storage.setup();
	add(storage, method({L"example", L"Job"}, L"run", L"void", L"(int)"));
	add(storage, method({L"example", L"Job"}, L"run", L"void", L"(long)"));
	storage.buildCaches();
	const auto ambiguous = st::resolveSymbol(storage, L"example.Job.run", ALL_KINDS);
	REQUIRE(ambiguous.status == st::Resolution::Ambiguous);
	REQUIRE(ambiguous.ids.size() == 2);
	REQUIRE(st::resolveSymbol(storage, L"example.Job.run(int)", ALL_KINDS).status == st::Resolution::Found);
	REQUIRE(st::resolveSymbol(storage, L"Job.run(long)", ALL_KINDS).status == st::Resolution::Found);
	REQUIRE(st::resolveSymbol(storage, L"nope", ALL_KINDS).status == st::Resolution::NotFound);
}

TEST_CASE("st reads keys from stdin ignoring blanks and carriage returns")
{
	std::istringstream in("a.B.c()\tmethod\tx:1\tsrc\r\n\n  \na.B.c()\r\nd.E\n");
	REQUIRE(st::readKeys(in) == std::vector<std::wstring>{L"a.B.c()", L"d.E"});
}

TEST_CASE("st relative path keeps spaces and apostrophes")
{
	const FilePath cwd(L"/home/u/o'neil/repo");
	REQUIRE(st::displayPath(FilePath(L"/home/u/o'neil/repo/src/My File.java"), cwd) == L"src/My File.java");
	REQUIRE(st::displayPath(FilePath(L"/elsewhere/A.java"), cwd) == L"/elsewhere/A.java");
}

TEST_CASE("st output sorts by path and line and reports overflow")
{
	st::OutputOptions options;
	options.limit = 2;
	options.cwd = FilePath(L"/r");
	std::ostringstream out;
	const int code = st::printResults({{L"c", L"method", FilePath(L"/r/b/B.java"), 3, " int c;"},
										  {L"a", L"method", FilePath(L"/r/a/A.java"), 9, "int a;"},
										  {L"b", L"method", FilePath(L"/r/a/A.java"), 2, "int b;"}},
		options, out);
	REQUIRE(code == 0);
	REQUIRE(out.str() == "b\tmethod\ta/A.java:2\tint b;\na\tmethod\ta/A.java:9\tint a;\n+1 more (--limit)\n");
	std::ostringstream empty;
	REQUIRE(st::printResults({}, options, empty) == 1);
	REQUIRE(empty.str().empty());
}

TEST_CASE("st finds project upward and through the registry")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QString base = QDir(dir.path()).canonicalPath();
	write(base + "/repo/x.srctrlprj");
	QDir().mkpath(base + "/repo/sub");
	write(base + "/p/y.srctrlprj");
	QDir().mkpath(base + "/src/repo2/sub");
	const QString registry = base + "/reg/st-projects.tsv";
	write(registry, QString(base + "/src/repo2\t" + base + "/p/y.srctrlprj\n").toUtf8());
	const FilePath registryFile(registry.toStdWString());
	REQUIRE(st::findProject(FilePath((base + "/repo/sub").toStdWString()), registryFile).wstr() ==
		(base + "/repo/x.srctrlprj").toStdWString());
	REQUIRE(st::findProject(FilePath((base + "/src/repo2/sub").toStdWString()), registryFile).wstr() ==
		(base + "/p/y.srctrlprj").toStdWString());
	REQUIRE(st::findProject(FilePath((base + "/src").toStdWString()), registryFile).empty());
}

TEST_CASE("st hides external symbols from resolution and find")
{
	PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
	storage.setup();
	NameHierarchy external(NAME_DELIMITER_JAVA);
	external.push(L"java");
	external.push(L"util");
	external.push(L"List");
	const Id externalId = storage.addNode(StorageNodeData(nodeKindToInt(NODE_INTERFACE), NameHierarchy::serialize(external))).first;
	NameHierarchy local(NAME_DELIMITER_JAVA);
	local.push(L"app");
	local.push(L"List");
	const Id localId = add(storage, local, NODE_CLASS);
	storage.buildCaches();
	REQUIRE(storage.getDefinitionKindForNodeId(externalId) == DEFINITION_NONE);
	REQUIRE(storage.getDefinitionKindForNodeId(localId) == DEFINITION_EXPLICIT);
	const auto resolved = st::resolveSymbol(storage, L"List", ALL_KINDS);
	REQUIRE(resolved.status == st::Resolution::Found);
	REQUIRE(resolved.ids == std::vector<Id>{localId});
	REQUIRE(st::findSymbols(storage, L"List", ALL_KINDS) == std::vector<Id>{localId});
}

TEST_CASE("st find matches substrings only and ranks exact before suffix before substring")
{
	PersistentStorage storage(FilePath(L":memory:"), FilePath(L":memory:"));
	storage.setup();
	const Id fuzzy = add(storage, method({L"c"}, L"mXaXrXkXFailed", L"void", L"()"));
	const Id substring = add(storage, method({L"a", L"Q"}, L"markFailedAll", L"void", L"()"));
	const Id suffix = add(storage, method({L"a", L"markFailed"}, L"markFailed", L"void", L"()"));
	const Id exact = add(storage, method({L"z"}, L"markFailed", L"void", L"()"));
	storage.buildCaches();
	const auto ids = st::findSymbols(storage, L"markFailed", ALL_KINDS);
	REQUIRE(std::find(ids.begin(), ids.end(), fuzzy) == ids.end());
	REQUIRE(ids.size() == 3);
	REQUIRE(ids.back() == substring);
	REQUIRE(std::set<Id>(ids.begin(), ids.begin() + 2) == std::set<Id>{exact, suffix});
	REQUIRE(st::findSymbols(storage, L"MARKFAILEDALL", ALL_KINDS) == std::vector<Id>{substring});
}

TEST_CASE("st output keeps best-ranked results under the limit")
{
	st::OutputOptions options;
	options.limit = 1;
	options.showSource = false;
	options.cwd = FilePath(L"/r");
	std::ostringstream out;
	st::ResultLine worse{L"w", L"method", FilePath(L"/r/a/A.java"), 1, "", 2};
	st::ResultLine best{L"b", L"method", FilePath(L"/r/z/Z.java"), 1, "", 0};
	REQUIRE(st::printResults({worse, best}, options, out) == 0);
	REQUIRE(out.str() == "b\tmethod\tz/Z.java:1\n+1 more (--limit)\n");
}

TEST_CASE("st keys skip the overflow marker line")
{
	std::istringstream in("a.B\tmethod\tx:1\n+12 more (--limit)\nc.D\n");
	REQUIRE(st::readKeys(in) == std::vector<std::wstring>{L"a.B", L"c.D"});
}

TEST_CASE("st registry root beats a stray project in a parent directory")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QString base = QDir(dir.path()).canonicalPath();
	write(base + "/a/stray.srctrlprj");
	write(base + "/p/real.srctrlprj");
	QDir().mkpath(base + "/a/repo/sub");
	const QString registry = base + "/reg.tsv";
	write(registry, QString(base + "/a/repo\t" + base + "/p/real.srctrlprj\n").toUtf8());
	REQUIRE(st::findProject(FilePath((base + "/a/repo/sub").toStdWString()), FilePath(registry.toStdWString())).wstr() ==
		(base + "/p/real.srctrlprj").toStdWString());
	REQUIRE(st::findProject(FilePath((base + "/a").toStdWString()), FilePath(registry.toStdWString())).wstr() ==
		(base + "/a/stray.srctrlprj").toStdWString());
}
