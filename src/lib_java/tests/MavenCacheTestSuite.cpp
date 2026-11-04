#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <QByteArray>

#include <gtest/gtest.h>

#include "FilePath.h"
#include "FileSystem.h"
#include "utilityMaven.h"
#include "utilityMavenCache.h"
#include "utilityString.h"

namespace {
const FilePath s_root = FilePath(L"data/MavenCacheTestSuite/tmp").makeAbsolute();

// Stands in for mvn: logs each goal it is asked to run and its arguments, and writes what the real goal would.
// A file fail-<goal> next to it makes that goal fail: "error" prints an [ERROR] line, "exit" exits with 3.
const char* const FAKE_MAVEN = R"sh(#!/bin/sh
bin="$(dirname "$0")"
for argument in "$@"; do
  case "$argument" in
    generate-sources|dependency:copy-dependencies|help:effective-pom)
      goal="$argument"
      echo "$argument" >> "$bin/invocations.log" ;;
    -DoutputDirectory=*) dependencies="${argument#-DoutputDirectory=}" ;;
    -Doutput=*) effectivePom="${argument#-Doutput=}" ;;
  esac
done
printf '%s\n' "$@" > "$bin/argv-$goal.log"
if [ -f "$bin/fail-$goal" ]; then
  case "$(cat "$bin/fail-$goal")" in
    error) echo "[ERROR] Failed to execute goal $goal on project app: boom"; echo "[ERROR] more"; exit 0 ;;
    exit) echo "[INFO] BUILD FAILURE"; exit 3 ;;
  esac
fi
if [ "$goal" = generate-sources ]; then
  mkdir -p "$PWD/target/generated-sources/annotations"
fi
if [ -n "$dependencies" ]; then
  mkdir -p "$dependencies"
  echo jar > "$dependencies/library-1.0.jar"
fi
if [ -n "$effectivePom" ]; then
  mkdir -p "$(dirname "$effectivePom")"
  cat > "$effectivePom" << EOF
<projects>
  <project><build>
    <sourceDirectory>$PWD/src/main/java</sourceDirectory>
    <testSourceDirectory>$PWD/src/test/java</testSourceDirectory>
    <directory>$PWD/target</directory>
  </build></project>
  <project><build>
    <sourceDirectory>$PWD/module/src/main/java</sourceDirectory>
    <directory>$PWD/module/target</directory>
  </build></project>
</projects>
EOF
fi
echo "[INFO] BUILD SUCCESS"
)sh";

struct MavenCacheFixture {
  MavenCacheFixture()
      : project(s_root.getConcatenated(L"project"))
      , output(s_root.getConcatenated(L"output"))
      , home(s_root.getConcatenated(L"home"))
      , maven(s_root.getConcatenated(L"bin/mvn")) {
    previousHome = qgetenv("HOME");
    std::filesystem::remove_all(s_root.str());
    FileSystem::createDirectory(home);
    qputenv("HOME", QByteArray(home.str().c_str()));
    FileSystem::createDirectory(project);
    write("pom.xml", "<project/>");
    write("module/pom.xml", "<project/>");
    write("src/main/java/Main.java", "class Main {}");
    write("module/src/main/java/Module.java", "class Module {}");
    write(".mvn/maven.config", "-Pdefault");
    writeAt(home.getConcatenated(L".m2/settings.xml"), "<settings/>");

    writeAt(maven, FAKE_MAVEN);
    std::filesystem::permissions(maven.str(), std::filesystem::perms::owner_all);
  }

  ~MavenCacheFixture() {
    qputenv("HOME", previousHome);
    std::filesystem::remove_all(s_root.str());
  }

  static void writeAt(const FilePath& file, const std::string& content) {
    const std::filesystem::path path(file.str());
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << content;
  }

  void write(const std::string& relativePath, const std::string& content) const {
    writeAt(project.getConcatenated(utility::decodeFromUtf8(relativePath)), content);
  }

