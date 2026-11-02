#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <set>
#include <sstream>

#include <boost/date_time/c_local_time_adjustor.hpp>
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
#include "StArchitecture.h"
#include "StClient.h"
#include "StGraph.h"
#include "StInput.h"
#include "StKey.h"
#include "StLombok.h"
#include "StOutput.h"
#include "StProject.h"
#include "StServer.h"
#include "StTestSupport.h"
#include "StWatch.h"
#include "StorageNode.h"
#include "StorageFile.h"
#include "StorageSymbol.h"
#include "TimeStamp.h"
#include "utilityGradle.h"
#include "utilityGradleCache.h"

using namespace st::test;

namespace
{
const NodeKindMask ALL_KINDS = (NODE_MAX_VALUE << 1) - 1;
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

TEST_CASE("st reports the upstream truncation marker it skipped")
{
	std::istringstream in("a.B\tclass\nc.D\n+12 more (--limit)\n");
	size_t truncated = 0;
	REQUIRE(st::readKeys(in, &truncated) == std::vector<std::wstring>{L"a.B", L"c.D"});
	REQUIRE(truncated == 12);
	std::istringstream complete("a.B\n");
	truncated = 7;
	st::readKeys(complete, &truncated);
	REQUIRE(truncated == 0);
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

TEST_CASE("st finds the project and relative paths from a symlinked checkout")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QString base = QDir(dir.path()).canonicalPath();
	write(base + "/real/src/A.java");
	write(base + "/proj/p.srctrlprj");
	REQUIRE(QFile::link(base + "/real", base + "/link"));
	const QString registry = base + "/reg/st-projects.tsv";
	write(registry, QString(base + "/real\t" + base + "/proj/p.srctrlprj\n").toUtf8());
	const FilePath registryFile(registry.toStdWString());
	REQUIRE(st::findProject(FilePath((base + "/link/src").toStdWString()), registryFile).wstr() ==
		(base + "/proj/p.srctrlprj").toStdWString());
	REQUIRE(st::displayPath(FilePath((base + "/real/src/A.java").toStdWString()), FilePath((base + "/link").toStdWString())) ==
		L"src/A.java");

	write(registry, QString(base + "/link\t" + base + "/proj/p.srctrlprj\n").toUtf8());
	REQUIRE(st::findProject(FilePath((base + "/real/src").toStdWString()), registryFile).wstr() ==
		(base + "/proj/p.srctrlprj").toStdWString());
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

TEST_CASE("st resolves a library symbol when no project symbol has the name")
{
	Code code;
	NameHierarchy scheduled(NAME_DELIMITER_JAVA);
	for (const wchar_t* element: {L"org", L"springframework", L"scheduling", L"annotation", L"Scheduled"})
	{
		scheduled.push(element);
	}
	// The indexer records a library annotation as a bare symbol without a definition.
	const Id annotation = code.storage.addNode(StorageNodeData(nodeKindToInt(NODE_SYMBOL), NameHierarchy::serialize(scheduled))).first;
	const Id job = code.type(L"Job");
	const Id run = code.member(job, L"Job", L"run");
	code.link(Edge::EDGE_ANNOTATION_USAGE, run, annotation, L"Job", {3});
	code.storage.buildCaches();
	REQUIRE(code.storage.getDefinitionKindForNodeId(annotation) == DEFINITION_NONE);
	for (const std::wstring name: {L"Scheduled", L"org.springframework.scheduling.annotation.Scheduled"})
	{
		const auto resolved = st::resolveSymbol(code.storage, name, ALL_KINDS);
		REQUIRE(resolved.status == st::Resolution::Found);
		REQUIRE(resolved.ids == std::vector<Id>{annotation});
	}
	const auto lines = st::references(code.storage, {annotation});
	REQUIRE(lines.size() == 1);
	REQUIRE(lines[0].key == L"p.Job.run()");
	REQUIRE(lines[0].kind == L"annotation usage");
	// find lists project symbols only
	REQUIRE(st::findSymbols(code.storage, L"Scheduled", ALL_KINDS).empty());
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
	REQUIRE(lineFor(two, L"p.Impl.run()")->kind == L"method via p.Iface.run(), depth 1");
}

TEST_CASE("st calls tier results by depth")
{
	Code code;
	const Id a = code.type(L"A");
	const Id f = code.member(a, L"A", L"f");
	const Id g = code.member(a, L"A", L"g");
	const Id h = code.member(a, L"A", L"h");
	code.link(Edge::EDGE_CALL, f, g, L"A", {3});
	code.link(Edge::EDGE_CALL, g, h, L"A", {4});
	code.storage.buildCaches();
	const auto lines = st::calls(code.storage, {f}, st::Direction::Outgoing, 2);
	REQUIRE(lineFor(lines, L"p.A.g()")->tier == 1);
	REQUIRE(lineFor(lines, L"p.A.h()")->tier == 2);
	REQUIRE(lineFor(lines, L"p.A.h()")->kind == L"method, depth 2");
	REQUIRE(lineFor(lines, L"p.A.g()")->kind == L"method, depth 1");
	const auto shallow = st::calls(code.storage, {f}, st::Direction::Outgoing, 1);
	REQUIRE(lineFor(shallow, L"p.A.g()")->kind == L"method");
	REQUIRE(lineFor(shallow, L"p.A.g()")->tier == 1);
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
	const auto chains = st::paths(code.storage, {m[L"A"]}, {m[L"D"]}, 6, 10, st::PATH_EDGES);
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
	REQUIRE(st::paths(code.storage, {m[L"A"]}, {m[L"D"]}, 6, 1, st::PATH_EDGES).size() == 1);
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

TEST_CASE("st freshness is stale when the Gradle source cache is missing")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QString base = QDir(dir.path()).canonicalPath();
	const QString repo = base + "/repo";
	write(repo + "/build.gradle", "plugins {}");
	write(repo + "/src/main/java/p/A.java", "class A {}");
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
	const TimeStamp indexedAt(boost::posix_time::second_clock::local_time() - boost::posix_time::minutes(5));
	const std::vector<FilePath> indexed = {FilePath((repo + "/src/main/java/p/A.java").toStdWString())};

	const auto freshness = st::checkFreshness(settings, indexedAt, indexed);
	REQUIRE(freshness.stale);
	REQUIRE(freshness.reason.find("Gradle source information") != std::string::npos);
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

TEST_CASE("scheduler waits for a quiet period")
{
	st::RefreshScheduler scheduler(500);
	REQUIRE_FALSE(scheduler.pending());
	REQUIRE_FALSE(scheduler.due(10000));
	scheduler.changed(0);
	scheduler.changed(200);
	scheduler.changed(400);
	REQUIRE(scheduler.pending());
	REQUIRE_FALSE(scheduler.due(800));
	REQUIRE(scheduler.due(900));
	scheduler.started();
	REQUIRE(scheduler.running());
	REQUIRE_FALSE(scheduler.pending());
	REQUIRE_FALSE(scheduler.due(5000));
	scheduler.finished();
	REQUIRE_FALSE(scheduler.running());
	REQUIRE_FALSE(scheduler.due(5000));
}

TEST_CASE("scheduler queues another refresh when changes arrive while one runs")
{
	st::RefreshScheduler scheduler(500);
	scheduler.changed(0);
	REQUIRE(scheduler.due(500));
	scheduler.started();
	scheduler.changed(1000);
	REQUIRE_FALSE(scheduler.due(2000));
	scheduler.finished();
	REQUIRE(scheduler.pending());
	REQUIRE_FALSE(scheduler.due(1400));
	REQUIRE(scheduler.due(1500));
}

TEST_CASE("pending refresh record is written at the next index read unless a later refresh recorded itself")
{
	const TimeStamp started(std::string("2026-11-02 10:00:00"));
	const TimeStamp produced(std::string("2026-11-02 10:00:12"));
	st::PendingRecord pending;
	REQUIRE_FALSE(pending.take(produced, std::nullopt).has_value());

	// The read after the refresh failed (project busy): the record waits for the next one.
	pending.succeeded(started);
	REQUIRE(pending.pending());
	const st::RefreshRecord older{TimeStamp(std::string("2026-11-02 09:00:00")), TimeStamp(std::string("2026-11-02 09:00:10"))};
	const auto record = pending.take(produced, older);
	REQUIRE(record.has_value());
	CHECK(record->startedAt.toString() == started.toString());
	CHECK(record->indexedAt.toString() == produced.toString());
	CHECK_FALSE(pending.pending());
	CHECK_FALSE(pending.take(produced, older).has_value());

	// A later index time is still recorded against this start: the earlier baseline can only cause a refresh.
	pending.succeeded(started);
	const auto later = pending.take(TimeStamp(std::string("2026-11-02 10:05:00")), std::nullopt);
	REQUIRE(later.has_value());
	CHECK(later->startedAt.toString() == started.toString());

	// A refresh that started after this one recorded itself: its record stays.
	pending.succeeded(started);
	const st::RefreshRecord newer{TimeStamp(std::string("2026-11-02 10:01:00")), TimeStamp(std::string("2026-11-02 10:01:09"))};
	CHECK_FALSE(pending.take(produced, newer).has_value());
	CHECK_FALSE(pending.pending());
}

namespace
{
// A Gradle project at <base>/repo with its project file at <base>/prj/x.srctrlprj and src/main/java cached as its
// source root.
void gradleFixture(const QString& base, ProjectSettings* settings)
{
	const QString repo = base + "/repo";
	write(repo + "/build.gradle", "plugins {}");
	write(base + "/prj/x.srctrlprj", QString(
		"<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<config><source_groups><source_group_g1>"
		"<gradle><project_file_path>" + repo + "/build.gradle</project_file_path><should_index_tests>0</should_index_tests></gradle>"
		"<source_extensions><source_extension>.java</source_extension></source_extensions>"
		"<status>enabled</status><type>Java Source Group from Gradle</type>"
		"</source_group_g1></source_groups><version>8</version></config>\n").toUtf8());
	REQUIRE(settings->load(FilePath((base + "/prj/x.srctrlprj").toStdWString()), true));
	const FilePath project(repo.toStdWString());
	utility::GradleProjectInfo info;
	info.fingerprint = utility::gradleBuildFingerprint(project);
	info.sourceDirectories[L"main"] = {project.getConcatenated(L"src/main/java")};
	utility::saveGradleProjectInfo(FilePath((base + "/prj/sourcetrail_dependencies/g1/gradle").toStdWString()), info);
}
}

TEST_CASE("watch paths skip build output and delombok copies")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QString base = QDir(dir.path()).canonicalPath();
	const QString repo = base + "/repo";
	write(repo + "/src/main/java/p/A.java", "class A {}");
	write(repo + "/src/main/java/p/q/B.java", "class B {}");
	write(repo + "/src/main/java/p/notes.txt", "");
	write(repo + "/src/main/java/build/C.java", "class C {}");
	write(repo + "/build/generated/p/G.java", "class G {}");
	write(repo + "/.gradle/p/H.java", "class H {}");
	write(repo + "/.git/p/I.java", "class I {}");
	ProjectSettings settings;
	gradleFixture(base, &settings);
	const QString delombok = base + "/prj/sourcetrail_dependencies/g1/delombok/main/p/A.java";
	write(delombok, "class A {}");
	const std::vector<FilePath> indexed = {FilePath(delombok.toStdWString()),
		FilePath((repo + "/build/generated/p/G.java").toStdWString()), FilePath((repo + "/src/main/java/p/A.java").toStdWString())};

	const std::vector<QString> paths = st::watchPaths(settings, indexed);
	const std::set<QString> watched(paths.begin(), paths.end());
	CHECK(watched.size() == paths.size());
	for (const QString& expected: {repo + "/build.gradle", repo + "/src/main/java", repo + "/src/main/java/p",
			 repo + "/src/main/java/p/q", repo + "/src/main/java/p/A.java", repo + "/src/main/java/p/q/B.java"})
	{
		CHECK(watched.count(expected));
	}
	for (const QString& path: watched)
	{
		INFO(path.toStdString());
		CHECK_FALSE(path.contains("/build/"));
		CHECK_FALSE(path.endsWith("/build"));
		CHECK_FALSE(path.contains("/.gradle"));
		CHECK_FALSE(path.contains("/.git"));
		CHECK_FALSE(path.contains("sourcetrail_dependencies"));
		CHECK_FALSE(path.endsWith(".txt"));
	}
}

TEST_CASE("watch paths fall back to directories above the limit")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QString base = QDir(dir.path()).canonicalPath();
	const QString repo = base + "/repo";
	write(repo + "/src/main/java/p/A.java", "class A {}");
	ProjectSettings settings;
	gradleFixture(base, &settings);
	std::vector<FilePath> indexed;
	for (int i = 0; i < 20001; i++)
	{
		indexed.push_back(FilePath((repo + "/src/main/java/p/r" + QString::number(i % 3) + "/F" + QString::number(i) + ".java").toStdWString()));
	}

	const std::vector<QString> paths = st::watchPaths(settings, indexed);
	const std::set<QString> watched(paths.begin(), paths.end());
	CHECK(watched.count(repo + "/src/main/java"));
	CHECK(watched.count(repo + "/src/main/java/p"));
	for (int i = 0; i < 3; i++)
	{
		CHECK(watched.count(repo + "/src/main/java/p/r" + QString::number(i)));
	}
	for (const QString& path: watched)
	{
		INFO(path.toStdString());
		CHECK_FALSE(path.endsWith(".java"));
		CHECK_FALSE(path.endsWith(".gradle"));
	}
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

TEST_CASE("st freshness tells an edit just before a refresh from one during it within the same second")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QString base = QDir(dir.path()).canonicalPath();
	const QString repo = base + "/repo";
	write(repo + "/build.gradle", "plugins {}");
	write(repo + "/src/main/java/p/A.java", "class A {}");
	write(base + "/prj/x.srctrlprj", QString(
		"<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<config><source_groups><source_group_g1>"
		"<gradle><project_file_path>" + repo + "/build.gradle</project_file_path><should_index_tests>0</should_index_tests></gradle>"
		"<source_extensions><source_extension>.java</source_extension></source_extensions>"
		"<status>enabled</status><type>Java Source Group from Gradle</type>"
		"</source_group_g1></source_groups><version>8</version></config>\n").toUtf8());
	ProjectSettings settings;
	REQUIRE(settings.load(FilePath((base + "/prj/x.srctrlprj").toStdWString()), true));
	// A whole second two minutes ago, as a file time and as the local time st records.
	const auto second = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now() - std::chrono::minutes(2));
	const auto fileTime = [&](std::chrono::milliseconds offset) {
		return std::filesystem::file_time_type::clock::now() +
			std::chrono::duration_cast<std::filesystem::file_time_type::duration>(second + offset - std::chrono::system_clock::now());
	};
	const boost::posix_time::ptime localSecond = boost::date_time::c_local_adjustor<boost::posix_time::ptime>::utc_to_local(
		boost::posix_time::from_time_t(std::chrono::system_clock::to_time_t(second)));
	const auto past = std::filesystem::file_time_type::clock::now() - std::chrono::minutes(10);
	std::filesystem::last_write_time(repo.toStdString(), past);
	for (const auto& entry: std::filesystem::recursive_directory_iterator(repo.toStdString()))
	{
		std::filesystem::last_write_time(entry.path(), past);
	}
	const FilePath project(repo.toStdWString());
	utility::GradleProjectInfo info;
	info.fingerprint = utility::gradleBuildFingerprint(project);
	info.sourceDirectories[L"main"] = {project.getConcatenated(L"src/main/java")};
	utility::saveGradleProjectInfo(FilePath((base + "/prj/sourcetrail_dependencies/g1/gradle").toStdWString()), info);
	const TimeStamp indexedAt(localSecond + boost::posix_time::seconds(3));
	const std::vector<FilePath> indexed = {FilePath((repo + "/src/main/java/p/A.java").toStdWString())};
	st::recordRefresh(settings, {TimeStamp(localSecond + boost::posix_time::milliseconds(600)), indexedAt});
	const TimeStamp baseline = st::freshnessBaseline(indexedAt, st::lastRefresh(settings));
	const std::string a = (repo + "/src/main/java/p/A.java").toStdString();

