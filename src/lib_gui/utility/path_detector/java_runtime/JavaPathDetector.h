#pragma once

#include <string>

#include "PathDetector.h"

class JavaPathDetector : public PathDetector {
public:
  JavaPathDetector(const std::string& name, int minimumMajorVersion);

  // The major version in `java -version` output ("1.8.0_412" gives 8, "21.0.3" gives 21), or 0 when there is none.
  static int majorVersion(const std::wstring& versionOutput);

protected:
  const int m_minimumMajorVersion;
};
