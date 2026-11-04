#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <sstream>

#include <QByteArray>

#include <gtest/gtest.h>

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include "FilePath.h"
#include "FileSystem.h"
#include "utilityDelombokPathAliases.h"
#include "utilityGradle.h"
#include "utilityGradleCache.h"

namespace {
const FilePath s_root(L"data/GradleCacheTestSuite/tmp");

struct GradleCacheFixture {
  GradleCacheFixture()
      : project(s_root.getConcatenated(L"project"))
      , output(s_root.getConcatenated(L"output"))
      , userHome(s_root.getConcatenated(L"gradle-home")) {
    const QByteArray previous = qgetenv("GRADLE_USER_HOME");
    hadUserHome = !previous.isEmpty();
    previousUserHome = previous;
    std::filesystem::remove_all(s_root.str());
    FileSystem::createDirectory(userHome);
    qputenv("GRADLE_USER_HOME", QByteArray(userHome.str().c_str()));
    FileSystem::createDirectory(project);
    FileSystem::createDirectory(output);
    FileSystem::createDirectory(project.getConcatenated(L"src/main/java"));
    write("build.gradle", "plugins {}");
    write("settings.gradle", "include 'sub'");
    write("sub/build.gradle", "plugins {}");
    write("build/build.gradle", "ignored");
  }

  ~GradleCacheFixture() {
    if(hadUserHome) {
      qputenv("GRADLE_USER_HOME", previousUserHome);
    } else {
      qunsetenv("GRADLE_USER_HOME");
    }
    std::filesystem::remove_all(s_root.str());
  }

  void write(const std::string& relativePath, const std::string& content) const {
    const std::filesystem::path path = std::filesystem::path(project.str()) / relativePath;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << content;
  }

  void bumpMtime(const std::string& relativePath) const {
    const std::filesystem::path path = std::filesystem::path(project.str()) / relativePath;
    std::filesystem::last_write_time(path, std::filesystem::last_write_time(path) + std::chrono::seconds(10));
  }

  utility::GradleProjectInfo makeInfo() const {
    utility::GradleProjectInfo info;
    info.fingerprint = utility::gradleBuildFingerprint(project);
    info.includesTests = false;
    info.lombokJars[L"main"] = FilePath(L"/libs/lombok.jar");
    info.sourceDirectories[L"main"] = {project.getConcatenated(L"src/main/java")};
    return info;
  }

  FilePath project;
  FilePath output;
  FilePath userHome;

private:
  bool hadUserHome = false;
  QByteArray previousUserHome;
};
}    // namespace

TEST(GradleCache, RoundTripsProjectInfo) {
  GradleCacheFixture f;
  const utility::GradleProjectInfo info = f.makeInfo();
  utility::saveGradleProjectInfo(f.output, info);

  const auto loaded = utility::loadGradleProjectInfo(f.output, f.project, false);
  ASSERT_TRUE(loaded.has_value());
  ASSERT_TRUE(loaded->fingerprint == info.fingerprint);
  ASSERT_TRUE(loaded->includesTests == false);
  ASSERT_TRUE(loaded->lombokJars == info.lombokJars);
  ASSERT_TRUE(loaded->sourceDirectories == info.sourceDirectories);
}

TEST(GradleCache, RoundTripsEverySourceSet) {
  GradleCacheFixture f;
  utility::GradleProjectInfo info = f.makeInfo();
  info.includesTests = true;
  info.lombokJars[L"integrationTest"] = FilePath(L"/libs/lombok-it.jar");
  info.sourceDirectories[L"test"] = {f.project.getConcatenated(L"src/test/java")};
  info.sourceDirectories[L"integrationTest"] = {f.project.getConcatenated(L"src/integrationTest/java")};
  utility::saveGradleProjectInfo(f.output, info);

  const auto loaded = utility::loadGradleProjectInfo(f.output, f.project, true);
  ASSERT_TRUE(loaded.has_value());
  ASSERT_TRUE(loaded->lombokJars == info.lombokJars);
  ASSERT_TRUE(loaded->sourceDirectories == info.sourceDirectories);
}