	// Saved 300 ms before the refresh started: the refresh indexed it.
	std::filesystem::last_write_time(a, fileTime(std::chrono::milliseconds(300)));
	REQUIRE_FALSE(st::checkFreshness(settings, baseline, indexed).stale);
	// Saved 200 ms after it started: the refresh may have missed it.
	std::filesystem::last_write_time(a, fileTime(std::chrono::milliseconds(800)));
	REQUIRE(st::checkFreshness(settings, baseline, indexed).stale);
	// A file written right after a refresh starts is never older than its start.
	std::filesystem::last_write_time(a, past);
	const TimeStamp started = st::refreshStart();
	write(repo + "/src/main/java/p/A.java", "class A { int x; }");
	REQUIRE(st::checkFreshness(settings, started, indexed).stale);
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

TEST_CASE("st refs leave out imports")
{
	Code code;
	const Id x = code.type(L"X");
	const Id y = code.type(L"Y");
	code.link(Edge::EDGE_IMPORT, code.file(L"X"), y, L"X", {1});
	code.link(Edge::EDGE_TYPE_USAGE, x, y, L"X", {5});
	code.storage.buildCaches();
	const auto lines = st::references(code.storage, {y});
	REQUIRE(lines.size() == 1);
	REQUIRE(lines[0].line == 5);
}

TEST_CASE("st definitions print the declaration line, not the annotation line")
{
	Code code;
	const Id run = add(code.storage, method({L"p", L"A"}, L"run", L"void", L"()"));
	const Id file = code.file(L"A");
	code.storage.addOccurrence(StorageOccurrence(run, code.storage.addSourceLocation(
		StorageSourceLocationData(file, 21, 1, 30, 2, locationTypeToInt(LOCATION_SCOPE)))));
	code.storage.addOccurrence(StorageOccurrence(run, code.storage.addSourceLocation(
		StorageSourceLocationData(file, 22, 8, 22, 10, locationTypeToInt(LOCATION_TOKEN)))));
	code.storage.buildCaches();
	const auto lines = st::definitionLines(code.storage, run, L"p.A.run()");
	REQUIRE(lines.size() == 1);
	REQUIRE(lines[0].line == 22);
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
	CHECK(mapped.file.wstr() == L"/src/main/java/p/A.java");
	CHECK(mapped.line == 0);
	CHECK(mapped.generated);
	CHECK(mapped.kind == L"call");

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

TEST_CASE("lombok alignment refuses a line whose surroundings repeat in the generated file")
{
	const std::vector<std::string> original = {"a();", "X();", "b();"};
	const std::vector<std::string> generated = {"a();", "X();", "b();", "void m() {", "a();", "X();", "b();", "}"};
	const auto aligned = st::alignLines(generated, original);
	CHECK_FALSE(aligned[1].has_value());
	CHECK_FALSE(aligned[5].has_value());
}

TEST_CASE("lombok alignment rejects repeated text matched to the wrong occurrence")
{
	const std::vector<std::string> original = {"int x;", "}"};
	const std::vector<std::string> generated = {"int x;", "int getX() {", "return x;", "}", "}"};
	const auto aligned = st::alignLines(generated, original);
	CHECK(aligned[0] == 0);
	CHECK_FALSE(aligned[3].has_value());
	CHECK(aligned[4] == 1);
}

TEST_CASE("lombok alignment keeps a duplicated line only where its neighbours agree")
{
	const std::vector<std::string> original = {"class A {", "A(String n) {", "this.n = n;", "}", "}"};
	const std::vector<std::string> generated = {
		"class A {", "A(String n) {", "this.n = n;", "}", "void setN(String n) {", "this.n = n;", "}", "}"};
	const auto aligned = st::alignLines(generated, original);
	CHECK(aligned[2] == 2);
	CHECK_FALSE(aligned[5].has_value());
}

TEST_CASE("lombok roots map each root's delombok directory to that root alone")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QString base = QDir(dir.path()).canonicalPath();
	const QString repo = base + "/repo";
	write(repo + "/build.gradle", "plugins {}");
	write(repo + "/a/src/test/java/p/Same.java", "class Same {}");
	write(repo + "/b/src/test/java/p/Same.java", "class Same { int x; }");
	write(base + "/prj/x.srctrlprj", QString(
		"<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<config><source_groups><source_group_g1>"
		"<gradle><project_file_path>" + repo + "/build.gradle</project_file_path><should_index_tests>1</should_index_tests></gradle>"
		"<source_extensions><source_extension>.java</source_extension></source_extensions>"
		"<status>enabled</status><type>Java Source Group from Gradle</type>"
		"</source_group_g1></source_groups><version>8</version></config>\n").toUtf8());
	ProjectSettings settings;
	REQUIRE(settings.load(FilePath((base + "/prj/x.srctrlprj").toStdWString()), true));
	const FilePath project(repo.toStdWString());
	const FilePath output((base + "/prj/sourcetrail_dependencies/g1/gradle").toStdWString());
	utility::GradleProjectInfo info;
	info.fingerprint = utility::gradleBuildFingerprint(project);
	info.includesTests = true;
	const std::vector<FilePath> roots = {project.getConcatenated(L"a/src/test/java"), project.getConcatenated(L"b/src/test/java")};
	info.sourceDirectories[L"test"] = roots;
	utility::saveGradleProjectInfo(output, info);
	const std::vector<FilePath> generated = utility::getDelombokRootDirectories(
		utility::getDelombokOutputDirectory(output, L"test"), roots, project);
	REQUIRE(generated.size() == 2);
	REQUIRE(generated[0] != generated[1]);
	for (const FilePath& directory: generated)
	{
		write(QString::fromStdWString(directory.wstr()) + "/p/Same.java", "class Same {}");
	}

	const st::LombokRoots lombok = st::lombokRoots(settings);
	for (size_t i = 0; i < 2; i++)
	{
		const auto original = st::toOriginalFile(generated[i].getConcatenated(L"p/Same.java"), lombok);
		REQUIRE(original.has_value());
		CHECK(original->wstr() == roots[i].getConcatenated(L"p/Same.java").wstr());
		const auto copy = st::toGeneratedFile(roots[i].getConcatenated(L"p/Same.java"), lombok);
		REQUIRE(copy.has_value());
		CHECK(copy->wstr() == generated[i].getConcatenated(L"p/Same.java").wstr());
	}
}

TEST_CASE("lombok location whose path exists under two original roots stays generated")
{
	st::LombokRoots roots;
	roots.delombokToOriginalRoots.push_back(
		{FilePath(L"/out/delombok/main"), {FilePath(L"/src/a"), FilePath(L"/src/b")}});
	const auto read = [](const FilePath& file) {
		const std::wstring path = file.wstr();
		return path == L"/out/delombok/main/p/A.java" || path == L"/src/a/p/A.java" || path == L"/src/b/p/A.java"
			? std::vector<std::string>{"package p;"}
			: std::vector<std::string>();
	};
	const st::ResultLine mapped =
		st::toOriginal({L"k", L"call", FilePath(L"/out/delombok/main/p/A.java"), 1, "package p;"}, roots, read);
	CHECK(mapped.file.wstr() == L"/out/delombok/main/p/A.java");
	CHECK(mapped.kind == L"call (generated)");
}

TEST_CASE("lombok location reached through a symlinked delombok root still maps")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const std::filesystem::path base = std::filesystem::canonical(dir.path().toStdString());
	std::filesystem::create_directories(base / "real" / "p");
	std::filesystem::create_symlink(base / "real", base / "link");
	st::LombokRoots roots;
	roots.delombokToOriginalRoots.push_back({FilePath((base / "real").wstring()), {FilePath(L"/src/main/java")}});
	const auto read = [](const FilePath& file) {
		const std::wstring path = file.wstr();
		return path.find(L"/src/main/java/p/A.java") == 0 ? std::vector<std::string>{"package p;"}
														  : std::vector<std::string>{"package p;"};
	};
	const st::ResultLine mapped =
		st::toOriginal({L"k", L"call", FilePath((base / "link" / "p" / "A.java").wstring()), 1, "x"}, roots, read);
	CHECK(mapped.file.wstr() == L"/src/main/java/p/A.java");
}