  static void bumpMtime(const FilePath& file) {
    const std::filesystem::path path(file.str());
    std::filesystem::last_write_time(path, std::filesystem::last_write_time(path) + std::chrono::seconds(10));
  }

  void bump(const std::string& relativePath) const {
    bumpMtime(project.getConcatenated(utility::decodeFromUtf8(relativePath)));
  }

  std::string fingerprint(bool includesTests = false, const FilePath& settings = FilePath()) const {
    return utility::mavenBuildFingerprint(maven, settings, project, includesTests);
  }

  std::vector<std::string> invocations() const {
    std::vector<std::string> goals;
    std::ifstream stream(s_root.getConcatenated(L"bin/invocations.log").str());
    for(std::string line; std::getline(stream, line);) {
      goals.push_back(line);
    }
    std::filesystem::remove(s_root.getConcatenated(L"bin/invocations.log").str());
    return goals;
  }

  utility::MavenPreparation prepareWithWarnings(bool fullRefresh, bool includesTests = false) const {
    return utility::mavenPrepareProject(maven, FilePath(), project, output, includesTests, fullRefresh, [](const std::wstring&) {});
  }

  // the error message, empty on success
  std::wstring prepare(bool fullRefresh, bool includesTests = false) const {
    return prepareWithWarnings(fullRefresh, includesTests).errorMessage;
  }

  std::vector<FilePath> sourceDirectories(bool includesTests = false) const {
    std::wstring errorMessage;
    std::vector<FilePath> directories = utility::mavenGetSourceDirectories(
        maven, FilePath(), project, output, includesTests, errorMessage);
    EXPECT_TRUE(errorMessage.empty());
    return directories;
  }

  // One refresh as SourceGroupJavaMaven runs it: prepare, then ask for the source directories.
  std::vector<std::string> refresh(bool fullRefresh, bool includesTests = false) const {
    EXPECT_TRUE(prepare(fullRefresh, includesTests).empty());
    EXPECT_FALSE(sourceDirectories(includesTests).empty());
    return invocations();
  }

  void fail(const std::string& goal, const std::string& how) const {
    writeAt(s_root.getConcatenated(utility::decodeFromUtf8("bin/fail-" + goal)), how);
  }

  void stopFailing(const std::string& goal) const {
    std::filesystem::remove(s_root.getConcatenated(utility::decodeFromUtf8("bin/fail-" + goal)).str());
  }

  std::vector<std::string> argv(const std::string& goal) const {
    std::vector<std::string> arguments;
    std::ifstream stream(s_root.getConcatenated(utility::decodeFromUtf8("bin/argv-" + goal + ".log")).str());
    for(std::string line; std::getline(stream, line);) {
      arguments.push_back(line);
    }
    return arguments;
  }

  bool exportMarkerExists() const {
    return std::filesystem::exists(std::filesystem::path(output.str()) / "sourcetrail-maven-exported.cache");
  }

  FilePath project;
  FilePath output;
  FilePath home;
  FilePath maven;

private:
  QByteArray previousHome;
};

const std::vector<std::string> ALL_GOALS = {"generate-sources", "dependency:copy-dependencies", "help:effective-pom"};
}    // namespace

TEST(MavenCache, FingerprintIsStableWhileNothingChanges) {
  MavenCacheFixture f;
  ASSERT_EQ(f.fingerprint(), f.fingerprint());
}

TEST(MavenCache, FingerprintChangesWhenTheRootPomChanges) {
  MavenCacheFixture f;
  const std::string before = f.fingerprint();
  f.bump("pom.xml");
  ASSERT_NE(before, f.fingerprint());
}

TEST(MavenCache, FingerprintChangesWhenAModulePomChanges) {
  MavenCacheFixture f;
  const std::string before = f.fingerprint();
  f.write("module/pom.xml", "<project><artifactId>module</artifactId></project>");
  ASSERT_NE(before, f.fingerprint());
}

TEST(MavenCache, FingerprintChangesWhenAPomIsAdded) {
  MavenCacheFixture f;
  const std::string before = f.fingerprint();
  f.write("other/pom.xml", "<project/>");
  ASSERT_NE(before, f.fingerprint());
}

