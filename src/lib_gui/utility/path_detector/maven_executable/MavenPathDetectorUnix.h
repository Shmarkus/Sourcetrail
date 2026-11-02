#pragma once

#include "PathDetector.h"

class MavenPathDetectorUnix : public PathDetector {
public:
  MavenPathDetectorUnix();

private:
  std::vector<FilePath> doGetPaths() const override;
};
