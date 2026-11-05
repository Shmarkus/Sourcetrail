#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "language_packages.h"
#include "ProjectSettings.h"
#include "SourceGroupSettings.h"
#include "SourceGroupSettingsPython.h"
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
  case LANGUAGE_PYTHON:
    *ostream << "LANGUAGE_PYTHON";
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

TEST(ProjectSettings, loadsAndSavesPythonSourceGroup) {
  ProjectSettings settings;
  ASSERT_TRUE(settings.loadFromString(
      "<config><version>8</version><source_groups><source_group_py><name>Python</name>"
      "<type>Python Source Group</type><status>enabled</status>"
      "<python><project_root>backend</project_root><interpreter_path>.venv/bin/python</interpreter_path></python>"
      "</source_group_py></source_groups></config>"));

  const auto allSourceGroupSettings = settings.getAllSourceGroupSettings();
  ASSERT_EQ(1, allSourceGroupSettings.size());
  const auto python = std::dynamic_pointer_cast<SourceGroupSettingsPython>(allSourceGroupSettings.front());
  ASSERT_TRUE(python);
  EXPECT_EQ(SOURCE_GROUP_PYTHON, python->getType());
  EXPECT_EQ(LANGUAGE_PYTHON, getLanguageTypeForSourceGroupType(python->getType()));
  EXPECT_EQ(L"backend", python->getProjectRoot().wstr());
  EXPECT_EQ(L".venv/bin/python", python->getInterpreterPath().wstr());

  python->setProjectRoot(FilePath(L"."));
  python->setInterpreterPath(FilePath());
  settings.setAllSourceGroupSettings({python});
  const auto saved = std::dynamic_pointer_cast<SourceGroupSettingsPython>(settings.getAllSourceGroupSettings().front());
  ASSERT_TRUE(saved);
  EXPECT_EQ(L".", saved->getProjectRoot().wstr());
  EXPECT_TRUE(saved->getInterpreterPath().empty());
  EXPECT_EQ("Python Source Group", sourceGroupTypeToString(saved->getType()));

  const auto copy = std::dynamic_pointer_cast<SourceGroupSettingsPython>(saved->createCopy());
  EXPECT_TRUE(copy->equalsSettings(saved.get()));
  copy->setInterpreterPath(FilePath(L"python3.12"));
  EXPECT_FALSE(copy->equalsSettings(saved.get()));
}

TEST(ProjectSettings, pythonProjectRootDefaultsToTheProjectFolder) {
  ProjectSettings settings;
  ASSERT_TRUE(settings.loadFromString(
      "<config><version>8</version><source_groups><source_group_py><name>Python</name>"
      "<type>Python Source Group</type><status>enabled</status></source_group_py></source_groups></config>"));
  const auto python = std::dynamic_pointer_cast<SourceGroupSettingsPython>(settings.getAllSourceGroupSettings().front());
  ASSERT_TRUE(python);
  EXPECT_EQ(settings.getProjectDirectoryPath().wstr(), python->getProjectRootExpandedAndAbsolute().wstr());
  EXPECT_TRUE(python->getInterpreterPathExpandedAndAbsolute().empty());
}

TEST(ProjectSettings, onlyTheFirstEnabledPythonGroupIsSupported) {
  ProjectSettings settings;
  ASSERT_TRUE(settings.loadFromString(
      "<config><version>8</version><source_groups>"
      "<source_group_a><name>Web</name><type>TypeScript Source Group</type><status>enabled</status>"
      "<typescript><tsconfig_path>web/tsconfig.json</tsconfig_path></typescript></source_group_a>"
      "<source_group_b><name>Old</name><type>Python Source Group</type><status>disabled</status>"
      "<python><project_root>old</project_root></python></source_group_b>"
      "<source_group_c><name>Backend</name><type>Python Source Group</type><status>enabled</status>"
      "<python><project_root>backend</project_root></python></source_group_c>"
      "<source_group_d><name>Tools</name><type>Python Source Group</type><status>enabled</status>"
      "<python><project_root>tools</project_root></python></source_group_d>"
      "</source_groups></config>"));

  auto groups = settings.getAllSourceGroupSettings();
  // a TypeScript group next to a Python one is fine
  EXPECT_TRUE(SourceGroupSettingsTypeScript::getUnsupportedGroups(groups).empty());
  const auto unsupported = SourceGroupSettingsPython::getUnsupportedGroups(groups);
  ASSERT_EQ(1, unsupported.size());
  EXPECT_EQ("Tools", unsupported.front()->getName());
  EXPECT_EQ(L"Python: only one Python source group per project is supported; skipping Tools",
            SourceGroupSettingsPython::getUnsupportedGroupStatus(*unsupported.front()));

  groups.pop_back();
  EXPECT_TRUE(SourceGroupSettingsPython::getUnsupportedGroups(groups).empty());
}

