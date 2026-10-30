#ifndef ST_PROJECT_H
#define ST_PROJECT_H

#include <QString>

#include "FilePath.h"

namespace st
{
// Nearest .srctrlprj in cwd or a parent directory, else the registry entry whose
// source root contains cwd. Empty FilePath if none.
FilePath findProject(const FilePath& cwd, const FilePath& registryFile);

// The lock file Sourcetrail's Project::load takes: canonical project path plus ".lock".
QString projectLockFile(const FilePath& project);

// Records the Gradle root of every enabled Gradle source group as `<root>\t<project>`.
void registerProject(const FilePath& registryFile, const FilePath& project);
}

#endif
