#include <filesystem>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "UserPaths.h"
#if defined(__linux__)
#  include <QByteArray>

#  include "details/ApplicationSettings.h"
#  include "includes.h"
#endif

namespace fs = std::filesystem;

TEST(UserPaths, goodCase) {
  const auto tempDir = fs::path{fs::temp_directory_path(), fs::path::path::generic_format};
  EXPECT_TRUE(UserPaths::getUserDataDirectoryPath().empty());
  UserPaths::setUserDataDirectoryPath(FilePath{tempDir});
  EXPECT_EQ(tempDir, UserPaths::getUserDataDirectoryPath().str());

  EXPECT_EQ(tempDir / "ApplicationSettings.xml", UserPaths::getAppSettingsFilePath().str());
  EXPECT_EQ(tempDir / "window_settings.ini", UserPaths::getWindowSettingsFilePath().str());
  EXPECT_EQ(tempDir / "log/", UserPaths::getLogDirectoryPath().str());
}

#if defined(__linux__)
// Settings written by the fork (which keeps them in $HOME/.config/sourcetrail) must be found and read as they are.
TEST(UserPaths, applicationSettingsStayWhereMasterKeepsThem) {
  const fs::path home = fs::temp_directory_path() / ("sourcetrail-user-paths-" + std::to_string(::getpid()));
  const fs::path settingsDirectory = home / ".config" / "sourcetrail";
  fs::create_directories(settingsDirectory);
  // written by a run of the fork; only the JDK path was replaced by a neutral one
  fs::copy_file("data/UserPathsTestSuite/ApplicationSettings.xml",
                settingsDirectory / "ApplicationSettings.xml",
                fs::copy_options::overwrite_existing);

  const QByteArray previousHome = qgetenv("HOME");
  qputenv("HOME", QByteArray::fromStdString(home.string()));
  setupPlatform(0, nullptr);
  qputenv("HOME", previousHome);

  EXPECT_EQ(settingsDirectory / "ApplicationSettings.xml", fs::path(UserPaths::getAppSettingsFilePath().str()));

  ApplicationSettings settings;
  ASSERT_TRUE(settings.load(UserPaths::getAppSettingsFilePath(), true));
  EXPECT_EQ(L"/opt/jdk-21/lib/server/libjvm.so", settings.getJavaPath().wstr());
  EXPECT_TRUE(settings.getHasPrefilledJavaPath());
  EXPECT_EQ(L"/usr/bin/mvn", settings.getMavenPath().wstr());

  fs::remove_all(home);
}
#endif
