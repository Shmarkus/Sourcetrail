#include "JavaTestEnvironment.h"

#include <cstdlib>

#include <gtest/gtest.h>

#include "ApplicationSettings.h"
#include "AppPath.h"
#include "FilePath.h"
#include "utilityJava.h"

namespace {

struct JavaTestEnvironment : testing::Environment {
  void SetUp() override {
    // the Java package finds its jars under <shared data>/data/java/lib; build/<preset>/app/data links to bin/app/data
    AppPath::setSharedDataDirectoryPath(FilePath(L"../app/").getCanonical());

    auto settings = std::make_shared<ApplicationSettings>();
    if(const char* home = std::getenv("HOME"); home != nullptr) {
      settings->load(FilePath(std::string(home) + "/.config/sourcetrail/ApplicationSettings.xml"), true);
    }
    if(settings->getJavaPath().empty()) {
      if(const char* javaHome = std::getenv("JAVA_HOME"); javaHome != nullptr) {
        settings->setJavaPath(FilePath(std::string(javaHome) + "/lib/server/libjvm.so"));
      }
    }
    IApplicationSettings::setInstance(settings);
  }

  void TearDown() override {
    IApplicationSettings::setInstance(nullptr);
  }
};

[[maybe_unused]] testing::Environment* const environment = testing::AddGlobalTestEnvironment(new JavaTestEnvironment);

}    // namespace

std::string java_test::setupJavaEnvironmentFactory() {
  // the same class path the application uses: <shared data>/data/java/lib/<jar>.jar
  return utility::prepareJavaEnvironment();
}
