#include "catch.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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
