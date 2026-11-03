#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "JavaProjectSetup.h"
#include "ProjectSettings.h"
#include "SourceGroupSettingsJavaEmpty.h"
#include "SourceGroupSettingsJavaGradle.h"
#include "SourceGroupSettingsJavaMaven.h"

namespace fs = std::filesystem;

namespace {

class JavaProjectSetupTest : public testing::Test {
protected:
  void SetUp() override {
    const auto* info = testing::UnitTest::GetInstance()->current_test_info();
    mRoot = fs::temp_directory_path() / "sourcetrail-java-project-setup" / info->name() / "shop";
    fs::remove_all(mRoot);
    fs::create_directories(mRoot);
  }

  void TearDown() override {
    fs::remove_all(mRoot.parent_path());
  }

  void touch(const fs::path& relative) const {
    fs::create_directories((mRoot / relative).parent_path());
    std::ofstream{mRoot / relative} << "\n";
  }

  [[nodiscard]] FilePath folder() const {
    return FilePath{mRoot.wstring()};
  }

  // The source group refers back to the loaded settings, so they live as long as the test.
  std::shared_ptr<SourceGroupSettings> onlySourceGroup(const FilePath& projectFile) {
    EXPECT_TRUE(mSettings.load(projectFile));
    auto groups = mSettings.getAllSourceGroupSettings();
    EXPECT_EQ(1U, groups.size());
    return groups.empty() ? nullptr : groups.front();
  }

  fs::path mRoot;
  ProjectSettings mSettings;
};

}    // namespace

TEST_F(JavaProjectSetupTest, detectsGradleFromBuildGradle) {
  touch("build.gradle");
  EXPECT_EQ(JavaBuildKind::Gradle, detectJavaBuild(folder()));
}

TEST_F(JavaProjectSetupTest, detectsGradleFromBuildGradleKts) {
  touch("build.gradle.kts");
  EXPECT_EQ(JavaBuildKind::Gradle, detectJavaBuild(folder()));
}

TEST_F(JavaProjectSetupTest, detectsGradleFromSettingsGradleOnly) {
  touch("settings.gradle");
  EXPECT_EQ(JavaBuildKind::Gradle, detectJavaBuild(folder()));
}

TEST_F(JavaProjectSetupTest, detectsGradleFromSettingsGradleKtsOnly) {
  touch("settings.gradle.kts");
  EXPECT_EQ(JavaBuildKind::Gradle, detectJavaBuild(folder()));
}

TEST_F(JavaProjectSetupTest, detectsMavenFromPom) {
  touch("pom.xml");
  EXPECT_EQ(JavaBuildKind::Maven, detectJavaBuild(folder()));
}

TEST_F(JavaProjectSetupTest, gradleWinsWhenBothExist) {
  touch("pom.xml");
  touch("build.gradle");
  EXPECT_EQ(JavaBuildKind::Gradle, detectJavaBuild(folder()));
}

TEST_F(JavaProjectSetupTest, nestedModuleBuildFilesAloneGiveNone) {
  touch("app/build.gradle");
  touch("core/pom.xml");
  touch("lib/settings.gradle.kts");
  EXPECT_EQ(JavaBuildKind::None, detectJavaBuild(folder()));
}

TEST_F(JavaProjectSetupTest, aDirectoryNamedLikeABuildFileGivesNone) {
  fs::create_directories(mRoot / "pom.xml");
  EXPECT_EQ(JavaBuildKind::None, detectJavaBuild(folder()));
}

TEST_F(JavaProjectSetupTest, projectFileIsNamedAfterTheFolder) {
  EXPECT_EQ(FilePath{(mRoot / ".sourcetrail" / "shop.srctrlprj").wstring()}, projectFileFor(folder()));
}

TEST_F(JavaProjectSetupTest, projectFileIgnoresATrailingSeparator) {
  EXPECT_EQ(projectFileFor(folder()), projectFileFor(FilePath{mRoot.wstring() + L"/"}));
}

TEST_F(JavaProjectSetupTest, createWritesProjectIntoDotFolderWithGitignore) {
  touch("build.gradle");

  const FilePath projectFile = createJavaProject(folder(), JavaBuildKind::Gradle);

  ASSERT_EQ(projectFileFor(folder()), projectFile);
  EXPECT_TRUE(fs::is_regular_file(mRoot / ".sourcetrail" / "shop.srctrlprj"));

  std::ifstream gitignore{mRoot / ".sourcetrail" / ".gitignore", std::ios::binary};
  std::stringstream content;
  content << gitignore.rdbuf();
  EXPECT_EQ("*\n", content.str());

  auto gradle = std::dynamic_pointer_cast<SourceGroupSettingsJavaGradle>(onlySourceGroup(projectFile));
  ASSERT_TRUE(gradle);
  EXPECT_TRUE(gradle->getShouldIndexGradleTests());
  EXPECT_EQ(FilePath{(mRoot / "build.gradle").wstring()}, gradle->getGradleProjectFilePathExpandedAndAbsolute());
}

