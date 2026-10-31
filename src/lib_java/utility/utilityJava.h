#ifndef UTILITY_JAVA_H
#define UTILITY_JAVA_H

#include <set>
#include <string>
#include <vector>

class FilePath;
class SourceGroupSettingsWithClasspath;

namespace utility
{
std::vector<std::wstring> getRequiredJarNames();
std::string prepareJavaEnvironment();
bool prepareJavaEnvironmentAndDisplayOccurringErrors();
std::set<FilePath> fetchRootDirectories(const std::set<FilePath>& sourceFilePaths);
std::vector<FilePath> getClassPath(
	const std::vector<FilePath>& classpathItems,
	bool useJreSystemLibrary,
	const std::set<FilePath>& sourceFilePaths);
// The class path for indexing one source file: its own source root first, so a class declared in several roots,
// like a test helper two modules both have, resolves to the copy its own module compiles against.
std::vector<FilePath> withOwnRootFirst(const std::vector<FilePath>& classPath, const FilePath& sourceFilePath);
void setJavaHomeVariableIfNotExists();
}	 // namespace utility

#endif	  // UTILITY_JAVA_H
