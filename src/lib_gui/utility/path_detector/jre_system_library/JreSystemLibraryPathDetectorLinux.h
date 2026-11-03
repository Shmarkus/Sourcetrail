#pragma once

#include "JreSystemLibraryPathDetector.h"

class JreSystemLibraryPathDetectorLinux : public JreSystemLibraryPathDetector {
public:
  explicit JreSystemLibraryPathDetectorLinux(const std::string& javaVersion);
};