TEST(MavenCache, FingerprintIgnoresPomsUnderBuildAndToolDirectories) {
  MavenCacheFixture f;
  const std::string before = f.fingerprint();
  f.write("target/pom.xml", "<project/>");
  f.write("module/target/classes/META-INF/maven/pom.xml", "<project/>");
  f.write("node_modules/x/pom.xml", "<project/>");
  f.write(".git/pom.xml", "<project/>");
  f.write(".sourcetrail/pom.xml", "<project/>");
  ASSERT_EQ(before, f.fingerprint());
}

TEST(MavenCache, FingerprintIgnoresSourceChanges) {
  MavenCacheFixture f;
  const std::string before = f.fingerprint();
  f.write("src/main/java/Main.java", "class Main { int x; }");
  f.bump("src/main/java/Main.java");
  ASSERT_EQ(before, f.fingerprint());
}

TEST(MavenCache, FingerprintChangesWhenAFileUnderDotMvnChanges) {
  MavenCacheFixture f;
  const std::string before = f.fingerprint();
  f.bump(".mvn/maven.config");
  ASSERT_NE(before, f.fingerprint());
}

TEST(MavenCache, FingerprintChangesWhenAFileUnderDotMvnIsAdded) {
  MavenCacheFixture f;
  const std::string before = f.fingerprint();
  f.write(".mvn/wrapper/maven-wrapper.properties", "distributionUrl=x");
  ASSERT_NE(before, f.fingerprint());
}

TEST(MavenCache, FingerprintChangesWhenTheUserSettingsChange) {
  MavenCacheFixture f;
  const std::string before = f.fingerprint();
  MavenCacheFixture::bumpMtime(f.home.getConcatenated(L".m2/settings.xml"));
  ASSERT_NE(before, f.fingerprint());
}

TEST(MavenCache, FingerprintChangesWhenTheConfiguredSettingsFileChanges) {
  MavenCacheFixture f;
  const FilePath settings = s_root.getConcatenated(L"team-settings.xml");
  MavenCacheFixture::writeAt(settings, "<settings/>");
  const std::string before = f.fingerprint(false, settings);
  ASSERT_NE(before, f.fingerprint());
  MavenCacheFixture::bumpMtime(settings);
  ASSERT_NE(before, f.fingerprint(false, settings));
}

TEST(MavenCache, FingerprintChangesWithTestInclusion) {
  MavenCacheFixture f;
  ASSERT_NE(f.fingerprint(false), f.fingerprint(true));
}

TEST(MavenCache, FingerprintChangesWithTheMavenExecutable) {
  MavenCacheFixture f;
  ASSERT_NE(f.fingerprint(), utility::mavenBuildFingerprint(FilePath(L"/usr/bin/mvn"), FilePath(), f.project, false));
}

TEST(MavenCache, FirstRefreshRunsMaven) {
  MavenCacheFixture f;
  ASSERT_EQ(ALL_GOALS, f.refresh(false));
}

TEST(MavenCache, UnchangedKeySkipsMaven) {
  MavenCacheFixture f;
  f.refresh(false);
  ASSERT_TRUE(f.refresh(false).empty());
  ASSERT_TRUE(f.refresh(false).empty());
}

TEST(MavenCache, CachedSourceDirectoriesMatchMavensAnswer) {
  MavenCacheFixture f;
  FileSystem::createDirectory(f.project.getConcatenated(L"src/test/java"));
  ASSERT_TRUE(f.prepare(false, true).empty());
  const std::vector<FilePath> fromMaven = f.sourceDirectories(true);
  ASSERT_EQ(ALL_GOALS, f.invocations());
  const std::vector<FilePath> fromCache = f.sourceDirectories(true);
  ASSERT_TRUE(f.invocations().empty());
  ASSERT_EQ(fromMaven, fromCache);
  ASSERT_EQ(4u, fromCache.size());    // the two main roots, the test root and the generated sources
}

