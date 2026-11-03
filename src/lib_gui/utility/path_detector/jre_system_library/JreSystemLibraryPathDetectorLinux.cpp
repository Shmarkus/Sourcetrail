#include "JreSystemLibraryPathDetectorLinux.h"

#include "JavaPathDetectorLinux.h"

JreSystemLibraryPathDetectorLinux::JreSystemLibraryPathDetectorLinux(int minimumMajorVersion)
    : JreSystemLibraryPathDetector(std::make_shared<JavaPathDetectorLinux>(minimumMajorVersion)) {}
