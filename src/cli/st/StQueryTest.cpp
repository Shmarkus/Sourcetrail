#include <algorithm>
#include <functional>
#include <set>
#include <sstream>
#include <stdexcept>

#include "StArchitecture.h"
#include "StFiles.h"
#include "StFilter.h"
#include "StGaps.h"
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

// A symbol the index knows by name only, without a definition.
Id undefined(PersistentStorage& storage, const NameHierarchy& name, NodeKind kind)
{
	return storage.addNode(StorageNodeData(nodeKindToInt(kind), NameHierarchy::serialize(name))).first;
}

NameHierarchy typeName(const std::vector<std::wstring>& path)
{
	NameHierarchy name(NAME_DELIMITER_JAVA);
	for (const std::wstring& element: path)
	{
		name.push(element);
	}
	return name;
}

// The Java indexer's placeholder for whatever it could not resolve, such as a call made inside a record.
Id unsolvedSymbol(Code& code)
{
	const Id id = add(code.storage, typeName({L"unsolved-symbol"}), NODE_ENUM_CONSTANT);
	addLocation(code.storage, id, code.file(L"Rec"), 1);
	return id;
}

// A file the indexer reported errors in.
Id brokenFile(Code& code, const std::wstring& type)
{
	const std::wstring path = L"/r/p/" + type + L".java";
	const Id id = code.storage.addNode(StorageNodeData(nodeKindToInt(NODE_FILE),
		NameHierarchy::serialize(NameHierarchy(path, NAME_DELIMITER_FILE)))).first;
	code.storage.addFile(StorageFile(id, path, L"java", "2026-10-30 12:00:00", true, false));
	code.files[type] = id;
	return id;
}

st::Gaps gapsOfCalls(Code& code, Id id, st::Direction direction, size_t depth)
{
	st::Walked walked;
	const auto lines = st::calls(code.storage, {id}, direction, depth, &walked);
	return st::findGaps(code.storage, walked, lines);
}

// An error the indexer reported at a line and column of the type's file.
void indexError(Code& code, const std::wstring& type, size_t line, size_t column, const std::wstring& message)
{
	const Id error = code.storage.addError(StorageErrorData(message, L"", false, true));
	const Id location = code.storage.addSourceLocation(
		StorageSourceLocationData(code.file(type), line, column, line, column + 4, locationTypeToInt(LOCATION_ERROR)));
	code.storage.addOccurrence(StorageOccurrence(error, location));
}

std::string printed(const st::Gaps& gaps)
{
	std::ostringstream out;
	st::printGaps(gaps, out);
	return out.str();
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
	// An absolute path from another machine (a CI or container stack trace) falls back to suffix matching.
	REQUIRE(st::matchIndexedFiles(st::indexedFiles(code.storage), L"/r/c/Util.java", cwd).size() == 2);
	REQUIRE(st::matchIndexedFiles(st::indexedFiles(code.storage), L"/r/c/Nope.java", cwd).empty());
}

