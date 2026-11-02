#include <gtest/gtest.h>

#include "utilityApp.h"

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