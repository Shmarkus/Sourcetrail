#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>
#include <sstream>

#include <boost/date_time/posix_time/posix_time.hpp>

#include "DefinitionKind.h"
#include "Edge.h"
#include "FilePath.h"
#include "LocationType.h"
#include "NameHierarchy.h"
#include "NodeKind.h"
#include "PersistentStorage.h"
#include "ProjectSettings.h"
#include "StFreshness.h"
#include "StGraph.h"
#include "StInput.h"
#include "StKey.h"
#include "StLombok.h"
#include "StOutput.h"
#include "StProject.h"
#include "StorageNode.h"
#include "StorageFile.h"
#include "StorageSymbol.h"
#include "TimeStamp.h"
#include "utilityGradleCache.h"

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

Id addFile(PersistentStorage& storage, const std::wstring& path)
{
	const Id id = storage.addNode(StorageNodeData(nodeKindToInt(NODE_FILE),
		NameHierarchy::serialize(NameHierarchy(path, NAME_DELIMITER_FILE)))).first;
	storage.addFile(StorageFile(id, path, L"java", "2026-10-30 12:00:00", true, true));
	return id;
}

void addLocation(PersistentStorage& storage, Id tokenId, Id fileId, size_t line)
{
	const Id locationId = storage.addSourceLocation(
		StorageSourceLocationData(fileId, line, 1, line, 5, locationTypeToInt(LOCATION_TOKEN)));
	storage.addOccurrence(StorageOccurrence(tokenId, locationId));
}

// A small Java-like code base: every symbol is defined on line 1 of its class's file.
struct Code
{
	Code(): storage(FilePath(L":memory:"), FilePath(L":memory:"))
	{
		storage.setup();
	}

	Id file(const std::wstring& type)
	{
		auto it = files.find(type);
		if (it == files.end())
		{
			it = files.emplace(type, addFile(storage, L"/r/p/" + type + L".java")).first;
		}
		return it->second;
	}

	Id type(const std::wstring& name, NodeKind kind = NODE_CLASS)
	{
		NameHierarchy hierarchy(NAME_DELIMITER_JAVA);
		hierarchy.push(L"p");
		hierarchy.push(name);
		const Id id = add(storage, hierarchy, kind);
		addLocation(storage, id, file(name), 1);
		return id;
	}

	Id member(Id parent, const std::wstring& type, const std::wstring& name, NodeKind kind = NODE_METHOD,
		const std::wstring& parameters = L"()")
	{
		const Id id = kind == NODE_METHOD ? add(storage, method({L"p", type}, name, L"void", parameters))
										  : add(storage, method({L"p", type}, name, L"", L""), kind);
		addLocation(storage, id, file(type), 1);
		storage.addEdge(StorageEdgeData(Edge::typeToInt(Edge::EDGE_MEMBER), parent, id));
		return id;
	}

	Id link(Edge::EdgeType type, Id from, Id to, const std::wstring& inFile, std::vector<size_t> lines)
	{
		const Id id = storage.addEdge(StorageEdgeData(Edge::typeToInt(type), from, to));
		for (size_t line: lines)
		{
			addLocation(storage, id, file(inFile), line);
		}
		return id;
	}

	PersistentStorage storage;
	std::map<std::wstring, Id> files;
};

std::set<std::wstring> keys(const std::vector<st::ResultLine>& lines)
{
	std::set<std::wstring> result;
	for (const auto& line: lines)
	{
		result.insert(line.key);
	}
	return result;
}

