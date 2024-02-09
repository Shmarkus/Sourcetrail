// STL
#include <csignal>
#include <cstdlib>
#include <iostream>
// fmt
#include <fmt/format.h>
//
#include "includes.h"
#include "language_packages.h"
// internal
#include "Application.h"
#include "ApplicationSettings.h"
#include "ApplicationSettingsPrefiller.h"
#include "CommandLineParser.h"
#include "ConsoleLogger.h"
#include "FileLogger.h"
#include "FilePath.h"
#include "LanguagePackageManager.h"
#include "LogManager.h"
#include "Logger.h"
#include "MessageIndexingInterrupted.h"
#include "MessageLoadProject.h"
#include "MessageStatus.h"
#include "QtApplication.h"
#include "QtCoreApplication.h"
#include "QtNetworkFactory.h"
#include "QtViewFactory.h"
#include "ResourcePaths.h"
#include "ScopedFunctor.h"
#include "SourceGroupFactory.h"
#include "SourceGroupFactoryModuleCustom.h"
#include "Version.h"
#include "logging.h"
#include "productVersion.h"
#include "utilityApp.h"
#include "utilityQt.h"
#include "utilityString.h"

#if BUILD_CXX_LANGUAGE_PACKAGE
#  include "LanguagePackageCxx.h"
#  include "SourceGroupFactoryModuleCxx.h"
#endif    // BUILD_CXX_LANGUAGE_PACKAGE

#if BUILD_JAVA_LANGUAGE_PACKAGE
#	include "LanguagePackageJava.h"
#	include "SourceGroupFactoryModuleJava.h"
#endif	  // BUILD_JAVA_LANGUAGE_PACKAGE

#if BUILD_PYTHON_LANGUAGE_PACKAGE
#	include "SourceGroupFactoryModulePython.h"
#endif	  // BUILD_PYTHON_LANGUAGE_PACKAGE

void signalHandler(int /*signum*/) {
  std::cout << "interrupt indexing" << std::endl;
  MessageIndexingInterrupted().dispatch();
}

void setupLogging() {
  auto* logManager = LogManager::getInstance().get();

  if(qEnvironmentVariableIsEmpty("ST_DISABLE_LOG_CONSOLE")) {
    auto consoleLogger = std::make_shared<ConsoleLogger>();
    auto defaultLogLevel = Logger::LogLevel::LOG_INFOS;
    if(auto logLevel = qgetenv("ST_CONSOLE_LOG_LEVEL"); !logLevel.isEmpty()) {
      defaultLogLevel = Logger::convertStringToLogLevel(logLevel.toStdString());
    }
    consoleLogger->setLogLevel(defaultLogLevel);
    logManager->addLogger(consoleLogger);
  }

  if(auto logFileEnv = qgetenv("ST_LOG_FILE"); !logFileEnv.isEmpty()) {
    auto fileLogger = std::make_shared<FileLogger>();
    fileLogger->setLogFilePath(FilePath {logFileEnv.toStdString()});
    auto defaultLogLevel = Logger::LogLevel::LOG_INFOS;
    if(auto logLevel = qgetenv("ST_FILE_LOG_LEVEL"); !logLevel.isEmpty()) {
      defaultLogLevel = Logger::convertStringToLogLevel(logLevel.toStdString());
    }
    fileLogger->setLogLevel(defaultLogLevel);
    logManager->addLogger(fileLogger);
  }
}

void addLanguagePackages() {
  SourceGroupFactory::getInstance()->addModule(std::make_shared<SourceGroupFactoryModuleCustom>());

#if BUILD_CXX_LANGUAGE_PACKAGE
  SourceGroupFactory::getInstance()->addModule(std::make_shared<SourceGroupFactoryModuleCxx>());
#endif    // BUILD_CXX_LANGUAGE_PACKAGE

#if BUILD_JAVA_LANGUAGE_PACKAGE
	SourceGroupFactory::getInstance()->addModule(std::make_shared<SourceGroupFactoryModuleJava>());
#endif	  // BUILD_JAVA_LANGUAGE_PACKAGE

#if BUILD_PYTHON_LANGUAGE_PACKAGE
	SourceGroupFactory::getInstance()->addModule(std::make_shared<SourceGroupFactoryModulePython>());
#endif	  // BUILD_PYTHON_LANGUAGE_PACKAGE

#if BUILD_CXX_LANGUAGE_PACKAGE
  LanguagePackageManager::getInstance()->addPackage(std::make_shared<LanguagePackageCxx>());
#endif    // BUILD_CXX_LANGUAGE_PACKAGE
}

