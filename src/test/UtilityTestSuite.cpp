#include "catch.hpp"

#include <chrono>

#include "ApplicationSettings.h"
#include "FilePath.h"
#include "utility.h"
#include "utilityApp.h"

TEST_CASE("trim blank spaces of string")
{
	REQUIRE(utility::trim(" foo  ") == "foo");
}

TEST_CASE("trim blank spaces of wstring")
{
	REQUIRE(utility::trim(L" foo  ") == L"foo");
}

TEST_CASE("execute process returns when a short process exits instead of waiting out the timeout")
{
	// A JVM closes its output a moment before it exits, which is when the exit signal used to get lost.
	const FilePath java = ApplicationSettings::getInstance()->getJavaPath().getParentDirectory().getParentDirectory()
		.getParentDirectory().getConcatenated(L"bin/java");
	if (!java.exists())
	{
		WARN("no Java configured, skipping");
		return;
	}
	size_t slow = 0;
	for (int i = 0; i < 40; i++)
	{
		const auto start = std::chrono::steady_clock::now();
		const utility::ProcessOutput output = utility::executeProcess(java.wstr(), {L"-version"}, FilePath(), true, 3000);
		REQUIRE(output.exitCode == 0);
		slow += std::chrono::steady_clock::now() - start > std::chrono::milliseconds(2000);
	}
	REQUIRE(slow == 0);
}