const st::ResultLine* lineFor(const std::vector<st::ResultLine>& lines, const std::wstring& key)
{
	for (const auto& line: lines)
	{
		if (line.key == key)
		{
			return &line;
		}
	}
	return nullptr;
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

TEST_CASE("st callers climb from implementation through interface")
{
	Code code;
	const Id iface = code.type(L"Iface", NODE_INTERFACE);
	const Id impl = code.type(L"Impl");
	const Id client = code.type(L"Client");
	const Id ifaceRun = code.member(iface, L"Iface", L"run");
	const Id implRun = code.member(impl, L"Impl", L"run");
	const Id go = code.member(client, L"Client", L"go");
	code.link(Edge::EDGE_OVERRIDE, implRun, ifaceRun, L"Impl", {3});
	code.link(Edge::EDGE_CALL, go, ifaceRun, L"Client", {7});
	code.storage.buildCaches();
	const auto lines = st::calls(code.storage, {implRun}, st::Direction::Incoming, 1);
	REQUIRE(keys(lines) == std::set<std::wstring>{L"p.Client.go()"});
	REQUIRE(lines[0].kind == L"method via p.Iface.run()");
	REQUIRE(lines[0].file.wstr() == L"/r/p/Client.java");
	REQUIRE(lines[0].line == 7);
}

TEST_CASE("st callees reach implementations via the interface method")
{
	Code code;
	const Id iface = code.type(L"Iface", NODE_INTERFACE);
	const Id impl = code.type(L"Impl");
	const Id a = code.type(L"A");
	const Id ifaceRun = code.member(iface, L"Iface", L"run");
	const Id implRun = code.member(impl, L"Impl", L"run");
	const Id f = code.member(a, L"A", L"f");
	const Id helper = code.member(impl, L"Impl", L"helper");
	code.link(Edge::EDGE_OVERRIDE, implRun, ifaceRun, L"Impl", {3});
	code.link(Edge::EDGE_CALL, f, ifaceRun, L"A", {5});
	code.link(Edge::EDGE_CALL, implRun, helper, L"Impl", {4});
	code.storage.buildCaches();
	const auto one = st::calls(code.storage, {f}, st::Direction::Outgoing, 1);
	REQUIRE(keys(one) == std::set<std::wstring>{L"p.Iface.run()", L"p.Impl.run()"});
	REQUIRE(lineFor(one, L"p.Iface.run()")->kind == L"method");
	REQUIRE(lineFor(one, L"p.Impl.run()")->kind == L"method via p.Iface.run()");
	REQUIRE(lineFor(one, L"p.Impl.run()")->line == 5);
	const auto two = st::calls(code.storage, {f}, st::Direction::Outgoing, 2);
	REQUIRE(keys(two) == std::set<std::wstring>{L"p.Iface.run()", L"p.Impl.run()", L"p.Impl.helper()"});
}

TEST_CASE("st callers count method references")
{
	Code code;
	const Id stream = code.type(L"Stream");
	const Id conv = code.type(L"Conv");
	const Id use = code.member(stream, L"Stream", L"use");
	const Id convert = code.member(conv, L"Conv", L"convert", NODE_METHOD, L"(int)");
	code.link(Edge::EDGE_USAGE, use, convert, L"Stream", {12});
	code.storage.buildCaches();
	const auto lines = st::calls(code.storage, {convert}, st::Direction::Incoming, 1);
	REQUIRE(keys(lines) == std::set<std::wstring>{L"p.Stream.use()"});
	REQUIRE(lines[0].line == 12);
}

TEST_CASE("st callers of a class cover its methods")
{
	Code code;
	const Id svc = code.type(L"Svc");
	const Id x = code.type(L"X");
	const Id a = code.member(svc, L"Svc", L"a");
	const Id b = code.member(svc, L"Svc", L"b");
	code.member(svc, L"Svc", L"count", NODE_FIELD);
	const Id xx = code.member(x, L"X", L"x");
	code.link(Edge::EDGE_CALL, xx, b, L"X", {4});
	code.storage.buildCaches();
	const auto expanded = st::expandClasses(code.storage, {svc, xx});
	REQUIRE(std::set<Id>(expanded.begin(), expanded.end()) == std::set<Id>{a, b, xx});
	REQUIRE(keys(st::calls(code.storage, st::expandClasses(code.storage, {svc}), st::Direction::Incoming, 1)) ==
		std::set<std::wstring>{L"p.X.x()"});
}

TEST_CASE("st callees hide fields and external symbols")
{
	Code code;
	const Id a = code.type(L"A");
	const Id b = code.type(L"B");
	const Id f = code.member(a, L"A", L"f");
	const Id count = code.member(a, L"A", L"count", NODE_FIELD);
	const Id g = code.member(b, L"B", L"g");
	const Id size = code.storage.addNode(StorageNodeData(nodeKindToInt(NODE_METHOD),
		NameHierarchy::serialize(method({L"java", L"util", L"List"}, L"size", L"int", L"()")))).first;
	code.link(Edge::EDGE_USAGE, f, count, L"A", {3});
	code.link(Edge::EDGE_CALL, f, size, L"A", {4});
	code.link(Edge::EDGE_CALL, f, g, L"A", {5});
	code.storage.buildCaches();
	REQUIRE(keys(st::calls(code.storage, {f}, st::Direction::Outgoing, 0)) == std::set<std::wstring>{L"p.B.g()"});
}

TEST_CASE("st refs list each reference location by using symbol")
{
	Code code;
	const Id x = code.type(L"X");
	const Id y = code.type(L"Y");
	const Id xx = code.member(x, L"X", L"x");
	const Id yy = code.member(y, L"Y", L"y");
	code.link(Edge::EDGE_CALL, xx, yy, L"X", {4, 9});
	code.storage.buildCaches();
	auto lines = st::references(code.storage, {yy});
	REQUIRE(lines.size() == 2);
	std::sort(lines.begin(), lines.end(), [](const auto& l, const auto& r) { return l.line < r.line; });
	REQUIRE(lines[0].key == L"p.X.x()");
	REQUIRE(lines[0].kind == L"call");
	REQUIRE(lines[0].line == 4);
	REQUIRE(lines[1].key == L"p.X.x()");
	REQUIRE(lines[1].line == 9);
	REQUIRE(st::linkKind(Edge::EDGE_USAGE) == L"usage");
	REQUIRE(st::linkKind(Edge::EDGE_TYPE_USAGE) == L"type usage");
}

TEST_CASE("st impls and supers follow inheritance transitively")
{
	Code code;
	const Id a = code.type(L"A");
	const Id b = code.type(L"B");
	const Id c = code.type(L"C");
	code.link(Edge::EDGE_INHERITANCE, b, a, L"B", {1});
	code.link(Edge::EDGE_INHERITANCE, c, b, L"C", {1});
	code.storage.buildCaches();
	REQUIRE(keys(st::hierarchy(code.storage, {a}, st::Direction::Incoming)) == std::set<std::wstring>{L"p.B", L"p.C"});
	REQUIRE(keys(st::hierarchy(code.storage, {c}, st::Direction::Outgoing)) == std::set<std::wstring>{L"p.B", L"p.A"});
}

TEST_CASE("st path returns shortest simple chains first and drops off-path nodes")
{
	Code code;
	std::map<std::wstring, Id> m;
	for (const std::wstring name: {L"A", L"B", L"C", L"D", L"E", L"Z"})
	{
		std::wstring lower = name;
		lower[0] = towlower(lower[0]);
		m[name] = code.member(code.type(name), name, lower);
	}
	code.link(Edge::EDGE_CALL, m[L"A"], m[L"B"], L"A", {2});
	code.link(Edge::EDGE_CALL, m[L"B"], m[L"D"], L"B", {3});
	code.link(Edge::EDGE_CALL, m[L"A"], m[L"C"], L"A", {4});
	code.link(Edge::EDGE_CALL, m[L"C"], m[L"E"], L"C", {5});
	code.link(Edge::EDGE_CALL, m[L"E"], m[L"D"], L"E", {6});
	code.link(Edge::EDGE_OVERRIDE, m[L"Z"], m[L"A"], L"Z", {7});
	code.storage.buildCaches();
	const auto chains = st::paths(code.storage, {m[L"A"]}, {m[L"D"]}, 6, 10);
	REQUIRE(chains.size() == 2);
	REQUIRE(chains[0].size() == 2);
	REQUIRE(chains[1].size() == 3);
	REQUIRE(chains[0][0].from == m[L"A"]);
	REQUIRE(chains[0][0].to == m[L"B"]);
	REQUIRE(chains[0][0].type == Edge::EDGE_CALL);
	REQUIRE(chains[0][0].line == 2);
	REQUIRE(chains[0][1].to == m[L"D"]);
	REQUIRE(chains[0][1].file.wstr() == L"/r/p/B.java");
	for (const auto& chain: chains)
	{
		for (const auto& hop: chain)
		{
			REQUIRE(hop.from != m[L"Z"]);
			REQUIRE(hop.to != m[L"Z"]);
		}
	}
	REQUIRE(st::paths(code.storage, {m[L"A"]}, {m[L"D"]}, 6, 1).size() == 1);
}

TEST_CASE("st freshness detects edits, new files and deletions")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QString base = QDir(dir.path()).canonicalPath();
	const QString repo = base + "/repo";
	write(repo + "/build.gradle", "plugins {}");
	write(repo + "/src/main/java/p/A.java", "class A {}");
	write(repo + "/src/main/java/p/B.java", "class B {}");
	write(base + "/prj/x.srctrlprj", QString(
		"<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<config><source_groups><source_group_g1>"
		"<gradle><project_file_path>" + repo + "/build.gradle</project_file_path><should_index_tests>0</should_index_tests></gradle>"
		"<source_extensions><source_extension>.java</source_extension></source_extensions>"
		"<status>enabled</status><type>Java Source Group from Gradle</type>"
		"</source_group_g1></source_groups><version>8</version></config>\n").toUtf8());
	ProjectSettings settings;
	REQUIRE(settings.load(FilePath((base + "/prj/x.srctrlprj").toStdWString()), true));
	const auto past = std::filesystem::file_time_type::clock::now() - std::chrono::minutes(10);
	const auto age = [&]() {
		std::filesystem::last_write_time(repo.toStdString(), past);
		for (const auto& entry: std::filesystem::recursive_directory_iterator(repo.toStdString()))
		{
			std::filesystem::last_write_time(entry.path(), past);
		}
	};
	age();
	const FilePath project(repo.toStdWString());
	utility::GradleProjectInfo info;
	info.fingerprint = utility::gradleBuildFingerprint(project);
	info.sourceDirectories[L"main"] = {project.getConcatenated(L"src/main/java")};
	utility::saveGradleProjectInfo(FilePath((base + "/prj/sourcetrail_dependencies/g1/gradle").toStdWString()), info);
	const TimeStamp indexedAt(boost::posix_time::second_clock::local_time() - boost::posix_time::minutes(5));
	const std::vector<FilePath> indexed = {FilePath((repo + "/src/main/java/p/A.java").toStdWString())};

	REQUIRE(st::checkFreshness(settings, indexedAt, indexed).reason.empty());
	REQUIRE_FALSE(st::checkFreshness(settings, indexedAt, indexed).stale);

	write(repo + "/src/main/java/p/A.java", "class A { int x; }");
	auto freshness = st::checkFreshness(settings, indexedAt, indexed);
	REQUIRE(freshness.stale);
	REQUIRE(freshness.changedFiles == 1);

	age();
	write(repo + "/src/main/java/p/C.java", "class C {}");
	REQUIRE(st::checkFreshness(settings, indexedAt, indexed).stale);

	age();
	REQUIRE(QFile::remove(repo + "/src/main/java/p/B.java"));
	freshness = st::checkFreshness(settings, indexedAt, indexed);
	REQUIRE(freshness.stale);
	REQUIRE(freshness.changedFiles == 1);

	age();
	REQUIRE_FALSE(st::checkFreshness(settings, indexedAt, indexed).stale);
	write(repo + "/build.gradle", "plugins { id 'java' }");
	REQUIRE(st::checkFreshness(settings, indexedAt, indexed).stale);
}

