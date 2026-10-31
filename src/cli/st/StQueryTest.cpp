#include <algorithm>
#include <set>
#include <sstream>
#include <stdexcept>

#include "StArchitecture.h"
#include "StFiles.h"
#include "StFilter.h"
#include "StGraph.h"
#include "StInput.h"
#include "StKey.h"
#include "StLombok.h"
#include "StTestSupport.h"

using namespace st::test;

namespace
{
bool passes(Code& code, Id id, const st::FilterOptions& options, const std::wstring& path = L"src/A.java")
{
	return st::matchesFilter(code.storage, id, options, path);
}

std::set<std::wstring> impactKeys(Code& code, const std::vector<Id>& ids, size_t depth = 0)
{
	return keys(st::impact(code.storage, ids, depth, st::IMPACT_EDGES, st::Group::None).lines);
}

bool includes(const std::set<std::wstring>& all, const std::set<std::wstring>& some)
{
	return std::includes(all.begin(), all.end(), some.begin(), some.end());
}
}

TEST_CASE("st filter keeps symbols carrying the annotation or inside an annotated type")
{
	Code code;
	const Id ctl = code.type(L"Ctl");
	const Id get = code.member(ctl, L"Ctl", L"get");
	const Id other = code.type(L"Other");
	const Id run = code.member(other, L"Other", L"run");
	NameHierarchy endpointName(NAME_DELIMITER_JAVA);
	endpointName.push(L"example");
	endpointName.push(L"web");
	endpointName.push(L"Endpoint");
	const Id endpoint = add(code.storage, endpointName, NODE_ANNOTATION);
	code.annotate(ctl, endpoint);
	code.storage.buildCaches();

	st::FilterOptions options;
	options.annotation = L"Endpoint";
	REQUIRE(passes(code, ctl, options));
	REQUIRE(passes(code, get, options));
	REQUIRE_FALSE(passes(code, other, options));
	REQUIRE_FALSE(passes(code, run, options));
	options.annotation = L"example.web.Endpoint";
	REQUIRE(passes(code, get, options));
	options.annotation = L"point";
	REQUIRE_FALSE(passes(code, ctl, options));
	REQUIRE_FALSE(passes(code, get, options));
}

TEST_CASE("st filter combines criteria and --not negates them")
{
	Code code;
	const Id a = code.type(L"A");
	const Id run = code.member(a, L"A", L"run");
	code.storage.buildCaches();

	st::FilterOptions options;
	options.kinds = NODE_METHOD;
	options.package = L"p";
	REQUIRE(passes(code, run, options));
	REQUIRE_FALSE(passes(code, a, options));
	options.package = L"q";
	REQUIRE_FALSE(passes(code, run, options));

	options.package = L"p";
	options.invert = true;
	REQUIRE_FALSE(passes(code, run, options));
	REQUIRE(passes(code, a, options));
}

TEST_CASE("st filter matches file patterns against the name or the displayed path")
{
	Code code;
	const Id a = code.type(L"A");
	code.storage.buildCaches();

	st::FilterOptions options;
	options.filePattern = L"*Test.java";
	REQUIRE(passes(code, a, options, L"src/ATest.java"));
	REQUIRE_FALSE(passes(code, a, options, L"src/A.java"));
	options.filePattern = L"src/*";
	REQUIRE(passes(code, a, options, L"src/ATest.java"));
	REQUIRE_FALSE(passes(code, a, options, L"lib/A.java"));
	REQUIRE_FALSE(passes(code, a, options, L""));
}

TEST_CASE("st filter keeps every passing input line unchanged")
{
	std::istringstream in(
		"p.A\tcall\tsrc/B.java:3\n"
		"p.A\tcall\tsrc/B.java:9\r\n"
		"+3 more (--limit)\n");
	size_t truncated = 0;
	const auto lines = st::readLines(in, &truncated);
	REQUIRE(lines.size() == 2);
	REQUIRE(lines[0].key == L"p.A");
	REQUIRE(lines[0].raw == "p.A\tcall\tsrc/B.java:3");
	REQUIRE(lines[1].raw == "p.A\tcall\tsrc/B.java:9");
	REQUIRE(truncated == 3);
}

