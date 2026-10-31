#include <sstream>

#include "StFilter.h"
#include "StInput.h"
#include "StKey.h"
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