TEST_CASE("st freshness counts a vanished source root but not a never-created one")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QString base = QDir(dir.path()).canonicalPath();
	const QString repo = base + "/repo";
	write(repo + "/build.gradle", "plugins {}");
	write(repo + "/src/main/java/p/A.java", "class A {}");
	write(repo + "/gen/main/java/p/G.java", "class G {}");
	write(base + "/prj/x.srctrlprj", QString(
		"<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<config><source_groups><source_group_g1>"
		"<gradle><project_file_path>" + repo + "/build.gradle</project_file_path><should_index_tests>0</should_index_tests></gradle>"
		"<source_extensions><source_extension>.java</source_extension></source_extensions>"
		"<status>enabled</status><type>Java Source Group from Gradle</type>"
		"</source_group_g1></source_groups><version>8</version></config>\n").toUtf8());
	ProjectSettings settings;
	REQUIRE(settings.load(FilePath((base + "/prj/x.srctrlprj").toStdWString()), true));
	const auto past = std::filesystem::file_time_type::clock::now() - std::chrono::minutes(10);
	std::filesystem::last_write_time(repo.toStdString(), past);
	for (const auto& entry: std::filesystem::recursive_directory_iterator(repo.toStdString()))
	{
		std::filesystem::last_write_time(entry.path(), past);
	}
	const FilePath project(repo.toStdWString());
	utility::GradleProjectInfo info;
	info.fingerprint = utility::gradleBuildFingerprint(project);
	info.sourceDirectories[L"main"] = {project.getConcatenated(L"src/main/java"), project.getConcatenated(L"gen/main/java"),
		project.getConcatenated(L"src/never/java")};
	utility::saveGradleProjectInfo(FilePath((base + "/prj/sourcetrail_dependencies/g1/gradle").toStdWString()), info);
	const TimeStamp indexedAt(boost::posix_time::second_clock::local_time() - boost::posix_time::minutes(5));
	const std::vector<FilePath> indexed = {FilePath((repo + "/src/main/java/p/A.java").toStdWString()),
		FilePath((repo + "/gen/main/java/p/G.java").toStdWString())};

	const auto fresh = st::checkFreshness(settings, indexedAt, indexed);
	REQUIRE_FALSE(fresh.stale);
	REQUIRE(fresh.reason.empty());
	REQUIRE(QDir(repo + "/gen").removeRecursively());
	std::filesystem::last_write_time(repo.toStdString(), past);
	REQUIRE(st::checkFreshness(settings, indexedAt, indexed).stale);
}

