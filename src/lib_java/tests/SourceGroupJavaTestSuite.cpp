#include <algorithm>
#include <fstream>

#include <gtest/gtest.h>

#include "Application.h"
#include "ApplicationSettings.h"
#include "AppPath.h"
#include "Factory.hpp"
#include "IndexerCommandJava.h"
#include "JavaEnvironmentFactory.h"
#include "JavaTestEnvironment.h"
#include "ProjectSettings.h"
#include "SourceGroupJavaEmpty.h"
#include "SourceGroupJavaGradle.h"
#include "SourceGroupJavaMaven.h"
#include "SourceGroupSettingsJavaEmpty.h"
#include "SourceGroupSettingsJavaGradle.h"
#include "SourceGroupSettingsJavaMaven.h"
#include "TextAccess.h"
#include "utilityJava.h"
#include "utilityPathDetection.h"
#include "utilityString.h"
#include "Version.h"

// The Java cases of master's SourceGroupTestSuite; tests/integration/lib/SourceGroupTestSuite.cpp holds the others.
namespace {
FilePath getInputDirectoryPath(const std::wstring& projectName) {
  return FilePath(L"data/SourceGroupTestSuite/" + projectName + L"/input").makeAbsolute().makeCanonical();
}

FilePath getOutputDirectoryPath(const std::wstring& projectName) {
  return FilePath(L"data/SourceGroupTestSuite/" + projectName + L"/expected_output").makeAbsolute().makeCanonical();
}

std::wstring indexerCommandJavaToString(std::shared_ptr<const IndexerCommandJava> indexerCommand, const FilePath& baseDirectory) {
  std::wstring result;
  result += L"SourceFilePath: \"" + indexerCommand->getSourceFilePath().getRelativeTo(baseDirectory).wstr() + L"\"\n";
  result += L"\tLanguageStandard: \"" + indexerCommand->getLanguageStandard() + L"\"\n";
  for(const FilePath& classPathItem : indexerCommand->getClassPath()) {
    result += L"\tClassPathItem: \"" + classPathItem.getRelativeTo(baseDirectory).wstr() + L"\"\n";
  }
  return result;
}

std::wstring indexerCommandToString(std::shared_ptr<IndexerCommand> indexerCommand, const FilePath& baseDirectory) {
  if(indexerCommand) {
    if(std::shared_ptr<const IndexerCommandJava> indexerCommandJava = std::dynamic_pointer_cast<const IndexerCommandJava>(
           indexerCommand)) {
      return indexerCommandJavaToString(indexerCommandJava, baseDirectory);
    }
    return L"Unsupported indexer command type: " +
        utility::decodeFromUtf8(indexerCommandTypeToString(indexerCommand->getIndexerCommandType()));
  }
  return L"No IndexerCommand provided.";
}

std::shared_ptr<TextAccess> generateExpectedOutput(const std::wstring& projectName, std::shared_ptr<const SourceGroup> sourceGroup) {
  const FilePath projectDataRoot = getInputDirectoryPath(projectName).makeAbsolute();

  RefreshInfo info;
  info.filesToIndex = sourceGroup->getAllSourceFilePaths();
  std::vector<std::shared_ptr<IndexerCommand>> indexerCommands = sourceGroup->getIndexerCommands(info);

  std::sort(
      indexerCommands.begin(), indexerCommands.end(), [](std::shared_ptr<IndexerCommand> a, std::shared_ptr<IndexerCommand> b) {
        return a->getSourceFilePath().wstr() < b->getSourceFilePath().wstr();
      });

  std::wstring outputString;
  for(std::shared_ptr<IndexerCommand> indexerCommand : indexerCommands) {
    outputString += indexerCommandToString(indexerCommand, projectDataRoot);
  }

  return TextAccess::createFromString(utility::encodeToUtf8(outputString));
}

void generateAndCompareExpectedOutput(const std::wstring& projectName, std::shared_ptr<const SourceGroup> sourceGroup) {
  const std::shared_ptr<const TextAccess> output = generateExpectedOutput(projectName, std::move(sourceGroup));
  const FilePath expectedOutputFilePath = getOutputDirectoryPath(projectName).concatenate(L"output_unix.txt");
  if(!expectedOutputFilePath.exists()) {
    std::ofstream expectedOutputFile;
    expectedOutputFile.open(expectedOutputFilePath.str());
    expectedOutputFile << output->getText();
    expectedOutputFile.close();
  } else {
    const std::shared_ptr<const TextAccess> expectedOutput = TextAccess::createFromFile(expectedOutputFilePath);
    ASSERT_EQ(expectedOutput->getLineCount(), output->getLineCount())
        << "Output does not match the expected line count for project \"" << utility::encodeToUtf8(projectName)
        << "\". Output was: " << output->getText();
    for(unsigned int i = 1; i <= expectedOutput->getLineCount(); i++) {
      EXPECT_EQ(expectedOutput->getLine(i), output->getLine(i));
    }
  }
}

// The Java source groups ask the Application for its dialog view. No shared memory garbage collector, which
// destroyInstance would leave running.
struct HeadlessFactory : lib::Factory {
  lib::ISharedMemoryGarbageCollector::Ptr createSharedMemoryGarbageCollector() noexcept override {
    return nullptr;
  }
};
}    // namespace