TEST(GradleCache, OfAnOlderVersionIsStale) {
  GradleCacheFixture f;
  utility::saveGradleProjectInfo(f.output, f.makeInfo());
  const std::filesystem::path cache = std::filesystem::path(f.output.str()) / "sourcetrail-gradle-project.cache";
  std::ifstream in(cache, std::ios::binary);
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  ASSERT_TRUE(content.rfind("version\t3\n", 0) == 0);
  content.replace(0, std::string("version\t3").size(), "version\t2");
  std::ofstream(cache, std::ios::binary | std::ios::trunc) << content;

  ASSERT_FALSE(utility::loadGradleProjectInfo(f.output, f.project, false).has_value());
}

TEST(GradleCache, IsMissingBeforeAnythingWasSaved) {
  GradleCacheFixture f;
  ASSERT_FALSE(utility::loadGradleProjectInfo(f.output, f.project, false).has_value());
}

TEST(GradleCache, IsInvalidatedByABuildFileChange) {
  GradleCacheFixture f;
  utility::saveGradleProjectInfo(f.output, f.makeInfo());
  f.bumpMtime("sub/build.gradle");
  ASSERT_FALSE(utility::loadGradleProjectInfo(f.output, f.project, false).has_value());
}

TEST(GradleCache, IgnoresFilesUnderBuildDirectories) {
  GradleCacheFixture f;
  utility::saveGradleProjectInfo(f.output, f.makeInfo());
  f.write("build/build.gradle", "changed and longer");
  f.bumpMtime("build/build.gradle");
  ASSERT_TRUE(utility::loadGradleProjectInfo(f.output, f.project, false).has_value());
}

TEST(GradleCache, IsInvalidatedWhenTestInclusionChanges) {
  GradleCacheFixture f;
  utility::saveGradleProjectInfo(f.output, f.makeInfo());
  ASSERT_FALSE(utility::loadGradleProjectInfo(f.output, f.project, true).has_value());
}

TEST(GradleCache, IsInvalidatedWhenASourceRootDisappears) {
  GradleCacheFixture f;
  utility::saveGradleProjectInfo(f.output, f.makeInfo());
  std::filesystem::remove_all(f.project.getConcatenated(L"src/main/java").str());
  ASSERT_FALSE(utility::loadGradleProjectInfo(f.output, f.project, false).has_value());
}

TEST(GradleCache, IsInvalidatedWhenADeclaredSourceRootAppearsLater) {
  GradleCacheFixture f;
  utility::GradleProjectInfo info = f.makeInfo();
  const FilePath testRoot = f.project.getConcatenated(L"src/test/java");
  info.sourceDirectories[L"test"] = {testRoot};
  utility::saveGradleProjectInfo(f.output, info);
  ASSERT_TRUE(utility::loadGradleProjectInfo(f.output, f.project, false).has_value());

  FileSystem::createDirectory(testRoot);
  ASSERT_FALSE(utility::loadGradleProjectInfo(f.output, f.project, false).has_value());
}

TEST(GradleCache, GradleDependenciesAreOnlyUpToDateAfterAnExportForTheCurrentBuildFiles) {
  GradleCacheFixture f;
  // info cache alone (written by non-export paths) must not count as an export
  utility::saveGradleProjectInfo(f.output, f.makeInfo());
  ASSERT_FALSE(utility::gradleDependenciesUpToDate(f.output, f.project, false));

  utility::markGradleDependenciesExported(f.output, utility::gradleBuildFingerprint(f.project), false);
  ASSERT_TRUE(utility::gradleDependenciesUpToDate(f.output, f.project, false));
  ASSERT_FALSE(utility::gradleDependenciesUpToDate(f.output, f.project, true));

  f.bumpMtime("build.gradle");
  utility::saveGradleProjectInfo(f.output, f.makeInfo());    // refreshed for the new fingerprint
  ASSERT_FALSE(utility::gradleDependenciesUpToDate(f.output, f.project, false));
}

TEST(GradleCache, GradleBuildFilesListKnownNamesAndSkipBuildDirectories) {
  GradleCacheFixture f;
  const std::vector<FilePath> files = utility::gradleBuildFiles(f.project);
  ASSERT_TRUE(files.size() == 3);
}