TEST_CASE("lombok range end without a counterpart is dropped")
{
	const std::vector<std::string> generated = {"package p;", "public class A {", "  void f() {", "    int z = 1;", "  public String getN() {", "    return n;", "}"};
	const std::vector<std::string> original = {"package p;", "public class A {", "  void f() {", "    int z = 1;", "}"};
	st::LombokRoots roots;
	roots.delombokToOriginalRoots.push_back({FilePath(L"/out/delombok/main"), {FilePath(L"/src/main/java")}});
	const auto read = [&](const FilePath& file) {
		return file.wstr() == L"/out/delombok/main/p/A.java" ? generated
			: file.wstr() == L"/src/main/java/p/A.java"	   ? original
															   : std::vector<std::string>();
	};
	st::ResultLine line{L"k", L"method", FilePath(L"/out/delombok/main/p/A.java"), 3, "void f() {"};
	line.endLine = 4;
	const st::ResultLine mapped = st::toOriginal(line, roots, read);
	CHECK(mapped.line == 3);
	CHECK(mapped.endLine == 4);

	line.endLine = 6;
	const st::ResultLine dropped = st::toOriginal(line, roots, read);
	CHECK(dropped.file.wstr() == L"/src/main/java/p/A.java");
	CHECK(dropped.line == 3);
	CHECK(dropped.endLine == 0);
}

