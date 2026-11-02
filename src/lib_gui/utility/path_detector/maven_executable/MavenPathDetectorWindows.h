#pragma once

#include "PathDetector.h"

class MavenPathDetectorWindows : public PathDetector {
public:
  MavenPathDetectorWindows();

private:
  std::vector<FilePath> doGetPaths() const override;
};
