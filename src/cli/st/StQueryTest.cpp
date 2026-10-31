#include <sstream>

#include "StFiles.h"
#include "StFilter.h"
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
	REQUIRE(st::matchIndexedFiles(code.storage, L"Util.java", cwd).size() == 2);
	const auto one = st::matchIndexedFiles(code.storage, L"a/Util.java", cwd);
	REQUIRE(one.size() == 1);
	REQUIRE(one.front().wstr() == L"/r/a/Util.java");
	REQUIRE(st::matchIndexedFiles(code.storage, L"/r/b/Util.java", cwd).size() == 1);
	REQUIRE(st::matchIndexedFiles(code.storage, L"/r/c/Util.java", cwd).empty());
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
