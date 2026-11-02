#pragma once
#include <filesystem>
#include <string>

#include <boost/predef.h>

#include "ApplicationArchitectureType.h"
#include "FilePath.h"
#include "OsType.h"

namespace utility {

struct ProcessOutput final {
  std::wstring output;
  std::wstring error;
  int exitCode;
};

std::filesystem::path searchPath(const std::filesystem::path& bin, bool& isOk);

std::filesystem::path searchPath(const std::filesystem::path& bin);

ProcessOutput executeProcess(const std::wstring& command,
                             const std::vector<std::wstring>& arguments,
                             const FilePath& workingDirectory = {},
                             const bool waitUntilNoOutput = false,
                             const int timeout = 30000,
                             bool logProcessOutput = false);

void killRunningProcesses();

int getIdealThreadCount();

// boost/predef defines every BOOST_OS_* and BOOST_ARCH_* macro (0 when it does not apply), so test their values.
constexpr OsType getOsType() {
#if BOOST_OS_WINDOWS
  return OsType::Windows;
#elif BOOST_OS_MACOS
  return OsType::Mac;
#elif BOOST_OS_LINUX
  return OsType::Linux;
#else
  return OsType::Unknown;
#endif
}

constexpr ApplicationArchitectureType getApplicationArchitectureType() {
#if BOOST_ARCH_X86_64
  return ApplicationArchitectureType::X86_64;
#elif BOOST_ARCH_X86_32
  return ApplicationArchitectureType::X86_32;
#elif BOOST_ARCH_ARM
  return ApplicationArchitectureType::ARM;
#else
  return ApplicationArchitectureType::Unknown;
#endif
}

std::string getAppArchTypeString(ApplicationArchitectureType archType = getApplicationArchitectureType());

}    // namespace utility
