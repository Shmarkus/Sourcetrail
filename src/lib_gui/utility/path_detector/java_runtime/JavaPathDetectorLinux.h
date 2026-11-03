#pragma once

#include "JavaPathDetector.h"

// Finds libjvm.so of the java on PATH, in JAVA_HOME or in the usual install places. An empty version accepts any.
class JavaPathDetectorLinux : public JavaPathDetector {
public:
  explicit JavaPathDetectorLinux(const std::string& javaVersion);

private:
  std::vector<FilePath> doGetPaths() const override;
  FilePath getJavaInPath() const;
  FilePath readLink(const FilePath& path) const;
  FilePath getJavaInJavaHome() const;
  bool checkVersion(const FilePath& path) const;

  FilePath getFilePathRelativeToJavaExecutable(const FilePath& javaExecutablePath) const;
};