TEST_CASE("lombok alignment maps every line of a byte-identical copy")
{
	const std::vector<std::string> lines = {"class A {\n", "void f() {\n", "a();\n", "}\n", "void g() {\n", "a();\n", "}\n",
		"void f() {\n", "a();\n", "}\n", "}\n"};
	const auto aligned = st::alignLines(lines, lines);
	for (size_t i = 0; i < lines.size(); i++)
	{
		CHECK(aligned[i] == i);
	}
	const auto read = [&](const FilePath&) { return lines; };
	CHECK(st::toGeneratedLine(FilePath(L"/o/A.java"), FilePath(L"/g/A.java"), 4, read) == 4);
	CHECK(st::toGeneratedLine(FilePath(L"/o/A.java"), FilePath(L"/g/A.java"), 7, read) == 7);
}

namespace
{
const std::vector<std::string> JOINED_ORIGINAL = {"class M {", "@Getter", "private int n;", "Optional<P> get(Order o) {",
	"return gw.find(b.getUuid())", "    .map(this::project);", "}", "public void alter(UUID id, Alter a,",
	"        Strategy s) {", "go();", "}", "}"};
const std::vector<std::string> JOINED_GENERATED = {"class M {", "private int n;", "Optional<P> get(Order o) {",
	"return gw.find(b.getUuid()).map(this::project);", "}", "public void alter(UUID id, Alter a, Strategy s) {", "go();",
	"}", "@lombok.Generated", "public int getN() {", "return this.n;", "}", "}"};

std::vector<std::string> joinedLines(const FilePath& file)
{
	return file.wstr() == L"/o/M.java" ? JOINED_ORIGINAL : JOINED_GENERATED;
}
}