TEST_CASE("st symbols-in lists symbols defined in a file, not ones it references")
{
	Code code;
	const Id a = code.type(L"A");
	const Id b = code.type(L"B");
	const Id go = code.member(b, L"B", L"go");
	const Id run = add(code.storage, method({L"p", L"A"}, L"run", L"void", L"()"));
	const Id x = add(code.storage, method({L"p", L"A"}, L"x", L"", L""), NODE_FIELD);
	addLocation(code.storage, x, code.file(L"A"), 9);
	addLocation(code.storage, run, code.file(L"A"), 4);
	code.link(Edge::EDGE_CALL, run, go, L"A", {6});
	code.storage.buildCaches();

	const auto ids = st::definedInFile(code.storage, FilePath(L"/r/p/A.java"));
	REQUIRE(ids == std::vector<Id>{a, run, x});
}

TEST_CASE("st at picks the innermost scope containing the line")
{
	Code code;
	const Id a = code.type(L"A");
	const Id run = code.member(a, L"A", L"run");
	code.scope(a, L"A", 1, 20);
	code.scope(run, L"A", 5, 9);
	code.storage.buildCaches();

	const FilePath file(L"/r/p/A.java");
	REQUIRE(st::symbolAt(code.storage, file, 7) == run);
	REQUIRE(st::symbolAt(code.storage, file, 12) == a);
	REQUIRE(st::symbolAt(code.storage, file, 30) == 0);
}

TEST_CASE("st at prefers a field named on the line over the class around it")
{
	Code code;
	const Id a = code.type(L"A");
	const Id x = add(code.storage, method({L"p", L"A"}, L"x", L"", L""), NODE_FIELD);
	addLocation(code.storage, x, code.file(L"A"), 12);
	code.scope(a, L"A", 1, 20);
	code.storage.buildCaches();

	REQUIRE(st::symbolAt(code.storage, FilePath(L"/r/p/A.java"), 12) == x);
	REQUIRE(st::symbolAt(code.storage, FilePath(L"/r/p/A.java"), 13) == a);
}

TEST_CASE("st at lists both files when a bare name is ambiguous")
{
	Code code;
	addFile(code.storage, L"/r/a/Util.java");
	addFile(code.storage, L"/r/b/Util.java");
	addFile(code.storage, L"/r/b/MyUtil.java");
	code.storage.buildCaches();

	const FilePath cwd(L"/nowhere");
	REQUIRE(st::matchIndexedFiles(st::indexedFiles(code.storage), L"Util.java", cwd).size() == 2);
	const auto one = st::matchIndexedFiles(st::indexedFiles(code.storage), L"a/Util.java", cwd);
	REQUIRE(one.size() == 1);
	REQUIRE(one.front().wstr() == L"/r/a/Util.java");
	REQUIRE(st::matchIndexedFiles(st::indexedFiles(code.storage), L"/r/b/Util.java", cwd).size() == 1);
	REQUIRE(st::matchIndexedFiles(st::indexedFiles(code.storage), L"/r/c/Util.java", cwd).empty());
}

namespace
{
std::vector<std::string> linesOf(const FilePath& file)
{
	if (file.wstr() == L"/o/P.java")
	{
		return {"@Getter", "class P {", "int x;", "}"};
	}
	return {"class P {", "int x;", "public int getX() {", "}", "}"};
}
}

TEST_CASE("st at maps an original lombok line to the generated file")
{
	const auto line = st::toGeneratedLine(FilePath(L"/o/P.java"), FilePath(L"/g/P.java"), 3, linesOf);
	REQUIRE(line.has_value());
	REQUIRE(*line == 2);
}

TEST_CASE("st at refuses an original line without a generated counterpart")
{
	REQUIRE_FALSE(st::toGeneratedLine(FilePath(L"/o/P.java"), FilePath(L"/g/P.java"), 1, linesOf).has_value());
}

TEST_CASE("st at ignores a symbol only referenced on the line")
{
	Code code;
	const Id a = code.type(L"A");
	const Id b = code.type(L"B");
	const Id run = code.member(a, L"A", L"run");
	const Id go = code.member(b, L"B", L"go");
	code.scope(a, L"A", 1, 20);
	code.scope(run, L"A", 5, 9);
	code.link(Edge::EDGE_CALL, run, go, L"A", {7});
	addLocation(code.storage, go, code.file(L"A"), 7);
	code.storage.buildCaches();

	REQUIRE(st::symbolAt(code.storage, FilePath(L"/r/p/A.java"), 7) == run);
}

