#include "ProjectSetup.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <system_error>

#include "logging.h"
#include "ProjectSettings.h"
#include "SourceGroupSettingsJavaEmpty.h"
#include "SourceGroupSettingsJavaGradle.h"
#include "SourceGroupSettingsJavaMaven.h"
#include "SourceGroupSettingsTypeScript.h"
#include "utilityUuid.h"

namespace fs = std::filesystem;

namespace {

constexpr auto ProjectFolderName = L".sourcetrail";
constexpr auto TsconfigName = L"tsconfig.json";

// Newest first, as the source group settings list them; an auto-created project has no one to ask, and the newest
// level reads the widest range of code.
constexpr auto JavaStandard = L"21";

fs::path toPath(const FilePath& path) {
  return fs::path{path.wstr()}.lexically_normal();
}

// The folder without a trailing separator, so its file name is the folder's name.
fs::path folderPath(const FilePath& folder) {
  fs::path path = toPath(folder);
  if(!path.has_filename() && path.has_parent_path()) {
    path = path.parent_path();
  }
  return path;
}

bool isFile(const fs::path& path) {
  std::error_code error;
  return fs::is_regular_file(path, error);
}

// The Gradle file the source group points at; Gradle builds from its directory, so any of them will do.
fs::path gradleFile(const fs::path& folder) {
  for(const auto* name : {L"build.gradle", L"build.gradle.kts", L"settings.gradle", L"settings.gradle.kts"}) {
    if(isFile(folder / name)) {
      return folder / name;
    }
  }
  return {};
}

// Relative to the project file's directory, so the checkout can move.
FilePath relativeToProject(const fs::path& path, const fs::path& projectDirectory) {
  return FilePath{path.lexically_relative(projectDirectory).wstring()};
}

std::shared_ptr<SourceGroupSettings> createJavaSourceGroup(const fs::path& folder,
                                                           JavaBuildKind kind,
                                                           const ProjectSettings* projectSettings) {
  const fs::path projectDirectory = folder / ProjectFolderName;
  const std::string id = utility::getUuidString();

  switch(kind) {
  case JavaBuildKind::Gradle: {
    auto settings = std::make_shared<SourceGroupSettingsJavaGradle>(id, projectSettings);
    settings->setName("Gradle");
    settings->setGradleProjectFilePath(relativeToProject(gradleFile(folder), projectDirectory));
    settings->setShouldIndexGradleTests(true);
    settings->setJavaStandard(JavaStandard);
    return settings;
  }
  case JavaBuildKind::Maven: {
    auto settings = std::make_shared<SourceGroupSettingsJavaMaven>(id, projectSettings);
    settings->setName("Maven");
    settings->setMavenProjectFilePath(relativeToProject(folder / "pom.xml", projectDirectory));
    settings->setShouldIndexMavenTests(true);
    settings->setJavaStandard(JavaStandard);
    return settings;
  }
  case JavaBuildKind::None: {
    auto settings = std::make_shared<SourceGroupSettingsJavaEmpty>(id, projectSettings);
    settings->setName("Java");
    // absolute: the settings page checks it before .sourcetrail/ exists, which a relative ".." needs
    settings->setSourcePaths({FilePath{folder.wstring()}});
    settings->setExcludeFilterStrings({L"**/" + std::wstring{ProjectFolderName} + L"/**"});
    settings->setJavaStandard(JavaStandard);
    return settings;
  }
  }
  return nullptr;
}

std::shared_ptr<SourceGroupSettings> createTypeScriptSourceGroup(const fs::path& folder, const ProjectSettings* projectSettings) {
  auto settings = std::make_shared<SourceGroupSettingsTypeScript>(utility::getUuidString(), projectSettings);
  settings->setName("TypeScript");
  settings->setTsconfigPath(relativeToProject(folder / TsconfigName, folder / ProjectFolderName));
  return settings;
}

}    // namespace

JavaBuildKind detectJavaBuild(const FilePath& folder) {
  const fs::path path = folderPath(folder);
  if(!gradleFile(path).empty()) {
    return JavaBuildKind::Gradle;
  }
  if(isFile(path / "pom.xml")) {
    return JavaBuildKind::Maven;
  }
  return JavaBuildKind::None;
}

DetectedBuild detectBuild(const FilePath& folder) {
  return {.java = detectJavaBuild(folder), .typeScript = isFile(folderPath(folder) / TsconfigName)};
}

FilePath projectFileFor(const FilePath& folder) {
  const fs::path path = folderPath(folder);
  return FilePath{(path / ProjectFolderName / path.filename()).wstring() + L".srctrlprj"};
}

