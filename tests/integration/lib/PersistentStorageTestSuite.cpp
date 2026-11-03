#include <range/v3/algorithm/copy.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "FilePath.h"
#include "FileSystem.h"
#include "PersistentStorage.h"
#include "SqliteIndexStorage.h"

namespace {

constexpr std::wstring_view kDbPath = L"data/test.sqlite";
constexpr std::wstring_view kBookmarkPath = L"data/testBookmarks.sqlite";

using testing::IsEmpty;
using testing::Test;

struct TaskClearStorageScenario : Test {
  void SetUp() override {
    mStorage = std::make_unique<PersistentStorage>(FilePath{kDbPath.data()}, FilePath{kBookmarkPath.data()});
    mStorage->setup();
  }

  void TearDown() override {}

  void createErrors() const {
    constexpr size_t ErrosCount = 10;
    for(Id index = 0; index < ErrosCount; ++index) {
      mStorage->addError({std::to_wstring(index), {}, false, false});
    }
  }

  std::unique_ptr<PersistentStorage> mStorage;
};

TEST_F(TaskClearStorageScenario, goodCase) {
  // Given:
  createErrors();
  // When:
  mStorage->clearAllErrors();
  // Then:
  ASSERT_THAT(mStorage->getErrors(), IsEmpty());
}

TEST(StorageVersion, version26DatabaseIsReportedIncompatible) {
  // Version 26 is what master's fork wrote; this build's index is version 27 and must be rebuilt from scratch.
  const FilePath databasePath(L"data/StorageVersionTestSuite/version26.sqlite");
  std::ignore = FileSystem::remove(databasePath);
  {
    SqliteIndexStorage oldStorage(databasePath);
    oldStorage.setup();
    oldStorage.setVersion(26);
  }

  bool incompatible = false;
  {
    const PersistentStorage storage(databasePath, FilePath{L"data/StorageVersionTestSuite/version26Bookmarks.sqlite"});
    incompatible = storage.isIncompatible();
  }
  std::ignore = FileSystem::remove(databasePath);
  std::ignore = FileSystem::remove(FilePath{L"data/StorageVersionTestSuite/version26Bookmarks.sqlite"});

  EXPECT_EQ(27U, SqliteIndexStorage::getStorageVersion());
  EXPECT_TRUE(incompatible);
}

}    // namespace