TEST_CASE("st at ignores a sibling only called on the line")
{
	Code code;
	const Id a = code.type(L"A");
	const Id run = code.member(a, L"A", L"run");
	const Id helper = add(code.storage, method({L"p", L"A"}, L"helper", L"void", L"()"));
	code.storage.addEdge(StorageEdgeData(Edge::typeToInt(Edge::EDGE_MEMBER), a, helper));
	addLocation(code.storage, helper, code.file(L"A"), 12);
	code.scope(a, L"A", 1, 20);
	code.scope(run, L"A", 5, 9);
	code.link(Edge::EDGE_CALL, run, helper, L"A", {7});
	addLocation(code.storage, helper, code.file(L"A"), 7);
	code.storage.buildCaches();

	const FilePath file(L"/r/p/A.java");
	REQUIRE(st::symbolAt(code.storage, file, 7) == run);
	REQUIRE(st::symbolAt(code.storage, file, 12) == helper);
}

TEST_CASE("st symbols-in and at do not take a qualifier for a definition")
{
	Code code;
	const Id a = code.type(L"A");
	const Id util = code.type(L"Util");
	const Id run = code.member(a, L"A", L"run");
	const Id x = add(code.storage, method({L"p", L"A"}, L"x", L"", L""), NODE_FIELD);
	addLocation(code.storage, x, code.file(L"A"), 3);
	addLocation(code.storage, x, code.file(L"A"), 8, LOCATION_QUALIFIER);
	code.scope(a, L"A", 1, 20);
	code.scope(run, L"A", 5, 9);
	addLocation(code.storage, util, code.file(L"A"), 7, LOCATION_QUALIFIER);
	code.storage.buildCaches();

	const FilePath file(L"/r/p/A.java");
	REQUIRE(st::definedInFile(code.storage, file) == std::vector<Id>{a, run, x});
	REQUIRE(st::symbolAt(code.storage, file, 7) == run);
	REQUIRE(st::symbolAt(code.storage, file, 8) == run);
}

TEST_CASE("st at aligns a delombok file once")
{
	st::LombokRoots roots;
	int reads = 0;
	const auto counting = [&](const FilePath& file) {
		reads++;
		return linesOf(file);
	};
	REQUIRE(*st::toGeneratedLine(FilePath(L"/o/P.java"), FilePath(L"/g/P.java"), 3, counting, &roots) == 2);
	REQUIRE(*st::toGeneratedLine(FilePath(L"/o/P.java"), FilePath(L"/g/P.java"), 3, counting, &roots) == 2);
	REQUIRE_FALSE(st::toGeneratedLine(FilePath(L"/o/P.java"), FilePath(L"/g/P.java"), 1, counting, &roots).has_value());
	REQUIRE(reads == 2);
}

TEST_CASE("st outline shows visibility and line ranges")
{
	Code code;
	const Id a = code.type(L"A");
	const Id run = add(code.storage, method({L"p", L"A"}, L"run", L"void", L"()"));
	addLocation(code.storage, run, code.file(L"A"), 5);
	code.scope(run, L"A", 5, 9);
	code.access(run, ACCESS_PUBLIC);
	code.storage.addEdge(StorageEdgeData(Edge::typeToInt(Edge::EDGE_MEMBER), a, run));
	const Id stop = code.member(a, L"A", L"stop");
	code.storage.buildCaches();

	const auto lines = st::outline(code.storage, {run, stop});
	REQUIRE(lines.size() == 2);
	CHECK(lines[0].kind == L"public method");
	CHECK(lines[0].line == 5);
	CHECK(lines[0].endLine == 9);
	CHECK(lines[1].kind == L"method");
	CHECK(lines[1].endLine == 0);
}

TEST_CASE("st outline of a class lists direct members only")
{
	Code code;
	const Id a = code.type(L"A");
	const Id run = code.member(a, L"A", L"run");
	const Id in = code.member(a, L"A", L"In", NODE_CLASS);
	code.member(in, L"A", L"go");
	code.storage.buildCaches();

	const auto members = st::directMembers(code.storage, a);
	CHECK(std::set<Id>(members.begin(), members.end()) == std::set<Id>{run, in});
	CHECK(members.size() == 2);
	CHECK(st::isTypeSymbol(code.storage, a));
	CHECK_FALSE(st::isTypeSymbol(code.storage, run));
}

TEST_CASE("st output prints a line range")
{
	st::OutputOptions options;
	options.cwd = FilePath(L"/r");
	std::ostringstream out;
	st::ResultLine line{L"k", L"method", FilePath(L"/r/A.java"), 5, "void f() {"};
	line.endLine = 9;
	st::printResults({line}, options, out);
	CHECK(out.str() == "k\tmethod\tA.java:5-9\tvoid f() {\n");
}

