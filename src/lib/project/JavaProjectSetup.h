#pragma once

#include <functional>

#include "FilePath.h"

// Creating a Java project from nothing but a source folder. Everything Sourcetrail writes for the project goes into
// <folder>/.sourcetrail/, which ignores itself in git.

enum class JavaBuildKind { Gradle, Maven, None };

// Looks at the folder's top level only: settings.gradle(.kts) or build.gradle(.kts) give Gradle, pom.xml gives Maven,
// both give Gradle.
JavaBuildKind detectJavaBuild(const FilePath& folder);

// <folder>/.sourcetrail/<folder name>.srctrlprj
FilePath projectFileFor(const FilePath& folder);

// The first <folder>/.sourcetrail/*.srctrlprj by name, or an empty path.
FilePath existingProjectFile(const FilePath& folder);

// Writes projectFileFor(folder) with one source group of the given kind (tests indexed; None gives a plain Java
// source group over the folder with an empty classpath) and .sourcetrail/.gitignore. Returns the project file, or an
// empty path when it could not be written.
FilePath createJavaProject(const FilePath& folder, JavaBuildKind kind);

struct JavaProjectChoice {
  FilePath projectFile;    // empty when nothing is to be opened
  bool created = false;
  JavaBuildKind kind = JavaBuildKind::None;
};

// What New Project does with the picked folder: the project already in its .sourcetrail/, else a new one for the
// detected build. Without a build, acceptPlainJava decides between a plain Java project and nothing.
JavaProjectChoice openOrCreateJavaProject(const FilePath& folder, const std::function<bool()>& acceptPlainJava);