TEST_CASE("lombok alignment maps a statement delombok joined onto one line")
{
	const auto aligned = st::alignLines(JOINED_GENERATED, JOINED_ORIGINAL);
	CHECK(aligned[3] == 4);
	CHECK(st::toGeneratedLine(FilePath(L"/o/M.java"), FilePath(L"/g/M.java"), 5, joinedLines) == 4);
	CHECK(st::toGeneratedLine(FilePath(L"/o/M.java"), FilePath(L"/g/M.java"), 6, joinedLines) == 4);
}

TEST_CASE("lombok alignment maps a method signature delombok joined onto one line")
{
	const auto aligned = st::alignLines(JOINED_GENERATED, JOINED_ORIGINAL);
	CHECK(aligned[5] == 7);
	CHECK(st::toGeneratedLine(FilePath(L"/o/M.java"), FilePath(L"/g/M.java"), 8, joinedLines) == 6);
	CHECK(st::toGeneratedLine(FilePath(L"/o/M.java"), FilePath(L"/g/M.java"), 9, joinedLines) == 6);
}

TEST_CASE("lombok alignment leaves a joined line unmapped when its run is not unique")
{
	const std::vector<std::string> original = {"class A {", "@Getter", "int n;", "void f() {", "go(a,", "b);", "go(a,", "b);",
		"}", "}"};
	const std::vector<std::string> generated = {"class A {", "int n;", "void f() {", "go(a, b);", "}",
		"@lombok.Generated", "int getN() {", "return n;", "}", "}"};
	const auto aligned = st::alignLines(generated, original);
	CHECK_FALSE(aligned[3].has_value());
	const auto read = [&](const FilePath& file) { return file.wstr() == L"/o/A.java" ? original : generated; };
	for (size_t line = 5; line <= 8; line++)
	{
		CHECK_FALSE(st::toGeneratedLine(FilePath(L"/o/A.java"), FilePath(L"/g/A.java"), line, read).has_value());
	}

	// One run, matched by two generated lines.
	const std::vector<std::string> twice = {"class A {", "int n;", "void f() {", "go(a, b);", "go(a, b);", "}",
		"@lombok.Generated", "int getN() {", "return n;", "}", "}"};
	const std::vector<std::string> once = {"class A {", "@Getter", "int n;", "void f() {", "go(a,", "b);", "}", "}"};
	const auto twiceAligned = st::alignLines(twice, once);
	CHECK_FALSE(twiceAligned[3].has_value());
	CHECK_FALSE(twiceAligned[4].has_value());
}

