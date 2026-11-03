#pragma once

#include "JavaPathDetector.h"

// Finds libjvm.so of the first java on PATH, in JAVA_HOME or in the usual install places whose major version is at
// least the given one.
class JavaPathDetectorLinux : public JavaPathDetector {
public:
  explicit JavaPathDetectorLinux(int minimumMajorVersion);

private:
  std::vector<FilePath> doGetPaths() const override;
  FilePath getJavaInPath() const;
  FilePath readLink(const FilePath& path) const;
  FilePath getJavaInJavaHome() const;
  bool checkVersion(const FilePath& path) const;

  FilePath getFilePathRelativeToJavaExecutable(const FilePath& javaExecutablePath) const;
};