TEST_F(JavaProjectSetupTest, createPointsGradleAtSettingsGradleWhenThereIsNoBuildGradle) {
  touch("settings.gradle.kts");

  auto gradle = std::dynamic_pointer_cast<SourceGroupSettingsJavaGradle>(
      onlySourceGroup(createJavaProject(folder(), JavaBuildKind::Gradle)));

  ASSERT_TRUE(gradle);
  EXPECT_EQ(FilePath{(mRoot / "settings.gradle.kts").wstring()}, gradle->getGradleProjectFilePathExpandedAndAbsolute());
}

TEST_F(JavaProjectSetupTest, createWritesAMavenSourceGroupWithTests) {
  touch("pom.xml");

  auto maven = std::dynamic_pointer_cast<SourceGroupSettingsJavaMaven>(
      onlySourceGroup(createJavaProject(folder(), JavaBuildKind::Maven)));

  ASSERT_TRUE(maven);
  EXPECT_TRUE(maven->getShouldIndexMavenTests());
  EXPECT_EQ(FilePath{(mRoot / "pom.xml").wstring()}, maven->getMavenProjectFilePathExpandedAndAbsolute());
}

TEST_F(JavaProjectSetupTest, createWithoutABuildWritesAPlainJavaSourceGroupOverTheFolder) {
  touch("src/Main.java");

  auto plain = std::dynamic_pointer_cast<SourceGroupSettingsJavaEmpty>(
      onlySourceGroup(createJavaProject(folder(), JavaBuildKind::None)));

  ASSERT_TRUE(plain);
  ASSERT_EQ(1U, plain->getSourcePathsExpandedAndAbsolute().size());
  EXPECT_EQ(folder(), plain->getSourcePathsExpandedAndAbsolute().front());
  EXPECT_TRUE(plain->getClasspath().empty());
}

TEST_F(JavaProjectSetupTest, existingProjectIsFound) {
  touch("pom.xml");
  EXPECT_TRUE(existingProjectFile(folder()).empty());

  const FilePath projectFile = createJavaProject(folder(), JavaBuildKind::Maven);

  ASSERT_FALSE(projectFile.empty());
  EXPECT_EQ(projectFile, existingProjectFile(folder()));
}

TEST_F(JavaProjectSetupTest, existingProjectWithAnotherNameIsFound) {
  touch(".sourcetrail/renamed.srctrlprj");
  touch(".sourcetrail/shop.srctrldb");

  EXPECT_EQ(FilePath{(mRoot / ".sourcetrail" / "renamed.srctrlprj").wstring()}, existingProjectFile(folder()));
}

TEST_F(JavaProjectSetupTest, openOrCreateOpensTheExistingProject) {
  touch("build.gradle");
  const FilePath projectFile = createJavaProject(folder(), JavaBuildKind::Gradle);

  const JavaProjectChoice choice = openOrCreateJavaProject(folder(), [] { return true; });

  EXPECT_EQ(projectFile, choice.projectFile);
  EXPECT_FALSE(choice.created);
}

TEST_F(JavaProjectSetupTest, openOrCreateCreatesAProjectForTheDetectedBuild) {
  touch("pom.xml");
  bool asked = false;

  const JavaProjectChoice choice = openOrCreateJavaProject(folder(), [&asked] { return asked = true; });

  EXPECT_FALSE(asked);
  EXPECT_TRUE(choice.created);
  EXPECT_EQ(JavaBuildKind::Maven, choice.kind);
  EXPECT_EQ(projectFileFor(folder()), choice.projectFile);
  EXPECT_TRUE(std::dynamic_pointer_cast<SourceGroupSettingsJavaMaven>(onlySourceGroup(choice.projectFile)));
}

TEST_F(JavaProjectSetupTest, openOrCreateWithoutABuildCreatesAPlainJavaProjectWhenAccepted) {
  const JavaProjectChoice choice = openOrCreateJavaProject(folder(), [] { return true; });

  EXPECT_TRUE(choice.created);
  EXPECT_EQ(JavaBuildKind::None, choice.kind);
  EXPECT_TRUE(std::dynamic_pointer_cast<SourceGroupSettingsJavaEmpty>(onlySourceGroup(choice.projectFile)));
}

TEST_F(JavaProjectSetupTest, openOrCreateWithoutABuildWritesNothingWhenDeclined) {
  const JavaProjectChoice choice = openOrCreateJavaProject(folder(), [] { return false; });

  EXPECT_TRUE(choice.projectFile.empty());
  EXPECT_FALSE(choice.created);
  EXPECT_FALSE(fs::exists(mRoot / ".sourcetrail"));
}