TEST_CASE("lombok location without a counterpart line names the original file only")
{
	st::LombokRoots roots;
	roots.delombokToOriginalRoots.push_back({FilePath(L"/out/delombok/main"), {FilePath(L"/r/src")}});
	const auto read = [](const FilePath& file) {
		return file.wstr() == L"/out/delombok/main/p/M.java" ? JOINED_GENERATED
			: file.wstr() == L"/r/src/p/M.java"			  ? JOINED_ORIGINAL
															  : std::vector<std::string>();
	};
	const st::ResultLine getter =
		st::toOriginal({L"p.M.getN()", L"method", FilePath(L"/out/delombok/main/p/M.java"), 10, "public int getN() {"}, roots, read);
	CHECK(getter.generated);
	CHECK(getter.file.wstr() == L"/r/src/p/M.java");

	st::OutputOptions options;
	options.cwd = FilePath(L"/r");
	std::ostringstream out;
	REQUIRE(st::printResults({getter, {L"p.M.get(Order)", L"method", FilePath(L"/r/src/p/M.java"), 4, "Optional<P> get(Order o) {"}},
				options, out) == 0);
	CHECK(out.str() ==
		"p.M.getN()\tmethod\tsrc/p/M.java (generated)\tpublic int getN() {\n"
		"p.M.get(Order)\tmethod\tsrc/p/M.java:4\tOptional<P> get(Order o) {\n");

	options.hideGenerated = true;
	std::ostringstream shown;
	REQUIRE(st::printResults({getter}, options, shown) == 1);
	CHECK(shown.str().empty());
}

TEST_CASE("lombok alignment leaves a joined line unmapped when a single original line matches it as well")
{
	const std::vector<std::string> original = {"class A {", "@Getter", "int n;", "void f() {", "go(a,", "b);", "go(a,b);", "}", "}"};
	const std::vector<std::string> generated = {"class A {", "int n;", "void f() {", "go(a, b);", "}", "@lombok.Generated",
		"int getN() {", "return n;", "}", "}"};
	const auto aligned = st::alignLines(generated, original);
	CHECK_FALSE(aligned[3].has_value());
	const auto read = [&](const FilePath& file) { return file.wstr() == L"/o/A.java" ? original : generated; };
	for (size_t line = 5; line <= 7; line++)
	{
		CHECK_FALSE(st::toGeneratedLine(FilePath(L"/o/A.java"), FilePath(L"/g/A.java"), line, read).has_value());
	}
}

namespace
{
st::LombokRoots joinedRoots()
{
	st::LombokRoots roots;
	roots.delombokToOriginalRoots.push_back({FilePath(L"/g"), {FilePath(L"/o")}});
	return roots;
}

st::ResultLine joinedAt(size_t line, size_t column)
{
	st::ResultLine result{L"k", L"call", FilePath(L"/g/M.java"), line, ""};
	result.column = column;
	return result;
}
}