TEST(GradleCache, GradleBuildFilesIncludeBuildSrcSources) {
  GradleCacheFixture f;
  f.write("buildSrc/src/main/groovy/conventions.gradle", "plugins {}");
  f.write("buildSrc/build/tmp/x.class", "ignored");

  std::vector<std::string> names;
  for(const FilePath& file : utility::gradleBuildFiles(f.project)) {
    names.push_back(file.str());
  }
  const auto listed = [&](const std::string& relativePath) {
    const std::string absolute = (std::filesystem::path(f.project.str()) / relativePath).string();
    return std::find(names.begin(), names.end(), absolute) != names.end();
  };

  ASSERT_TRUE(listed("buildSrc/src/main/groovy/conventions.gradle"));
  ASSERT_FALSE(listed("buildSrc/build/tmp/x.class"));
}

TEST(GradleCache, GradleBuildFingerprintChangesWhenTheUserGradlePropertiesChange) {
  GradleCacheFixture f;
  const std::filesystem::path properties = std::filesystem::path(f.userHome.str()) / "gradle.properties";
  std::ofstream(properties, std::ios::binary) << "org.gradle.jvmargs=-Xmx1g";
  const std::string before = utility::gradleBuildFingerprint(f.project);

  std::ofstream(properties, std::ios::binary | std::ios::trunc) << "org.gradle.jvmargs=-Xmx2048m";

  ASSERT_TRUE(utility::gradleBuildFingerprint(f.project) != before);
}

TEST(GradleCache, GradleBuildFilesSkipAMissingUserGradleProperties) {
  GradleCacheFixture f;
  for(const FilePath& file : utility::gradleBuildFiles(f.project)) {
    ASSERT_TRUE(file.str().rfind(f.project.str(), 0) == 0);
  }
}

namespace {
// Mimics Lombok's CLI layout: directory inputs keep their relative paths, file inputs land flat.
struct FakeDelombok {
  std::vector<std::vector<FilePath>> runs;
  std::vector<std::vector<FilePath>> sourcePaths;
  bool failOnFiles = false;

  static void copy(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::filesystem::create_directories(to.parent_path());
    std::ifstream in(from, std::ios::binary);
    std::ofstream out(to, std::ios::binary);
    if(from.extension() == ".java") {
      out << "// delomboked\n";
    }
    out << in.rdbuf();
  }

  utility::DelombokRunner runner() {
    return [this](const std::vector<FilePath>& inputs, const std::vector<FilePath>& sourcePath, const FilePath& outputDirectory) {
      runs.push_back(inputs);
      sourcePaths.push_back(sourcePath);
      const std::filesystem::path out(outputDirectory.str());
      for(const FilePath& input : inputs) {
        const std::filesystem::path in(input.str());
        if(std::filesystem::is_directory(in)) {
          for(const auto& entry : std::filesystem::recursive_directory_iterator(in)) {
            if(entry.is_regular_file()) {
              copy(entry.path(), out / entry.path().lexically_relative(in));
            }
          }
        } else if(failOnFiles) {
          return false;
        } else {
          copy(in, out / in.filename());
        }
      }
      return true;
    };
  }

  bool lastRunWasWholeTree() const {
    return !runs.empty() && std::all_of(runs.back().begin(), runs.back().end(), [](const FilePath& input) {
      return std::filesystem::is_directory(input.str());
    });
  }
};

std::map<std::string, std::string> readTree(const FilePath& directory) {
  std::map<std::string, std::string> files;
  const std::filesystem::path root(directory.str());
  for(const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    if(entry.is_regular_file()) {
      std::ifstream in(entry.path(), std::ios::binary);
      std::stringstream content;
      content << in.rdbuf();
      files[entry.path().lexically_relative(root).generic_string()] = content.str();
    }
  }
  return files;
}

struct DelombokFixture : public GradleCacheFixture {
  DelombokFixture() : delombok(utility::getDelombokOutputDirectory(output, L"main")) {
    write("a/src/main/java/pkg/A.java", "class A {}");
    write("a/src/main/java/pkg/B.java", "class B {}");
    write("a/src/main/java/pkg/data.txt", "data");
    write("b/src/main/java/other/C.java", "class C {}");
    roots = {project.getConcatenated(L"a/src/main/java"), project.getConcatenated(L"b/src/main/java")};
    fingerprint = utility::gradleBuildFingerprint(project);
  }

  bool run(FakeDelombok& fake, const FilePath& target, bool wholeTree = false) const {
    return utility::delombokSourceSet(roots, target, project, fingerprint, fake.runner(), wholeTree);
  }

  bool run(FakeDelombok& fake) const {
    return run(fake, delombok);
  }

  void edit(const std::string& relativePath, const std::string& content) const {
    write(relativePath, content);
    bumpMtime(relativePath);
  }