TEST_CASE("st remembers its last refresh and the index time it produced")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	ProjectSettings settings(FilePath((QDir(dir.path()).canonicalPath() + "/x.srctrlprj").toStdWString()));
	REQUIRE_FALSE(st::lastRefresh(settings).has_value());
	st::recordRefresh(settings, {TimeStamp(std::string("2026-10-30 12:34:56")), TimeStamp(std::string("2026-10-30 12:35:10"))});
	const auto last = st::lastRefresh(settings);
	REQUIRE(last.has_value());
	REQUIRE(last->startedAt.toString() == "2026-10-30 12:34:56");
	REQUIRE(last->indexedAt.toString() == "2026-10-30 12:35:10");
}

TEST_CASE("st freshness baseline sees edits made during a refresh")
{
	const TimeStamp started(std::string("2026-10-30 12:00:00"));
	const TimeStamp finished(std::string("2026-10-30 12:00:30"));
	const TimeStamp later(std::string("2026-10-30 13:00:00"));
	const st::RefreshRecord last{started, finished};
	// The index is still the one st's refresh produced: a file edited at 12:00:10 is not in it.
	REQUIRE(st::freshnessBaseline(finished, last).toString() == "2026-10-30 12:00:00");
	// Someone else indexed since: their index time is the baseline.
	REQUIRE(st::freshnessBaseline(later, last).toString() == "2026-10-30 13:00:00");
	REQUIRE(st::freshnessBaseline(finished, std::nullopt).toString() == "2026-10-30 12:00:30");
}

