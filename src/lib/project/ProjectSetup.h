#pragma once

#include <functional>
#include <memory>

#include "FilePath.h"

class ProjectSettings;

// Creating a project from nothing but a source folder: its Java build, its TypeScript project, or both. Everything
// Sourcetrail writes for the project goes into <folder>/.sourcetrail/, which ignores itself in git.

enum class JavaBuildKind { Gradle, Maven, None };

// Looks at the folder's top level only: settings.gradle(.kts) or build.gradle(.kts) give Gradle, pom.xml gives Maven,
// both give Gradle.
JavaBuildKind detectJavaBuild(const FilePath& folder);

struct DetectedBuild {
  JavaBuildKind java = JavaBuildKind::None;
  bool typeScript = false;    // a tsconfig.json at the folder's top level
};

DetectedBuild detectBuild(const FilePath& folder);

// <folder>/.sourcetrail/<folder name>.srctrlprj
FilePath projectFileFor(const FilePath& folder);

// The first <folder>/.sourcetrail/*.srctrlprj by name, or an empty path.
FilePath existingProjectFile(const FilePath& folder);

// The tsconfig.json a new TypeScript source group of a project starts with: the one in the project directory, else the
// one in the folder a project directory named .sourcetrail sits in, else an empty path.
FilePath rootTsconfigFor(const FilePath& projectDirectory);

// Writes projectFileFor(folder) with a source group for the Java build (tests indexed) and one for the TypeScript
// project, and .sourcetrail/.gitignore; with neither, a plain Java source group over the folder with an empty
// classpath. Returns the project file, or an empty path when it could not be written.
FilePath createProject(const FilePath& folder, const DetectedBuild& build);

// The settings createProject writes, in memory only; their file is projectFileFor(folder).
std::shared_ptr<ProjectSettings> projectSettingsFor(const FilePath& folder, const DetectedBuild& build);

// Appends a TypeScript source group for <folder>/tsconfig.json to the project file and saves it; false when the
// project cannot be read or written.
bool addTypeScriptGroup(const FilePath& projectFile, const FilePath& folder);

// Creates <folder>/.sourcetrail/ with its .gitignore; false when either cannot be written.
bool prepareProjectFolder(const FilePath& folder);

struct ProjectChoice {
  FilePath projectFile;    // empty when nothing is to be opened
  bool created = false;
  // A plain Java project (no build found) is not written yet: edit projectSettingsFor(folder, build) and save it.
  bool needsSetup = false;
  // The existing project has no TypeScript source group, but the folder has a tsconfig.json: see addTypeScriptGroup.
  bool canAddTypeScript = false;
  DetectedBuild build;
};

// What New Project does with the picked folder: the project already in its .sourcetrail/, else a new one for the
// detected Java build and TypeScript project. With neither, acceptPlainJava decides between a plain Java project (left
// to set up) and nothing.
ProjectChoice openOrCreateProject(const FilePath& folder, const std::function<bool()>& acceptPlainJava);
