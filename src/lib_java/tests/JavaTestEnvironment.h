#pragma once
#include <string>

// Shared setup of the Java test suites. Linking JavaTestEnvironment.cpp into a test registers a global gtest
// environment that points the shared data directory at ../app/ (where the jars are staged) and installs the application settings
// of $HOME/.config/sourcetrail/ApplicationSettings.xml (read only) as the IApplicationSettings instance. When those settings name
// no JVM, $JAVA_HOME/lib/server/libjvm.so is used.
namespace java_test {

// Creates the process-wide JavaEnvironmentFactory with the staged jars; returns the error text.
std::string setupJavaEnvironmentFactory();

}    // namespace java_test