TEST_CASE("st refs include inheritance and override links")
{
	Code code;
	const Id a = code.type(L"A", NODE_INTERFACE);
	const Id b = code.type(L"B");
	const Id run = code.member(a, L"A", L"run");
	const Id implRun = code.member(b, L"B", L"run");
	code.link(Edge::EDGE_INHERITANCE, b, a, L"B", {2});
	code.link(Edge::EDGE_OVERRIDE, implRun, run, L"B", {5});
	code.storage.buildCaches();
	const auto types = st::references(code.storage, {a});
	REQUIRE(lineFor(types, L"p.B") != nullptr);
	REQUIRE(lineFor(types, L"p.B")->kind == L"inheritance");
	REQUIRE(lineFor(types, L"p.B")->line == 2);
	const auto methods = st::references(code.storage, {run});
	REQUIRE(lineFor(methods, L"p.B.run()") != nullptr);
	REQUIRE(lineFor(methods, L"p.B.run()")->kind == L"override");
	REQUIRE(keys(st::references(code.storage, {implRun})).empty());
}

TEST_CASE("st locks the canonical project path like the indexer")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QString base = QDir(dir.path()).canonicalPath();
	write(base + "/real/x.srctrlprj");
	REQUIRE(QFile::link(base + "/real", base + "/link"));
	REQUIRE(st::projectLockFile(FilePath((base + "/link/x.srctrlprj").toStdWString())) == base + "/real/x.srctrlprj.lock");
}