TEST(MavenCache, CachedSourceDirectoriesPickUpAGeneratedSourcesDirectoryThatAppears) {
  MavenCacheFixture f;
  f.refresh(false);
  FileSystem::createDirectory(f.project.getConcatenated(L"module/target/generated-sources"));
  const std::vector<FilePath> fromCache = f.sourceDirectories();
  ASSERT_TRUE(f.invocations().empty());
  ASSERT_EQ(4u, fromCache.size());
  ASSERT_EQ(utility::mavenGetAllDirectoriesFromEffectivePom(f.maven, FilePath(), f.project, f.output, false), fromCache);
}

TEST(MavenCache, FullRefreshAlwaysRunsMaven) {
  MavenCacheFixture f;
  f.refresh(false);
  ASSERT_EQ(ALL_GOALS, f.refresh(true));
  ASSERT_EQ(ALL_GOALS, f.refresh(true));
  ASSERT_TRUE(f.refresh(false).empty());
}

TEST(MavenCache, PomChangeRerunsMaven) {
  MavenCacheFixture f;
  f.refresh(false);
  f.bump("module/pom.xml");
  ASSERT_EQ(ALL_GOALS, f.refresh(false));
  ASSERT_TRUE(f.refresh(false).empty());
}

TEST(MavenCache, SettingsChangeRerunsMaven) {
  MavenCacheFixture f;
  f.refresh(false);
  MavenCacheFixture::bumpMtime(f.home.getConcatenated(L".m2/settings.xml"));
  ASSERT_EQ(ALL_GOALS, f.refresh(false));
}

TEST(MavenCache, TestInclusionChangeRerunsMaven) {
  MavenCacheFixture f;
  f.refresh(false, false);
  ASSERT_EQ(ALL_GOALS, f.refresh(false, true));
}

TEST(MavenCache, MissingDependencyDirectoryRerunsMaven) {
  MavenCacheFixture f;
  f.refresh(false);
  std::filesystem::remove_all(f.output.str());
  ASSERT_EQ(ALL_GOALS, f.refresh(false));
  ASSERT_TRUE(f.refresh(false).empty());
}

TEST(MavenCache, MissingDependencyJarRerunsTheDependencyCopy) {
  MavenCacheFixture f;
  f.refresh(false);
  std::filesystem::remove(f.output.getConcatenated(L"library-1.0.jar").str());
  // the source directories still come from the cache: the key did not change
  ASSERT_EQ(std::vector<std::string>(ALL_GOALS.begin(), ALL_GOALS.begin() + 2), f.refresh(false));
  ASSERT_TRUE(f.output.getConcatenated(L"library-1.0.jar").recheckExists());
}

TEST(MavenCache, CorruptCacheRerunsMaven) {
  MavenCacheFixture f;
  f.refresh(false);
  for(const auto& entry : std::filesystem::directory_iterator(f.output.str())) {
    if(entry.path().extension() == ".cache") {
      std::ofstream(entry.path(), std::ios::binary | std::ios::trunc) << "garbage\n";
    }
  }
  ASSERT_EQ(ALL_GOALS, f.refresh(false));
}

TEST(MavenCache, CacheOfAnotherFormatVersionIsStale) {
  MavenCacheFixture f;
  f.refresh(false);
  for(const auto& entry : std::filesystem::directory_iterator(f.output.str())) {
    if(entry.path().extension() == ".cache") {
      std::ifstream in(entry.path(), std::ios::binary);
      std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      in.close();
      ASSERT_EQ(0u, content.rfind("version\t2\n", 0));
      content.replace(0, std::string("version\t2").size(), "version\t1");
      std::ofstream(entry.path(), std::ios::binary | std::ios::trunc) << content;
    }
  }
  ASSERT_EQ(ALL_GOALS, f.refresh(false));
}