TEST_CASE("st outline labels default access as package and omits it in interfaces")
{
	Code code;
	const Id a = code.type(L"A");
	const Id run = code.member(a, L"A", L"run");
	code.access(run, ACCESS_DEFAULT);
	const Id i = code.type(L"I", NODE_INTERFACE);
	const Id go = code.member(i, L"I", L"go");
	code.access(go, ACCESS_DEFAULT);
	code.storage.buildCaches();

	CHECK(st::outline(code.storage, {run}).front().kind == L"package method");
	CHECK(st::outline(code.storage, {go}).front().kind == L"method");
}

namespace
{
Id addGeneric(Code& code, const std::wstring& parameters)
{
	NameHierarchy type(NAME_DELIMITER_JAVA);
	type.push(L"p");
	type.push(L"Result<T>");
	const Id result = add(code.storage, type, NODE_CLASS);
	addLocation(code.storage, outcome, code.file(L"Result"), 1);

	NameHierarchy name = type;
	name.push(NameElement(L"failed<T>", L"Result<T>", parameters));
	const Id failed = add(code.storage, name);
	addLocation(code.storage, failed, code.file(L"Result"), 2);
	code.storage.addEdge(StorageEdgeData(Edge::typeToInt(Edge::EDGE_MEMBER), result, failed));
	return failed;
}
}

TEST_CASE("st resolves names without type parameters")
{
	Code code;
	const Id failed = addGeneric(code, L"(java.lang.String)");
	code.storage.buildCaches();

	for (const wchar_t* text: {L"p.Result.failed", L"Result.failed", L"p.Result<T>.failed<T>(java.lang.String)",
			 L"p.Result.failed(java.lang.String)", L"failed"})
	{
		INFO(std::string(text, text + std::wcslen(text)));
		const auto resolved = st::resolveSymbol(code.storage, text, NODE_METHOD);
		REQUIRE(resolved.status == st::Resolution::Found);
		CHECK(resolved.ids == std::vector<Id>{failed});
	}
	for (const wchar_t* text: {L"p.Result", L"Result"})
	{
		const auto resolved = st::resolveSymbol(code.storage, text, NODE_CLASS);
		REQUIRE(resolved.status == st::Resolution::Found);
	}
}

TEST_CASE("st keeps overloads of a generic method ambiguous")
{
	Code code;
	addGeneric(code, L"(java.lang.String)");
	NameHierarchy name(NAME_DELIMITER_JAVA);
	name.push(L"p");
	name.push(L"Result<T>");
	name.push(NameElement(L"failed<T>", L"Result<T>", L"(int)"));
	addLocation(code.storage, add(code.storage, name), code.file(L"Result"), 3);
	code.storage.buildCaches();

	CHECK(st::resolveSymbol(code.storage, L"Result.failed", NODE_METHOD).status == st::Resolution::Ambiguous);
	CHECK(st::resolveSymbol(code.storage, L"Result.failed(int)", NODE_METHOD).status == st::Resolution::Found);
}

TEST_CASE("st hides the unsolved-symbol pseudo node")
{
	Code code;
	const Id a = code.type(L"A");
	const Id run = code.member(a, L"A", L"run");
	NameHierarchy name(NAME_DELIMITER_JAVA);
	name.push(L"unsolved-symbol");
	const Id unsolved = add(code.storage, name, NODE_ENUM_CONSTANT);
	addLocation(code.storage, unsolved, code.file(L"A"), 4);
	code.link(Edge::EDGE_CALL, unsolved, run, L"A", {4});
	code.link(Edge::EDGE_CALL, run, unsolved, L"A", {5});
	const Id other = code.member(code.type(L"B"), L"B", L"other");
	code.link(Edge::EDGE_CALL, other, unsolved, L"B", {3});
	code.storage.buildCaches();

	CHECK(st::definedInFile(code.storage, FilePath(L"/r/p/A.java")) == std::vector<Id>{a, run});
	CHECK(st::findSymbols(code.storage, L"unsolved", ~NodeKindMask(0)).empty());
	CHECK(st::resolveSymbol(code.storage, L"unsolved-symbol", ~NodeKindMask(0)).status == st::Resolution::NotFound);
	CHECK(st::calls(code.storage, {run}, st::Direction::Incoming, 1).empty());
	CHECK(st::calls(code.storage, {run}, st::Direction::Outgoing, 1).empty());
	CHECK(st::references(code.storage, {run}).empty());
	CHECK(st::impact(code.storage, {run}, 0, st::IMPACT_EDGES, st::Group::None).lines.empty());
}