  FilePath delombok;
  std::vector<FilePath> roots;
  std::string fingerprint;
};
}    // namespace

TEST(GradleCache, DelombokManifestRoundTrip) {
  DelombokFixture f;
  f.write("lombok.config", "lombok.addLombokGeneratedAnnotation = true");
  const utility::DelombokManifest manifest = utility::scanDelombokOriginals(f.roots, f.fingerprint);
  ASSERT_TRUE(manifest.files.size() == 4);
  ASSERT_TRUE(manifest.files.count({0, L"pkg/A.java"}) == 1);
  ASSERT_TRUE(manifest.files.count({1, L"other/C.java"}) == 1);
  ASSERT_TRUE(manifest.lombokConfigs.size() == 1);
  ASSERT_TRUE(manifest.crossFileOriginals.empty());

  utility::saveDelombokManifest(f.delombok, manifest);

  const auto loaded = utility::loadDelombokManifest(f.delombok);
  ASSERT_TRUE(loaded.has_value());
  ASSERT_TRUE(*loaded == manifest);
  ASSERT_TRUE(loaded->sourceDirectories == f.roots);
  ASSERT_FALSE(std::filesystem::exists(std::filesystem::path(f.delombok.str()) / "main.manifest"));
}

TEST(GradleCache, DelombokManifestRoundTripKeepsAnyBuildFingerprint) {
  DelombokFixture f;
  f.write("a/src/main/java/pkg/X.java", "import lombok.experimental.Delegate;\nclass X { @Delegate B b; }");
  for(const std::string& fingerprint : {std::string("a|1|2"), std::string("a|1|2\n\nb|3|4\n"), std::string()}) {
    const utility::DelombokManifest manifest = utility::scanDelombokOriginals(f.roots, fingerprint);
    utility::saveDelombokManifest(f.delombok, manifest);
    const auto loaded = utility::loadDelombokManifest(f.delombok);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_TRUE(*loaded == manifest);
    ASSERT_TRUE(loaded->crossFileOriginals.count({0, L"pkg/X.java"}) == 1);
  }
  ASSERT_TRUE(utility::scanDelombokOriginals(f.roots, "a|1|2").fingerprint !=
              utility::scanDelombokOriginals(f.roots, "a|1|3").fingerprint);
}

TEST(GradleCache, DelombokManifestOfAnOlderVersionIsUnreadable) {
  DelombokFixture f;
  utility::saveDelombokManifest(f.delombok, utility::scanDelombokOriginals(f.roots, f.fingerprint));
  const std::string path = f.delombok.str() + ".manifest";
  std::ifstream in(path, std::ios::binary);
  std::stringstream content;
  content << in.rdbuf();
  in.close();
  std::string text = content.str();
  text.replace(0, text.find('\n'), "version\t3");    // the shared output tree of all roots
  std::ofstream(path, std::ios::binary | std::ios::trunc) << text;

  ASSERT_FALSE(utility::loadDelombokManifest(f.delombok).has_value());
}

TEST(GradleCache, DelombokOutputDirectoriesAreNamedAfterTheirRoots) {
  DelombokFixture f;
  ASSERT_TRUE(
      (utility::getDelombokRootDirectories(f.delombok, f.roots, f.project) ==
       std::vector<FilePath>{f.delombok.getConcatenated(L"a_src_main_java"), f.delombok.getConcatenated(L"b_src_main_java")}));

  // a root outside the project is named after its absolute path, a name taken twice gets a suffix
  const std::vector<FilePath> roots = {
      f.project.getConcatenated(L"a/src"), f.project.getConcatenated(L"a_src"), FilePath(L"/elsewhere/java")};
  ASSERT_TRUE((utility::getDelombokRootDirectories(f.delombok, roots, f.project) ==
               std::vector<FilePath>{f.delombok.getConcatenated(L"a_src"),
                                     f.delombok.getConcatenated(L"a_src_2"),
                                     f.delombok.getConcatenated(L"elsewhere_java")}));
}