TEST(ProjectSettings, pythonTriggerFilesAndSkippedFolders) {
  for(const wchar_t* name : {L"a/b.py", L"pyproject.toml", L"setup.py", L"setup.cfg", L"requirements.txt",
                             L"requirements-dev.txt", L"poetry.lock", L"uv.lock", L"Pipfile.lock"}) {
    EXPECT_TRUE(SourceGroupSettingsPython::isTriggerFile(FilePath(name))) << FilePath(name).str();
  }
  for(const wchar_t* name : {L"a/b.pyc", L"a/b.pyi", L"notes.txt", L"Pipfile", L"package.json", L"a/b.ts"}) {
    EXPECT_FALSE(SourceGroupSettingsPython::isTriggerFile(FilePath(name))) << FilePath(name).str();
  }
  // Gradle, Maven and IDE output too, so that a Python part next to a Java build leaves its generated files alone
  for(const wchar_t* name : {L"node_modules", L".sourcetrail", L"build", L"dist", L".git", L".venv", L"venv",
                             L"__pycache__", L".tox", L".mypy_cache", L".gradle", L"target", L"out", L".idea"}) {
    EXPECT_TRUE(SourceGroupSettingsPython::isUnwalkedDirectory(name)) << FilePath(name).str();
  }
  for(const wchar_t* name : {L"src", L"tests", L"shop", L"output", L"targets"}) {
    EXPECT_FALSE(SourceGroupSettingsPython::isUnwalkedDirectory(name)) << FilePath(name).str();
  }
}

namespace {
// A project file in a fresh folder below the temporary directory, loaded; the folder holds `paths` (a trailing / makes
// a folder, anything else an empty file).
class PythonProjectOnDisk {
public:
  PythonProjectOnDisk(const std::string& group, const std::vector<std::string>& paths) {
    const auto* info = testing::UnitTest::GetInstance()->current_test_info();
    mRoot = std::filesystem::temp_directory_path() / "sourcetrail-project-settings" / info->name();
    std::filesystem::remove_all(mRoot);
    std::filesystem::create_directories(mRoot);
    for(const std::string& path : paths) {
      std::filesystem::create_directories((mRoot / path).parent_path());
      if(!path.ends_with('/')) {
        std::ofstream{mRoot / path} << "\n";
      }
    }
    std::ofstream{mRoot / "old.srctrlprj"} << "<config><version>8</version><source_groups>" << group << "</source_groups></config>";
    EXPECT_TRUE(mSettings.load(FilePath{(mRoot / "old.srctrlprj").wstring()}));
  }

  ~PythonProjectOnDisk() {
    std::filesystem::remove_all(mRoot.parent_path());
  }

  std::shared_ptr<SourceGroupSettingsPython> python() const {
    const auto groups = mSettings.getAllSourceGroupSettings();
    EXPECT_EQ(1U, groups.size());
    return groups.empty() ? nullptr : std::dynamic_pointer_cast<SourceGroupSettingsPython>(groups.front());
  }

  FilePath path(const std::string& relative) const {
    return FilePath{(mRoot / relative).wstring()};
  }

private:
  std::filesystem::path mRoot;
  ProjectSettings mSettings;
};

std::string upstreamPythonGroup(const std::string& sourcePaths, const std::string& environment) {
  return "<source_group_py><name>Python</name><type>Python Source Group</type><status>enabled</status>"
         "<source_paths>" +
      sourcePaths + "</source_paths><python_environment_path>" + environment +
      "</python_environment_path><source_extensions><source_extension>.py</source_extension></source_extensions>"
      "<exclude_filters><exclude_filter>**/migrations/**</exclude_filter></exclude_filters></source_group_py>";
}
}    // namespace