#if BUILD_JAVA_LANGUAGE_PACKAGE
	LanguagePackageManager::getInstance()->addPackage(std::make_shared<LanguagePackageJava>());
#endif	  // BUILD_JAVA_LANGUAGE_PACKAGE

void checkRunFromScript() {
#ifndef _WIN32
  const auto expectedShareDirectory = FilePath(QCoreApplication::applicationDirPath().toStdWString() + L"/../share");
  if(qEnvironmentVariableIsEmpty("SOURCETRAIL_VIA_SCRIPT") && !expectedShareDirectory.exists()) {
    LOG_WARNING("Please run Sourcetrail via the Sourcetrail.sh script!");
  }
#endif
}

int runConsole(int argc, char** argv, const Version& version, commandline::CommandLineParser& commandLineParser) {
  QtCoreApplication qtApp(argc, argv);

  checkRunFromScript();

  setupApp(argc, argv);

  setupLogging();

  Application::createInstance(version, nullptr, nullptr);
  [[maybe_unused]] ScopedFunctor scopedFunctor([]() { Application::destroyInstance(); });

  ApplicationSettingsPrefiller::prefillPaths(ApplicationSettings::getInstance().get());
  addLanguagePackages();

  // TODO(Hussein): Replace with Boost or Qt
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);
  std::signal(SIGABRT, signalHandler);

  commandLineParser.parse();

  if(commandLineParser.exitApplication()) {
    return EXIT_SUCCESS;
  }

  if(commandLineParser.hasError()) {
    std::wcout << commandLineParser.getError() << std::endl;
  } else {
    MessageLoadProject(commandLineParser.getProjectFilePath(),
                       false,
                       commandLineParser.getRefreshMode(),
                       commandLineParser.getShallowIndexingRequested())
        .dispatch();
  }

  return QCoreApplication::exec();
}

int runGui(int argc, char** argv, const Version& version, commandline::CommandLineParser& commandLineParser) {
#ifdef D_WINDOWS
  {
    HWND consoleWnd = GetConsoleWindow();
    DWORD dwProcessId;
    GetWindowThreadProcessId(consoleWnd, &dwProcessId);
    if(GetCurrentProcessId() == dwProcessId) {    // Sourcetrail has not been started from console and thus has it's own console
      ShowWindow(consoleWnd, SW_HIDE);
    }
  }
#endif
  QtApplication qtApp(argc, argv);

  checkRunFromScript();

  setupApp(argc, argv);

  setupLogging();

  qtApp.setAttribute(Qt::AA_UseHighDpiPixmaps);

  QtViewFactory viewFactory;
  QtNetworkFactory networkFactory;

  Application::createInstance(version, &viewFactory, &networkFactory);
  [[maybe_unused]] ScopedFunctor destroyApplication([]() { Application::destroyInstance(); });

  ApplicationSettingsPrefiller::prefillPaths(ApplicationSettings::getInstance().get());
  addLanguagePackages();

  // NOTE(Hussein): Extract to function
  utility::loadFontsFromDirectory(ResourcePaths::getFontsDirectoryPath(), L".otf");
  utility::loadFontsFromDirectory(ResourcePaths::getFontsDirectoryPath(), L".ttf");

  if(commandLineParser.hasError()) {
    Application::getInstance()->handleDialog(commandLineParser.getError());
  } else {
    MessageLoadProject(commandLineParser.getProjectFilePath(), false, REFRESH_NONE).dispatch();
  }

  return QApplication::exec();
}

int main(int argc, char* argv[]) {
  QCoreApplication::addLibraryPath(QStringLiteral("."));

  QApplication::setApplicationName(QStringLiteral("Sourcetrail"));

#ifdef D_LINUX
  QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling, true);
#endif

  const Version version(VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
  QApplication::setApplicationVersion(version.toString().c_str());

  const auto message = fmt::format("Starting Sourcetrail {}bit, version {}", utility::getAppArchTypeString(), version.toString());
  MessageStatus(utility::decodeFromUtf8(message)).dispatch();

  commandline::CommandLineParser commandLineParser(version.toString());
  std::vector<std::string> args;
  if(argc > 1) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    args = std::vector<std::string>(static_cast<size_t>(argc - 1), argv[1]);
  }

  commandLineParser.preparse(std::move(args));
  if(commandLineParser.exitApplication()) {
    return EXIT_SUCCESS;
  }

  setupPlatform(argc, argv);

  if(commandLineParser.runWithoutGUI()) {
    return runConsole(argc, argv, version, commandLineParser);
  }
  return runGui(argc, argv, version, commandLineParser);
}