TEST_CASE("lombok alignment maps reformatted code to original lines")
{
	const std::vector<std::string> generated = {"// header", "package p;", "", "public class A {", "  private final String n;",
		"  @lombok.Generated", "  public String getN() {", "    return this.n;", "  }", "  void run() {", "    go();", "  }", "}"};
	const std::vector<std::string> original = {"package p;", "import lombok.Getter;", "@Getter", "public class A {",
		"    private final String n;", "    void run() {", "        go();", "    }", "}"};
	const auto aligned = st::alignLines(generated, original);
	REQUIRE(aligned.size() == generated.size());
	CHECK(aligned[9] == 5);
	CHECK(aligned[10] == 6);
}

TEST_CASE("lombok location without a matching original line stays generated")
{
	const std::vector<std::string> generated = {"package p;", "public class A {", "  public String getN() {", "  }", "}"};
	const std::vector<std::string> original = {"package p;", "@Getter", "public class A {", "}"};
	CHECK_FALSE(st::alignLines(generated, original)[2].has_value());

	st::LombokRoots roots;
	roots.delombokToOriginalRoots.push_back({FilePath(L"/out/delombok/main"), {FilePath(L"/src/main/java")}});
	const auto read = [&](const FilePath& file) {
		return file.wstr() == L"/out/delombok/main/p/A.java" ? generated
			: file.wstr() == L"/src/main/java/p/A.java"	   ? original
															   : std::vector<std::string>();
	};
	const st::ResultLine line{L"k", L"call", FilePath(L"/out/delombok/main/p/A.java"), 3, "public String getN() {"};
	const st::ResultLine mapped = st::toOriginal(line, roots, read);
	CHECK(mapped.file.wstr() == L"/out/delombok/main/p/A.java");
	CHECK(mapped.line == 3);
	CHECK(mapped.kind == L"call (generated)");

	const st::ResultLine kept = st::toOriginal(
		{L"k", L"call", FilePath(L"/out/delombok/main/p/A.java"), 2, "public class A {"}, roots, read);
	CHECK(kept.file.wstr() == L"/src/main/java/p/A.java");
	CHECK(kept.line == 3);
	CHECK(kept.source == "public class A {");
	CHECK(kept.kind == L"call");
}

TEST_CASE("lombok location of a deleted original stays generated")
{
	st::LombokRoots roots;
	roots.delombokToOriginalRoots.push_back({FilePath(L"/out/delombok/main"), {FilePath(L"/src/main/java")}});
	const auto read = [](const FilePath& file) {
		return file.wstr() == L"/out/delombok/main/p/A.java" ? std::vector<std::string>{"package p;"}
															   : std::vector<std::string>();
	};
	const st::ResultLine mapped =
		st::toOriginal({L"k", L"call", FilePath(L"/out/delombok/main/p/A.java"), 1, "package p;"}, roots, read);
	CHECK(mapped.file.wstr() == L"/out/delombok/main/p/A.java");
	CHECK(mapped.kind == L"call (generated)");
}