TEST_CASE("st impact reaches callers through interfaces and subtypes")
{
	Code code;
	const Id api = code.type(L"Api", NODE_INTERFACE);
	const Id apiRun = code.member(api, L"Api", L"run");
	const Id impl = code.type(L"Impl");
	const Id implRun = code.member(impl, L"Impl", L"run");
	code.link(Edge::EDGE_OVERRIDE, implRun, apiRun, L"Impl", {3});
	const Id other = code.type(L"Other");
	const Id otherRun = code.member(other, L"Other", L"run");
	code.link(Edge::EDGE_OVERRIDE, otherRun, apiRun, L"Other", {3});
	const Id use = code.member(code.type(L"Client"), L"Client", L"use");
	code.link(Edge::EDGE_CALL, use, apiRun, L"Client", {4});
	const Id sub = code.type(L"Sub");
	code.link(Edge::EDGE_INHERITANCE, sub, impl, L"Sub", {2});
	const Id field = code.member(code.type(L"Holder"), L"Holder", L"field", NODE_FIELD);
	code.link(Edge::EDGE_TYPE_USAGE, field, impl, L"Holder", {5});
	code.storage.buildCaches();

	const auto ofRun = st::impact(code.storage, {implRun}, 0, st::IMPACT_EDGES, st::Group::None);
	CHECK(includes(keys(ofRun.lines), {L"p.Api.run()", L"p.Client.use()"}));
	CHECK_FALSE(keys(ofRun.lines).count(L"p.Impl.run()"));
	// A sibling implementation is not affected by a change to this one.
	CHECK_FALSE(keys(ofRun.lines).count(L"p.Other.run()"));
	REQUIRE(lineFor(ofRun.lines, L"p.Client.use()"));
	CHECK(lineFor(ofRun.lines, L"p.Client.use()")->line == 4);
	CHECK(lineFor(ofRun.lines, L"p.Client.use()")->kind == L"method");

	const auto ofImpl = impactKeys(code, {impl});
	CHECK(includes(ofImpl, {L"p.Sub", L"p.Holder.field", L"p.Client.use()"}));
	CHECK_FALSE(ofImpl.count(L"p.Impl"));
	CHECK_FALSE(ofImpl.count(L"p.Impl.run()"));

	CHECK(includes(impactKeys(code, {apiRun}), {L"p.Impl.run()", L"p.Other.run()"}));
}

TEST_CASE("st impact honours depth with free override hops")
{
	Code code;
	const Id a = code.member(code.type(L"A"), L"A", L"a");
	const Id b = code.member(code.type(L"B"), L"B", L"b");
	const Id c = code.member(code.type(L"C"), L"C", L"c");
	code.link(Edge::EDGE_CALL, a, b, L"A", {2});
	code.link(Edge::EDGE_CALL, b, c, L"B", {2});
	const Id i = code.member(code.type(L"I", NODE_INTERFACE), L"I", L"i");
	const Id d = code.member(code.type(L"D"), L"D", L"d");
	const Id x = code.member(code.type(L"X"), L"X", L"x");
	code.link(Edge::EDGE_OVERRIDE, d, i, L"D", {2});
	code.link(Edge::EDGE_CALL, x, i, L"X", {2});
	code.link(Edge::EDGE_CALL, a, x, L"A", {3});
	code.storage.buildCaches();

	CHECK(impactKeys(code, {c}, 1) == std::set<std::wstring>{L"p.B.b()"});
	CHECK(impactKeys(code, {c}, 0) == std::set<std::wstring>{L"p.A.a()", L"p.B.b()"});
	CHECK(impactKeys(code, {d}, 1) == std::set<std::wstring>{L"p.I.i()", L"p.X.x()"});
	CHECK(impactKeys(code, {d}, 2) == std::set<std::wstring>{L"p.I.i()", L"p.X.x()", L"p.A.a()"});
}

TEST_CASE("st impact terminates on cycles and leaves out its input")
{
	Code code;
	const Id a = code.member(code.type(L"A"), L"A", L"a");
	const Id b = code.member(code.type(L"B"), L"B", L"b");
	code.link(Edge::EDGE_CALL, a, b, L"A", {2});
	code.link(Edge::EDGE_CALL, b, a, L"B", {2});
	code.storage.buildCaches();

	CHECK(impactKeys(code, {a}) == std::set<std::wstring>{L"p.B.b()"});
}

