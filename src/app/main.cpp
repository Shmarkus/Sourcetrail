// STL
#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>

#include <fmt/core.h>
#include <fmt/format.h>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <system_error>

#include "Application.h"
#include "ApplicationSettings.h"
#include "ApplicationSettingsPrefiller.h"
#include "CommandLineParser.h"
#include "FilePath.h"
#include "IApplicationSettings.hpp"
#include "impls/Factory.hpp"
#include "includes.h"
#include "language_packages.h"
#include "LanguagePackageManager.h"
#include "logging.h"
#include "productVersion.h"
#include "QtApplication.h"
#include "QtCoreApplication.h"
#include "QtNetworkFactory.h"
#include "QtViewFactory.h"
#include "ResourcePaths.h"
#include "ScopedFunctor.h"
#include "SourceGroupFactory.h"
#include "SourceGroupFactoryModuleCustom.h"
#include "type/indexing/MessageIndexingInterrupted.h"
#include "type/MessageLoadProject.h"
#include "type/MessageStatus.h"
#include "utilityApp.h"
#include "utilityQt.h"
#include "utilityString.h"
#include "Version.h"

#if BUILD_CXX_LANGUAGE_PACKAGE
#  include "LanguagePackageCxx.h"
#  include "SourceGroupFactoryModuleCxx.h"
#endif    // BUILD_CXX_LANGUAGE_PACKAGE
#if BUILD_JAVA_LANGUAGE_PACKAGE
#  include "LanguagePackageJava.h"
#  include "SourceGroupFactoryModuleJava.h"
#endif    // BUILD_JAVA_LANGUAGE_PACKAGE

namespace {
void signalHandler(int signum) {
  fmt::println("Interrupt signal received. {}", signum);
  MessageIndexingInterrupted{}.dispatch();
}

void addLanguagePackages() {
  SourceGroupFactory::getInstance()->addModule(std::make_shared<SourceGroupFactoryModuleCustom>());

#if BUILD_CXX_LANGUAGE_PACKAGE
  SourceGroupFactory::getInstance()->addModule(std::make_shared<SourceGroupFactoryModuleCxx>());
  LanguagePackageManager::getInstance()->addPackage(std::make_shared<LanguagePackageCxx>());
#endif    // BUILD_CXX_LANGUAGE_PACKAGE
#if BUILD_JAVA_LANGUAGE_PACKAGE
  SourceGroupFactory::getInstance()->addModule(std::make_shared<SourceGroupFactoryModuleJava>());
  LanguagePackageManager::getInstance()->addPackage(std::make_shared<LanguagePackageJava>());
#endif    // BUILD_JAVA_LANGUAGE_PACKAGE
}

// The console sinks of a headless run, behind one sink whose sub-sinks can be swapped under its lock while other
// threads log. The logger's own sink list is not thread-safe to change once the message loop runs.
class ConsoleSink final : public spdlog::sinks::dist_sink_mt {
public:
  using spdlog::sinks::dist_sink_mt::dist_sink_mt;

  // `index --verbose`: INFO log lines (such as the indexer's "indexing source file:" lines) go to stderr, whatever the
  // logging settings say, apart from the status lines on stdout; the configured level stays for the log file. Without
  // it a headless run prints only its status lines.
  void logInfoToStderr() {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      auto stderrSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
      stderrSink->set_formatter(formatter_->clone());
      sinks_ = {std::move(stderrSink)};
    }
    set_level(spdlog::level::info);
  }
};

// Must run before Application::createInstance starts the message loop and scheduler threads, which log. Moves the
// logger's console sinks into a ConsoleSink, which keeps their level (loadSettings sets it on the logger's sinks).
std::shared_ptr<ConsoleSink> wrapConsoleSinks() {
  auto* logger = spdlog::default_logger_raw();
  if(nullptr == logger) {
    return nullptr;
  }
  std::vector<spdlog::sink_ptr> consoleSinks;
  std::vector<spdlog::sink_ptr> otherSinks;
  spdlog::level::level_enum level = spdlog::level::off;
  for(auto& sink : logger->sinks()) {
    if(dynamic_cast<spdlog::sinks::basic_file_sink_mt*>(sink.get()) == nullptr) {
      level = std::min(level, sink->level());
      sink->set_level(spdlog::level::trace);
      consoleSinks.push_back(sink);
    } else {
      otherSinks.push_back(sink);
    }
  }
  auto consoleSink = std::make_shared<ConsoleSink>(std::move(consoleSinks));
  consoleSink->set_level(level);
  otherSinks.insert(otherSinks.begin(), consoleSink);
  logger->sinks() = std::move(otherSinks);
  return consoleSink;
}