TEST_CASE("lombok location inside a joined statement maps to the original line holding its column")
{
	const st::LombokRoots roots = joinedRoots();
	// "return gw.find(b.getUuid()).map(this::project);": `find` at column 11, `map` at column 29.
	const st::ResultLine find = st::toOriginal(joinedAt(4, 11), roots, joinedLines);
	CHECK(find.line == 5);
	CHECK(find.source == "return gw.find(b.getUuid())");
	const st::ResultLine map = st::toOriginal(joinedAt(4, 29), roots, joinedLines);
	CHECK_FALSE(map.generated);
	CHECK(map.line == 6);
	CHECK(map.source == "    .map(this::project);");
	// "public void alter(UUID id, Alter a, Strategy s) {": `Strategy` at column 37.
	CHECK(st::toOriginal(joinedAt(6, 37), roots, joinedLines).line == 9);
	CHECK(st::toOriginal(joinedAt(6, 13), roots, joinedLines).line == 8);
}

TEST_CASE("lombok location inside a joined statement without a placeable column stays generated")
{
	const st::LombokRoots roots = joinedRoots();
	for (const size_t column: {0, 7, 200})
	{
		const st::ResultLine mapped = st::toOriginal(joinedAt(4, column), roots, joinedLines);
		CHECK(mapped.generated);
		CHECK(mapped.line == 0);
		CHECK(mapped.file.wstr() == L"/o/M.java");
	}
	// A line that maps one to one needs no column.
	CHECK(st::toOriginal(joinedAt(3, 0), roots, joinedLines).line == 4);
}

TEST_CASE("lombok joined statement columns count a tab and a non-ascii character as one")
{
	const std::vector<std::string> original = {"class A {", "@Getter", "int n;", "void f() {", "\t\tlog(\"\xc3\xb5\",",
		"\t\t\tgo());", "}", "}"};
	const std::vector<std::string> generated = {"class A {", "int n;", "void f() {", "\t\tlog(\"\xc3\xb5\", go());", "}",
		"@lombok.Generated", "int getN() {", "return n;", "}", "}"};
	st::LombokRoots roots;
	roots.delombokToOriginalRoots.push_back({FilePath(L"/g"), {FilePath(L"/o")}});
	const auto read = [&](const FilePath& file) { return file.wstr() == L"/o/A.java" ? original : generated; };
	st::ResultLine go{L"k", L"call", FilePath(L"/g/A.java"), 4, ""};
	// Two tabs, `log("õ",` and a space: `go` starts at column 12.
	go.column = 12;
	CHECK(st::toOriginal(go, roots, read).line == 6);
	go.column = 3;
	CHECK(st::toOriginal(go, roots, read).line == 5);
}

TEST_CASE("wire request and reply round-trip")
{
	for (const QByteArray& input: {QByteArray(), QByteArray(1024 * 1024, 'k')})
	{
		const st::WireRequest sent{st::PROTOCOL_VERSION, "/bin/st|1|2",
			{{"st", "callers", QString::fromUtf8("Grüße.日本()")}, QString::fromUtf8("/tmp/ä ö"), false}, input};
		st::WireRequest received;
		REQUIRE(st::decode(st::encode(sent), &received));
		CHECK(received.version == sent.version);
		CHECK(received.buildId == sent.buildId);
		CHECK(received.request.arguments == sent.request.arguments);
		CHECK(received.request.cwd == sent.request.cwd);
		CHECK(received.request.stdinIsTerminal == false);
		CHECK(received.stdinData == input);
	}
	const st::WireReply sent{3, QByteArray("a\tb\n"), QByteArray::fromStdString("st: ü\n"), true, "is from another build"};
	st::WireReply received;
	REQUIRE(st::decode(st::encode(sent), &received));
	CHECK(received.exitCode == 3);
	CHECK(received.out == sent.out);
	CHECK(received.err == sent.err);
	CHECK(received.refused);
	CHECK(received.refusal == sent.refusal);

	st::WireReply truncated;
	CHECK_FALSE(st::decode(st::encode(sent).left(5), &truncated));
}

TEST_CASE("socket name is stable, per project, and short")
{
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QString base = QDir(dir.path()).canonicalPath();
	REQUIRE(QDir(base).mkpath("a/" + QString(150, 'x')));
	REQUIRE(QDir(base).mkpath("b"));
	const QString first = base + "/a/" + QString(150, 'x') + "/p.srctrlprj";
	const QString second = base + "/b/p.srctrlprj";
	for (const QString& path: {first, second})
	{
		QFile file(path);
		REQUIRE(file.open(QIODevice::WriteOnly));
	}
	REQUIRE(QFile::link(base + "/b", base + "/link"));

	const QString name = st::serverSocketName(FilePath(first.toStdWString()));
	CHECK(name == st::serverSocketName(FilePath(first.toStdWString())));
	CHECK(name != st::serverSocketName(FilePath(second.toStdWString())));
	CHECK(name.size() <= 100);
	CHECK(name.endsWith(".sock"));
	CHECK(st::serverSocketName(FilePath((base + "/link/p.srctrlprj").toStdWString())) ==
		st::serverSocketName(FilePath(second.toStdWString())));
}