TEST_CASE("st impact groups by package")
{
	Code code;
	NameHierarchy package(NAME_DELIMITER_JAVA);
	package.push(L"p");
	add(code.storage, package, NODE_PACKAGE);
	const Id a = code.member(code.type(L"A"), L"A", L"a");
	const Id b = code.member(code.type(L"B"), L"B", L"b");
	const Id c = code.member(code.type(L"C"), L"C", L"c");
	code.link(Edge::EDGE_CALL, a, c, L"A", {2});
	code.link(Edge::EDGE_CALL, b, c, L"B", {2});
	// q has no package node: its package is the prefix before the top-level type.
	NameHierarchy zName(NAME_DELIMITER_JAVA);
	zName.push(L"q");
	zName.push(L"Z");
	const Id zType = add(code.storage, zName, NODE_CLASS);
	addLocation(code.storage, zType, code.file(L"Z"), 1);
	const Id z = add(code.storage, method({L"q", L"Z"}, L"z", L"void", L"()"));
	addLocation(code.storage, z, code.file(L"Z"), 3);
	code.link(Edge::EDGE_CALL, z, c, L"Z", {4});
	code.storage.buildCaches();

	const auto byPackage = st::impact(code.storage, {c}, 0, st::IMPACT_EDGES, st::Group::Package);
	CHECK(keys(byPackage.lines) == std::set<std::wstring>{L"p", L"q"});
	for (const auto& line: byPackage.lines)
	{
		CHECK(line.kind.rfind(L"package, ", 0) == 0);
	}
	REQUIRE(lineFor(byPackage.lines, L"p"));
	CHECK(lineFor(byPackage.lines, L"p")->kind == L"package, 2 symbols");
	CHECK(lineFor(byPackage.lines, L"p")->file.wstr() == L"/r/p/A.java");
	CHECK(byPackage.symbols == 3);
	CHECK(byPackage.files == 3);
	CHECK(byPackage.packages == 2);

	const auto byFile = st::impact(code.storage, {c}, 0, st::IMPACT_EDGES, st::Group::File);
	CHECK(keys(byFile.lines) == std::set<std::wstring>{L"/r/p/A.java", L"/r/p/B.java", L"/r/p/Z.java"});
	REQUIRE(lineFor(byFile.lines, L"/r/p/Z.java"));
	CHECK(lineFor(byFile.lines, L"/r/p/Z.java")->kind == L"file, 1 symbols");
	CHECK(lineFor(byFile.lines, L"/r/p/Z.java")->line == 3);
}

TEST_CASE("st edges parse names and reject unknown ones")
{
	CHECK(st::parseEdges(L"call,override") == (Edge::EDGE_CALL | Edge::EDGE_OVERRIDE));
	CHECK(st::parseEdges(L" type-usage , annotation,import") ==
		(Edge::EDGE_TYPE_USAGE | Edge::EDGE_ANNOTATION_USAGE | Edge::EDGE_IMPORT));
	std::string message;
	try
	{
		st::parseEdges(L"call,calls");
	}
	catch (const std::invalid_argument& error)
	{
		message = error.what();
	}
	CHECK(message.find("calls") != std::string::npos);
	CHECK_THROWS_AS(st::parseEdges(L"call,,usage"), std::invalid_argument);
	CHECK_THROWS_AS(st::parseEdges(L""), std::invalid_argument);
}

TEST_CASE("st path over inheritance only when asked")
{
	Code code;
	const Id go = code.member(code.type(L"Caller"), L"Caller", L"go");
	const Id base = code.type(L"Base");
	const Id derived = code.type(L"Derived");
	code.link(Edge::EDGE_USAGE, go, base, L"Caller", {2});
	code.link(Edge::EDGE_INHERITANCE, derived, base, L"Derived", {1});
	code.storage.buildCaches();

	CHECK(st::paths(code.storage, {go}, {derived}, 6, 10, st::PATH_EDGES).empty());
	const auto chains = st::paths(code.storage, {go}, {derived}, 6, 10, st::PATH_EDGES | Edge::EDGE_INHERITANCE);
	REQUIRE(chains.size() == 1);
	REQUIRE(chains[0].size() == 2);
	CHECK(chains[0][1].type == Edge::EDGE_INHERITANCE);
	CHECK(chains[0][1].to == derived);
}
