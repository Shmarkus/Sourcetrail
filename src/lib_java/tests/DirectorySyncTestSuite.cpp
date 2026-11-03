#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include <unistd.h>

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

TEST_F(DirectorySync, reportsFilesItCannotWrite) {
  if(geteuid() == 0) {
    GTEST_SKIP() << "running as root, read-only directories are still writable";
  }
  write(source, "a/A.java", "class A { int x; }");
  write(target, "a/A.java", "class A {}");

  const std::filesystem::path targetDirectory = std::filesystem::path(target.str()) / "a";
  std::filesystem::permissions(targetDirectory, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);

  utility::DirectorySyncResult result;
  EXPECT_NO_THROW(result = utility::syncDirectoryContents(source, target));
  std::filesystem::permissions(targetDirectory, std::filesystem::perms::owner_all);

  EXPECT_EQ(1U, result.failed);
  EXPECT_EQ(0U, result.written);
  EXPECT_EQ("class A {}", read(target, "a/A.java"));
}

TEST_F(DirectorySync, fileSyncWritesOnlyListedFilesWhoseBytesChanged) {
  write(source, "A.java", "class A { int x; }");
  write(source, "B.java", "class B {}");
  write(target, "pkg/A.java", "class A {}");
  write(target, "pkg/B.java", "class B {}");
  write(target, "pkg/C.java", "class C {}");

  const std::filesystem::path unchangedFile = std::filesystem::path(target.str()) / "pkg/B.java";
  const auto past = std::filesystem::last_write_time(unchangedFile) - std::chrono::seconds(10);
  std::filesystem::last_write_time(unchangedFile, past);

  const utility::DirectorySyncResult result = utility::syncFiles(
      {{source.getConcatenated(L"A.java"), target.getConcatenated(L"pkg/A.java")},
       {source.getConcatenated(L"B.java"), target.getConcatenated(L"pkg/B.java")},
       {source.getConcatenated(L"D.java"), target.getConcatenated(L"new/D.java")}});

  EXPECT_EQ("class A { int x; }", read(target, "pkg/A.java"));
  EXPECT_EQ(past, std::filesystem::last_write_time(unchangedFile));
  EXPECT_TRUE(exists(target, "pkg/C.java"));
  EXPECT_FALSE(exists(target, "new/D.java"));
  EXPECT_EQ(1U, result.written);
  EXPECT_EQ(1U, result.unchanged);
  EXPECT_EQ(1U, result.failed);
}

TEST_F(DirectorySync, fillsATargetThatWasCheckedBeforeItExisted) {
  write(source, "a/A.java", "class A {}");
  const FilePath fresh = s_root.getConcatenated(L"fresh");
  ASSERT_FALSE(fresh.recheckExists());

  const utility::DirectorySyncResult result = utility::syncDirectoryContents(source, fresh);

  EXPECT_EQ("class A {}", read(fresh, "a/A.java"));
  EXPECT_EQ(1U, result.written);
  EXPECT_EQ(0U, result.removed);
}
