#include <gtest/gtest.h>

// As in the fork, these run on Windows only: the fixtures' Gradle 8.1.1 wrappers cannot run on the JDK 21 the tests use.
#ifdef _WIN32

#  include "FilePath.h"
#  include "FileSystem.h"
#  include "TextAccess.h"
#  include "utility.h"
#  include "utilityGradle.h"

namespace {
FilePath tmpFolder = FilePath(L"data/UtilityGradleTestSuite/tmp");

void cleanup() {
  if(tmpFolder.recheckExists()) {
    for(const FilePath& path : FileSystem::getFilePathsFromDirectory(tmpFolder)) {
      FileSystem::remove(path);
    }
    FileSystem::remove(tmpFolder);
  }
}
}    // namespace

TEST(UtilityGradle, GradleWrapperDetectsSourceDirectoriesOfSimpleProjects) {
  std::vector<FilePath> result = utility::gradleGetAllSourceDirectories(
      FilePath(L"data/UtilityGradleTestSuite/simple_gradle_project"), tmpFolder.makeAbsolute(), false);

  ASSERT_TRUE(utility::containsElement<FilePath>(
      result, FilePath(L"data/UtilityGradleTestSuite/simple_gradle_project/src/main/java").makeAbsolute()));

  ASSERT_TRUE(!utility::containsElement<FilePath>(
      result, FilePath(L"data/UtilityGradleTestSuite/simple_gradle_project/src/test/java").makeAbsolute()));
}

TEST(UtilityGradle, GradleWrapperDetectsSourceAndTestDirectoriesOfSimpleProjects) {
  std::vector<FilePath> result = utility::gradleGetAllSourceDirectories(
      FilePath(L"data/UtilityGradleTestSuite/simple_gradle_project"), tmpFolder.makeAbsolute(), true);

  ASSERT_TRUE(utility::containsElement<FilePath>(
      result, FilePath(L"data/UtilityGradleTestSuite/simple_gradle_project/src/main/java").makeAbsolute()));

  ASSERT_TRUE(utility::containsElement<FilePath>(
      result, FilePath(L"data/UtilityGradleTestSuite/simple_gradle_project/src/test/java").makeAbsolute()));
}

TEST(UtilityGradle, GradleWrapperPreparesLombokSourcesAndReplacesSourceDirectories) {
  const FilePath projectPath(L"data/UtilityGradleTestSuite/delombok_gradle_project");
  const FilePath generatedDirectory = tmpFolder.makeAbsolute().getConcatenated(L"delombok").getConcatenated(L"main");
  const FilePath generatedFile = generatedDirectory.getConcatenated(L"hello/Greeter.java");

  cleanup();
  if(generatedDirectory.recheckExists()) {
    const std::vector<FilePath> subDirectories = FileSystem::getRecursiveSubDirectories(generatedDirectory);
    for(auto it = subDirectories.rbegin(); it != subDirectories.rend(); ++it) {
      FileSystem::remove(*it);
    }
    FileSystem::remove(generatedDirectory);
  }

  ASSERT_TRUE(utility::gradlePrepareLombokSources(projectPath, tmpFolder.makeAbsolute(), false));
  ASSERT_TRUE(generatedFile.recheckExists());

  const std::shared_ptr<TextAccess> generatedGreeter = TextAccess::createFromFile(generatedFile);
  ASSERT_TRUE(generatedGreeter->getText().find("getGreeting") != std::string::npos);

  const std::vector<FilePath> sourceDirectories = utility::gradleGetAllSourceDirectories(
      projectPath, tmpFolder.makeAbsolute(), false);
  ASSERT_TRUE(utility::containsElement<FilePath>(sourceDirectories, generatedDirectory));
  ASSERT_TRUE(!utility::containsElement<FilePath>(
      sourceDirectories, FilePath(L"data/UtilityGradleTestSuite/delombok_gradle_project/src/main/java").makeAbsolute()));
  cleanup();
}

TEST(UtilityGradle, GradleWrapperDetectsSourceDependenciesOfSimpleProjects) {
  std::vector<std::wstring> requiredDependencies = {L"joda-time-2.2.jar"};

  cleanup();
  ASSERT_TRUE(utility::gradleCopyDependencies(
      FilePath(L"data/UtilityGradleTestSuite/simple_gradle_project"), tmpFolder.makeAbsolute(), false));

  const std::vector<FilePath> copiedDependencies = FileSystem::getFilePathsFromDirectory(tmpFolder);

  for(const std::wstring requiredDependency : requiredDependencies) {
    ASSERT_TRUE(utility::containsElement(copiedDependencies, tmpFolder.getConcatenated(requiredDependency)));
  }
  cleanup();
}

TEST(UtilityGradle, GradleWrapperDetectsSourceAndTestDependenciesOfSimpleProjects) {
  std::vector<std::wstring> requiredDependencies = {L"joda-time-2.2.jar", L"hamcrest-core-1.3.jar", L"junit-4.12.jar"};

  cleanup();
  ASSERT_TRUE(utility::gradleCopyDependencies(
      FilePath(L"data/UtilityGradleTestSuite/simple_gradle_project"), tmpFolder.makeAbsolute(), true));

  const std::vector<FilePath> copiedDependencies = FileSystem::getFilePathsFromDirectory(tmpFolder);

  for(const std::wstring requiredDependency : requiredDependencies) {
    ASSERT_TRUE(utility::containsElement(copiedDependencies, tmpFolder.getConcatenated(requiredDependency)));
  }
  cleanup();
}

#endif    // _WIN32