// master sets up the Java environment in "can setup environment factory", creates the Application in "can create
// application instance" and destroys it in "can destroy application instance"; ctest runs every case in its own
// process, so the cases that need them do that here.
struct SourceGroupJavaFix : testing::Test {
  void SetUp() override {
    // before createInstance, which replaces the application settings that name the JVM
    ASSERT_EQ("", java_test::setupJavaEnvironmentFactory());
    // loads the application settings from the user paths
    Application::createInstance(Version(), std::make_shared<HeadlessFactory>(), nullptr, nullptr);
    ASSERT_TRUE(Application::getInstance().use_count() >= 1);
  }

  void TearDown() override {
    if(!Application::getInstance()) {
      return;    // SetUp failed before createInstance
    }
    Application::destroyInstance();
    ASSERT_TRUE(0 == Application::getInstance().use_count());
  }
};

TEST(SourceGroupJava, FindsAllJarDependencies) {
  for(const std::wstring& jarName : utility::getRequiredJarNames()) {
    FilePath jarPath = FilePath(L"../app/data/java/lib/").concatenate(jarName + L".jar");
    ASSERT_TRUE(jarPath.exists()) << "Jar dependency path does not exist: " << jarPath.str();
  }
}

TEST(SourceGroupJava, CanSetupEnvironmentFactory) {
  const std::string errorString = java_test::setupJavaEnvironmentFactory();

  ASSERT_TRUE("" == errorString);

  // if this one fails, maybe your java_path in the test settings is wrong.
  ASSERT_TRUE(JavaEnvironmentFactory::getInstance().use_count() >= 1);
}

TEST_F(SourceGroupJavaFix, SourcegroupJavaEmptyGeneratesExpectedOutput) {
  const std::wstring projectName = L"java_empty";

  ProjectSettings projectSettings;
  projectSettings.setProjectFilePath(L"non_existent_project", getInputDirectoryPath(projectName));

  std::shared_ptr<SourceGroupSettingsJavaEmpty> sourceGroupSettings = std::make_shared<SourceGroupSettingsJavaEmpty>(
      "fake_id", &projectSettings);
  sourceGroupSettings->setSourceExtensions({L".java"});
  sourceGroupSettings->setExcludeFilterStrings({L"**/Foo.java"});
  sourceGroupSettings->setJavaStandard({L"10"});
  sourceGroupSettings->setSourcePaths({getInputDirectoryPath(projectName).concatenate(L"src")});
  sourceGroupSettings->setUseJreSystemLibrary(true);
  sourceGroupSettings->setClasspath({getInputDirectoryPath(projectName).concatenate(L"lib/dependency.jar"),
                                     getInputDirectoryPath(projectName).concatenate(L"classpath_dir")});

  auto* applicationSettings = IApplicationSettings::getInstanceRaw();

  std::vector<FilePath> storedJreSystemLibraryPaths = applicationSettings->getJreSystemLibraryPaths();

  applicationSettings->setJreSystemLibraryPaths({FilePath(L"test/jre/system/library/path.jar")});

  generateAndCompareExpectedOutput(projectName, std::make_shared<SourceGroupJavaEmpty>(sourceGroupSettings));

  applicationSettings->setJreSystemLibraryPaths(storedJreSystemLibraryPaths);
}