// Upstream Sourcetrail's Python source group named its source paths and an environment (a folder or an interpreter):
// its project root becomes the folder holding the source paths, its interpreter the environment's.
TEST(ProjectSettings, migratesAnUpstreamPythonSourceGroup) {
  const PythonProjectOnDisk project(
      upstreamPythonGroup("<source_path>backend/app</source_path><source_path>backend/tools</source_path>", "backend/.venv"),
      {"backend/app/", "backend/tools/", "backend/.venv/bin/python"});

  const auto python = project.python();
  ASSERT_TRUE(python);
  EXPECT_EQ(L"backend", python->getProjectRoot().wstr());
  EXPECT_EQ(project.path("backend"), python->getProjectRootExpandedAndAbsolute());
  EXPECT_EQ(L"backend/.venv/bin/python", python->getInterpreterPath().wstr());
  EXPECT_EQ(project.path("backend/.venv/bin/python"), python->getInterpreterPathExpandedAndAbsolute());
}

// A source path that is a package (it has an __init__.py) lies inside the project: its modules are named from the
// folder holding it.
TEST(ProjectSettings, migratesAnUpstreamPythonPackageSourcePathToTheFolderHoldingIt) {
  const PythonProjectOnDisk project(
      upstreamPythonGroup("<source_path>shop</source_path>", "/usr/bin/python3"), {"shop/__init__.py", "shop/models.py"});

  const auto python = project.python();
  ASSERT_TRUE(python);
  // the project folder
  EXPECT_TRUE(python->getProjectRoot().empty());
  // an interpreter stays the interpreter
  EXPECT_EQ(L"/usr/bin/python3", python->getInterpreterPath().wstr());
}

TEST(ProjectSettings, migratesAnUpstreamPythonSourceGroupWithoutAnEnvironment) {
  const PythonProjectOnDisk project(upstreamPythonGroup("<source_path>src</source_path>", ""), {"src/app.py"});

  const auto python = project.python();
  ASSERT_TRUE(python);
  EXPECT_EQ(L"src", python->getProjectRoot().wstr());
  EXPECT_TRUE(python->getInterpreterPath().empty());
}

// Once saved, the group is a Python source group of this fork, whatever upstream keys the file still holds.
TEST(ProjectSettings, aPythonProjectRootWinsOverUpstreamKeys) {
  const PythonProjectOnDisk project(
      "<source_group_py><name>Python</name><type>Python Source Group</type><status>enabled</status>"
      "<python><project_root></project_root></python>"
      "<source_paths><source_path>backend</source_path></source_paths>"
      "<python_environment_path>backend/.venv</python_environment_path></source_group_py>",
      {"backend/"});

  const auto python = project.python();
  ASSERT_TRUE(python);
  EXPECT_TRUE(python->getProjectRoot().empty());
  EXPECT_TRUE(python->getInterpreterPath().empty());
}

// What the wizard shows while the interpreter is left to the indexer.
TEST(ProjectSettings, findsTheEnvironmentInterpreterOfAPythonRoot) {
  const PythonProjectOnDisk project(
      "<source_group_py><name>Python</name><type>Python Source Group</type><status>enabled</status>"
      "<python><project_root>app</project_root></python></source_group_py>",
      {"app/shop/__init__.py", "plain/x.py", "both/.venv/bin/python", "both/venv/bin/python", "venv/venv/bin/python"});

  EXPECT_EQ(project.path("both/.venv/bin/python"), SourceGroupSettingsPython::findEnvironmentInterpreter(project.path("both")));
  EXPECT_EQ(project.path("venv/venv/bin/python"), SourceGroupSettingsPython::findEnvironmentInterpreter(project.path("venv")));
  EXPECT_TRUE(SourceGroupSettingsPython::findEnvironmentInterpreter(project.path("plain")).empty());
  EXPECT_TRUE(SourceGroupSettingsPython::findEnvironmentInterpreter(project.python()->getProjectRootExpandedAndAbsolute()).empty());
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
