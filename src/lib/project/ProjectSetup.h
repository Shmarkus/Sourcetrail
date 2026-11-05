#pragma once

#include <functional>
#include <memory>
#include <string>

#include "FilePath.h"

class ProjectSettings;

// Creating a project from nothing but a source folder: its Java build, its TypeScript project, its Python project, its
// C/C++ compilation database, or any of them together. Everything Sourcetrail writes for the project goes into
// <folder>/.sourcetrail/, which ignores itself in git.

enum class JavaBuildKind { Gradle, Maven, None };

// Looks at the folder's top level only: settings.gradle(.kts) or build.gradle(.kts) give Gradle, pom.xml gives Maven,
// both give Gradle.
JavaBuildKind detectJavaBuild(const FilePath& folder);

struct DetectedBuild {
  JavaBuildKind java = JavaBuildKind::None;
  bool typeScript = false;    // a tsconfig.json at the folder's top level
  // A pyproject.toml, setup.py, setup.cfg or requirements*.txt at the folder's top level; without a Gradle or Maven
  // build, a tsconfig.json and a compilation database, also .py files anywhere below it and no .java file, nor a C/C++
  // file next to a CMakeLists.txt (outside the folders the Python indexer skips). A few Python scripts in a Java,
  // TypeScript or C/C++ project make no Python project.
  bool python = false;
  // With C/C++ support: the compile_commands.json at the folder's top level, else in build/, else in the first
  // cmake-build-*/ or out/build/*/ folder by name; empty when there is none.
  FilePath compilationDatabase;
  // With C/C++ support: a CMakeLists.txt at the folder's top level but no compilation database, which CMake writes when
  // asked (compilationDatabaseCommand).
  bool cmakeWithoutDatabase = false;
};

DetectedBuild detectBuild(const FilePath& folder);

// The command, run in a CMake project's folder, that writes the compilation database detectBuild finds.
std::wstring compilationDatabaseCommand();

// <folder>/.sourcetrail/<folder name>.srctrlprj
FilePath projectFileFor(const FilePath& folder);

// The first <folder>/.sourcetrail/*.srctrlprj by name, or an empty path.
FilePath existingProjectFile(const FilePath& folder);

// The tsconfig.json a new TypeScript source group of a project starts with: the one in the project directory, else the
// one in the folder a project directory named .sourcetrail sits in, else an empty path.
FilePath rootTsconfigFor(const FilePath& projectDirectory);

// Writes projectFileFor(folder) with a source group for the Java build (tests indexed), one for the TypeScript
// project, one for the Python project (the folder) and one for the compilation database (the folder's headers
// indexed), and .sourcetrail/.gitignore; with none, a plain Java source group over the folder with an empty classpath. Returns
// the project file, or an empty path when it could not be written.
FilePath createProject(const FilePath& folder, const DetectedBuild& build);

// The settings createProject writes, in memory only; their file is projectFileFor(folder).
std::shared_ptr<ProjectSettings> projectSettingsFor(const FilePath& folder, const DetectedBuild& build);

// Appends a TypeScript source group for <folder>/tsconfig.json to the project file and saves it; false when the
// project cannot be read or written.
bool addTypeScriptGroup(const FilePath& projectFile, const FilePath& folder);

// Appends a Python source group for <folder> to the project file and saves it; false when the project cannot be read
// or written.
bool addPythonGroup(const FilePath& projectFile, const FilePath& folder);

// Appends a C/C++ source group for the compilation database detectBuild finds in <folder> to the project file and saves
// it; false when there is none or the project cannot be read or written.
bool addCxxGroup(const FilePath& projectFile, const FilePath& folder);

// The project root a new Python source group of a project starts with: ".." for a project directory named
// .sourcetrail, the folder it sits in; else empty, the project directory itself.
FilePath pythonRootFor(const FilePath& projectDirectory);

// Creates <folder>/.sourcetrail/ with its .gitignore; false when either cannot be written.
bool prepareProjectFolder(const FilePath& folder);

struct ProjectChoice {
  FilePath projectFile;    // empty when nothing is to be opened
  bool created = false;
  // A plain Java project (no build found) is not written yet: edit projectSettingsFor(folder, build) and save it.
  bool needsSetup = false;
  // The existing project has no TypeScript source group, but the folder has a tsconfig.json: see addTypeScriptGroup.
  bool canAddTypeScript = false;
  // The existing project has no Python source group, but the folder has a Python project: see addPythonGroup.
  bool canAddPython = false;
  // The existing project has no compilation database source group, but the folder has a compilation database: see
  // addCxxGroup.
  bool canAddCxx = false;
  // Nothing is opened because the folder is a CMake project without a compilation database: tell the user the command
  // that writes one (compilationDatabaseCommand). A project created or opened for another build leaves it false.
  bool compilationDatabaseMissing = false;
  DetectedBuild build;
};

// What New Project does with the picked folder: the project already in its .sourcetrail/, else a new one for the
// detected Java build, TypeScript project, Python project and compilation database. With none, a CMake project gets
// nothing (build.cmakeWithoutDatabase says why), else acceptPlainJava decides between a plain Java project (left to
// set up) and nothing.
ProjectChoice openOrCreateProject(const FilePath& folder, const std::function<bool()>& acceptPlainJava);