TEST(GradleCache, DelombokKeepsOriginalsWithTheSameRelativePathInDifferentRootsApart) {
  DelombokFixture f;
  FakeDelombok fake;
  f.write("b/src/main/java/pkg/A.java", "class OtherA {}");
  ASSERT_TRUE(f.run(fake));
  ASSERT_TRUE(fake.lastRunWasWholeTree());
  // one run per group of roots without shared paths, each seeing every root's sources
  ASSERT_TRUE((fake.runs == std::vector<std::vector<FilePath>>{{f.roots[0]}, {f.roots[1]}}));
  ASSERT_TRUE((fake.sourcePaths == std::vector<std::vector<FilePath>>{f.roots, f.roots}));
  ASSERT_TRUE(readTree(f.delombok)["a_src_main_java/pkg/A.java"] == "// delomboked\nclass A {}");
  ASSERT_TRUE(readTree(f.delombok)["b_src_main_java/pkg/A.java"] == "// delomboked\nclass OtherA {}");
  ASSERT_TRUE(readTree(f.delombok)["b_src_main_java/other/C.java"] == "// delomboked\nclass C {}");

  f.edit("b/src/main/java/pkg/A.java", "class OtherA { int x; }");
  const size_t runs = fake.runs.size();
  ASSERT_TRUE(f.run(fake));
  ASSERT_TRUE(fake.runs.size() == runs + 1);
  ASSERT_FALSE(fake.lastRunWasWholeTree());
  ASSERT_TRUE(readTree(f.delombok)["a_src_main_java/pkg/A.java"] == "// delomboked\nclass A {}");
  ASSERT_TRUE(readTree(f.delombok)["b_src_main_java/pkg/A.java"] == "// delomboked\nclass OtherA { int x; }");

  std::filesystem::remove(std::filesystem::path(f.project.str()) / "b/src/main/java/pkg/A.java");
  ASSERT_TRUE(f.run(fake));
  ASSERT_TRUE(readTree(f.delombok).count("b_src_main_java/pkg/A.java") == 0);
  ASSERT_TRUE(readTree(f.delombok)["a_src_main_java/pkg/A.java"] == "// delomboked\nclass A {}");
}

TEST(GradleCache, DelombokReplacesTheSharedOutputTreeOfAnOlderVersion) {
  DelombokFixture f;
  FakeDelombok fake;
  const std::filesystem::path stale = std::filesystem::path(f.delombok.str()) / "pkg/A.java";
  std::filesystem::create_directories(stale.parent_path());
  std::ofstream(stale, std::ios::binary) << "class A {}";

  ASSERT_TRUE(f.run(fake));
  ASSERT_TRUE(fake.lastRunWasWholeTree());
  ASSERT_FALSE(std::filesystem::exists(stale.parent_path()));
  ASSERT_TRUE(readTree(f.delombok)["a_src_main_java/pkg/A.java"] == "// delomboked\nclass A {}");
}

TEST(GradleCache, IncrementalDelombokMatchesAFullRun) {
  DelombokFixture f;
  FakeDelombok fake;
  ASSERT_TRUE(f.run(fake));
  ASSERT_TRUE(fake.lastRunWasWholeTree());
  ASSERT_TRUE(fake.runs.back() == f.roots);
  ASSERT_TRUE(fake.sourcePaths.back().empty());
  const auto unchangedMtime = std::filesystem::last_write_time(std::filesystem::path(f.delombok.str()) /
                                                               "a_src_main_java/pkg/B.java");

  f.edit("a/src/main/java/pkg/A.java", "class A { int x; }");
  f.write("b/src/main/java/other/D.java", "class D {}");
  ASSERT_TRUE(f.run(fake));

  ASSERT_TRUE(fake.runs.size() == 2);
  ASSERT_TRUE(fake.runs.back().size() == 2);
  ASSERT_TRUE(fake.sourcePaths.back() == f.roots);
  ASSERT_FALSE(fake.lastRunWasWholeTree());
  ASSERT_TRUE(std::filesystem::last_write_time(std::filesystem::path(f.delombok.str()) / "a_src_main_java/pkg/B.java") ==
              unchangedMtime);

  FakeDelombok full;
  const FilePath fullOutput = utility::getDelombokOutputDirectory(f.output, L"full");
  ASSERT_TRUE(f.run(full, fullOutput));
  ASSERT_TRUE(full.lastRunWasWholeTree());
  ASSERT_TRUE(readTree(f.delombok) == readTree(fullOutput));
  ASSERT_TRUE(readTree(f.delombok)["a_src_main_java/pkg/A.java"] == "// delomboked\nclass A { int x; }");
}

