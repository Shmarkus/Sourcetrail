#include <gtest/gtest.h>

#include "FilePath.h"
#include "utility.h"
#include "utilityMaven.h"
#include "utilityPathDetection.h"

TEST(UtilityMaven, MavenPathDetectorIsWorking) {
  std::shared_ptr<PathDetector> mavenPathDetector = utility::getMavenExecutablePathDetector();
  ASSERT_TRUE(mavenPathDetector->isWorking());
}

TEST(UtilityMaven, MavenWrapperDetectsSourceDirectoriesOfSimpleProjects) {
  std::shared_ptr<PathDetector> mavenPathDetector = utility::getMavenExecutablePathDetector();
  ASSERT_TRUE(!mavenPathDetector->getPaths().empty());

  if(!mavenPathDetector->getPaths().empty()) {
    std::vector<FilePath> result = utility::mavenGetAllDirectoriesFromEffectivePom(
        mavenPathDetector->getPaths().front(),
        FilePath(),
        FilePath(L"data/UtilityMavenTestSuite/simple_maven_project"),
        FilePath(L"data/UtilityMavenTestSuite").makeAbsolute(),
        false);

    ASSERT_TRUE(utility::containsElement<FilePath>(
        result, FilePath(L"data/UtilityMavenTestSuite/simple_maven_project/src/main/java").makeAbsolute()));

    ASSERT_TRUE(!utility::containsElement<FilePath>(
        result, FilePath(L"data/UtilityMavenTestSuite/simple_maven_project/src/test/java").makeAbsolute()));
  }
}

TEST(UtilityMaven, MavenWrapperDetectsSourceAndTestDirectoriesOfSimpleProjects) {
  std::shared_ptr<PathDetector> mavenPathDetector = utility::getMavenExecutablePathDetector();
  ASSERT_TRUE(!mavenPathDetector->getPaths().empty());

  if(!mavenPathDetector->getPaths().empty()) {
    std::vector<FilePath> result = utility::mavenGetAllDirectoriesFromEffectivePom(
        mavenPathDetector->getPaths().front(),
        FilePath(),
        FilePath(L"data/UtilityMavenTestSuite/simple_maven_project"),
        FilePath(L"data/UtilityMavenTestSuite").makeAbsolute(),
        true);

    ASSERT_TRUE(utility::containsElement<FilePath>(
        result, FilePath(L"data/UtilityMavenTestSuite/simple_maven_project/src/main/java").makeAbsolute()));

    ASSERT_TRUE(utility::containsElement<FilePath>(
        result, FilePath(L"data/UtilityMavenTestSuite/simple_maven_project/src/test/java").makeAbsolute()));
  }
}

TEST(UtilityMaven, MavenWrapperDetectsSourceDirectoriesOfNestedModules) {
  std::shared_ptr<PathDetector> mavenPathDetector = utility::getMavenExecutablePathDetector();
  ASSERT_TRUE(!mavenPathDetector->getPaths().empty());

  if(!mavenPathDetector->getPaths().empty()) {
    std::vector<FilePath> result = utility::mavenGetAllDirectoriesFromEffectivePom(
        mavenPathDetector->getPaths().front(),
        FilePath(),
        FilePath(L"data/UtilityMavenTestSuite/nested_maven_project"),
        FilePath(L"data/UtilityMavenTestSuite").makeAbsolute(),
        false);

    ASSERT_TRUE(utility::containsElement<FilePath>(
        result, FilePath(L"data/UtilityMavenTestSuite/nested_maven_project/module_1/src/main/java").makeAbsolute()));

    ASSERT_TRUE(utility::containsElement<FilePath>(
        result, FilePath(L"data/UtilityMavenTestSuite/nested_maven_project/module_2/src/main/java").makeAbsolute()));

    ASSERT_TRUE(!utility::containsElement<FilePath>(
        result, FilePath(L"data/UtilityMavenTestSuite/nested_maven_project/module_1/src/test/java").makeAbsolute()));

    ASSERT_TRUE(!utility::containsElement<FilePath>(
        result, FilePath(L"data/UtilityMavenTestSuite/nested_maven_project/module_2/src/test/java").makeAbsolute()));
  }
}

TEST(UtilityMaven, MavenWrapperDetectsSourceAndTestDirectoriesOfNestedModules) {
  std::shared_ptr<PathDetector> mavenPathDetector = utility::getMavenExecutablePathDetector();
  ASSERT_TRUE(!mavenPathDetector->getPaths().empty());

  if(!mavenPathDetector->getPaths().empty()) {
    std::vector<FilePath> result = utility::mavenGetAllDirectoriesFromEffectivePom(
        mavenPathDetector->getPaths().front(),
        FilePath(),
        FilePath(L"data/UtilityMavenTestSuite/nested_maven_project"),
        FilePath(L"data/UtilityMavenTestSuite").makeAbsolute(),
        true);

    ASSERT_TRUE(utility::containsElement<FilePath>(
        result, FilePath(L"data/UtilityMavenTestSuite/nested_maven_project/module_1/src/main/java").makeAbsolute()));

    ASSERT_TRUE(utility::containsElement<FilePath>(
        result, FilePath(L"data/UtilityMavenTestSuite/nested_maven_project/module_2/src/main/java").makeAbsolute()));

    ASSERT_TRUE(utility::containsElement<FilePath>(
        result, FilePath(L"data/UtilityMavenTestSuite/nested_maven_project/module_1/src/test/java").makeAbsolute()));

    ASSERT_TRUE(utility::containsElement<FilePath>(
        result, FilePath(L"data/UtilityMavenTestSuite/nested_maven_project/module_2/src/test/java").makeAbsolute()));
  }
}
