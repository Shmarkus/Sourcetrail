#pragma once

struct IApplicationSettings;

class ApplicationSettingsPrefiller final {
public:
  static void prefillPaths(IApplicationSettings* settings);

private:
  static bool prefillCxxHeaderPaths(IApplicationSettings* settings);
  static bool prefillCxxFrameworkPaths(IApplicationSettings* settings);
  static bool prefillJavaRuntimePath(IApplicationSettings* settings);
  static bool prefillJreSystemLibraryPaths(IApplicationSettings* settings);
  static bool prefillMavenExecutablePath(IApplicationSettings* settings);
};