TEST(MavenCache, SourceDirectoriesAloneDoNotCountAsPrepared) {
  MavenCacheFixture f;
  // the setup dialog asks for the source directories without preparing the project
  ASSERT_FALSE(f.sourceDirectories().empty());
  ASSERT_EQ(std::vector<std::string>{"help:effective-pom"}, f.invocations());
  ASSERT_EQ(std::vector<std::string>(ALL_GOALS.begin(), ALL_GOALS.begin() + 2), f.refresh(false));
}

TEST(MavenCache, MavenCleanRerunsGenerateSources) {
  MavenCacheFixture f;
  f.refresh(false);
  const FilePath generated = f.project.getConcatenated(L"target/generated-sources");
  ASSERT_TRUE(generated.recheckExists());
  std::filesystem::remove_all(f.project.getConcatenated(L"target").str());
  ASSERT_EQ(std::vector<std::string>(ALL_GOALS.begin(), ALL_GOALS.begin() + 2), f.refresh(false));
  ASSERT_TRUE(generated.recheckExists());
  ASSERT_TRUE(f.refresh(false).empty());
}

TEST(MavenCache, EveryMavenRunIsInBatchMode) {
  MavenCacheFixture f;
  f.refresh(false);
  for(const std::string& goal : ALL_GOALS) {
    const std::vector<std::string> arguments = f.argv(goal);
    ASSERT_FALSE(arguments.empty()) << goal;
    ASSERT_EQ("-B", arguments.front()) << goal;
  }
}

TEST(MavenCache, AFailingGoalIsAWarningWithTheModuleAndTheFirstErrorLine) {
  for(const std::string goal : {"generate-sources", "dependency:copy-dependencies"}) {
    MavenCacheFixture f;
    f.fail(goal, "error");
    const utility::MavenPreparation preparation = f.prepareWithWarnings(false);
    ASSERT_TRUE(preparation.errorMessage.empty()) << goal;
    ASSERT_EQ(std::vector<std::wstring>{L"Maven: " + utility::decodeFromUtf8(goal) + L" failed in app: Failed to execute goal " +
                                        utility::decodeFromUtf8(goal) + L" on project app: boom; indexing with partial results"},
              preparation.warnings)
        << goal;
  }
}

TEST(MavenCache, ANonZeroExitCodeIsAWarning) {
  for(const std::string goal : {"generate-sources", "dependency:copy-dependencies"}) {
    MavenCacheFixture f;
    f.fail(goal, "exit");
    const utility::MavenPreparation preparation = f.prepareWithWarnings(false);
    ASSERT_TRUE(preparation.errorMessage.empty()) << goal;
    ASSERT_EQ(std::vector<std::wstring>{L"Maven: " + utility::decodeFromUtf8(goal) +
                                        L" failed in the project: exit code 3; indexing with partial results"},
              preparation.warnings)
        << goal;
  }
}

TEST(MavenCache, AWarningIsCachedAndShownAgainOnEveryCachedRefresh) {
  MavenCacheFixture f;
  f.fail("dependency:copy-dependencies", "error");
  const std::vector<std::wstring> warnings = f.prepareWithWarnings(false).warnings;
  ASSERT_EQ(1u, warnings.size());
  ASSERT_TRUE(f.exportMarkerExists());
  ASSERT_FALSE(f.sourceDirectories().empty());
  ASSERT_EQ(ALL_GOALS, f.invocations());
  ASSERT_EQ(warnings, f.prepareWithWarnings(false).warnings);
  ASSERT_EQ(warnings, f.prepareWithWarnings(false).warnings);
  ASSERT_TRUE(f.invocations().empty());
  ASSERT_EQ(warnings, utility::loadMavenWarnings(f.output));
}

TEST(MavenCache, AFullRefreshRunsMavenAgainAndDropsAFixedWarning) {
  MavenCacheFixture f;
  f.fail("generate-sources", "exit");
  ASSERT_EQ(1u, f.prepareWithWarnings(false).warnings.size());
  f.invocations();
  f.stopFailing("generate-sources");
  ASSERT_TRUE(f.prepareWithWarnings(true).warnings.empty());
  ASSERT_EQ(ALL_GOALS, f.invocations());
  ASSERT_TRUE(f.prepareWithWarnings(false).warnings.empty());
  ASSERT_EQ(std::vector<std::wstring>(), utility::loadMavenWarnings(f.output));
}

