#include <chrono>
#include <cstdlib>
#include <filesystem>

#include <gtest/gtest.h>

#include "FilePath.h"
#include "utilityApp.h"
#include "utilityString.h"

TEST(utilityAppTestSuite, searchPath) {
#ifdef D_WINDOWS
  const std::filesystem::path exec = "";
  const std::filesystem::path full_exec = "";
#else
  const std::filesystem::path exec = "bash";
  const std::filesystem::path full_exec = "/usr/bin/bash";
#endif
  EXPECT_EQ(utility::searchPath(exec), full_exec);
}

TEST(utilityAppTestSuite, emptyPath) {
#ifdef D_WINDOWS
  const std::filesystem::path exec = "";
  const std::filesystem::path full_exec = "";
#else
  const std::filesystem::path exec = "";
  const std::filesystem::path full_exec = "";
#endif
  EXPECT_EQ(utility::searchPath(exec), full_exec);
}

TEST(utilityAppTestSuite, getOsType) {
#if defined(_WIN32)
  EXPECT_EQ(utility::getOsType(), OsType::Windows);
#elif defined(__APPLE__)
  EXPECT_EQ(utility::getOsType(), OsType::Mac);
#elif defined(__linux__)
  EXPECT_EQ(utility::getOsType(), OsType::Linux);
#endif
}

TEST(utilityAppTestSuite, getApplicationArchitectureType) {
#if defined(__x86_64__) || defined(_M_X64)
  EXPECT_EQ(utility::getApplicationArchitectureType(), ApplicationArchitectureType::X86_64);
#elif defined(__aarch64__) || defined(__arm__) || defined(_M_ARM64)
  EXPECT_EQ(utility::getApplicationArchitectureType(), ApplicationArchitectureType::ARM);
#endif
}
TEST(utilityAppTestSuite, executeProcessReturnsWhenAShortProcessExitsInsteadOfWaitingOutTheTimeout) {
  // A JVM closes its output a moment before it exits, which is when the exit signal used to get lost.
  const char* javaHome = std::getenv("JAVA_HOME");
  const FilePath java = javaHome != nullptr ? FilePath(std::string(javaHome)).getConcatenated(L"bin/java") :
                                              FilePath(utility::searchPath("java").wstring());
  if(!java.exists()) {
    GTEST_SKIP() << "no Java found";
  }
  size_t slow = 0;
  for(int i = 0; i < 40; i++) {
    const auto start = std::chrono::steady_clock::now();
    const utility::ProcessOutput output = utility::executeProcess(java.wstr(), {L"-version"}, FilePath(), true, 3000);
    ASSERT_EQ(0, output.exitCode);
    slow += std::chrono::steady_clock::now() - start > std::chrono::milliseconds(2000) ? 1 : 0;
  }
  EXPECT_EQ(0U, slow);
}

#ifndef D_WINDOWS
TEST(utilityAppTestSuite, executeProcessPassesNonAsciiArgumentsAndWorkingDirectory) {
  const std::filesystem::path directory = std::filesystem::temp_directory_path() / L"utilityAppTestSuite-ünïcödé";
  std::filesystem::create_directories(directory);

  const utility::ProcessOutput output = utility::executeProcess(
      L"/bin/sh", {L"-c", L"printf %s \"$PWD:$1\"", L"_", L"väärtus-ü"}, FilePath(directory.wstring()));
  std::filesystem::remove(directory);

  EXPECT_EQ(0, output.exitCode) << utility::encodeToUtf8(output.error);
  EXPECT_EQ(directory.wstring() + L":väärtus-ü", output.output);
}
#endif