namespace
{
// Accepts one connection on the project's socket and reads its request. With a reply it acknowledges the request and
// replies; without one it holds the connection for holdMs and closes it unacknowledged.
class FakeServer
{
public:
	FakeServer(const QString& name, std::optional<st::WireReply> reply, int holdMs = 0)
		: m_thread(QThread::create([this, name, reply, holdMs]() {
			QLocalServer server;
			QLocalServer::removeServer(name);
			m_listening.set_value(server.listen(name));
			if (!server.waitForNewConnection(5000))
			{
				return;
			}
			QLocalSocket* socket = server.nextPendingConnection();
			QByteArray payload;
			m_received = st::readFrame(*socket, &payload, 5000) && st::decode(payload, &m_request);
			if (reply)
			{
				st::writeAck(*socket, 5000);
				st::writeFrame(*socket, st::encode(*reply), 5000);
			}
			else
			{
				QThread::msleep(holdMs);
			}
			socket->disconnectFromServer();
			delete socket;
		}))
	{
		m_thread->start();
	}

	~FakeServer()
	{
		m_thread->wait();
	}

	bool listening()
	{
		return m_listening.get_future().get();
	}

	// The request the server read; waits for it to finish.
	std::optional<st::WireRequest> request()
	{
		m_thread->wait();
		return m_received ? std::optional<st::WireRequest>(m_request) : std::nullopt;
	}

private:
	bool m_received = false;
	st::WireRequest m_request;
	std::promise<bool> m_listening;
	std::unique_ptr<QThread> m_thread;
};
}

TEST_CASE("client answers cold when the server is from another build")
{
	int argc = 1;
	char name[] = "st";
	char* argv[] = {name, nullptr};
	QCoreApplication app(argc, argv);
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const FilePath project((QDir(dir.path()).canonicalPath() + "/p.srctrlprj").toStdWString());
	const st::Request request{{"st", "callers"}, dir.path(), false};
	QElapsedTimer timer;
	qint64 asked = 0;
	const auto ask = [&](std::optional<st::WireReply> reply, std::ostringstream& out, std::ostringstream& err, int holdMs = 0) {
		FakeServer server(st::serverSocketName(project), reply, holdMs);
		REQUIRE(server.listening());
		timer.start();
		const std::optional<int> code = st::tryServer(project, request, "key\n", out, err);
		asked = timer.elapsed();
		return std::make_pair(code, server.request());
	};

	std::ostringstream out;
	std::ostringstream err;
	const auto refused = ask(st::WireReply{0, "ignored\n", {}, true, "is from another build"}, out, err);
	CHECK_FALSE(refused.first);
	CHECK(out.str().empty());
	CHECK(err.str() == "st: server is from another build; answering without it\n");
	REQUIRE(refused.second);
	CHECK(refused.second->version == st::PROTOCOL_VERSION);
	CHECK(refused.second->buildId == st::buildId());
	CHECK(refused.second->request.arguments == request.arguments);
	CHECK(refused.second->stdinData == "key\n");

	std::ostringstream out2;
	std::ostringstream err2;
	CHECK_FALSE(ask(std::nullopt, out2, err2).first);
	CHECK(err2.str() == "st: server did not answer; answering without it\n");

	std::ostringstream out3;
	std::ostringstream err3;
	CHECK(ask(st::WireReply{2, "a\n", "st: ambiguous\n"}, out3, err3).first == std::optional<int>(2));
	CHECK(out3.str() == "a\n");
	CHECK(err3.str() == "st: ambiguous\n");

	// a server that takes the request but never acknowledges it, e.g. a suspended one, is given up on after 2 s
	std::ostringstream out4;
	std::ostringstream err4;
	CHECK_FALSE(ask(std::nullopt, out4, err4, 4000).first);
	CHECK(asked >= 1900);
	CHECK(asked < 3000);
	CHECK(err4.str() == "st: server did not answer; answering without it\n");
}

TEST_CASE("frame read gives up on a slow client after one deadline")
{
	int argc = 1;
	char program[] = "st";
	char* argv[] = {program, nullptr};
	QCoreApplication app(argc, argv);
	QTemporaryDir dir;
	REQUIRE(dir.isValid());
	const QString name = dir.path() + "/s.sock";
	QLocalServer server;
	REQUIRE(server.listen(name));
	// announces 100 bytes, then trickles one byte every 100 ms: each byte arrives within the timeout, the frame does not
	std::unique_ptr<QThread> slow(QThread::create([name]() {
		QLocalSocket socket;
		socket.connectToServer(name);
		socket.waitForConnected(1000);
		const char header[4] = {0, 0, 0, 100};
		socket.write(header, 4);
		for (int i = 0; i < 20 && socket.state() == QLocalSocket::ConnectedState; i++)
		{
			socket.write("x", 1);
			socket.waitForBytesWritten(1000);
			QThread::msleep(100);
		}
	}));
	slow->start();
	REQUIRE(server.waitForNewConnection(2000));
	std::unique_ptr<QLocalSocket> first(server.nextPendingConnection());
	QByteArray payload;
	QElapsedTimer timer;
	timer.start();
	CHECK_FALSE(st::readFrame(*first, &payload, 500));
	CHECK(timer.elapsed() < 1000);
	first->disconnectFromServer();

	// the next client is still read
	std::unique_ptr<QThread> quick(QThread::create([name]() {
		QLocalSocket socket;
		socket.connectToServer(name);
		socket.waitForConnected(1000);
		st::writeFrame(socket, "hello", 1000);
		socket.waitForDisconnected(2000);
	}));
	quick->start();
	REQUIRE(server.waitForNewConnection(2000));
	std::unique_ptr<QLocalSocket> second(server.nextPendingConnection());
	CHECK(st::readFrame(*second, &payload, 500));
	CHECK(payload == "hello");
	second->disconnectFromServer();
	quick->wait();
	slow->wait();
}
