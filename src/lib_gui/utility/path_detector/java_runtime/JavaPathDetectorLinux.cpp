#include "JavaPathDetectorLinux.h"

#include <array>
#include <cstdlib>

#include "FilePath.h"
#include "utilityApp.h"
#include "utilityString.h"

namespace {
// JDK 9 and later, then the JDK 8 layout.
#ifdef __x86_64__
constexpr std::array JvmLibPathsRelativeToJavaExecutable = {L"/../lib/server/libjvm.so", L"/../lib/amd64/server/libjvm.so"};
#else
constexpr std::array JvmLibPathsRelativeToJavaExecutable = {L"/../lib/server/libjvm.so", L"/../lib/i386/server/libjvm.so"};
#endif
}    // namespace

JavaPathDetectorLinux::JavaPathDetectorLinux(const std::string& javaVersion)
    : JavaPathDetector(javaVersion.empty() ? "Java for Linux" : "Java " + javaVersion + " for Linux", javaVersion) {}

std::vector<FilePath> JavaPathDetectorLinux::doGetPaths() const {
  std::vector<FilePath> paths;
  FilePath p = getJavaInPath();
  if(!p.empty()) {
    paths.push_back(p);
  }
  p = getJavaInJavaHome();
  if(!p.empty()) {
    paths.push_back(p);
  }

  // some default paths for java
  paths.emplace_back(L"/etc/alternatives/java");
  paths.emplace_back(L"/usr/lib/jvm/default/bin/java");
  paths.emplace_back(L"/usr/lib/jvm/java-openjdk/bin/java");

  for(const FilePath& path : paths) {
    if(checkVersion(path)) {
      FilePath jvmLibrary = getFilePathRelativeToJavaExecutable(readLink(path));
      if(jvmLibrary.exists()) {
        return {jvmLibrary};
      }
    }
  }

  return {};
}

FilePath JavaPathDetectorLinux::getJavaInPath() const {
  bool ok = false;
  FilePath javaPath(utility::searchPath(L"java", ok).wstring());
  if(ok && !javaPath.empty() && javaPath.exists()) {
    return javaPath;
  }
  return {};
}

FilePath JavaPathDetectorLinux::readLink(const FilePath& path) const {
  const utility::ProcessOutput out = utility::executeProcess(L"readlink", std::vector<std::wstring>{L"-f", path.wstr()});

  if(out.exitCode == 0 && !out.output.empty()) {
    FilePath javaPath(utility::trim(out.output));

    if(!javaPath.empty()) {
      return javaPath;
    }
  }
  return {};
}

FilePath JavaPathDetectorLinux::getFilePathRelativeToJavaExecutable(const FilePath& javaExecutablePath) const {
  for(const wchar_t* relativePath : JvmLibPathsRelativeToJavaExecutable) {
    FilePath p = javaExecutablePath.getParentDirectory().concatenate(relativePath);
    if(p.exists()) {
      return p.makeCanonical();
    }
  }
  return {};
}

FilePath JavaPathDetectorLinux::getJavaInJavaHome() const {
  const char* javaHome = std::getenv("JAVA_HOME");    // NOLINT(concurrency-mt-unsafe)
  if(javaHome == nullptr) {
    return {};
  }

  FilePath javaPath(std::string(javaHome) + "/bin/java");
  if(!javaPath.empty() && javaPath.exists()) {
    return javaPath;
  }
  return {};
}

bool JavaPathDetectorLinux::checkVersion(const FilePath& path) const {
  const utility::ProcessOutput out = utility::executeProcess(path.wstr(), {L"-version"});

  return out.exitCode == 0 &&
      (m_javaVersion.empty() || out.output.find(utility::decodeFromUtf8(m_javaVersion)) != std::wstring::npos);
}
