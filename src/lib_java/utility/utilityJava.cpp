#include "utilityGradle.h"

#include "Application.h"
#include "ApplicationSettings.h"
#include "DialogView.h"
#include "JavaEnvironment.h"
#include "JavaEnvironmentFactory.h"
#include "MessageStatus.h"
#include "ResourcePaths.h"
#include "ScopedFunctor.h"
#include "SourceGroupSettingsWithClasspath.h"
#include "TextAccess.h"
#include "logging.h"
#include "utilityString.h"

namespace utility
{
std::vector<std::wstring> getRequiredJarNames()
{
	return {
		L"gradle-tooling-api-8.6.jar",
		L"java-indexer.jar",
		L"org.eclipse.core.commands_3.11.200.v20231108-1058.jar",
		L"org.eclipse.core.contenttype_3.9.200.v20230914-0751.jar",
		L"org.eclipse.core.expressions_3.9.200.v20230921-0857.jar",
		L"org.eclipse.core.filesystem_1.10.200.v20231102-0934.jar",
		L"org.eclipse.core.jobs_3.15.100.v20230930-1207.jar",
		L"org.eclipse.core.resources_3.20.0.v20231102-0934.jar",
		L"org.eclipse.core.runtime_3.30.0.v20231102-0719.jar",
		L"org.eclipse.equinox.app_1.6.400.v20231103-0807.jar",
		L"org.eclipse.equinox.common_3.18.200.v20231106-1826.jar",
		L"org.eclipse.equinox.preferences_3.10.400.v20231102-2218.jar",
		L"org.eclipse.equinox.registry_3.11.400.v20231102-2218.jar",
		L"org.eclipse.jdt.core_3.36.0.v20231115-1055.jar",
		L"org.eclipse.osgi_3.18.600.v20231110-1900.jar",
		L"org.eclipse.text_3.13.100.v20230801-1334.jar",
		L"org.osgi.service.prefs_1.1.2.202109301733.jar",
		L"ecj-3.36.0.jar",
		L"slf4j-api-2.0.12.jar",
		L"slf4j-simple-2.0.12.jar"};
}

std::string prepareJavaEnvironment()
{
	std::string errorString;

	if (!JavaEnvironmentFactory::getInstance())
	{
#ifdef _WIN32
		const std::string separator = ";";
#else
		const std::string separator = ":";
#endif

		std::string classPath = "";
		{
			const std::vector<std::wstring> jarNames = getRequiredJarNames();
			for (size_t i = 0; i < jarNames.size(); i++)
			{
				if (i != 0)
				{
					classPath += separator;
				}
				classPath +=
					ResourcePaths::getJavaDirectoryPath().concatenate(L"lib/" + jarNames[i]).str();
			}
		}

		JavaEnvironmentFactory::createInstance(classPath, errorString);
	}

	return errorString;
}

bool prepareJavaEnvironmentAndDisplayOccurringErrors()
{
	const std::wstring errorString = utility::decodeFromUtf8(utility::prepareJavaEnvironment());

	if (!errorString.empty())
	{
		LOG_ERROR(errorString);
		MessageStatus(errorString, true, false).dispatch();
	}

	if (!JavaEnvironmentFactory::getInstance())
	{
		std::wstring dialogMessage =
			L"Sourcetrail was unable to initialize Java environment on this machine.\n"
			"Please make sure to provide the correct Java Path in the preferences.";

		if (!errorString.empty())
		{
			dialogMessage += L"\n\nError: " + errorString;
		}

		MessageStatus(dialogMessage, true, false).dispatch();

		Application::getInstance()->handleDialog(dialogMessage);
		return false;
	}
	return true;
}

std::set<FilePath> fetchRootDirectories(const std::set<FilePath>& sourceFilePaths)
{
	std::shared_ptr<DialogView> dialogView = Application::getInstance()->getDialogView(
		DialogView::UseCase::PROJECT_SETUP);
	dialogView->showUnknownProgressDialog(L"Preparing Project", L"Gathering Root\nDirectories");

	ScopedFunctor dialogHider([&dialogView]() { dialogView->hideUnknownProgressDialog(); });

	std::set<FilePath> rootDirectories;

	std::shared_ptr<JavaEnvironment> javaEnvironment =
		JavaEnvironmentFactory::getInstance()->createEnvironment();
	for (const FilePath& filePath: sourceFilePaths)
	{
		std::shared_ptr<TextAccess> textAccess = TextAccess::createFromFile(filePath);

		std::string packageName = "";
		javaEnvironment->callStaticStringMethod(
			"com/sourcetrail/JavaIndexer", "getPackageName", packageName, textAccess->getText());

		if (packageName.empty())
		{
			continue;
		}

		FilePath rootPath = filePath.getParentDirectory();
		bool success = true;

		const std::vector<std::string> packageNameParts = utility::splitToVector(packageName, ".");
		for (std::vector<std::string>::const_reverse_iterator it = packageNameParts.rbegin();
			 it != packageNameParts.rend();
			 it++)
		{
			if (rootPath.fileName() != utility::decodeFromUtf8(*it))
			{
				success = false;
				break;
			}
			rootPath = rootPath.getParentDirectory();
		}

		if (success)
		{
			rootDirectories.insert(rootPath);
		}
	}

	return rootDirectories;
}

std::vector<FilePath> getClassPath(
	const std::vector<FilePath>& classpathItems,
	bool useJreSystemLibrary,
	const std::set<FilePath>& sourceFilePaths)
{
	std::vector<FilePath> classPath;

	for (const FilePath& classpath: classpathItems)
	{
		if (classpath.exists())
		{
			LOG_INFO(L"Adding path to classpath: " + classpath.wstr());
			classPath.push_back(classpath);
		}
	}

	if (useJreSystemLibrary)
	{
		for (const FilePath& systemLibraryPath:
			 ApplicationSettings::getInstance()->getJreSystemLibraryPathsExpanded())
		{
			LOG_INFO(L"Adding JRE system library path to classpath: " + systemLibraryPath.wstr());
			classPath.push_back(systemLibraryPath);
		}
	}

	for (const FilePath& rootDirectory: utility::fetchRootDirectories(sourceFilePaths))
	{
		if (rootDirectory.exists())
		{
			LOG_INFO(L"Adding root directory to classpath: " + rootDirectory.wstr());
			classPath.push_back(rootDirectory);
		}
	}

	return classPath;
}

void setJavaHomeVariableIfNotExists()
{
#pragma warning(push)
#pragma warning(disable : 4996)
	if (getenv("JAVA_HOME") == nullptr)
#pragma warning(pop)
	{
		const FilePath javaPath = ApplicationSettings::getInstance()->getJavaPath();
		const FilePath javaHomePath =
			javaPath.getParentDirectory().getParentDirectory().getParentDirectory();

		LOG_WARNING(
			"Environment variable \"JAVA_HOME\" not found on system. Setting value to \"" +
			javaHomePath.str() + "\" for this process.");
#pragma warning(push)
#pragma warning(disable : 4996)
		putenv(const_cast<char*>(("JAVA_HOME=" + javaHomePath.str()).c_str()));
#pragma warning(pop)
	}
}
}	 // namespace utility