TEST(MavenCache, NoWarningsAreKnownBeforeMavenRan) {
  MavenCacheFixture f;
  ASSERT_FALSE(utility::loadMavenWarnings(f.output).has_value());
}

TEST(MavenCache, AFailingEffectivePomFailsThePreparationAndLeavesNoMarker) {
  for(const std::string how : {"error", "exit"}) {
    MavenCacheFixture f;
    f.fail("help:effective-pom", how);
    const std::wstring errorMessage = f.prepare(false);
    ASSERT_NE(std::wstring::npos, errorMessage.find(how == "error" ? L"on project app: boom" : L"exit code 3")) << how;
    ASSERT_FALSE(f.exportMarkerExists()) << how;
    f.invocations();
    f.stopFailing("help:effective-pom");
    ASSERT_EQ(ALL_GOALS, f.refresh(false)) << how;
  }
}

TEST(MavenCache, AFailedRunAfterACachedOneRemovesTheMarker) {
  MavenCacheFixture f;
  f.refresh(false);
  f.fail("help:effective-pom", "exit");
  ASSERT_FALSE(f.prepare(true).empty());
  ASSERT_FALSE(f.exportMarkerExists());
}

TEST(MavenCache, AMissingMavenIsAnError) {
  MavenCacheFixture f;
  MavenCacheFixture::writeAt(f.maven, "#!/bin/sh\nexit 1\n");
  ASSERT_NE(std::wstring::npos, f.prepare(false).find(L"unable to locate Maven"));
  ASSERT_FALSE(f.exportMarkerExists());
}

TEST(MavenCache, AFailedEffectivePomIsReportedForTheSourceDirectories) {
  MavenCacheFixture f;
  f.fail("help:effective-pom", "exit");
  std::wstring errorMessage;
  ASSERT_TRUE(utility::mavenGetSourceDirectories(f.maven, FilePath(), f.project, f.output, false, errorMessage).empty());
  ASSERT_NE(std::wstring::npos, errorMessage.find(L"exit code 3"));
  f.invocations();
  f.stopFailing("help:effective-pom");
  ASSERT_FALSE(f.sourceDirectories().empty());
  ASSERT_EQ(std::vector<std::string>{"help:effective-pom"}, f.invocations());
}

TEST(MavenCache, ATruncatedCacheFileIsAMiss) {
  MavenCacheFixture f;
  f.refresh(false);
  for(const auto& entry : std::filesystem::directory_iterator(f.output.str())) {
    if(entry.path().extension() == ".cache") {
      std::ifstream in(entry.path(), std::ios::binary);
      std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      in.close();
      ASSERT_EQ(content.size() - 4, content.rfind("end\n"));
      std::ofstream(entry.path(), std::ios::binary | std::ios::trunc) << content.substr(0, content.size() - 4);
    }
  }
  ASSERT_EQ(ALL_GOALS, f.refresh(false));
}

TEST(MavenCache, TheSettingsFileIsPassedAsAnOptionAndItsOwnArgument) {
  MavenCacheFixture f;
  const FilePath settings = s_root.getConcatenated(L"team settings.xml");
  MavenCacheFixture::writeAt(settings, "<settings/>");
  ASSERT_TRUE(utility::mavenPrepareProject(f.maven, settings, f.project, f.output, false, false, [](const std::wstring&) {
              }).errorMessage.empty());
  for(const std::string& goal : {"generate-sources", "dependency:copy-dependencies", "help:effective-pom"}) {
    const std::vector<std::string> arguments = f.argv(goal);
    ASSERT_GE(arguments.size(), 4u) << goal;
    ASSERT_EQ("-B", arguments[0]) << goal;
    ASSERT_EQ("--settings", arguments[1]) << goal;
    ASSERT_EQ(settings.str(), arguments[2]) << goal;
    ASSERT_EQ(goal, arguments[3]) << goal;
  }
}
