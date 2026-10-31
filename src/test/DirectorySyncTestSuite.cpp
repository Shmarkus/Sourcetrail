#include "catch.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "FilePath.h"
#include "FileSystem.h"
#include "utilityDirectorySync.h"

namespace
{
const FilePath s_root(L"data/DirectorySyncTestSuite/tmp");

struct SyncFixture
{
	SyncFixture()
		: source(s_root.getConcatenated(L"source")), target(s_root.getConcatenated(L"target"))
	{
		std::filesystem::remove_all(s_root.str());
		FileSystem::createDirectory(source);
		FileSystem::createDirectory(target);
	}

	~SyncFixture()
	{
		std::error_code ec;
		for (const auto& entry: std::filesystem::recursive_directory_iterator(s_root.str(), ec))
		{
			if (entry.is_directory(ec))
			{
				std::filesystem::permissions(entry.path(), std::filesystem::perms::owner_all, ec);
			}
		}
		std::filesystem::remove_all(s_root.str());
	}

	static void write(const FilePath& directory, const std::string& relativePath, const std::string& content)
	{
		const std::filesystem::path path = std::filesystem::path(directory.str()) / relativePath;
		std::filesystem::create_directories(path.parent_path());
		std::ofstream(path, std::ios::binary) << content;
	}

	static std::string read(const FilePath& directory, const std::string& relativePath)
	{
		std::ifstream stream(std::filesystem::path(directory.str()) / relativePath, std::ios::binary);
		std::stringstream buffer;
		buffer << stream.rdbuf();
		return buffer.str();
	}

	static bool exists(const FilePath& directory, const std::string& relativePath)
	{
		return std::filesystem::exists(std::filesystem::path(directory.str()) / relativePath);
	}

	FilePath source;
	FilePath target;
};
}	 // namespace

TEST_CASE("directory sync keeps identical files untouched")
{
	SyncFixture f;
	f.write(f.source, "a/A.java", "class A {}");
	f.write(f.target, "a/A.java", "class A {}");

	const std::filesystem::path targetFile = std::filesystem::path(f.target.str()) / "a/A.java";
	const auto past = std::filesystem::last_write_time(targetFile) - std::chrono::seconds(10);
	std::filesystem::last_write_time(targetFile, past);

	const utility::DirectorySyncResult result = utility::syncDirectoryContents(f.source, f.target);

	REQUIRE(std::filesystem::last_write_time(targetFile) == past);
	REQUIRE(result.unchanged == 1);
	REQUIRE(result.written == 0);
	REQUIRE(result.removed == 0);
}

TEST_CASE("directory sync rewrites changed files and adds new ones")
{
	SyncFixture f;
	f.write(f.source, "a/A.java", "class A { int x; }");
	f.write(f.source, "b/B.java", "class B {}");
	f.write(f.target, "a/A.java", "class A {}");

	const utility::DirectorySyncResult result = utility::syncDirectoryContents(f.source, f.target);

	REQUIRE(f.read(f.target, "a/A.java") == "class A { int x; }");
	REQUIRE(f.read(f.target, "b/B.java") == "class B {}");
	REQUIRE(result.written == 2);
	REQUIRE(result.unchanged == 0);
}

TEST_CASE("directory sync removes files missing from the source")
{
	SyncFixture f;
	f.write(f.source, "a/A.java", "class A {}");
	f.write(f.target, "a/A.java", "class A {}");
	f.write(f.target, "old/Old.java", "class Old {}");

	const utility::DirectorySyncResult result = utility::syncDirectoryContents(f.source, f.target);

	REQUIRE_FALSE(f.exists(f.target, "old/Old.java"));
	REQUIRE_FALSE(f.exists(f.target, "old"));
	REQUIRE(f.exists(f.target, "a/A.java"));
	REQUIRE(result.removed == 1);
}

TEST_CASE("directory sync reports files it cannot write")
{
	SyncFixture f;
	f.write(f.source, "a/A.java", "class A { int x; }");
	f.write(f.target, "a/A.java", "class A {}");

	const std::filesystem::path targetDirectory = std::filesystem::path(f.target.str()) / "a";
	std::filesystem::permissions(
		targetDirectory,
		std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);

#ifndef _WIN32
	if (geteuid() == 0)
	{
		WARN("running as root, read-only directories are still writable; skipping");
		return;
	}
#endif

	utility::DirectorySyncResult result;
	REQUIRE_NOTHROW(result = utility::syncDirectoryContents(f.source, f.target));

	REQUIRE(result.failed == 1);
	REQUIRE(result.written == 0);
	REQUIRE(f.read(f.target, "a/A.java") == "class A {}");
}

TEST_CASE("file sync writes only listed files whose bytes changed")
{
	SyncFixture f;
	f.write(f.source, "A.java", "class A { int x; }");
	f.write(f.source, "B.java", "class B {}");
	f.write(f.target, "pkg/A.java", "class A {}");
	f.write(f.target, "pkg/B.java", "class B {}");
	f.write(f.target, "pkg/C.java", "class C {}");

	const std::filesystem::path unchangedFile = std::filesystem::path(f.target.str()) / "pkg/B.java";
	const auto past = std::filesystem::last_write_time(unchangedFile) - std::chrono::seconds(10);
	std::filesystem::last_write_time(unchangedFile, past);

	const utility::DirectorySyncResult result = utility::syncFiles(
		{{f.source.getConcatenated(L"A.java"), f.target.getConcatenated(L"pkg/A.java")},
		 {f.source.getConcatenated(L"B.java"), f.target.getConcatenated(L"pkg/B.java")},
		 {f.source.getConcatenated(L"D.java"), f.target.getConcatenated(L"new/D.java")}});

	REQUIRE(f.read(f.target, "pkg/A.java") == "class A { int x; }");
	REQUIRE(std::filesystem::last_write_time(unchangedFile) == past);
	REQUIRE(f.exists(f.target, "pkg/C.java"));
	REQUIRE_FALSE(f.exists(f.target, "new/D.java"));
	REQUIRE(result.written == 1);
	REQUIRE(result.unchanged == 1);
	REQUIRE(result.failed == 1);
}

TEST_CASE("directory sync fills a target that was checked before it existed")
{
	SyncFixture f;
	f.write(f.source, "a/A.java", "class A {}");
	const FilePath target = s_root.getConcatenated(L"fresh");
	REQUIRE_FALSE(target.recheckExists());

	const utility::DirectorySyncResult result = utility::syncDirectoryContents(f.source, target);

	REQUIRE(f.read(target, "a/A.java") == "class A {}");
	REQUIRE(result.written == 1);
	REQUIRE(result.removed == 0);
}
