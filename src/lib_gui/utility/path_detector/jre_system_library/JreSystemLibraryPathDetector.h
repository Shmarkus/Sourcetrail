#pragma once

#include <memory>

#include "PathDetector.h"

class JavaPathDetector;

class JreSystemLibraryPathDetector : public PathDetector {
public:
  explicit JreSystemLibraryPathDetector(std::shared_ptr<JavaPathDetector> javaPathDetector);

private:
  std::vector<FilePath> doGetPaths() const override;

  std::shared_ptr<JavaPathDetector> m_javaPathDetector;
};