TEST(SourceGroupJava, JavaClassPathPutsTheSourceFilesOwnRootFirst) {
  const std::vector<FilePath> classPath = {FilePath(L"/p/a/src/test/java"),
                                           FilePath(L"/libs/x.jar"),
                                           FilePath(L"/p/b/src/test/java"),
                                           FilePath(L"/p/b/src/test/java/gen")};

  const std::vector<FilePath> ownRootFirst = {FilePath(L"/p/b/src/test/java"),
                                              FilePath(L"/p/a/src/test/java"),
                                              FilePath(L"/libs/x.jar"),
                                              FilePath(L"/p/b/src/test/java/gen")};
  ASSERT_TRUE(utility::withOwnRootFirst(classPath, FilePath(L"/p/b/src/test/java/q/Helper.java")) == ownRootFirst);
  // the innermost root holding the file wins
  ASSERT_TRUE(utility::withOwnRootFirst(classPath, FilePath(L"/p/b/src/test/java/gen/q/G.java")).front() ==
              FilePath(L"/p/b/src/test/java/gen"));
  ASSERT_TRUE(utility::withOwnRootFirst(classPath, FilePath(L"/p/a/src/test/java/q/A.java")) == classPath);
  ASSERT_TRUE(utility::withOwnRootFirst(classPath, FilePath(L"/p/b/src/test/javax/q/A.java")) == classPath);
}

TEST_F(SourceGroupJavaFix, SourcegroupJavaGradleGeneratesExpectedOutput) {
#ifdef __linux__
  GTEST_SKIP() << "master runs this case on Windows and macOS only";
#else
  const std::wstring projectName = L"java_gradle";

  ProjectSettings projectSettings;
  projectSettings.setProjectFilePath(L"non_existent_project", getInputDirectoryPath(projectName));

  std::shared_ptr<SourceGroupSettingsJavaGradle> sourceGroupSettings = std::make_shared<SourceGroupSettingsJavaGradle>(
      "fake_id", &projectSettings);
  sourceGroupSettings->setSourceExtensions({L".java"});
  sourceGroupSettings->setExcludeFilterStrings({L"**/HelloWorld.java"});
  sourceGroupSettings->setJavaStandard({L"10"});
  sourceGroupSettings->setGradleProjectFilePath({getInputDirectoryPath(projectName).concatenate(L"build.gradle")});
  sourceGroupSettings->setShouldIndexGradleTests(true);

  auto* applicationSettings = IApplicationSettings::getInstanceRaw();

  const FilePath storedAppPath = AppPath::getSharedDataDirectoryPath();
  AppPath::setSharedDataDirectoryPath(storedAppPath.getConcatenated(L"../app").makeAbsolute());

  std::vector<FilePath> storedJreSystemLibraryPaths = applicationSettings->getJreSystemLibraryPaths();
  applicationSettings->setJreSystemLibraryPaths({FilePath(L"test/jre/system/library/path.jar")});

  generateAndCompareExpectedOutput(projectName, std::make_shared<SourceGroupJavaGradle>(sourceGroupSettings));

  applicationSettings->setJreSystemLibraryPaths(storedJreSystemLibraryPaths);
  AppPath::setSharedDataDirectoryPath(storedAppPath);
#endif
}

TEST_F(SourceGroupJavaFix, SourcegroupJavaMavenGeneratesExpectedOutput) {
  std::vector<FilePath> mavenPaths = utility::getMavenExecutablePathDetector()->getPaths();

  ASSERT_TRUE(!mavenPaths.empty());

  if(!mavenPaths.empty()) {
    IApplicationSettings::getInstanceRaw()->setMavenPath(mavenPaths.front());
  }

  const std::wstring projectName = L"java_maven";

  ProjectSettings projectSettings;
  projectSettings.setProjectFilePath(L"non_existent_project", getInputDirectoryPath(projectName));

  std::shared_ptr<SourceGroupSettingsJavaMaven> sourceGroupSettings = std::make_shared<SourceGroupSettingsJavaMaven>(
      "fake_id", &projectSettings);
  sourceGroupSettings->setSourceExtensions({L".java"});
  sourceGroupSettings->setExcludeFilterStrings({L"**/Foo.java"});
  sourceGroupSettings->setJavaStandard({L"10"});
  sourceGroupSettings->setMavenProjectFilePath({getInputDirectoryPath(projectName).concatenate(L"my-app/pom.xml")});
  sourceGroupSettings->setShouldIndexMavenTests(true);

  auto* applicationSettings = IApplicationSettings::getInstanceRaw();

  std::vector<FilePath> storedJreSystemLibraryPaths = applicationSettings->getJreSystemLibraryPaths();

  applicationSettings->setJreSystemLibraryPaths({FilePath(L"test/jre/system/library/path.jar")});

  generateAndCompareExpectedOutput(projectName, std::make_shared<SourceGroupJavaMaven>(sourceGroupSettings));

  applicationSettings->setJreSystemLibraryPaths(storedJreSystemLibraryPaths);
}
