#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "ProjectSettings.h"
#include "ProjectSetup.h"
#include "SourceGroupSettingsJavaEmpty.h"
#include "SourceGroupSettingsJavaGradle.h"
#include "SourceGroupSettingsJavaMaven.h"
#include "SourceGroupSettingsPython.h"
#include "SourceGroupSettingsTypeScript.h"
#include "SourceGroupSettingsWithJavaStandard.h"

namespace fs = std::filesystem;

namespace {

class ProjectSetupTest : public testing::Test {
protected:
  void SetUp() override {
    const auto* info = testing::UnitTest::GetInstance()->current_test_info();
    mRoot = fs::temp_directory_path() / "sourcetrail-project-setup" / info->name() / "shop";
    fs::remove_all(mRoot);
    fs::create_directories(mRoot);
  }

  void TearDown() override {
    fs::remove_all(mRoot.parent_path());
  }

  void touch(const fs::path& relative) const {
    write(relative, "\n");
  }

  void write(const fs::path& relative, const std::string& content) const {
    fs::create_directories((mRoot / relative).parent_path());
    std::ofstream{mRoot / relative, std::ios::binary} << content;
  }

  // The Java standard of the one source group createProject writes for the folder's build.
  std::wstring createdJavaStandard() {
    auto java = std::dynamic_pointer_cast<SourceGroupSettingsWithJavaStandard>(
        onlySourceGroup(createProject(folder(), detectBuild(folder()))));
    EXPECT_TRUE(java);
    return java ? java->getJavaStandard() : L"";
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

  std::vector<std::shared_ptr<SourceGroupSettings>> sourceGroups(const FilePath& projectFile) {
    EXPECT_TRUE(mSettings.load(projectFile));
    return mSettings.getAllSourceGroupSettings();
  }

  // The project file lists its source groups by id, so they load in no particular order.
  template <typename Settings>
  static std::shared_ptr<Settings> groupOf(const std::vector<std::shared_ptr<SourceGroupSettings>>& groups) {
    for(const auto& group : groups) {
      if(auto settings = std::dynamic_pointer_cast<Settings>(group)) {
        return settings;
      }
    }
    return nullptr;
  }

  fs::path mRoot;
  ProjectSettings mSettings;
};

}    // namespace

TEST_F(ProjectSetupTest, detectsGradleFromBuildGradle) {
  touch("build.gradle");
  EXPECT_EQ(JavaBuildKind::Gradle, detectJavaBuild(folder()));
}

TEST_F(ProjectSetupTest, detectsGradleFromBuildGradleKts) {
  touch("build.gradle.kts");
  EXPECT_EQ(JavaBuildKind::Gradle, detectJavaBuild(folder()));
}

TEST_F(ProjectSetupTest, detectsGradleFromSettingsGradleOnly) {
  touch("settings.gradle");
  EXPECT_EQ(JavaBuildKind::Gradle, detectJavaBuild(folder()));
}

TEST_F(ProjectSetupTest, detectsGradleFromSettingsGradleKtsOnly) {
  touch("settings.gradle.kts");
  EXPECT_EQ(JavaBuildKind::Gradle, detectJavaBuild(folder()));
}

TEST_F(ProjectSetupTest, detectsMavenFromPom) {
  touch("pom.xml");
  EXPECT_EQ(JavaBuildKind::Maven, detectJavaBuild(folder()));
}

TEST_F(ProjectSetupTest, gradleWinsWhenBothExist) {
  touch("pom.xml");
  touch("build.gradle");
  EXPECT_EQ(JavaBuildKind::Gradle, detectJavaBuild(folder()));
}

TEST_F(ProjectSetupTest, nestedModuleBuildFilesAloneGiveNone) {
  touch("app/build.gradle");
  touch("core/pom.xml");
  touch("lib/settings.gradle.kts");
  EXPECT_EQ(JavaBuildKind::None, detectJavaBuild(folder()));
}

TEST_F(ProjectSetupTest, aDirectoryNamedLikeABuildFileGivesNone) {
  fs::create_directories(mRoot / "pom.xml");
  EXPECT_EQ(JavaBuildKind::None, detectJavaBuild(folder()));
}

TEST_F(ProjectSetupTest, detectsTypeScriptNextToGradle) {
  touch("build.gradle");
  touch("tsconfig.json");

  const DetectedBuild build = detectBuild(folder());

  EXPECT_EQ(JavaBuildKind::Gradle, build.java);
  EXPECT_TRUE(build.typeScript);
}

TEST_F(ProjectSetupTest, detectsTypeScriptOnly) {
  touch("tsconfig.json");

  const DetectedBuild build = detectBuild(folder());

  EXPECT_EQ(JavaBuildKind::None, build.java);
  EXPECT_TRUE(build.typeScript);
}

TEST_F(ProjectSetupTest, noneWhenNeitherExists) {
  touch("src/main/webapp/tsconfig.json");
  touch("tsconfig.app.json");

  const DetectedBuild build = detectBuild(folder());

  EXPECT_EQ(JavaBuildKind::None, build.java);
  EXPECT_FALSE(build.typeScript);
  EXPECT_FALSE(build.python);
}

TEST_F(ProjectSetupTest, detectsPythonFromEachProjectFileAtTheTopLevel) {
  for(const char* name : {"pyproject.toml", "setup.py", "setup.cfg", "requirements.txt", "requirements-dev.txt"}) {
    fs::remove_all(mRoot);
    touch(name);
    const DetectedBuild build = detectBuild(folder());
    EXPECT_TRUE(build.python) << name;
    EXPECT_EQ(JavaBuildKind::None, build.java) << name;
    EXPECT_FALSE(build.typeScript) << name;
  }
}

TEST_F(ProjectSetupTest, nestedPythonProjectFilesAloneGiveNoPythonNextToABuild) {
  touch("pom.xml");
  touch("tools/pyproject.toml");
  touch("tools/requirements.txt");
  touch("tools/release.py");

  EXPECT_FALSE(detectBuild(folder()).python);
}

TEST_F(ProjectSetupTest, detectsPythonFromLooseSourcesWithoutAnotherBuild) {
  touch("shop/__init__.py");
  touch("shop/models.py");

  const DetectedBuild build = detectBuild(folder());

  EXPECT_TRUE(build.python);
  EXPECT_EQ(JavaBuildKind::None, build.java);
  EXPECT_FALSE(build.typeScript);
}

// A Java or TypeScript project with a few Python scripts is no Python project; a Python project file makes it one.
TEST_F(ProjectSetupTest, loosePythonScriptsNextToABuildGiveNoPython) {
  touch("build.gradle");
  touch("scripts/release.py");
  touch("scripts/bump_version.py");
  EXPECT_FALSE(detectBuild(folder()).python);

  fs::remove_all(mRoot);
  touch("tsconfig.json");
  touch("scripts/release.py");
  EXPECT_FALSE(detectBuild(folder()).python);

  // plain Java sources with a script stay a plain Java project, as before
  fs::remove_all(mRoot);
  touch("src/Main.java");
  touch("scripts/release.py");
  EXPECT_FALSE(detectBuild(folder()).python);
}

TEST_F(ProjectSetupTest, pythonSourcesInEnvironmentsAndBuildOutputAreNoPythonProject) {
  for(const char* path : {".venv/lib/site.py",
                          "venv/lib/site.py",
                          "node_modules/x/gyp.py",
                          "build/gen.py",
                          "target/classes/gen.py",
                          ".gradle/x.py",
                          ".sourcetrail/x.py",
                          "__pycache__/x.py"}) {
    touch(path);
  }
  EXPECT_FALSE(detectBuild(folder()).python);
}

TEST_F(ProjectSetupTest, detectsPythonNextToGradleAndTypeScript) {
  touch("build.gradle");
  touch("tsconfig.json");
  touch("requirements.txt");

  const DetectedBuild build = detectBuild(folder());

  EXPECT_EQ(JavaBuildKind::Gradle, build.java);
  EXPECT_TRUE(build.typeScript);
  EXPECT_TRUE(build.python);
}

TEST_F(ProjectSetupTest, projectFileIsNamedAfterTheFolder) {
  EXPECT_EQ(FilePath{(mRoot / ".sourcetrail" / "shop.srctrlprj").wstring()}, projectFileFor(folder()));
}

TEST_F(ProjectSetupTest, projectFileIgnoresATrailingSeparator) {
  EXPECT_EQ(projectFileFor(folder()), projectFileFor(FilePath{mRoot.wstring() + L"/"}));
}

TEST_F(ProjectSetupTest, createWritesProjectIntoDotFolderWithGitignore) {
  touch("build.gradle");

  const FilePath projectFile = createProject(folder(), {.java = JavaBuildKind::Gradle});

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

TEST_F(ProjectSetupTest, createPointsGradleAtSettingsGradleWhenThereIsNoBuildGradle) {
  touch("settings.gradle.kts");

  auto gradle = std::dynamic_pointer_cast<SourceGroupSettingsJavaGradle>(
      onlySourceGroup(createProject(folder(), {.java = JavaBuildKind::Gradle})));

  ASSERT_TRUE(gradle);
  EXPECT_EQ(FilePath{(mRoot / "settings.gradle.kts").wstring()}, gradle->getGradleProjectFilePathExpandedAndAbsolute());
}

TEST_F(ProjectSetupTest, createWritesAMavenSourceGroupWithTests) {
  touch("pom.xml");

  auto maven = std::dynamic_pointer_cast<SourceGroupSettingsJavaMaven>(
      onlySourceGroup(createProject(folder(), {.java = JavaBuildKind::Maven})));

  ASSERT_TRUE(maven);
  EXPECT_TRUE(maven->getShouldIndexMavenTests());
  EXPECT_EQ(FilePath{(mRoot / "pom.xml").wstring()}, maven->getMavenProjectFilePathExpandedAndAbsolute());
}

TEST_F(ProjectSetupTest, createWithoutABuildWritesAPlainJavaSourceGroupOverTheFolder) {
  touch("src/Main.java");

  auto plain = std::dynamic_pointer_cast<SourceGroupSettingsJavaEmpty>(
      onlySourceGroup(createProject(folder(), {.java = JavaBuildKind::None})));

  ASSERT_TRUE(plain);
  ASSERT_EQ(1U, plain->getSourcePathsExpandedAndAbsolute().size());
  EXPECT_EQ(folder(), plain->getSourcePathsExpandedAndAbsolute().front());
  EXPECT_TRUE(plain->getClasspath().empty());
}

TEST_F(ProjectSetupTest, javaStandardComesFromTheGradleToolchain) {
  write("build.gradle.kts",
        "java {\n"
        "    toolchain {\n"
        "        languageVersion = JavaLanguageVersion.of(25)\n"
        "    }\n"
        "}\n");

  EXPECT_EQ(L"25", createdJavaStandard());
}

TEST_F(ProjectSetupTest, javaStandardComesFromGradleSourceCompatibility) {
  write("build.gradle", "plugins { id 'java' }\nsourceCompatibility = '1.8'\ntargetCompatibility = '11'\n");

  EXPECT_EQ(L"8", createdJavaStandard());
}

TEST_F(ProjectSetupTest, javaStandardComesFromGradleJavaVersion) {
  write("build.gradle.kts", "java {\n    targetCompatibility = JavaVersion.VERSION_17\n}\n");

  EXPECT_EQ(L"17", createdJavaStandard());
}

TEST_F(ProjectSetupTest, javaStandardComesFromAnOldStyleGradleJavaVersion) {
  write("build.gradle", "java {\n    sourceCompatibility JavaVersion.VERSION_1_7\n}\n");

  EXPECT_EQ(L"7", createdJavaStandard());
}

TEST_F(ProjectSetupTest, javaStandardIgnoresCommentedOutGradleLines) {
  write("build.gradle",
        "// sourceCompatibility = '11'\n"
        "/* java { toolchain { languageVersion = JavaLanguageVersion.of(17) } } */\n"
        "java { toolchain { languageVersion = JavaLanguageVersion.of(\"22\") } }\n");

  EXPECT_EQ(L"22", createdJavaStandard());
}

TEST_F(ProjectSetupTest, javaStandardIsClampedToTheNewestSupportedOne) {
  write("build.gradle", "java { toolchain { languageVersion = JavaLanguageVersion.of(27) } }\n");

  EXPECT_EQ(L"25", createdJavaStandard());
}

TEST_F(ProjectSetupTest, javaStandardDefaultsTo21WithoutAGradleVersion) {
  write("build.gradle", "plugins { id 'java' }\n");

  EXPECT_EQ(L"21", createdJavaStandard());
}

TEST_F(ProjectSetupTest, javaStandardIsNotReadFromSettingsGradle) {
  write("settings.gradle", "// JavaLanguageVersion.of(17)\nrootProject.name = 'shop'\n");

  EXPECT_EQ(L"21", createdJavaStandard());
}

TEST_F(ProjectSetupTest, javaStandardComesFromMavenCompilerRelease) {
  write("pom.xml",
        "<project><properties>\n"
        "  <java.version>11</java.version>\n"
        "  <maven.compiler.release>17</maven.compiler.release>\n"
        "</properties></project>\n");

  EXPECT_EQ(L"17", createdJavaStandard());
}

TEST_F(ProjectSetupTest, javaStandardComesFromMavenCompilerSource) {
  write("pom.xml", "<project><properties><maven.compiler.source>1.8</maven.compiler.source></properties></project>\n");

  EXPECT_EQ(L"8", createdJavaStandard());
}

TEST_F(ProjectSetupTest, javaStandardComesFromTheSpringBootJavaVersion) {
  write("pom.xml", "<project><properties><java.version>24</java.version></properties></project>\n");

  EXPECT_EQ(L"24", createdJavaStandard());
}

TEST_F(ProjectSetupTest, javaStandardResolvesAPropertyInTheCompilerPluginRelease) {
  write("pom.xml",
        "<project>\n"
        "  <!-- <maven.compiler.release>11</maven.compiler.release> -->\n"
        "  <properties><jdk>23</jdk></properties>\n"
        "  <build><plugins><plugin>\n"
        "    <artifactId>maven-compiler-plugin</artifactId>\n"
        "    <configuration><release>${jdk}</release></configuration>\n"
        "  </plugin></plugins></build>\n"
        "</project>\n");

  EXPECT_EQ(L"23", createdJavaStandard());
}

TEST_F(ProjectSetupTest, javaStandardDefaultsTo21WithAnUnresolvedMavenProperty) {
  write("pom.xml", "<project><properties><java.version>${jdk}</java.version></properties></project>\n");

  EXPECT_EQ(L"21", createdJavaStandard());
}

TEST_F(ProjectSetupTest, javaStandardDefaultsTo21WithoutABuild) {
  touch("src/Main.java");

  EXPECT_EQ(L"21", createdJavaStandard());
}

TEST_F(ProjectSetupTest, createWritesBothGroups) {
  touch("build.gradle");
  touch("tsconfig.json");

  const auto groups = sourceGroups(createProject(folder(), detectBuild(folder())));

  ASSERT_EQ(2U, groups.size());
  auto gradle = groupOf<SourceGroupSettingsJavaGradle>(groups);
  auto typeScript = groupOf<SourceGroupSettingsTypeScript>(groups);
  ASSERT_TRUE(gradle);
  ASSERT_TRUE(typeScript);
  EXPECT_EQ(FilePath{(mRoot / "build.gradle").wstring()}, gradle->getGradleProjectFilePathExpandedAndAbsolute());
  EXPECT_EQ(FilePath{(mRoot / "tsconfig.json").wstring()}, typeScript->getTsconfigPathExpandedAndAbsolute());
  EXPECT_EQ("TypeScript", typeScript->getName());
  EXPECT_NE(gradle->getId(), typeScript->getId());
}

TEST_F(ProjectSetupTest, createTypeScriptOnlyProject) {
  touch("tsconfig.json");

  const auto groups = sourceGroups(createProject(folder(), detectBuild(folder())));

  ASSERT_EQ(1U, groups.size());
  auto typeScript = std::dynamic_pointer_cast<SourceGroupSettingsTypeScript>(groups.front());
  ASSERT_TRUE(typeScript);
  EXPECT_EQ(FilePath{(mRoot / "tsconfig.json").wstring()}, typeScript->getTsconfigPathExpandedAndAbsolute());
  // relative, so the checkout can move
  EXPECT_FALSE(typeScript->getTsconfigPath().isAbsolute());
}

TEST_F(ProjectSetupTest, createPythonOnlyProject) {
  touch("pyproject.toml");

  const auto groups = sourceGroups(createProject(folder(), detectBuild(folder())));

  ASSERT_EQ(1U, groups.size());
  auto python = std::dynamic_pointer_cast<SourceGroupSettingsPython>(groups.front());
  ASSERT_TRUE(python);
  EXPECT_EQ("Python", python->getName());
  EXPECT_EQ(folder(), python->getProjectRootExpandedAndAbsolute());
  // relative, so the checkout can move
  EXPECT_FALSE(python->getProjectRoot().isAbsolute());
  EXPECT_FALSE(python->getProjectRoot().empty());
  // the indexer finds the environment itself
  EXPECT_TRUE(python->getInterpreterPath().empty());
}

TEST_F(ProjectSetupTest, createWritesJavaTypeScriptAndPythonGroups) {
  touch("pom.xml");
  touch("tsconfig.json");
  touch("setup.py");

  const auto groups = sourceGroups(createProject(folder(), detectBuild(folder())));

  ASSERT_EQ(3U, groups.size());
  EXPECT_TRUE(groupOf<SourceGroupSettingsJavaMaven>(groups));
  EXPECT_TRUE(groupOf<SourceGroupSettingsTypeScript>(groups));
  auto python = groupOf<SourceGroupSettingsPython>(groups);
  ASSERT_TRUE(python);
  EXPECT_EQ(folder(), python->getProjectRootExpandedAndAbsolute());
}

TEST_F(ProjectSetupTest, addTypeScriptGroupKeepsTheJavaGroup) {
  touch("pom.xml");
  const FilePath projectFile = createProject(folder(), {.java = JavaBuildKind::Maven});
  touch("tsconfig.json");

  ASSERT_TRUE(addTypeScriptGroup(projectFile, folder()));

  const auto groups = sourceGroups(projectFile);
  ASSERT_EQ(2U, groups.size());
  EXPECT_TRUE(groupOf<SourceGroupSettingsJavaMaven>(groups));
  auto typeScript = groupOf<SourceGroupSettingsTypeScript>(groups);
  ASSERT_TRUE(typeScript);
  EXPECT_EQ(FilePath{(mRoot / "tsconfig.json").wstring()}, typeScript->getTsconfigPathExpandedAndAbsolute());
}

TEST_F(ProjectSetupTest, addTypeScriptGroupFailsWithoutAProject) {
  touch("tsconfig.json");
  EXPECT_FALSE(addTypeScriptGroup(projectFileFor(folder()), folder()));
  EXPECT_FALSE(fs::exists(mRoot / ".sourcetrail"));
}

TEST_F(ProjectSetupTest, addPythonGroupKeepsTheOtherGroups) {
  touch("build.gradle");
  touch("tsconfig.json");
  const FilePath projectFile = createProject(folder(), detectBuild(folder()));
  touch("requirements.txt");

  ASSERT_TRUE(addPythonGroup(projectFile, folder()));

  const auto groups = sourceGroups(projectFile);
  ASSERT_EQ(3U, groups.size());
  EXPECT_TRUE(groupOf<SourceGroupSettingsJavaGradle>(groups));
  EXPECT_TRUE(groupOf<SourceGroupSettingsTypeScript>(groups));
  auto python = groupOf<SourceGroupSettingsPython>(groups);
  ASSERT_TRUE(python);
  EXPECT_EQ(folder(), python->getProjectRootExpandedAndAbsolute());
}

TEST_F(ProjectSetupTest, addPythonGroupFailsWithoutAProject) {
  touch("pyproject.toml");
  EXPECT_FALSE(addPythonGroup(projectFileFor(folder()), folder()));
  EXPECT_FALSE(fs::exists(mRoot / ".sourcetrail"));
}

TEST_F(ProjectSetupTest, pythonRootIsTheFolderAProjectFolderNamedSourcetrailSitsIn) {
  EXPECT_EQ(FilePath{L".."}, pythonRootFor(FilePath{(mRoot / ".sourcetrail").wstring()}));
  EXPECT_EQ(FilePath{L".."}, pythonRootFor(FilePath{(mRoot / ".sourcetrail").wstring() + L"/"}));
  // elsewhere the project folder itself
  EXPECT_TRUE(pythonRootFor(folder()).empty());
  EXPECT_TRUE(pythonRootFor(FilePath{}).empty());
}

TEST_F(ProjectSetupTest, rootTsconfigIsTheOneNextToTheProjectFolder) {
  EXPECT_TRUE(rootTsconfigFor(FilePath{(mRoot / ".sourcetrail").wstring()}).empty());

  touch("tsconfig.json");
  EXPECT_EQ(FilePath{(mRoot / "tsconfig.json").wstring()}, rootTsconfigFor(FilePath{(mRoot / ".sourcetrail").wstring()}));
  EXPECT_EQ(FilePath{(mRoot / "tsconfig.json").wstring()}, rootTsconfigFor(folder()));
  EXPECT_TRUE(rootTsconfigFor(FilePath{(mRoot / "elsewhere").wstring()}).empty());
}

TEST_F(ProjectSetupTest, rootTsconfigNeedsAProjectLocation) {
  touch("tsconfig.json");
  const fs::path workingDirectory = fs::current_path();
  fs::current_path(mRoot);

  const FilePath tsconfig = rootTsconfigFor(FilePath{});

  fs::current_path(workingDirectory);
  EXPECT_TRUE(tsconfig.empty());
}

TEST_F(ProjectSetupTest, existingProjectIsFound) {
  touch("pom.xml");
  EXPECT_TRUE(existingProjectFile(folder()).empty());

  const FilePath projectFile = createProject(folder(), {.java = JavaBuildKind::Maven});

  ASSERT_FALSE(projectFile.empty());
  EXPECT_EQ(projectFile, existingProjectFile(folder()));
}

TEST_F(ProjectSetupTest, existingProjectWithAnotherNameIsFound) {
  touch(".sourcetrail/renamed.srctrlprj");
  touch(".sourcetrail/shop.srctrldb");

  EXPECT_EQ(FilePath{(mRoot / ".sourcetrail" / "renamed.srctrlprj").wstring()}, existingProjectFile(folder()));
}

TEST_F(ProjectSetupTest, openOrCreateOpensTheExistingProject) {
  touch("build.gradle");
  const FilePath projectFile = createProject(folder(), {.java = JavaBuildKind::Gradle});

  const ProjectChoice choice = openOrCreateProject(folder(), [] { return true; });

  EXPECT_EQ(projectFile, choice.projectFile);
  EXPECT_FALSE(choice.created);
}

TEST_F(ProjectSetupTest, openOrCreateOffersTypeScriptForAnExistingProjectWithoutIt) {
  touch("build.gradle");
  const FilePath projectFile = createProject(folder(), {.java = JavaBuildKind::Gradle});
  EXPECT_FALSE(openOrCreateProject(folder(), [] { return true; }).canAddTypeScript);
  touch("tsconfig.json");

  const ProjectChoice choice = openOrCreateProject(folder(), [] { return true; });

  EXPECT_EQ(projectFile, choice.projectFile);
  EXPECT_FALSE(choice.created);
  EXPECT_TRUE(choice.canAddTypeScript);

  ASSERT_TRUE(addTypeScriptGroup(projectFile, folder()));
  EXPECT_FALSE(openOrCreateProject(folder(), [] { return true; }).canAddTypeScript);
}

TEST_F(ProjectSetupTest, openOrCreateOffersPythonForAnExistingProjectWithoutIt) {
  touch("build.gradle");
  const FilePath projectFile = createProject(folder(), {.java = JavaBuildKind::Gradle});
  touch("scripts/release.py");
  EXPECT_FALSE(openOrCreateProject(folder(), [] { return true; }).canAddPython);
  touch("pyproject.toml");

  const ProjectChoice choice = openOrCreateProject(folder(), [] { return true; });

  EXPECT_EQ(projectFile, choice.projectFile);
  EXPECT_FALSE(choice.created);
  EXPECT_TRUE(choice.canAddPython);
  EXPECT_FALSE(choice.canAddTypeScript);

  ASSERT_TRUE(addPythonGroup(projectFile, folder()));
  EXPECT_FALSE(openOrCreateProject(folder(), [] { return true; }).canAddPython);
}

TEST_F(ProjectSetupTest, openOrCreateCreatesAPythonOnlyProjectWithoutAsking) {
  touch("app.py");
  bool asked = false;

  const ProjectChoice choice = openOrCreateProject(folder(), [&asked] { return asked = true; });

  EXPECT_FALSE(asked);
  EXPECT_TRUE(choice.created);
  EXPECT_FALSE(choice.needsSetup);
  EXPECT_FALSE(choice.canAddPython);
  EXPECT_TRUE(choice.build.python);
  EXPECT_TRUE(std::dynamic_pointer_cast<SourceGroupSettingsPython>(onlySourceGroup(choice.projectFile)));
}

TEST_F(ProjectSetupTest, openOrCreateCreatesATypeScriptOnlyProjectWithoutAsking) {
  touch("tsconfig.json");
  bool asked = false;

  const ProjectChoice choice = openOrCreateProject(folder(), [&asked] { return asked = true; });

  EXPECT_FALSE(asked);
  EXPECT_TRUE(choice.created);
  EXPECT_FALSE(choice.needsSetup);
  EXPECT_FALSE(choice.canAddTypeScript);
  EXPECT_TRUE(choice.build.typeScript);
  EXPECT_TRUE(std::dynamic_pointer_cast<SourceGroupSettingsTypeScript>(onlySourceGroup(choice.projectFile)));
}

TEST_F(ProjectSetupTest, openOrCreateCreatesAProjectForTheDetectedBuild) {
  touch("pom.xml");
  bool asked = false;

  const ProjectChoice choice = openOrCreateProject(folder(), [&asked] { return asked = true; });

  EXPECT_FALSE(asked);
  EXPECT_TRUE(choice.created);
  EXPECT_FALSE(choice.needsSetup);
  EXPECT_EQ(JavaBuildKind::Maven, choice.build.java);
  EXPECT_EQ(projectFileFor(folder()), choice.projectFile);
  EXPECT_TRUE(std::dynamic_pointer_cast<SourceGroupSettingsJavaMaven>(onlySourceGroup(choice.projectFile)));
}

TEST_F(ProjectSetupTest, openOrCreateWithoutABuildLeavesThePlainJavaProjectToTheEditorWhenAccepted) {
  const ProjectChoice choice = openOrCreateProject(folder(), [] { return true; });

  EXPECT_EQ(projectFileFor(folder()), choice.projectFile);
  EXPECT_FALSE(choice.created);
  EXPECT_TRUE(choice.needsSetup);
  EXPECT_EQ(JavaBuildKind::None, choice.build.java);
  EXPECT_FALSE(fs::exists(mRoot / ".sourcetrail"));
}

TEST_F(ProjectSetupTest, plainJavaSettingsAreWrittenOnlyWhenSaved) {
  const auto settings = projectSettingsFor(folder(), {});

  ASSERT_TRUE(settings);
  EXPECT_EQ(projectFileFor(folder()), settings->getFilePath());
  EXPECT_FALSE(fs::exists(mRoot / ".sourcetrail"));
  // the settings page checks the source paths before anything is written
  auto unsaved = std::dynamic_pointer_cast<SourceGroupSettingsJavaEmpty>(settings->getAllSourceGroupSettings().front());
  ASSERT_TRUE(unsaved);
  EXPECT_TRUE(unsaved->getSourcePathsExpandedAndAbsolute().front().exists());

  ASSERT_TRUE(prepareProjectFolder(folder()));
  std::ifstream gitignore{mRoot / ".sourcetrail" / ".gitignore", std::ios::binary};
  std::stringstream content;
  content << gitignore.rdbuf();
  EXPECT_EQ("*\n", content.str());

  ASSERT_TRUE(settings->save(settings->getFilePath()));
  auto plain = std::dynamic_pointer_cast<SourceGroupSettingsJavaEmpty>(onlySourceGroup(projectFileFor(folder())));
  ASSERT_TRUE(plain);
  EXPECT_EQ(folder(), plain->getSourcePathsExpandedAndAbsolute().front());
}

TEST_F(ProjectSetupTest, openOrCreateWithoutABuildWritesNothingWhenDeclined) {
  const ProjectChoice choice = openOrCreateProject(folder(), [] { return false; });

  EXPECT_TRUE(choice.projectFile.empty());
  EXPECT_FALSE(choice.created);
  EXPECT_FALSE(fs::exists(mRoot / ".sourcetrail"));
}
