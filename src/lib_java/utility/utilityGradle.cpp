#include "utilityGradle.h"

#include <map>
#include <optional>
#include <set>

#include "Application.h"
#include "ApplicationSettings.h"
#include "FileSystem.h"
#include "JavaEnvironment.h"
#include "JavaEnvironmentFactory.h"
#include "MessageStatus.h"
#include "ResourcePaths.h"
#include "TextAccess.h"
#include "logging.h"
#include "utility.h"
#include "utilityApp.h"
#include "utilityDirectorySync.h"
#include "utilityGradleCache.h"
#include "utilityJava.h"
#include "utilityString.h"

namespace utility
{
namespace
{
struct LombokInfo
{
	bool mainHasLombok = false;
	bool testHasLombok = false;
	FilePath mainJarPath;
	FilePath testJarPath;
};

FilePath getGradleInitScriptPath()
{
	return ResourcePaths::getJavaDirectoryPath().concatenate(L"gradle/init.gradle");
}

std::shared_ptr<JavaEnvironment> createPreparedJavaEnvironment()
{
	utility::setJavaHomeVariableIfNotExists();
	utility::prepareJavaEnvironment();
	return JavaEnvironmentFactory::getInstance()->createEnvironment();
}

bool showGradleErrorIfNeeded(const std::string& output)
{
	if (!utility::isPrefix<std::string>("[ERROR]", utility::trim(output)))
	{
		return false;
	}

	const std::wstring dialogMessage =
		L"The following error occurred while executing a Gradle command:\n\n" +
		utility::decodeFromUtf8(utility::replace(output, "\r\n", "\n"));
	MessageStatus(dialogMessage, true, false).dispatch();
	if (Application::getInstance())
	{
		Application::getInstance()->handleDialog(dialogMessage);
	}
	return true;
}

std::string callGradleStringMethod(
	std::shared_ptr<JavaEnvironment> javaEnvironment,
	const std::string& methodName,
	const FilePath& projectDirectoryPath,
	const FilePath& gradleInitScriptPath)
{
	std::string output = "";
	javaEnvironment->callStaticStringMethod(
		"com/sourcetrail/gradle/InfoRetriever",
		methodName,
		output,
		utility::encodeToUtf8(projectDirectoryPath.wstr()),
		utility::encodeToUtf8(gradleInitScriptPath.wstr()));
	return output;
}

std::optional<LombokInfo> getLombokInfo(const FilePath& projectDirectoryPath)
{
	std::shared_ptr<JavaEnvironment> javaEnvironment = createPreparedJavaEnvironment();
	const std::string output = callGradleStringMethod(
		javaEnvironment, "getLombokInfo", projectDirectoryPath, getGradleInitScriptPath());

	if (showGradleErrorIfNeeded(output))
	{
		return std::nullopt;
	}

	LombokInfo info;
	for (const std::string& line: utility::splitToVector(output, "\n"))
	{
		const std::string trimmedLine = utility::trim(line);
		if (trimmedLine.empty())
		{
			continue;
		}

		const std::vector<std::string> parts = utility::splitToVector(trimmedLine, "|");
		if (parts.size() != 2)
		{
			continue;
		}

		const FilePath jarPath(utility::decodeFromUtf8(parts[1]));
		if (parts[0] == "main")
		{
			info.mainHasLombok = true;
			info.mainJarPath = jarPath;
		}
		else if (parts[0] == "test")
		{
			info.testHasLombok = true;
			info.testJarPath = jarPath;
		}
	}

	return info;
}

bool clearDirectoryContents(const FilePath& directoryPath)
{
	if (!directoryPath.recheckExists() || !directoryPath.isDirectory())
	{
		return true;
	}

	const std::vector<FilePath> files = FileSystem::getFilePathsFromDirectory(directoryPath);
	for (const FilePath& file: files)
	{
		if (!FileSystem::remove(file))
		{
			return false;
		}
	}

	std::vector<FilePath> subDirectories = FileSystem::getRecursiveSubDirectories(directoryPath);
	for (auto it = subDirectories.rbegin(); it != subDirectories.rend(); ++it)
	{
		if (!FileSystem::remove(*it))
		{
			return false;
		}
	}

	return true;
}

bool runDelombok(
	const FilePath& projectDirectoryPath,
	const FilePath& lombokJarPath,
	const std::vector<FilePath>& sourceDirectories,
	const FilePath& outputDirectoryPath)
{
	if (sourceDirectories.empty())
	{
		return true;
	}

	const FilePath temporaryOutputPath(outputDirectoryPath.wstr() + L".tmp");
	FileSystem::createDirectory(temporaryOutputPath);
	if (!clearDirectoryContents(temporaryOutputPath))
	{
		LOG_ERROR(L"Failed to clear delombok temporary directory: " + temporaryOutputPath.wstr());
		return false;
	}

	const FilePath javaPath = ApplicationSettings::getInstance()->getJavaPath();
	if (javaPath.empty())
	{
		LOG_ERROR("Cannot run delombok because configured Java path is empty.");
		return false;
	}

	FilePath javaExecutablePath = javaPath.getParentDirectory().getParentDirectory().getParentDirectory();
	javaExecutablePath = javaExecutablePath.getConcatenated(L"bin/java");
	if (!javaExecutablePath.exists())
	{
		LOG_ERROR(L"Cannot run delombok because Java executable is missing at \"" + javaExecutablePath.wstr() + L"\".");
		return false;
	}

	std::vector<std::wstring> args;
	args.push_back(L"-jar");
	args.push_back(lombokJarPath.wstr());
	args.push_back(L"delombok");
	for (const FilePath& sourceDirectory: sourceDirectories)
	{
		args.push_back(sourceDirectory.wstr());
	}
	args.push_back(L"-f");
	args.push_back(L"generateDelombokComment:skip");
	args.push_back(L"-d");
	args.push_back(temporaryOutputPath.wstr());

	const utility::ProcessOutput output =
		utility::executeProcess(javaExecutablePath.wstr(), args, projectDirectoryPath, true, 60000, true);

	if (output.exitCode != 0)
	{
		LOG_ERROR_STREAM(
			<< "Delombok failed with exit code " << output.exitCode << ": "
			<< utility::encodeToUtf8(output.output));
		return false;
	}

	// only touch files whose delombok output changed, so refreshes re-index just those
	const DirectorySyncResult syncResult = syncDirectoryContents(temporaryOutputPath, outputDirectoryPath);
	LOG_INFO_STREAM(
		<< "Delombok output synced: " << syncResult.written << " written, " << syncResult.unchanged
		<< " unchanged, " << syncResult.removed << " removed");

	clearDirectoryContents(temporaryOutputPath);
	FileSystem::remove(temporaryOutputPath);

	if (syncResult.failed > 0)
	{
		LOG_ERROR(L"Failed to sync delombok output into: " + outputDirectoryPath.wstr());
		return false;
	}

	return outputDirectoryPath.recheckExists();
}

std::optional<std::map<std::wstring, std::vector<FilePath>>> getSourceDirectoriesBySourceSet(
	const FilePath& projectDirectoryPath,
	const FilePath& gradleInitScriptPath,
	bool addTestDirectories)
{
	std::map<std::wstring, std::vector<FilePath>> directories;
	std::shared_ptr<JavaEnvironment> javaEnvironment = createPreparedJavaEnvironment();

	const std::string mainOutput = callGradleStringMethod(
		javaEnvironment, "getMainSrcDirs", projectDirectoryPath, gradleInitScriptPath);
	if (showGradleErrorIfNeeded(mainOutput))
	{
		return std::nullopt;
	}

	for (const std::string& mainSrcDir: utility::splitToVector(mainOutput, ";"))
	{
		if (!mainSrcDir.empty())
		{
			directories[L"main"].push_back(FilePath(utility::decodeFromUtf8(mainSrcDir)));
		}
	}

	if (addTestDirectories)
	{
		const std::string testOutput = callGradleStringMethod(
			javaEnvironment, "getTestSrcDirs", projectDirectoryPath, gradleInitScriptPath);
		if (showGradleErrorIfNeeded(testOutput))
		{
			return std::nullopt;
		}

		for (const std::string& testSrcDir: utility::splitToVector(testOutput, ";"))
		{
			if (!testSrcDir.empty())
			{
				directories[L"test"].push_back(FilePath(utility::decodeFromUtf8(testSrcDir)));
			}
		}
	}

	return directories;
}
std::map<std::wstring, std::vector<FilePath>> filterExistingDirectories(
	const std::map<std::wstring, std::vector<FilePath>>& directoriesBySourceSet)
{
	std::map<std::wstring, std::vector<FilePath>> existing;
	for (const auto& [sourceSet, directories]: directoriesBySourceSet)
	{
		for (const FilePath& directory: directories)
		{
			if (directory.exists())
			{
				existing[sourceSet].push_back(directory);
			}
		}
	}
	return existing;
}

bool outputDirectoryHoldsJars(const FilePath& outputDirectoryPath)
{
	return outputDirectoryPath.recheckExists() &&
		!FileSystem::getFilePathsFromDirectory(outputDirectoryPath, {L".jar"}).empty();
}

// Asks Gradle for lombok and source directory info. Only stores it when both calls succeeded.
GradleProjectInfo queryGradleProjectInfo(
	const FilePath& projectDirectoryPath, const FilePath& outputDirectoryPath, bool includesTests)
{
	GradleProjectInfo info;
	info.fingerprint = gradleBuildFingerprint(projectDirectoryPath);
	info.includesTests = includesTests;

	const std::optional<LombokInfo> lombokInfo = getLombokInfo(projectDirectoryPath);
	if (!lombokInfo)
	{
		return info;
	}
	info.mainHasLombok = lombokInfo->mainHasLombok;
	info.testHasLombok = lombokInfo->testHasLombok;
	info.mainLombokJar = lombokInfo->mainJarPath;
	info.testLombokJar = lombokInfo->testJarPath;

	const auto sourceDirectories =
		getSourceDirectoriesBySourceSet(projectDirectoryPath, getGradleInitScriptPath(), includesTests);
	if (!sourceDirectories)
	{
		return info;
	}
	info.sourceDirectories = *sourceDirectories;

	saveGradleProjectInfo(outputDirectoryPath, info);
	return info;
}

GradleProjectInfo getGradleProjectInfo(
	const FilePath& projectDirectoryPath, const FilePath& outputDirectoryPath, bool includesTests)
{
	if (const std::optional<GradleProjectInfo> cached =
			loadGradleProjectInfo(outputDirectoryPath, projectDirectoryPath, includesTests))
	{
		return *cached;
	}
	return queryGradleProjectInfo(projectDirectoryPath, outputDirectoryPath, includesTests);
}
} // namespace

FilePath getDelombokOutputDirectory(const FilePath& outputDirectoryPath, const std::wstring& sourceSetName)
{
	return outputDirectoryPath.getConcatenated(L"delombok").getConcatenated(sourceSetName);
}

bool gradleCopyDependencies(
	const FilePath& projectDirectoryPath, const FilePath& outputDirectoryPath, bool addTestDependencies)
{
	if (gradleDependenciesUpToDate(outputDirectoryPath, projectDirectoryPath, addTestDependencies) &&
		outputDirectoryHoldsJars(outputDirectoryPath))
	{
		LOG_INFO("Gradle build files unchanged, reusing exported dependencies.");
		return true;
	}

	const std::string exportedFingerprint = gradleBuildFingerprint(projectDirectoryPath);
	const FilePath gradleInitScriptPath = getGradleInitScriptPath();
	std::shared_ptr<JavaEnvironment> javaEnvironment = createPreparedJavaEnvironment();
	bool success = javaEnvironment->callStaticVoidMethod(
		"com/sourcetrail/gradle/InfoRetriever",
		"copyCompileLibs",
		utility::encodeToUtf8(projectDirectoryPath.wstr()),
		utility::encodeToUtf8(gradleInitScriptPath.wstr()),
		utility::encodeToUtf8(outputDirectoryPath.wstr()));

	if (success && addTestDependencies)
	{
		success = javaEnvironment->callStaticVoidMethod(
			"com/sourcetrail/gradle/InfoRetriever",
			"copyTestCompileLibs",
			utility::encodeToUtf8(projectDirectoryPath.wstr()),
			utility::encodeToUtf8(gradleInitScriptPath.wstr()),
			utility::encodeToUtf8(outputDirectoryPath.wstr()));
	}

	if (success)
	{
		// refresh the cached info next to the freshly exported jars
		queryGradleProjectInfo(projectDirectoryPath, outputDirectoryPath, addTestDependencies);
		markGradleDependenciesExported(outputDirectoryPath, exportedFingerprint, addTestDependencies);
	}

	return success;
}

bool gradlePrepareLombokSources(
	const FilePath& projectDirectoryPath,
	const FilePath& outputDirectoryPath,
	bool addTestSources)
{
	const GradleProjectInfo info = getGradleProjectInfo(projectDirectoryPath, outputDirectoryPath, addTestSources);
	if (!info.mainHasLombok && !(addTestSources && info.testHasLombok))
	{
		return true;
	}

	const std::map<std::wstring, std::vector<FilePath>> sourceDirectoriesBySourceSet =
		filterExistingDirectories(info.sourceDirectories);

	if (info.mainHasLombok)
	{
		const auto it = sourceDirectoriesBySourceSet.find(L"main");
		if (it != sourceDirectoriesBySourceSet.end() &&
			!runDelombok(
				projectDirectoryPath,
				info.mainLombokJar,
				it->second,
				getDelombokOutputDirectory(outputDirectoryPath, L"main")))
		{
			return false;
		}
	}

	if (addTestSources && info.testHasLombok)
	{
		const auto it = sourceDirectoriesBySourceSet.find(L"test");
		if (it != sourceDirectoriesBySourceSet.end() &&
			!runDelombok(
				projectDirectoryPath,
				info.testLombokJar,
				it->second,
				getDelombokOutputDirectory(outputDirectoryPath, L"test")))
		{
			return false;
		}
	}

	return true;
}

std::vector<FilePath> gradleGetAllSourceDirectories(
	const FilePath& projectDirectoryPath,
	const FilePath& outputDirectoryPath,
	bool addTestDirectories)
{
	const GradleProjectInfo info = getGradleProjectInfo(projectDirectoryPath, outputDirectoryPath, addTestDirectories);
	const std::map<std::wstring, std::vector<FilePath>> sourceDirectoriesBySourceSet =
		filterExistingDirectories(info.sourceDirectories);

	std::vector<FilePath> directories;
	const auto mainIt = sourceDirectoriesBySourceSet.find(L"main");
	if (info.mainHasLombok)
	{
		const FilePath delombokMainDirectory = getDelombokOutputDirectory(outputDirectoryPath, L"main");
		if (delombokMainDirectory.exists())
		{
			directories.push_back(delombokMainDirectory);
		}
	}
	else if (mainIt != sourceDirectoriesBySourceSet.end())
	{
		utility::append(directories, mainIt->second);
	}

	if (addTestDirectories)
	{
		const auto testIt = sourceDirectoriesBySourceSet.find(L"test");
		if (info.testHasLombok)
		{
			const FilePath delombokTestDirectory = getDelombokOutputDirectory(outputDirectoryPath, L"test");
			if (delombokTestDirectory.exists())
			{
				directories.push_back(delombokTestDirectory);
			}
		}
		else if (testIt != sourceDirectoriesBySourceSet.end())
		{
			utility::append(directories, testIt->second);
		}
	}

	LOG_INFO("Found " + std::to_string(directories.size()) + " directories on system.");
	return directories;
}
} // namespace utility