FilePath existingProjectFile(const FilePath& folder) {
  std::error_code error;
  fs::directory_iterator entries{folderPath(folder) / ProjectFolderName, error};
  if(error) {
    return {};
  }

  std::vector<fs::path> projectFiles;
  for(const auto& entry : entries) {
    if(entry.path().extension() == ".srctrlprj" && isFile(entry.path())) {
      projectFiles.push_back(entry.path());
    }
  }
  if(projectFiles.empty()) {
    return {};
  }
  return FilePath{std::ranges::min(projectFiles).wstring()};
}

FilePath rootTsconfigFor(const FilePath& projectDirectory) {
  const fs::path directory = folderPath(projectDirectory);
  if(!directory.is_absolute()) {
    return {};    // no project location yet, which would read the working directory
  }
  for(const fs::path& candidate : {directory, directory.parent_path()}) {
    if(isFile(candidate / TsconfigName) && (candidate == directory || directory.filename() == ProjectFolderName)) {
      return FilePath{(candidate / TsconfigName).wstring()};
    }
  }
  return {};
}

FilePath createProject(const FilePath& folder, const DetectedBuild& build) {
  if(!prepareProjectFolder(folder)) {
    return {};
  }

  const auto settings = projectSettingsFor(folder, build);
  if(!settings->save(settings->getFilePath())) {
    LOG_ERROR("Cannot write {}", settings->getFilePath().str());
    return {};
  }
  return settings->getFilePath();
}

std::shared_ptr<ProjectSettings> projectSettingsFor(const FilePath& folder, const DetectedBuild& build) {
  const fs::path path = folderPath(folder);
  auto settings = std::make_shared<ProjectSettings>();
  settings->setProjectFilePath(path.filename().wstring(), FilePath{(path / ProjectFolderName).wstring()});
  settings->setVersion(ProjectSettings::VERSION);

  std::vector<std::shared_ptr<SourceGroupSettings>> groups;
  if(build.java != JavaBuildKind::None || !build.typeScript) {
    groups.push_back(createJavaSourceGroup(path, build.java, settings.get()));
  }
  if(build.typeScript) {
    groups.push_back(createTypeScriptSourceGroup(path, settings.get()));
  }
  settings->setAllSourceGroupSettings(groups);
  return settings;
}

bool addTypeScriptGroup(const FilePath& projectFile, const FilePath& folder) {
  ProjectSettings settings;
  if(!settings.load(projectFile)) {
    LOG_ERROR("Cannot read {}", projectFile.str());
    return false;
  }

  auto groups = settings.getAllSourceGroupSettings();
  groups.push_back(createTypeScriptSourceGroup(folderPath(folder), &settings));
  settings.setAllSourceGroupSettings(groups);
  if(!settings.save(projectFile)) {
    LOG_ERROR("Cannot write {}", projectFile.str());
    return false;
  }
  return true;
}

bool prepareProjectFolder(const FilePath& folder) {
  const fs::path projectDirectory = folderPath(folder) / ProjectFolderName;

  std::error_code error;
  fs::create_directories(projectDirectory, error);
  if(error) {
    LOG_ERROR("Cannot create {}: {}", projectDirectory.string(), error.message());
    return false;
  }

  std::ofstream gitignore{projectDirectory / ".gitignore", std::ios::binary | std::ios::trunc};
  gitignore << "*\n";
  if(!gitignore) {
    LOG_ERROR("Cannot write {}", (projectDirectory / ".gitignore").string());
    return false;
  }
  return true;
}

ProjectChoice openOrCreateProject(const FilePath& folder, const std::function<bool()>& acceptPlainJava) {
  const DetectedBuild build = detectBuild(folder);

  if(FilePath existing = existingProjectFile(folder); !existing.empty()) {
    bool hasTypeScript = false;
    ProjectSettings settings;
    if(settings.load(existing)) {
      hasTypeScript = std::ranges::any_of(
          settings.getAllSourceGroupSettings(), [](const auto& group) { return group->getType() == SOURCE_GROUP_TYPESCRIPT; });
    }
    return {.projectFile = existing, .canAddTypeScript = build.typeScript && !hasTypeScript, .build = build};
  }

  if(build.java == JavaBuildKind::None && !build.typeScript) {
    if(!acceptPlainJava()) {
      return {};
    }
    return {.projectFile = projectFileFor(folder), .needsSetup = true, .build = build};
  }

  FilePath projectFile = createProject(folder, build);
  const bool created = !projectFile.empty();
  return {.projectFile = std::move(projectFile), .created = created, .build = build};
}
