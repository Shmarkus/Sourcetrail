#include "JavaProjectSetup.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <system_error>

#include "logging.h"
#include "ProjectSettings.h"
#include "SourceGroupSettingsJavaEmpty.h"
#include "SourceGroupSettingsJavaGradle.h"
#include "SourceGroupSettingsJavaMaven.h"
#include "utilityUuid.h"

namespace fs = std::filesystem;

namespace {

constexpr auto ProjectFolderName = L".sourcetrail";

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

std::shared_ptr<SourceGroupSettings> createSourceGroup(const fs::path& folder,
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
    settings->setSourcePaths({relativeToProject(folder, projectDirectory)});
    settings->setExcludeFilterStrings({L"**/" + std::wstring{ProjectFolderName} + L"/**"});
    settings->setJavaStandard(JavaStandard);
    return settings;
  }
  }
  return nullptr;
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

FilePath createJavaProject(const FilePath& folder, JavaBuildKind kind) {
  const fs::path path = folderPath(folder);
  const fs::path projectDirectory = path / ProjectFolderName;

  std::error_code error;
  fs::create_directories(projectDirectory, error);
  if(error) {
    LOG_ERROR("Cannot create {}: {}", projectDirectory.string(), error.message());
    return {};
  }

  {
    std::ofstream gitignore{projectDirectory / ".gitignore", std::ios::binary | std::ios::trunc};
    gitignore << "*\n";
    if(!gitignore) {
      LOG_ERROR("Cannot write {}", (projectDirectory / ".gitignore").string());
      return {};
    }
  }

  const FilePath projectFile = projectFileFor(folder);
  ProjectSettings settings;
  settings.setVersion(ProjectSettings::VERSION);
  settings.setAllSourceGroupSettings({createSourceGroup(path, kind, &settings)});
  if(!settings.save(projectFile)) {
    LOG_ERROR("Cannot write {}", projectFile.str());
    return {};
  }
  return projectFile;
}

JavaProjectChoice openOrCreateJavaProject(const FilePath& folder, const std::function<bool()>& acceptPlainJava) {
  if(FilePath existing = existingProjectFile(folder); !existing.empty()) {
    return {existing, false, detectJavaBuild(folder)};
  }

  const JavaBuildKind kind = detectJavaBuild(folder);
  if(kind == JavaBuildKind::None && !acceptPlainJava()) {
    return {};
  }

  FilePath projectFile = createJavaProject(folder, kind);
  const bool created = !projectFile.empty();
  return {std::move(projectFile), created, kind};
}
