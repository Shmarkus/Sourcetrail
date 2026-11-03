#pragma once

#include "PathDetector.h"

class JavaPathDetector : public PathDetector {
public:
  JavaPathDetector(const std::string& name, const std::string& javaVersion);

protected:
  const std::string m_javaVersion;
};