TEST(GradleCache, DelombokSkipsTheRunWhenNoOriginalChanged) {
  DelombokFixture f;
  FakeDelombok fake;
  ASSERT_TRUE(f.run(fake));
  ASSERT_TRUE(f.run(fake));
  ASSERT_TRUE(fake.runs.size() == 1);
}

TEST(GradleCache, DelombokRunsOnTheWholeTreeWhenLombokConfigChanges) {
  // one run per Catch2 SECTION of the original test
  const std::vector<std::pair<std::string, std::function<void(DelombokFixture&)>>> variants = {
      {"above the roots", [](DelombokFixture& f) { f.edit("lombok.config", "config.stopBubbling = false"); }},
      {"inside a root", [](DelombokFixture& f) { f.write("a/src/main/java/pkg/lombok.config", "lombok.accessors.chain = true"); }},
      {"added above a root", [](DelombokFixture& f) { f.write("b/lombok.config", "lombok.accessors.chain = true"); }},
  };
  for(const auto& [section, change] : variants) {
    SCOPED_TRACE(section);
    DelombokFixture f;
    FakeDelombok fake;
    f.write("lombok.config", "config.stopBubbling = true");
    ASSERT_TRUE(f.run(fake));

    change(f);
    f.edit("a/src/main/java/pkg/A.java", "class A { int y; }");

    ASSERT_TRUE(f.run(fake));
    ASSERT_TRUE(fake.runs.size() == 2);
    ASSERT_TRUE(fake.lastRunWasWholeTree());
  }
}

TEST(GradleCache, DelombokRunsOnTheWholeTreeWhenTheBuildFingerprintChanges) {
  DelombokFixture f;
  FakeDelombok fake;
  ASSERT_TRUE(f.run(fake));

  f.fingerprint += "changed";
  ASSERT_TRUE(f.run(fake));
  ASSERT_TRUE(fake.runs.size() == 2);
  ASSERT_TRUE(fake.lastRunWasWholeTree());
}

TEST(GradleCache, DelombokRunsOnTheWholeTreeWhenTheManifestIsMissing) {
  DelombokFixture f;
  FakeDelombok fake;
  ASSERT_TRUE(f.run(fake));

  utility::removeDelombokManifest(f.delombok);
  ASSERT_TRUE(f.run(fake));
  ASSERT_TRUE(fake.runs.size() == 2);
  ASSERT_TRUE(fake.lastRunWasWholeTree());
}

TEST(GradleCache, DelombokRunsOnTheWholeTreeWhenAskedToDespiteACurrentManifest) {
  DelombokFixture f;
  FakeDelombok fake;
  ASSERT_TRUE(f.run(fake));

  // an output changed out of band is invisible to the manifest
  std::filesystem::remove(std::filesystem::path(f.delombok.str()) / "a_src_main_java/pkg/B.java");
  ASSERT_TRUE(f.run(fake));
  ASSERT_TRUE(fake.runs.size() == 1);

  ASSERT_TRUE(f.run(fake, f.delombok, true));
  ASSERT_TRUE(fake.runs.size() == 2);
  ASSERT_TRUE(fake.lastRunWasWholeTree());
  ASSERT_TRUE(readTree(f.delombok)["a_src_main_java/pkg/B.java"] == "// delomboked\nclass B {}");
}

TEST(GradleCache, DelombokRemovesTheCopyOfADeletedOriginal) {
  DelombokFixture f;
  FakeDelombok fake;
  ASSERT_TRUE(f.run(fake));
  ASSERT_TRUE(std::filesystem::exists(std::filesystem::path(f.delombok.str()) / "b_src_main_java/other/C.java"));

  std::filesystem::remove(std::filesystem::path(f.project.str()) / "b/src/main/java/other/C.java");
  ASSERT_TRUE(f.run(fake));

  ASSERT_TRUE(fake.runs.size() == 1);
  ASSERT_FALSE(std::filesystem::exists(std::filesystem::path(f.delombok.str()) / "b_src_main_java/other"));
  ASSERT_TRUE(std::filesystem::exists(std::filesystem::path(f.delombok.str()) / "a_src_main_java/pkg/A.java"));

  ASSERT_TRUE(f.run(fake));
  ASSERT_TRUE(fake.runs.size() == 1);
}

