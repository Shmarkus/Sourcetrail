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

// Stands in for mvn: logs each goal it is asked to run and writes what the real goal would.
const char* const FAKE_MAVEN = R"sh(#!/bin/sh
log="$(dirname "$0")/invocations.log"
for argument in "$@"; do
  case "$argument" in
    generate-sources|dependency:copy-dependencies|help:effective-pom) echo "$argument" >> "$log" ;;
    -DoutputDirectory=*) dependencies="${argument#-DoutputDirectory=}" ;;
    -Doutput=*) effectivePom="${argument#-Doutput=}" ;;
  esac
done
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

  bool prepare(bool fullRefresh, bool includesTests = false) const {
    return utility::mavenPrepareProject(maven, FilePath(), project, output, includesTests, fullRefresh, [](const std::wstring&) {});
  }

  std::vector<FilePath> sourceDirectories(bool includesTests = false) const {
    return utility::mavenGetSourceDirectories(maven, FilePath(), project, output, includesTests);
  }

  // One refresh as SourceGroupJavaMaven runs it: prepare, then ask for the source directories.
  std::vector<std::string> refresh(bool fullRefresh, bool includesTests = false) const {
    EXPECT_TRUE(prepare(fullRefresh, includesTests));
    EXPECT_FALSE(sourceDirectories(includesTests).empty());
    return invocations();
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
  ASSERT_TRUE(f.prepare(false, true));
  const std::vector<FilePath> fromMaven = f.sourceDirectories(true);
  ASSERT_EQ(ALL_GOALS, f.invocations());
  const std::vector<FilePath> fromCache = f.sourceDirectories(true);
  ASSERT_TRUE(f.invocations().empty());
  ASSERT_EQ(fromMaven, fromCache);
  ASSERT_EQ(3u, fromCache.size());    // the two main roots and the test root; no generated sources exist
}

TEST(MavenCache, CachedSourceDirectoriesPickUpAGeneratedSourcesDirectoryThatAppears) {
  MavenCacheFixture f;
  f.refresh(false);
  FileSystem::createDirectory(f.project.getConcatenated(L"target/generated-sources"));
  const std::vector<FilePath> fromCache = f.sourceDirectories();
  ASSERT_TRUE(f.invocations().empty());
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
      ASSERT_EQ(0u, content.rfind("version\t1\n", 0));
      content.replace(0, std::string("version\t1").size(), "version\t0");
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
