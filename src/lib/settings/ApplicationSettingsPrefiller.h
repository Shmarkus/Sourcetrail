#pragma once

class ApplicationSettings;

class ApplicationSettingsPrefiller {
public:
  static void prefillPaths(ApplicationSettings* settings);

private:
	static bool prefillJavaRuntimePath(ApplicationSettings* settings);
	static bool prefillJreSystemLibraryPaths(ApplicationSettings* settings);
	static bool prefillMavenExecutablePath(ApplicationSettings* settings);
  static bool prefillCxxHeaderPaths(ApplicationSettings* settings);
  static bool prefillCxxFrameworkPaths(ApplicationSettings* settings);
};