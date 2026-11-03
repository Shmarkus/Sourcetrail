#include <gtest/gtest.h>

#include "language_packages.h"
#include "ProjectSettings.h"
#include "SourceGroupSettings.h"
#include "SourceGroupSettingsTypeScript.h"
#include "SourceGroupSettingsWithSourcePaths.h"
#if BUILD_JAVA_LANGUAGE_PACKAGE
#  include "SourceGroupSettingsJavaMaven.h"
#endif    // BUILD_JAVA_LANGUAGE_PACKAGE

constexpr auto SettingsPath = L"data/SettingsTestSuite/settings.xml";

void PrintTo(LanguageType type, std::ostream* ostream) {
  switch(type) {
#if BUILD_CXX_LANGUAGE_PACKAGE
  case LANGUAGE_CPP:
    *ostream << "LANGUAGE_CPP";
    break;
  case LANGUAGE_C:
    *ostream << "LANGUAGE_C";
    break;
#endif    // BUILD_CXX_LANGUAGE_PACKAGE
#if BUILD_JAVA_LANGUAGE_PACKAGE
  case LANGUAGE_JAVA:
    *ostream << "LANGUAGE_JAVA";
    break;
#endif    // BUILD_JAVA_LANGUAGE_PACKAGE
  case LANGUAGE_CUSTOM:
    *ostream << "LANGUAGE_CUSTOM";
    break;
  case LANGUAGE_UNKNOWN:
    *ostream << "LANGUAGE_UNKNOWN";
    break;
  }
}

TEST(ProjectSettings, getLanguageOfProject) {
  EXPECT_EQ(LANGUAGE_CUSTOM, ProjectSettings::getLanguageOfProject(FilePath(SettingsPath)));
}

TEST(ProjectSettings, loadProjectSettingsFromFile) {
  ProjectSettings settings;
  EXPECT_TRUE(settings.load(FilePath(SettingsPath)));
}

TEST(ProjectSettings, loadSourcePathFromFile) {
  ProjectSettings projectSettings;
  projectSettings.load(FilePath(SettingsPath));
  const auto pSourceGroupSettings = std::dynamic_pointer_cast<SourceGroupSettingsWithSourcePaths>(
      projectSettings.getAllSourceGroupSettings().front());
  const auto paths = pSourceGroupSettings->getSourcePaths();

  EXPECT_EQ(2, paths.size());
  EXPECT_EQ(paths[0].wstr(), L"src");
  EXPECT_EQ(paths[1].wstr(), L"test");
}

TEST(ProjectSettings, loadsAndSavesTypeScriptSourceGroup) {
  ProjectSettings settings;
  ASSERT_TRUE(
      settings.loadFromString("<config><version>8</version><source_groups><source_group_ts><name>TypeScript</name>"
                              "<type>TypeScript Source Group</type><status>enabled</status>"
                              "<typescript><tsconfig_path>web/tsconfig.json</tsconfig_path></typescript>"
                              "</source_group_ts></source_groups></config>"));

  const auto allSourceGroupSettings = settings.getAllSourceGroupSettings();
  ASSERT_EQ(1, allSourceGroupSettings.size());
  const auto typeScript = std::dynamic_pointer_cast<SourceGroupSettingsTypeScript>(allSourceGroupSettings.front());
  ASSERT_TRUE(typeScript);
  EXPECT_EQ(SOURCE_GROUP_TYPESCRIPT, typeScript->getType());
  EXPECT_EQ(L"web/tsconfig.json", typeScript->getTsconfigPath().wstr());

  typeScript->setTsconfigPath(FilePath(L"tsconfig.json"));
  settings.setAllSourceGroupSettings({typeScript});
  const auto saved = std::dynamic_pointer_cast<SourceGroupSettingsTypeScript>(settings.getAllSourceGroupSettings().front());
  ASSERT_TRUE(saved);
  EXPECT_EQ(L"tsconfig.json", saved->getTsconfigPath().wstr());
  EXPECT_EQ("TypeScript Source Group", sourceGroupTypeToString(saved->getType()));
}

#if BUILD_JAVA_LANGUAGE_PACKAGE
TEST(ProjectSettings, migratesVersion1JavaMavenProjectToMavenSourceGroup) {
  ProjectSettings settings;
  ASSERT_TRUE(settings.loadFromString(
      "<config><version>1</version>"
      "<language_settings><language>Java</language><standard>8</standard></language_settings>"
      "<source><maven><project_file_path>pom.xml</project_file_path>"
      "<dependencies_directory>deps</dependencies_directory><should_index_tests>1</should_index_tests></maven>"
      "</source></config>"));
  ASSERT_TRUE(settings.needMigration());

  settings.migrate();

  const auto allSourceGroupSettings = settings.getAllSourceGroupSettings();
  ASSERT_EQ(1, allSourceGroupSettings.size());
  EXPECT_EQ(SOURCE_GROUP_JAVA_MAVEN, allSourceGroupSettings.front()->getType());
  const auto maven = std::dynamic_pointer_cast<SourceGroupSettingsJavaMaven>(allSourceGroupSettings.front());
  ASSERT_TRUE(maven);
  EXPECT_EQ(L"pom.xml", maven->getMavenProjectFilePath().wstr());
  EXPECT_TRUE(maven->getShouldIndexMavenTests());
  EXPECT_EQ(LANGUAGE_JAVA, getLanguageTypeForSourceGroupType(maven->getType()));
}

TEST(ProjectSettings, migratesJavaStandardKey) {
  ProjectSettings settings;
  ASSERT_TRUE(
      settings.loadFromString("<config><version>6</version><source_groups><source_group_a>"
                              "<type>Java Source Group from Maven</type><standard>8</standard>"
                              "</source_group_a></source_groups></config>"));

  settings.migrate();

  const auto maven = std::dynamic_pointer_cast<SourceGroupSettingsJavaMaven>(settings.getAllSourceGroupSettings().front());
  ASSERT_TRUE(maven);
  EXPECT_EQ(L"8", maven->getJavaStandard());
}
#endif    // BUILD_JAVA_LANGUAGE_PACKAGE