TEST(GradleCache, DelombokRunsFilesSharingANameInSeparateBatches) {
  DelombokFixture f;
  FakeDelombok fake;
  f.write("b/src/main/java/other/A.java", "class OtherA {}");
  ASSERT_TRUE(f.run(fake));

  f.edit("a/src/main/java/pkg/A.java", "class A { int z; }");
  f.edit("b/src/main/java/other/A.java", "class OtherA { int z; }");
  ASSERT_TRUE(f.run(fake));

  ASSERT_TRUE(fake.runs.size() == 3);
  ASSERT_TRUE(readTree(f.delombok)["a_src_main_java/pkg/A.java"] == "// delomboked\nclass A { int z; }");
  ASSERT_TRUE(readTree(f.delombok)["b_src_main_java/other/A.java"] == "// delomboked\nclass OtherA { int z; }");
}

TEST(GradleCache, DelombokRunsOnTheWholeTreeWhenAnUnchangedFileUsesACrossFileAnnotation) {
  // one run per Catch2 SECTION of the original test
  const std::vector<std::pair<std::string, std::string>> variants = {
      {"@Delegate", "class X { @lombok.experimental.Delegate private final B b; }"},
      {"@ExtensionMethod", "import lombok.experimental.ExtensionMethod;\n@ExtensionMethod(B.class) class X {}"},
      {"deprecated @lombok.Delegate", "class X { @lombok.Delegate private final B b; }"},
      {"lombok.val", "import lombok.val;\nclass X { void f() { val b = new B(); } }"},
      {"val through a wildcard import", "import lombok.*;\nclass X { void f() { val b = new B(); } }"},
      {"lombok.var", "import lombok.var;\nclass X { void f() { var b = new B(); } }"},
  };
  for(const auto& [section, content] : variants) {
    SCOPED_TRACE(section);
    DelombokFixture f;
    FakeDelombok fake;
    f.write("b/src/main/java/other/X.java", content);
    ASSERT_TRUE(f.run(fake));

    f.edit("a/src/main/java/pkg/B.java", "class B { int make() { return 1; } }");
    ASSERT_TRUE(f.run(fake));
    ASSERT_TRUE(fake.runs.size() == 2);
    ASSERT_TRUE(fake.lastRunWasWholeTree());
  }
}

TEST(GradleCache, DelombokOfACrossFileOriginalAloneStaysIncremental) {
  DelombokFixture f;
  FakeDelombok fake;
  f.write("b/src/main/java/other/X.java", "class X { @Delegate private final B b; }");
  ASSERT_TRUE(f.run(fake));

  f.edit("b/src/main/java/other/X.java", "class X { @Delegate private final B b2; }");
  ASSERT_TRUE(f.run(fake));
  ASSERT_TRUE(fake.runs.size() == 2);
  ASSERT_FALSE(fake.lastRunWasWholeTree());
}

TEST(GradleCache, DelombokRunsOnTheWholeTreeWhenMoreThan200FilesChanged) {
  DelombokFixture f;
  FakeDelombok fake;
  for(int i = 0; i < 201; i++) {
    f.write("a/src/main/java/many/M" + std::to_string(i) + ".java", "class M {}");
  }
  ASSERT_TRUE(f.run(fake));

  for(int i = 0; i < 201; i++) {
    f.edit("a/src/main/java/many/M" + std::to_string(i) + ".java", "class M { int x; }");
  }
  ASSERT_TRUE(f.run(fake));
  ASSERT_TRUE(fake.runs.size() == 2);
  ASSERT_TRUE(fake.lastRunWasWholeTree());

  for(int i = 0; i < 200; i++) {
    f.edit("a/src/main/java/many/M" + std::to_string(i) + ".java", "class M { long x; }");
  }
  ASSERT_TRUE(f.run(fake));
  ASSERT_TRUE(fake.runs.size() == 3);
  ASSERT_FALSE(fake.lastRunWasWholeTree());
}

TEST(GradleCache, DelombokFallsBackToTheWholeTreeWhenAnIncrementalRunFails) {
  DelombokFixture f;
  FakeDelombok fake;
  ASSERT_TRUE(f.run(fake));

  fake.failOnFiles = true;
  f.edit("a/src/main/java/pkg/A.java", "class A { long x; }");
  ASSERT_TRUE(f.run(fake));

  ASSERT_TRUE(fake.runs.size() == 3);
  ASSERT_TRUE(fake.lastRunWasWholeTree());
  ASSERT_TRUE(readTree(f.delombok)["a_src_main_java/pkg/A.java"] == "// delomboked\nclass A { long x; }");
}

