#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "FilePath.h"
#include "JavaPathDetector.h"
#include "JavaPathDetectorLinux.h"

namespace fs = std::filesystem;

TEST(JavaPathDetector, readsTheMajorVersion) {
  EXPECT_EQ(8, JavaPathDetector::majorVersion(L"openjdk version \"1.8.0_412\"\nOpenJDK Runtime Environment"));
  EXPECT_EQ(11, JavaPathDetector::majorVersion(L"openjdk version \"11.0.23\" 2024-04-16"));
  EXPECT_EQ(17, JavaPathDetector::majorVersion(L"java version \"17.0.10\" 2024-01-16 LTS"));
  EXPECT_EQ(21, JavaPathDetector::majorVersion(L"openjdk version \"21.0.3\" 2024-04-16 LTS"));
  EXPECT_EQ(21, JavaPathDetector::majorVersion(L"openjdk version \"21\" 2023-09-19"));
  EXPECT_EQ(25, JavaPathDetector::majorVersion(L"Picked up JAVA_TOOL_OPTIONS: -Xmx1g\nopenjdk version \"25\" 2025-09-16"));
  EXPECT_EQ(9, JavaPathDetector::majorVersion(L"java version \"9-ea\""));
}

TEST(JavaPathDetector, withoutAVersionGivesZero) {
  EXPECT_EQ(0, JavaPathDetector::majorVersion(L""));
  EXPECT_EQ(0, JavaPathDetector::majorVersion(L"bash: java: command not found"));
  EXPECT_EQ(0, JavaPathDetector::majorVersion(L"openjdk version \"\""));
}

#ifdef __linux__
namespace {

class ScopedEnvironment {
public:
  ScopedEnvironment(const char* name, const std::string& value) : mName(name) {
    if(const char* previous = std::getenv(name); previous != nullptr) {    // NOLINT(concurrency-mt-unsafe)
      mPrevious = previous;
    }
    ::setenv(name, value.c_str(), 1);    // NOLINT(concurrency-mt-unsafe)
  }
  ScopedEnvironment(const ScopedEnvironment&) = delete;
  ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;
  ~ScopedEnvironment() {
    if(mPrevious) {
      ::setenv(mName, mPrevious->c_str(), 1);    // NOLINT(concurrency-mt-unsafe)
    } else {
      ::unsetenv(mName);                         // NOLINT(concurrency-mt-unsafe)
    }
  }

private:
  const char* mName;
  std::optional<std::string> mPrevious;
};

// A JDK whose bin/java reports the given version and whose lib/server/libjvm.so exists.
fs::path fakeJdk(const fs::path& root, const std::string& version) {
  fs::create_directories(root / "bin");
  fs::create_directories(root / "lib" / "server");
  std::ofstream{root / "lib" / "server" / "libjvm.so"} << "";
  std::ofstream{root / "bin" / "java"} << "#!/bin/sh\necho 'openjdk version \"" << version << "\" 2024-01-01' >&2\n";
  fs::permissions(root / "bin" / "java", fs::perms::owner_all, fs::perm_options::add);
  return fs::canonical(root / "lib" / "server" / "libjvm.so");
}

class JavaPathDetectorLinuxTest : public testing::Test {
protected:
  void SetUp() override {
    mRoot = fs::temp_directory_path() / "sourcetrail-java-path-detector";
    fs::remove_all(mRoot);
    mOldJvm = fakeJdk(mRoot / "jdk-17", "17.0.2");
    mNewJvm = fakeJdk(mRoot / "jdk-21", "21.0.3");
  }

  void TearDown() override {
    fs::remove_all(mRoot);
  }

  fs::path mRoot;
  fs::path mOldJvm;
  fs::path mNewJvm;
};

}    // namespace

TEST_F(JavaPathDetectorLinuxTest, skipsAJavaOnThePathThatIsTooOld) {
  const char* path = std::getenv("PATH");    // NOLINT(concurrency-mt-unsafe)
  ScopedEnvironment pathEnvironment("PATH", (mRoot / "jdk-17" / "bin").string() + ":" + (path ? path : ""));
  ScopedEnvironment javaHome("JAVA_HOME", (mRoot / "jdk-21").string());

  const std::vector<FilePath> paths = JavaPathDetectorLinux(21).getPaths();

  ASSERT_EQ(1U, paths.size());
  EXPECT_EQ(FilePath(mNewJvm.wstring()), paths.front());
}

TEST_F(JavaPathDetectorLinuxTest, takesTheJavaOnThePathWhenItIsRecentEnough) {
  const char* path = std::getenv("PATH");    // NOLINT(concurrency-mt-unsafe)
  ScopedEnvironment pathEnvironment("PATH", (mRoot / "jdk-17" / "bin").string() + ":" + (path ? path : ""));
  ScopedEnvironment javaHome("JAVA_HOME", (mRoot / "jdk-21").string());

  const std::vector<FilePath> paths = JavaPathDetectorLinux(17).getPaths();

  ASSERT_EQ(1U, paths.size());
  EXPECT_EQ(FilePath(mOldJvm.wstring()), paths.front());
}
#endif    // __linux__
