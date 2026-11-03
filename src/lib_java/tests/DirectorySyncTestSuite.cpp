#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "FilePath.h"
#include "FileSystem.h"
#include "utilityDirectorySync.h"

namespace {
const FilePath s_root(L"data/DirectorySyncTestSuite/tmp");

class DirectorySync : public testing::Test {
protected:
  void SetUp() override {
    std::filesystem::remove_all(s_root.str());
    FileSystem::createDirectory(source);
    FileSystem::createDirectory(target);
  }

  void TearDown() override {
    std::filesystem::remove_all(s_root.str());
  }

  static void write(const FilePath& directory, const std::string& relativePath, const std::string& content) {
    const std::filesystem::path path = std::filesystem::path(directory.str()) / relativePath;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << content;
  }

  static std::string read(const FilePath& directory, const std::string& relativePath) {
    std::ifstream stream(std::filesystem::path(directory.str()) / relativePath, std::ios::binary);
    std::stringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
  }

  static bool exists(const FilePath& directory, const std::string& relativePath) {
    return std::filesystem::exists(std::filesystem::path(directory.str()) / relativePath);
  }

  FilePath source = s_root.getConcatenated(L"source");
  FilePath target = s_root.getConcatenated(L"target");
};
}    // namespace

TEST_F(DirectorySync, keepsIdenticalFilesUntouched) {
  write(source, "a/A.java", "class A {}");
  write(target, "a/A.java", "class A {}");

  const std::filesystem::path targetFile = std::filesystem::path(target.str()) / "a/A.java";
  const auto past = std::filesystem::last_write_time(targetFile) - std::chrono::seconds(10);
  std::filesystem::last_write_time(targetFile, past);

  const utility::DirectorySyncResult result = utility::syncDirectoryContents(source, target);

  EXPECT_EQ(past, std::filesystem::last_write_time(targetFile));
  EXPECT_EQ(1U, result.unchanged);
  EXPECT_EQ(0U, result.written);
  EXPECT_EQ(0U, result.removed);
}

TEST_F(DirectorySync, rewritesChangedFilesAndAddsNewOnes) {
  write(source, "a/A.java", "class A { int x; }");
  write(source, "b/B.java", "class B {}");
  write(target, "a/A.java", "class A {}");

  const utility::DirectorySyncResult result = utility::syncDirectoryContents(source, target);

  EXPECT_EQ("class A { int x; }", read(target, "a/A.java"));
  EXPECT_EQ("class B {}", read(target, "b/B.java"));
  EXPECT_EQ(2U, result.written);
  EXPECT_EQ(0U, result.unchanged);
}

TEST_F(DirectorySync, removesFilesMissingFromTheSource) {
  write(source, "a/A.java", "class A {}");
  write(target, "a/A.java", "class A {}");
  write(target, "old/Old.java", "class Old {}");

  const utility::DirectorySyncResult result = utility::syncDirectoryContents(source, target);

  EXPECT_FALSE(exists(target, "old/Old.java"));
  EXPECT_FALSE(exists(target, "old"));
  EXPECT_TRUE(exists(target, "a/A.java"));
  EXPECT_EQ(1U, result.removed);
}
