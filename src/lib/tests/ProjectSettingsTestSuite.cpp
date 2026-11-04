#include <gtest/gtest.h>

#include "language_packages.h"
#include "ProjectSettings.h"
#include "SourceGroupSettings.h"
#include "SourceGroupSettingsTypeScript.h"
#include "SourceGroupSettingsWithSourcePaths.h"
#if BUILD_JAVA_LANGUAGE_PACKAGE
#  include "SourceGroupSettingsJavaGradle.h"
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
  case LANGUAGE_TYPESCRIPT:
    *ostream << "LANGUAGE_TYPESCRIPT";
    break;
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

TEST(ProjectSettings, onlyTheFirstEnabledTypeScriptGroupIsSupported) {
  ProjectSettings settings;
  ASSERT_TRUE(settings.loadFromString(
      "<config><version>8</version><source_groups>"
      "<source_group_a><name>Old</name><type>TypeScript Source Group</type><status>disabled</status>"
      "<typescript><tsconfig_path>old/tsconfig.json</tsconfig_path></typescript></source_group_a>"
      "<source_group_b><name>Web</name><type>TypeScript Source Group</type><status>enabled</status>"
      "<typescript><tsconfig_path>web/tsconfig.json</tsconfig_path></typescript></source_group_b>"
      "<source_group_c><name>Admin</name><type>TypeScript Source Group</type><status>enabled</status>"
      "<typescript><tsconfig_path>admin/tsconfig.json</tsconfig_path></typescript></source_group_c>"
      "</source_groups></config>"));

  auto groups = settings.getAllSourceGroupSettings();
  const auto unsupported = SourceGroupSettingsTypeScript::getUnsupportedGroups(groups);
  ASSERT_EQ(1, unsupported.size());
  EXPECT_EQ("Admin", unsupported.front()->getName());
  EXPECT_EQ(L"TypeScript: only one TypeScript source group per project is supported; skipping Admin",
            SourceGroupSettingsTypeScript::getUnsupportedGroupStatus(*unsupported.front()));

  // a disabled one is no second group, so a single enabled one is fine
  groups.pop_back();
  EXPECT_TRUE(SourceGroupSettingsTypeScript::getUnsupportedGroups(groups).empty());
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

TEST(ProjectSettings, keepsJava25Standard) {
  ProjectSettings settings;
  ASSERT_TRUE(
      settings.loadFromString("<config><version>8</version><source_groups><source_group_a>"
                              "<type>Java Source Group from Gradle</type><java_standard>25</java_standard>"
                              "</source_group_a></source_groups></config>"));

  settings.migrate();

  const auto gradle = std::dynamic_pointer_cast<SourceGroupSettingsJavaGradle>(settings.getAllSourceGroupSettings().front());
  ASSERT_TRUE(gradle);
  EXPECT_EQ(L"25", gradle->getJavaStandard());
  const std::vector<std::wstring> standards = gradle->getAvailableJavaStandards();
  ASSERT_GE(standards.size(), 5U);
  EXPECT_EQ((std::vector<std::wstring>{L"25", L"24", L"23", L"22", L"21"}),
            std::vector<std::wstring>(standards.begin(), standards.begin() + 5));
}
#endif    // BUILD_JAVA_LANGUAGE_PACKAGE