namespace {
// A directory of its own below the system's temporary directory, with a non-ASCII and an ASCII project inside.
struct DelombokPathAliasesFixture {
  DelombokPathAliasesFixture()
      : root(std::filesystem::temp_directory_path() / ("DelombokPathAliases-" + std::to_string(std::random_device()())))
      , temporary(root / "tmp")
      , nonAscii(root / L"ünï" / "x" / L"cödé" / "app" / "src")
      , ascii(root / "plain" / "app" / "src") {
    std::filesystem::create_directories(temporary);
    std::filesystem::create_directories(nonAscii);
    std::filesystem::create_directories(ascii);
  }

  ~DelombokPathAliasesFixture() {
    std::filesystem::remove_all(root);
  }

  size_t aliasDirectories() const {
    return static_cast<size_t>(
        std::distance(std::filesystem::directory_iterator(temporary), std::filesystem::directory_iterator()));
  }

  std::filesystem::path root;
  std::filesystem::path temporary;
  std::filesystem::path nonAscii;
  std::filesystem::path ascii;
};

bool isAscii(const std::wstring& text) {
  return std::all_of(text.begin(), text.end(), [](wchar_t c) { return c <= 0x7F; });
}

// Routes the default logger into a string stream while it lives.
struct CapturedLog {
  CapturedLog() : previous(spdlog::default_logger()) {
    auto logger = std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::ostream_sink_mt>(stream));
    logger->set_level(spdlog::level::trace);
    spdlog::set_default_logger(logger);
  }

  ~CapturedLog() {
    spdlog::set_default_logger(previous);
  }

  std::ostringstream stream;
  std::shared_ptr<spdlog::logger> previous;
};
}    // namespace

TEST(DelombokPathAliases, aNonAsciiPathGoesThroughAnAsciiLinkToItsDeepestNonAsciiDirectory) {
  const DelombokPathAliasesFixture f;
  std::filesystem::path alias;
  {
    const utility::DelombokPathAliases aliases({FilePath(f.nonAscii.wstring())}, f.temporary);
    const FilePath mapped = aliases.map(FilePath(f.nonAscii.wstring()));

    EXPECT_TRUE(isAscii(mapped.wstr())) << mapped.str();
    EXPECT_TRUE(std::filesystem::equivalent(mapped.wstr(), f.nonAscii));
    // The link is to "cödé", so a lombok.config in app/ still applies.
    alias = std::filesystem::path(mapped.wstr()).parent_path().parent_path();
    EXPECT_TRUE(std::filesystem::is_symlink(alias));
    EXPECT_EQ(f.root / L"ünï" / "x" / L"cödé", std::filesystem::read_symlink(alias));
    // Another path below the same directory takes the same link.
    EXPECT_EQ(alias / "app", std::filesystem::path(aliases.map(FilePath((f.nonAscii.parent_path()).wstring())).wstr()));
#ifndef _WIN32
    const auto permissions = std::filesystem::status(alias.parent_path()).permissions();
    EXPECT_EQ(std::filesystem::perms::owner_all, permissions & std::filesystem::perms::all);
#endif
  }
  EXPECT_FALSE(std::filesystem::exists(alias.parent_path()));
  EXPECT_EQ(0U, f.aliasDirectories());
}

TEST(DelombokPathAliases, anAsciiPathStaysAsItIs) {
  const DelombokPathAliasesFixture f;
  const utility::DelombokPathAliases aliases({FilePath(f.ascii.wstring())}, f.temporary);

  EXPECT_EQ(f.ascii.wstring(), aliases.map(FilePath(f.ascii.wstring())).wstr());
  EXPECT_EQ(0U, f.aliasDirectories());
}

TEST(DelombokPathAliases, aLinkThatCannotBeMadeLeavesThePathAsItIsWithAWarning) {
  const DelombokPathAliasesFixture f;
  const CapturedLog log;
  const utility::DelombokPathAliases aliases({FilePath(f.nonAscii.wstring())}, f.root / "missing");

  EXPECT_EQ(FilePath(f.nonAscii.wstring()).wstr(), aliases.map(FilePath(f.nonAscii.wstring())).wstr());
  EXPECT_NE(std::string::npos, log.stream.str().find("delombok gets them as they are")) << log.stream.str();
}