TEST_CASE("st takes an absolute path from another machine by its longest indexed suffix")
{
	Code code;
	addFile(code.storage, L"/r/src/main/java/a/Util.java");
	addFile(code.storage, L"/r/src/test/java/a/Util.java");
	addFile(code.storage, L"/r/src/main/java/b/Util.java");
	code.storage.buildCaches();

	const FilePath cwd(L"/nowhere");
	const auto main = st::matchIndexedFiles(st::indexedFiles(code.storage), L"/workspace/src/main/java/a/Util.java", cwd);
	REQUIRE(main.size() == 1);
	CHECK(main.front().wstr() == L"/r/src/main/java/a/Util.java");
	CHECK(st::matchIndexedFiles(st::indexedFiles(code.storage), L"/workspace/a/Util.java", cwd).size() == 2);
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

TEST_CASE("st at answers an unmapped original line only when both mapped neighbours share a symbol")
{
	Code code;
	const Id a = code.type(L"A");
	const Id run = code.member(a, L"A", L"run");
	const Id other = code.member(a, L"A", L"other");
	code.scope(a, L"A", 1, 15);
	code.scope(run, L"A", 3, 7);
	code.scope(other, L"A", 8, 10);
	code.storage.buildCaches();
	const std::vector<std::string> generated = {"class A {", "int n;", "void run() {", "a();", "final String x = f();", "c();",
		"}", "void other() {", "d();", "}", "@lombok.Generated", "int getN() {", "return n;", "}", "}"};
	const std::vector<std::string> original = {"@Getter", "class A {", "int n;", "void run() {", "a();", "val x = f();",
		"c();", "}", "@Synchronized", "void other() {", "d();", "}", "}"};
	const auto read = [&](const FilePath& file) { return file.wstr() == L"/o/A.java" ? original : generated; };
	const FilePath indexed(L"/r/p/A.java");
	const FilePath source(L"/o/A.java");

	REQUIRE(st::symbolAtOriginal(code.storage, indexed, source, 5, read) == run);
	REQUIRE(st::symbolAtOriginal(code.storage, indexed, source, 6, read) == run);
	REQUIRE_FALSE(st::symbolAtOriginal(code.storage, indexed, source, 9, read).has_value());
	REQUIRE_FALSE(st::symbolAtOriginal(code.storage, indexed, source, 1, read).has_value());
}

TEST_CASE("st at refuses an unmapped line when a nested member lies between its mapped neighbours")
{
	Code code;
	const Id a = code.type(L"A");
	const Id run = code.member(a, L"A", L"run");
	code.scope(a, L"A", 1, 10);
	code.scope(run, L"A", 2, 8);
	code.storage.buildCaches();
	const std::vector<std::string> generated = {"class A {", "void run() {", "try {", "Thread.sleep(1);",
		"} catch (final java.lang.Throwable $ex) {", "throw lombok.Lombok.sneakyThrow($ex);", "}", "}", "int x;", "}"};
	const std::vector<std::string> original = {"class A {", "@SneakyThrows void run() { Thread.sleep(1); }", "int x;", "}"};
	const auto read = [&](const FilePath& file) { return file.wstr() == L"/o/A.java" ? original : generated; };

	REQUIRE(st::symbolAtOriginal(code.storage, FilePath(L"/r/p/A.java"), FilePath(L"/o/A.java"), 1, read) == a);
	REQUIRE(st::symbolAtOriginal(code.storage, FilePath(L"/r/p/A.java"), FilePath(L"/o/A.java"), 3, read) == a);
	REQUIRE_FALSE(st::symbolAtOriginal(code.storage, FilePath(L"/r/p/A.java"), FilePath(L"/o/A.java"), 2, read).has_value());
}

TEST_CASE("st at refuses an unmapped line when an anonymous class lies between its mapped neighbours")
{
	Code code;
	const Id a = code.type(L"A");
	const Id run = code.member(a, L"A", L"run");
	const Id anonymous = code.member(a, L"A", L"anonymous");
	code.scope(a, L"A", 1, 11);
	code.scope(run, L"A", 2, 10);
	code.scope(anonymous, L"A", 4, 8);
	code.storage.buildCaches();
	const std::vector<std::string> generated = {"class A {", "void run() {", "a();", "new Runnable() {", "public void run() {",
		"try { x(); } catch (final Throwable e) { throw e; }", "}", "}.run();", "b();", "}", "}"};
	const std::vector<std::string> original = {"class A {", "void run() {", "a();",
		"new Runnable() { @SneakyThrows public void run() { x(); } }.run();", "b();", "}", "}"};
	const auto read = [&](const FilePath& file) { return file.wstr() == L"/o/A.java" ? original : generated; };

	REQUIRE(st::symbolAtOriginal(code.storage, FilePath(L"/r/p/A.java"), FilePath(L"/o/A.java"), 3, read) == run);
	REQUIRE(st::symbolAtOriginal(code.storage, FilePath(L"/r/p/A.java"), FilePath(L"/o/A.java"), 5, read) == run);
	REQUIRE_FALSE(st::symbolAtOriginal(code.storage, FilePath(L"/r/p/A.java"), FilePath(L"/o/A.java"), 4, read).has_value());
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

TEST_CASE("st resolves a bare class name with constructors to the class")
{
	Code code;
	const Id foo = code.type(L"Foo");
	code.member(foo, L"Foo", L"Foo", NODE_METHOD, L"(int)");
	code.member(foo, L"Foo", L"Foo", NODE_METHOD, L"(long)");
	code.storage.buildCaches();

	for (const wchar_t* text: {L"Foo", L"p.Foo"})
	{
		const auto resolved = st::resolveSymbol(code.storage, text, ~NodeKindMask(0));
		REQUIRE(resolved.status == st::Resolution::Found);
		CHECK(resolved.ids == std::vector<Id>{foo});
	}
}

TEST_CASE("st keeps a type and a same-named method of another type ambiguous")
{
	Code code;
	const Id foo = code.type(L"Foo");
	code.member(foo, L"Foo", L"Foo", NODE_METHOD, L"(int)");
	const Id bar = code.type(L"Bar");
	code.member(bar, L"Bar", L"Foo", NODE_METHOD, L"(int)");
	code.storage.buildCaches();

	CHECK(st::resolveSymbol(code.storage, L"Foo", ~NodeKindMask(0)).status == st::Resolution::Ambiguous);
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
	CHECK(lineFor(ofRun.lines, L"p.Client.use()")->kind == L"method, depth 1");

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

TEST_CASE("st deps rolls up to top-level classes and counts links")
{
	Code code;
	const Id ctl = code.type(L"web", L"Ctl");
	const Id get = code.member(ctl, L"Ctl", L"get");
	const Id svc = code.type(L"domain", L"Svc");
	const Id a = code.member(svc, L"Svc", L"a");
	const Id in = code.member(svc, L"Svc", L"In", NODE_CLASS);
	code.link(Edge::EDGE_CALL, get, a, L"Ctl", {7, 3});
	code.link(Edge::EDGE_TYPE_USAGE, get, in, L"Ctl", {9});
	code.storage.buildCaches();

	const auto lines = st::deps(code.storage, {get}, st::Direction::Outgoing);
	REQUIRE(lines.size() == 1);
	CHECK(lines[0].key == L"domain.Svc");
	CHECK(lines[0].kind == L"class, 3 links");
	CHECK(lines[0].line == 3);
}

TEST_CASE("st deps of a package rolls up to packages")
{
	Code code;
	const Id ctl = code.type(L"web", L"Ctl");
	const Id get = code.member(ctl, L"Ctl", L"get");
	const Id svc = code.type(L"domain", L"Svc");
	const Id a = code.member(svc, L"Svc", L"a");
	code.link(Edge::EDGE_CALL, get, a, L"Ctl", {4});
	code.storage.buildCaches();

	const auto lines = st::deps(code.storage, {code.package(L"web")}, st::Direction::Outgoing);
	REQUIRE(lines.size() == 1);
	CHECK(lines[0].key == L"domain");
	CHECK(lines[0].kind == L"package, 1 links");
}

TEST_CASE("st deps ignores links inside the input")
{
	Code code;
	const Id svc = code.type(L"domain", L"Svc");
	const Id a = code.member(svc, L"Svc", L"a");
	const Id b = code.member(svc, L"Svc", L"b");
	code.link(Edge::EDGE_CALL, a, b, L"Svc", {5});
	code.storage.buildCaches();

	CHECK(st::deps(code.storage, {svc}, st::Direction::Outgoing).empty());
	CHECK(st::deps(code.storage, {a}, st::Direction::Outgoing).empty());
}

TEST_CASE("st deps incoming lists the dependants")
{
	Code code;
	const Id ctl = code.type(L"web", L"Ctl");
	const Id get = code.member(ctl, L"Ctl", L"get");
	const Id svc = code.type(L"domain", L"Svc");
	const Id a = code.member(svc, L"Svc", L"a");
	code.link(Edge::EDGE_CALL, get, a, L"Ctl", {4});
	code.storage.buildCaches();

	const auto lines = st::deps(code.storage, {svc}, st::Direction::Incoming);
	REQUIRE(lines.size() == 1);
	CHECK(lines[0].key == L"web.Ctl");
	CHECK(st::deps(code.storage, {code.package(L"domain")}, st::Direction::Incoming).front().key == L"web");
}

TEST_CASE("st deps never lists builtin types")
{
	Code code;
	const Id svc = code.type(L"domain", L"Svc");
	const Id a = code.member(svc, L"Svc", L"a");
	NameHierarchy voidName(NAME_DELIMITER_JAVA);
	voidName.push(L"void");
	const Id voidType = add(code.storage, voidName, NODE_BUILTIN_TYPE);
	code.link(Edge::EDGE_TYPE_USAGE, a, voidType, L"Svc", {2});
	code.storage.buildCaches();

	CHECK(st::deps(code.storage, {a}, st::Direction::Outgoing).empty());
}

TEST_CASE("st deps of a package covers its sub-packages")
{
	Code code;
	const Id deep = code.type(L"d.x", L"Deep");
	const Id run = code.member(deep, L"Deep", L"run");
	const Id top = code.type(L"d", L"Top");
	const Id topGo = code.member(top, L"Top", L"go");
	const Id web = code.type(L"w", L"Web");
	const Id serve = code.member(web, L"Web", L"serve");
	code.link(Edge::EDGE_CALL, run, serve, L"Deep", {3});
	code.link(Edge::EDGE_CALL, run, topGo, L"Deep", {4});
	code.link(Edge::EDGE_CALL, serve, run, L"Web", {5});
	code.storage.buildCaches();

	const auto out = st::deps(code.storage, {code.package(L"d")}, st::Direction::Outgoing);
	REQUIRE(out.size() == 1);
	CHECK(out[0].key == L"w");
	const auto in = st::deps(code.storage, {code.package(L"d")}, st::Direction::Incoming);
	REQUIRE(in.size() == 1);
	CHECK(in[0].key == L"w");
}

TEST_CASE("st callers keeps a field whose initializer calls the symbol")
{
	Code code;
	const Id rules = code.type(L"p", L"F");
	const Id required = code.member(rules, L"F", L"required");
	const Id schema = code.type(L"p", L"ShopSchema");
	const Id field = code.member(schema, L"ShopSchema", L"SCHEMA", NODE_FIELD);
	const Id user = code.member(code.type(L"p", L"User"), L"User", L"use");
	code.link(Edge::EDGE_CALL, field, required, L"ShopSchema", {4});
	code.link(Edge::EDGE_CALL, user, field, L"User", {5});
	code.storage.buildCaches();

	const auto lines = st::calls(code.storage, {required}, st::Direction::Incoming, 0);
	REQUIRE(lines.size() == 1);
	CHECK(lines[0].key == L"p.ShopSchema.SCHEMA");
	CHECK(lines[0].line == 4);
}

TEST_CASE("st deps covers a sub-package without a member link")
{
	Code code;
	const Id d = code.package(L"d");
	NameHierarchy lone(NAME_DELIMITER_JAVA);
	lone.push(L"d");
	lone.push(L"x");
	code.packages[L"d.x"] = add(code.storage, lone, NODE_PACKAGE);	// no member edge from d
	const Id deep = code.type(L"d.x", L"Deep");
	const Id run = code.member(deep, L"Deep", L"run");
	const Id web = code.type(L"w", L"Web");
	const Id serve = code.member(web, L"Web", L"serve");
	code.link(Edge::EDGE_CALL, run, serve, L"Deep", {3});
	code.storage.buildCaches();

	const auto out = st::deps(code.storage, {d}, st::Direction::Outgoing);
	REQUIRE(out.size() == 1);
	CHECK(out[0].key == L"w");
}

TEST_CASE("st resolves parameters by their simple names")
{
	Code code;
	const Id service = code.type(L"p", L"PaymentService");
	const Id full = code.member(service, L"PaymentService", L"sendPayments",
		NODE_METHOD, L"(java.util.UUID, p.PaymentPlan, java.util.Collection<p.Payment>)");
	code.member(service, L"PaymentService", L"sendPayments", NODE_METHOD, L"(java.util.UUID, java.util.List<p.X>)");
	code.storage.buildCaches();

	auto resolved = st::resolveSymbol(code.storage,
		L"p.PaymentService.sendPayments(UUID, PaymentPlan, Collection<Payment>)", NODE_METHOD);
	REQUIRE(resolved.status == st::Resolution::Found);
	CHECK(resolved.ids == std::vector<Id>{full});
	resolved = st::resolveSymbol(code.storage, L"sendPayments(UUID, PaymentPlan, Collection)", NODE_METHOD);
	CHECK(resolved.status == st::Resolution::Found);
	CHECK(st::resolveSymbol(code.storage, L"sendPayments(ID, PaymentPlan, Collection)", NODE_METHOD).status ==
		st::Resolution::NotFound);
	CHECK(st::resolveSymbol(code.storage, L"sendPayments(UUID)", NODE_METHOD).status == st::Resolution::NotFound);
}

TEST_CASE("st impact rows carry the walk depth")
{
	Code code;
	const Id a = code.member(code.type(L"A"), L"A", L"a");
	const Id b = code.member(code.type(L"B"), L"B", L"b");
	const Id c = code.member(code.type(L"C"), L"C", L"c");
	code.link(Edge::EDGE_CALL, a, b, L"A", {2});
	code.link(Edge::EDGE_CALL, b, c, L"B", {2});
	code.storage.buildCaches();

	const auto lines = st::impact(code.storage, {c}, 0, st::IMPACT_EDGES, st::Group::None).lines;
	REQUIRE(lines.size() == 2);
	for (const auto& line: lines)
	{
		CHECK(line.kind == (line.key == L"p.B.b()" ? L"method, depth 1" : L"method, depth 2"));
	}
}

TEST_CASE("st find hides type parameters unless the kind asks for them")
{
	Code code;
	const Id failed = addGeneric(code, L"(java.lang.String)");
	NameHierarchy name(NAME_DELIMITER_JAVA);
	name.push(L"p");
	name.push(L"Result<T>");
	name.push(NameElement(L"failed<T>", L"Result<T>", L"(java.lang.String)"));
	name.push(L"O");
	const Id parameter = add(code.storage, name, NODE_TYPE_PARAMETER);
	addLocation(code.storage, parameter, code.file(L"Result"), 2);
	code.storage.buildCaches();

	const auto hidden = st::findSymbols(code.storage, L"Result.failed", ~0);
	CHECK(std::find(hidden.begin(), hidden.end(), parameter) == hidden.end());
	CHECK(std::find(hidden.begin(), hidden.end(), failed) != hidden.end());
	CHECK(st::findSymbols(code.storage, L"Result.failed", NODE_TYPE_PARAMETER) == std::vector<Id>{parameter});
}

TEST_CASE("st impact keeps the lowest depths under the limit")
{
	Code code;
	const Id a = code.member(code.type(L"A"), L"A", L"a");
	const Id b = code.member(code.type(L"B"), L"B", L"b");
	const Id c = code.member(code.type(L"C"), L"C", L"c");
	code.link(Edge::EDGE_CALL, a, b, L"A", {2});
	code.link(Edge::EDGE_CALL, b, c, L"B", {2});
	code.storage.buildCaches();

	st::OutputOptions options;
	options.limit = 1;
	options.showSource = false;
	std::ostringstream out;
	st::printResults(st::impact(code.storage, {c}, 0, st::IMPACT_EDGES, st::Group::None).lines, options, out);
	CHECK(out.str() == "p.B.b()\tmethod, depth 1\t/r/p/B.java:2\n+1 more (--limit)\n");
}

TEST_CASE("st lists the overloads of a name whose parameter list matches none")
{
	Code code;
	const Id service = code.type(L"p", L"PaymentService");
	const Id one = code.member(service, L"PaymentService", L"sendPayments", NODE_METHOD, L"(java.util.UUID)");
	const Id two = code.member(service, L"PaymentService", L"sendPayments", NODE_METHOD, L"(java.util.UUID, int)");
	code.storage.buildCaches();

	const std::vector<Id> overloads = st::overloadsOf(code.storage, L"PaymentService.sendPayments(String)", NODE_METHOD);
	CHECK(std::set<Id>(overloads.begin(), overloads.end()) == std::set<Id>{one, two});
	CHECK(st::overloadsOf(code.storage, L"PaymentService.sendPayments", NODE_METHOD).empty());
	CHECK(st::overloadsOf(code.storage, L"PaymentService.missing(String)", NODE_METHOD).empty());
}

TEST_CASE("st result lines carry the column of their location")
{
	Code code;
	const Id a = code.member(code.type(L"A"), L"A", L"a");
	const Id b = code.member(code.type(L"B"), L"B", L"b");
	const Id call = code.storage.addEdge(StorageEdgeData(Edge::typeToInt(Edge::EDGE_CALL), a, b));
	code.storage.addOccurrence(StorageOccurrence(call, code.storage.addSourceLocation(
		StorageSourceLocationData(code.file(L"A"), 7, 13, 7, 15, locationTypeToInt(LOCATION_TOKEN)))));
	code.storage.buildCaches();

	const auto callers = st::calls(code.storage, {b}, st::Direction::Incoming, 1);
	REQUIRE(callers.size() == 1);
	CHECK(callers[0].column == 13);
	const auto refs = st::references(code.storage, {b});
	REQUIRE(refs.size() == 1);
	CHECK(refs[0].column == 13);
	const auto impacted = st::impact(code.storage, {b}, 0, st::IMPACT_EDGES, st::Group::None).lines;
	REQUIRE(impacted.size() == 1);
	CHECK(impacted[0].column == 13);
	const auto chains = st::paths(code.storage, {a}, {b}, 2, 10, Edge::EDGE_CALL);
	REQUIRE(chains.size() == 1);
	CHECK(chains[0][0].column == 13);
	const auto definition = st::definitionLines(code.storage, b, L"p.B.b()");
	REQUIRE(definition.size() == 1);
	CHECK(definition[0].column == 1);
}

TEST_CASE("st impact of a package covers what it and its sub-packages own")
{
	Code code;
	const Id d = code.package(L"d");
	NameHierarchy lone(NAME_DELIMITER_JAVA);
	lone.push(L"d");
	lone.push(L"x");
	code.packages[L"d.x"] = add(code.storage, lone, NODE_PACKAGE);	// no member edge from d
	const Id deepRun = code.member(code.type(L"d.x", L"Deep"), L"Deep", L"run");
	const Id topGo = code.member(code.type(L"d", L"Top"), L"Top", L"go");
	const Id serve = code.member(code.type(L"w", L"Web"), L"Web", L"serve");
	const Id main = code.member(code.type(L"w", L"Main"), L"Main", L"main");
	const Id other = code.member(code.type(L"dx", L"Other"), L"Other", L"other");
	code.link(Edge::EDGE_CALL, serve, deepRun, L"Web", {3});
	code.link(Edge::EDGE_CALL, main, serve, L"Main", {4});
	code.link(Edge::EDGE_CALL, deepRun, topGo, L"Deep", {5});
	code.link(Edge::EDGE_CALL, other, topGo, L"Other", {6});
	code.storage.buildCaches();

	const auto result = st::impact(code.storage, {d}, 0, st::IMPACT_EDGES, st::Group::None);
	CHECK(keys(result.lines) == std::set<std::wstring>{L"dx.Other.other()", L"w.Web.serve()", L"w.Main.main()"});
	REQUIRE(lineFor(result.lines, L"w.Main.main()"));
	CHECK(lineFor(result.lines, L"w.Main.main()")->kind == L"method, depth 2");
}

TEST_CASE("st callers of a method also called from an unresolved symbol counts the unresolved links")
{
	Code code;
	const Id a = code.type(L"p", L"A");
	const Id run = code.member(a, L"A", L"run");
	const Id use = code.member(code.type(L"p", L"User"), L"User", L"use");
	const Id top = code.member(code.type(L"p", L"Top"), L"Top", L"main");
	code.link(Edge::EDGE_CALL, use, run, L"User", {3});
	code.link(Edge::EDGE_CALL, top, use, L"Top", {3});
	// Calls made inside record bodies: one link with two sites, and one a level up.
	const Id unsolved = unsolvedSymbol(code);
	code.link(Edge::EDGE_CALL, unsolved, run, L"Rec", {4, 6});
	code.link(Edge::EDGE_CALL, unsolved, use, L"Rec", {8});
	// What run calls doesn't make its callers incomplete.
	code.link(Edge::EDGE_CALL, run, unsolved, L"A", {5});
	code.storage.buildCaches();

	st::Walked walked;
	const auto lines = st::calls(code.storage, {run}, st::Direction::Incoming, 1, &walked);
	CHECK(keys(lines) == std::set<std::wstring>{L"p.User.use()"});
	const st::Gaps gaps = st::findGaps(code.storage, walked, lines);
	CHECK(gaps.unresolvedLinks == 1);
	CHECK(gaps.indexErrors == 0);
	CHECK(gapsOfCalls(code, run, st::Direction::Incoming, 2).unresolvedLinks == 2);
	CHECK(gapsOfCalls(code, run, st::Direction::Incoming, 0).unresolvedLinks == 2);
	CHECK(gapsOfCalls(code, top, st::Direction::Incoming, 0).unresolvedLinks == 0);
}

TEST_CASE("st callees count calls into records and generated classes but not into libraries or implicit members")
{
	Code code;
	code.type(L"p", L"A");
	const Id use = code.member(code.type(L"p", L"User"), L"User", L"use");
	// A record's type and accessor have no definition; neither has a class only the build generates.
	const Id record = undefined(code.storage, typeName({L"p", L"Rec"}), NODE_SYMBOL);
	const Id accessor = undefined(code.storage, method({L"p", L"Rec"}, L"value", L"int", L"()"), NODE_METHOD);
	const Id generated = undefined(code.storage, method({L"p", L"AMapperImpl"}, L"AMapperImpl", L"", L"()"), NODE_METHOD);
	const Id library = undefined(code.storage, method({L"java", L"util", L"List"}, L"size", L"int", L"()"), NODE_METHOD);
	const Id constructor = undefined(code.storage, method({L"p", L"A"}, L"A", L"", L"()"), NODE_METHOD);
	for (const Id callee: {accessor, generated, library, constructor})
	{
		code.link(Edge::EDGE_CALL, use, callee, L"User", {4});
	}
	code.link(Edge::EDGE_TYPE_USAGE, use, record, L"User", {5});
	code.link(Edge::EDGE_CALL, use, unsolvedSymbol(code), L"User", {6});
	code.storage.buildCaches();

	CHECK(gapsOfCalls(code, use, st::Direction::Outgoing, 1).unresolvedLinks == 3);
	// The placeholder, the record, its accessor and the generated class's constructor.
	CHECK(st::countUnresolved(code.storage) == 4);
}

TEST_CASE("st attributes each index error to the innermost named symbol around it")
{
	Code code;
	brokenFile(code, L"Broken");
	const Id broken = code.type(L"p", L"Broken");
	code.scope(broken, L"Broken", 3, 30);
	const Id call = code.member(broken, L"Broken", L"call");
	code.scope(call, L"Broken", 10, 15);
	const Id other = code.member(broken, L"Broken", L"other");
	code.scope(other, L"Broken", 16, 20);
	// An anonymous class inside call, with a method of its own.
	NameHierarchy anonymousName = method({L"p", L"Broken"}, L"call", L"void", L"()");
	anonymousName.push(NameElement(L"anonymous class (Broken.java<13:5>)"));
	const Id anonymous = add(code.storage, anonymousName, NODE_CLASS);
	code.scope(anonymous, L"Broken", 13, 14);
	NameHierarchy anonymousRunName = anonymousName;
	anonymousRunName.push(NameElement(L"run", L"void", L"()"));
	const Id anonymousRun = add(code.storage, anonymousRunName, NODE_METHOD);
	code.scope(anonymousRun, L"Broken", 13, 14);
	// Fields have no scope, only a name on their declaration line.
	const Id mapper = add(code.storage, typeName({L"p", L"Broken", L"mapper"}), NODE_FIELD);
	addLocation(code.storage, mapper, code.file(L"Broken"), 5);
	const Id name = add(code.storage, typeName({L"p", L"Broken", L"name"}), NODE_FIELD);
	addLocation(code.storage, name, code.file(L"Broken"), 6);
	const Id inner = add(code.storage, typeName({L"p", L"Broken", L"Inner"}), NODE_CLASS);
	code.scope(inner, L"Broken", 22, 28);

	indexError(code, L"Broken", 1, 8, L"The import p.Gone cannot be resolved");
	indexError(code, L"Broken", 5, 3, L"MapperImpl cannot be resolved to a type");
	indexError(code, L"Broken", 12, 9, L"Gone_ cannot be resolved to a variable");
	indexError(code, L"Broken", 13, 20, L"Gone cannot be resolved");
	code.storage.buildCaches();

	const auto errorsOf = [&](std::set<Id> nodes, const std::vector<st::ResultLine>& lines = {}) {
		st::Walked walked;
		walked.nodes = std::move(nodes);
		return st::findGaps(code.storage, walked, lines).indexErrors;
	};
	// Inside a method, also through an anonymous class within it.
	CHECK(errorsOf({call}) == 2);
	CHECK(errorsOf({other}) == 0);
	CHECK(errorsOf({anonymous, anonymousRun}) == 0);
	// At class level, on a field's line: the field's, not the class's.
	CHECK(errorsOf({mapper}) == 1);
	CHECK(errorsOf({name}) == 0);
	// Outside every scope (an import): the file's top-level types.
	CHECK(errorsOf({broken}) == 1);
	CHECK(errorsOf({inner}) == 0);
	// The symbol enclosing a result row.
	const FilePath file(L"/r/p/Broken.java");
	CHECK(errorsOf({}, {{L"p.X.use()", L"call", file, 11, ""}}) == 2);
	CHECK(errorsOf({}, {{L"p.X.use()", L"call", file, 18, ""}}) == 0);
}

TEST_CASE("st counts the index errors of a wide walk")
{
	Code code;
	brokenFile(code, L"Broken");
	const Id broken = code.member(code.type(L"p", L"Broken"), L"Broken", L"call");
	code.scope(broken, L"Broken", 10, 15);
	indexError(code, L"Broken", 12, 9, L"Gone_ cannot be resolved to a variable");
	const Id wide = code.type(L"p", L"Wide");
	for (int i = 0; i < 12; i++)
	{
		const Id member = code.member(wide, L"Wide", L"m" + std::to_wstring(i));
		if (i == 0)
		{
			code.link(Edge::EDGE_CALL, broken, member, L"Wide", {7});
		}
	}
	code.storage.buildCaches();

	const st::ImpactResult impact = st::impact(code.storage, {wide}, 0, st::IMPACT_EDGES, st::Group::None);
	REQUIRE(keys(impact.lines).count(L"p.Broken.call()"));
	CHECK(lineFor(impact.lines, L"p.Broken.call()")->file.wstr() == L"/r/p/Wide.java");
	CHECK(st::findGaps(code.storage, impact.walked, impact.lines).indexErrors == 1);
	const st::ImpactResult narrow = st::impact(code.storage, {broken}, 0, st::IMPACT_EDGES, st::Group::None);
	CHECK(st::findGaps(code.storage, narrow.walked, narrow.lines).indexErrors == 1);
}

TEST_CASE("st counts a symbol as the project's own only inside a package the project declares")
{
	Code code;
	const Id use = code.member(code.type(L"p", L"User"), L"User", L"use");
	// A library package below an own one, and a package of the project's own holding no defined type.
	undefined(code.storage, typeName({L"p", L"lib"}), NODE_PACKAGE);
	const Id library = undefined(code.storage, typeName({L"p", L"lib", L"Thing"}), NODE_SYMBOL);
	add(code.storage, typeName({L"p", L"recs"}), NODE_PACKAGE);
	const Id record = undefined(code.storage, typeName({L"p", L"recs", L"Rec"}), NODE_SYMBOL);
	code.link(Edge::EDGE_TYPE_USAGE, use, library, L"User", {4});
	code.link(Edge::EDGE_TYPE_USAGE, use, record, L"User", {5});
	code.storage.buildCaches();

	st::Walked walked;
	walked.scopes.push_back({{use}, 0, Edge::EDGE_TYPE_USAGE});
	CHECK(st::findGaps(code.storage, walked, {}).unresolvedLinks == 1);
	CHECK(st::countUnresolved(code.storage) == 1);
}

TEST_CASE("st graph commands count the unresolved links they followed")
{
	Code code;
	const Id api = code.type(L"p", L"Api", NODE_INTERFACE);
	const Id apiRun = code.member(api, L"Api", L"run");
	const Id a = code.type(L"p", L"A");
	const Id run = code.member(a, L"A", L"run");
	code.link(Edge::EDGE_OVERRIDE, run, apiRun, L"A", {2});
	const Id use = code.member(code.type(L"p", L"User"), L"User", L"use");
	code.link(Edge::EDGE_CALL, use, run, L"User", {3});
	const Id unsolved = unsolvedSymbol(code);
	code.link(Edge::EDGE_CALL, unsolved, run, L"Rec", {4});
	// A record implementing the interface.
	code.link(Edge::EDGE_OVERRIDE, unsolved, apiRun, L"Rec", {5});
	code.storage.buildCaches();

	st::Walked refs;
	const auto refLines = st::references(code.storage, {run}, &refs);
	CHECK(st::findGaps(code.storage, refs, refLines).unresolvedLinks == 1);

	st::Walked impls;
	const auto implLines = st::hierarchy(code.storage, {apiRun}, st::Direction::Incoming, &impls);
	CHECK(st::findGaps(code.storage, impls, implLines).unresolvedLinks == 1);
	st::Walked supers;
	const auto superLines = st::hierarchy(code.storage, {run}, st::Direction::Outgoing, &supers);
	CHECK(st::findGaps(code.storage, supers, superLines).unresolvedLinks == 0);

	const st::ImpactResult impact = st::impact(code.storage, {run}, 0, st::IMPACT_EDGES, st::Group::None);
	// The record's override is a sibling of run: a change to run doesn't reach it.
	CHECK(st::findGaps(code.storage, impact.walked, impact.lines).unresolvedLinks == 1);

	st::Walked depsIn;
	const auto depLines = st::deps(code.storage, {a}, st::Direction::Incoming, &depsIn);
	CHECK(st::findGaps(code.storage, depsIn, depLines).unresolvedLinks == 1);
	st::Walked depsOut;
	const auto depOutLines = st::deps(code.storage, {use}, st::Direction::Outgoing, &depsOut);
	CHECK(st::findGaps(code.storage, depsOut, depOutLines).unresolvedLinks == 0);

	st::Walked path;
	st::paths(code.storage, {use}, {run}, 6, 10, st::PATH_EDGES, &path);
	CHECK(st::findGaps(code.storage, path, {}).unresolvedLinks == 1);
}

TEST_CASE("no incomplete line when nothing is missing")
{
	Code code;
	const Id run = code.member(code.type(L"p", L"A"), L"A", L"run");
	const Id use = code.member(code.type(L"p", L"User"), L"User", L"use");
	code.link(Edge::EDGE_CALL, use, run, L"User", {3});
	code.storage.buildCaches();

	const st::Gaps gaps = gapsOfCalls(code, run, st::Direction::Incoming, 0);
	CHECK(gaps.unresolvedLinks == 0);
	CHECK(gaps.indexErrors == 0);
	CHECK(printed(gaps).empty());
	CHECK(printed({2, 0}) == "st: incomplete: 2 links to unresolved symbols (see st status)\n");
	CHECK(printed({0, 3}) == "st: incomplete: 3 index errors (see st status)\n");
	CHECK(printed({2, 3}) == "st: incomplete: 2 links to unresolved symbols, 3 index errors (see st status)\n");
}

TEST_CASE("st reports a failed gap check as incomplete")
{
	const auto printedOf = [](const std::function<st::Gaps()>& findGaps) {
		std::ostringstream out;
		st::printGaps(findGaps, out);
		return out.str();
	};
	CHECK(printedOf([]() { return st::Gaps{0, 3}; }) == "st: incomplete: 3 index errors (see st status)\n");
	CHECK(printedOf([]() -> st::Gaps { throw std::runtime_error("no such table: error"); }) ==
		"st: incomplete: gap check failed (no such table: error)\n");
	CHECK(printedOf([]() -> st::Gaps { throw 1; }) == "st: incomplete: gap check failed (unknown error)\n");
}

TEST_CASE("st definition lines list each file of a class declared in two modules")
{
	Code code;
	const Id a = code.type(L"A");
	const Id field = code.member(a, L"A", L"f", NODE_FIELD);
	const Id other = addFile(code.storage, L"/r/sub/p/A.java");
	addLocation(code.storage, a, other, 3);
	addLocation(code.storage, field, other, 4);
	// a qualifier in a third file is a use, not a definition
	const Id user = addFile(code.storage, L"/r/q/User.java");
	addLocation(code.storage, a, user, 7, LOCATION_QUALIFIER);
	code.storage.buildCaches();

	const auto lines = st::definitionLines(code.storage, a, L"p.A");
	REQUIRE(lines.size() == 2);
	CHECK(lines[0].file.wstr() == L"/r/p/A.java");
	CHECK(lines[0].line == 1);
	CHECK(lines[1].file.wstr() == L"/r/sub/p/A.java");
	CHECK(lines[1].line == 3);
	REQUIRE(st::outline(code.storage, {field}).size() == 2);

	// a file's symbols list only that file's line
	const std::set<FilePath> sub = {FilePath(L"/r/sub/p/A.java")};
	const auto inSub = st::definitionLines(code.storage, a, L"p.A", &sub);
	REQUIRE(inSub.size() == 1);
	CHECK(inSub[0].line == 3);
	const auto outlined = st::outline(code.storage, {field}, {{field, sub}});
	REQUIRE(outlined.size() == 1);
	CHECK(outlined[0].file.wstr() == L"/r/sub/p/A.java");
}

TEST_CASE("st gaps count walked symbols whose type two modules declare")
{
	// none, the callee's class, or only the caller's class (a result row, which the walk did not go beyond)
	for (const std::wstring twice: {L"", L"A", L"B"})
	{
		Code code;
		const Id pkg = code.package(L"q");
		const Id a = code.type(L"q", L"A");
		const Id f = code.member(a, L"A", L"f");
		const Id b = code.type(L"q", L"B");
		const Id g = code.member(b, L"B", L"g");
		code.link(Edge::EDGE_CALL, g, f, L"B", {3});
		// the package is declared in every file and never counts
		addLocation(code.storage, pkg, code.file(L"A"), 1);
		addLocation(code.storage, pkg, code.file(L"B"), 1);
		// Code puts a type and its members on one line, and a batched location query keeps one token per position
		addLocation(code.storage, a, code.file(L"A"), 5);
		addLocation(code.storage, b, code.file(L"B"), 5);
		if (!twice.empty())
		{
			addLocation(code.storage, twice == L"A" ? a : b, addFile(code.storage, L"/r/sub/q/" + twice + L".java"), 2);
		}
		code.storage.buildCaches();

		const bool warns = twice == L"A";
		const st::Gaps gaps = gapsOfCalls(code, f, st::Direction::Incoming, 1);
		CHECK(gaps.multiDeclared == (warns ? 1 : 0));
		CHECK(printed(gaps) == (warns ? "st: incomplete: 1 symbols declared in several modules (see st status)\n" : ""));
	}
	CHECK(printed({2, 0, 1}) ==
		"st: incomplete: 2 links to unresolved symbols, 1 symbols declared in several modules (see st status)\n");
}