void checkRunFromScript() {
#ifndef D_WINDOWS
  std::error_code errorCode;
  const auto expectedShareDirectory = std::filesystem::path{QCoreApplication::applicationDirPath().toStdString()}.parent_path() /
      "share";
  if(qEnvironmentVariableIsEmpty("SOURCETRAIL_VIA_SCRIPT") && !std::filesystem::exists(expectedShareDirectory, errorCode)) {
    LOG_WARNING("Please run Sourcetrail via the Sourcetrail.sh script!");
  }
#endif
}

int runConsole(int argc, char** argv, const Version& version, commandline::CommandLineParser& commandLineParser) {
  QtCoreApplication qtApp(argc, argv);

  checkRunFromScript();

  setupApp(argc, argv);

  const auto consoleSink = wrapConsoleSinks();
  auto factory = std::make_shared<lib::Factory>();
  Application::createInstance(version, factory, nullptr, nullptr);
  [[maybe_unused]] const ScopedFunctor scopedFunctor([]() { Application::destroyInstance(); });
  // The listeners register with the IMessageQueue, which createInstance has just created.
  qtApp.startListening();

  ApplicationSettingsPrefiller::prefillPaths(IApplicationSettings::getInstanceRaw());
  addLanguagePackages();

  // TODO(Hussein): Replace with Boost or Qt
  std::ignore = std::signal(SIGINT, signalHandler);
  std::ignore = std::signal(SIGTERM, signalHandler);
  std::ignore = std::signal(SIGABRT, signalHandler);

  commandLineParser.parse();

  if(commandLineParser.exitApplication()) {
    return EXIT_SUCCESS;
  }
  if(commandLineParser.getVerbose() && consoleSink) {
    consoleSink->logInfoToStderr();
  }

  if(commandLineParser.hasError()) {
    std::cout << utility::encodeToUtf8(commandLineParser.getError()) << '\n';
  } else {
    MessageLoadProject{commandLineParser.getProjectFilePath(),
                       false,
                       commandLineParser.getRefreshMode(),
                       commandLineParser.getShallowIndexingRequested()}
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
  const QtApplication qtApp(argc, argv);

  checkRunFromScript();

  setupApp(argc, argv);

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
  QtApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif

  QtViewFactory viewFactory;
  QtNetworkFactory networkFactory;

  auto factory = std::make_shared<lib::Factory>();
  Application::createInstance(version, factory, &viewFactory, &networkFactory);
  [[maybe_unused]] const ScopedFunctor destroyApplication([]() { Application::destroyInstance(); });

  const auto message = fmt::format("Starting Sourcetrail {}bit, version {}", utility::getAppArchTypeString(), version.toString());
  MessageStatus{utility::decodeFromUtf8(message)}.dispatch();

  ApplicationSettingsPrefiller::prefillPaths(IApplicationSettings::getInstanceRaw());
  addLanguagePackages();

  // NOTE(Hussein): Extract to function
  utility::loadFontsFromDirectory(ResourcePaths::getFontsDirectoryPath(), L".otf");
  utility::loadFontsFromDirectory(ResourcePaths::getFontsDirectoryPath(), L".ttf");

  if(commandLineParser.hasError()) {
    Application::getInstance()->handleDialog(commandLineParser.getError());
  } else {
    MessageLoadProject{commandLineParser.getProjectFilePath(), false, RefreshMode::None}.dispatch();
  }

  return QApplication::exec();
}
}    // namespace

int main(int argc, char* argv[]) {
  // Disable logger as default till load it from settings
  if(auto* logger = spdlog::default_logger_raw(); nullptr != logger) {
    for(auto& sink : logger->sinks()) {
      sink->set_level(spdlog::level::level_enum::off);
    }
  }

  QCoreApplication::addLibraryPath(QStringLiteral("."));

  QApplication::setApplicationName(QStringLiteral("Sourcetrail"));

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#  ifdef D_LINUX
  QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling, true);
#  endif
#endif

  const Version version(VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
  // The about dialog and the start screen show this string, suffix included.
  QApplication::setApplicationVersion(QStringLiteral(VERSION_STRING));

  commandline::CommandLineParser commandLineParser(VERSION_STRING);
  std::vector<std::string> args;
  if(argc > 1) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    args = std::vector<std::string>(argv + 1, argv + argc);
  }

  commandLineParser.preparse(std::move(args));
  if(commandLineParser.exitApplication()) {
    return EXIT_SUCCESS;
  }

  setupPlatform(argc, argv);

  IApplicationSettings::setInstance(std::make_shared<ApplicationSettings>());
  if(commandLineParser.runWithoutGUI()) {
    return runConsole(argc, argv, version, commandLineParser);
  }
  return runGui(argc, argv, version, commandLineParser);
}
