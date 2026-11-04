#include "utilityMaven.h"

#include <cstdlib>
#include <optional>

#include "Application.h"
#include "ApplicationSettings.h"
#include "FilePath.h"
#include "logging.h"
#include "TextAccess.h"
#include "type/MessageStatus.h"
#include "utility.h"
#include "utilityApp.h"
#include "utilityJava.h"
#include "utilityMavenCache.h"
#include "utilityString.h"
#include "utilityXml.h"

namespace {
void fetchDirectories(std::vector<FilePath>& pathList,
                      std::shared_ptr<TextAccess> xmlAccess,
                      const std::vector<std::string>& tags,
                      const FilePath& toAppend = FilePath()) {
  {
    std::string tagString = "";
    for(size_t i = 0; i < tags.size(); i++) {
      if(i != 0) {
        tagString += " -> ";
      }
      tagString += tags[i];
    }
    LOG_INFO("Fetching source directories in \"{}\".", tagString);
  }

  std::vector<std::string> fetchedDirectories = utility::getValuesOfAllXmlElementsOnPath(xmlAccess, tags);
  LOG_INFO("Found {} source directories.", fetchedDirectories.size());

  for(const std::string& fetchedDirectory : fetchedDirectories) {
    FilePath path(fetchedDirectory);
    if(!toAppend.empty()) {
      path.concatenate(toAppend);
    }
    pathList.push_back(path);
    LOG_INFO(L"Found directory \"{}\".", path.wstr());
  }
}

std::wstring getErrorMessageFromMavenOutput(std::shared_ptr<const TextAccess> mavenOutput) {
  const std::string errorPrefix = "[ERROR]";
  const std::string fatalPrefix = "[FATAL]";

  std::wstring errorMessage;

  for(const std::string& line : mavenOutput->getAllLines()) {
    const std::string trimmedLine = utility::trim(line);

    if(utility::isPrefix<std::string>(errorPrefix, trimmedLine)) {
      errorMessage += utility::decodeFromUtf8(utility::trim(trimmedLine.substr(errorPrefix.size())) + "\n");
    } else if(utility::isPrefix<std::string>(fatalPrefix, trimmedLine)) {
      errorMessage += utility::decodeFromUtf8(trimmedLine + "\n");
    }
  }

  if(!errorMessage.empty()) {
    errorMessage = L"The following error occurred while executing a Maven command:\n\n" + errorMessage;
  }

  return errorMessage;
}

std::vector<std::wstring> getMavenArgs(const FilePath& settingsFilePath) {
  // batch mode prints no colours, so the [ERROR] prefix check sees Maven's errors
  std::vector<std::wstring> args = {L"-B"};
  if(!settingsFilePath.empty() && settingsFilePath.exists()) {
    // separate arguments: the process is started without a shell, so quotes would reach Maven as part of the path
    args.push_back(L"--settings");
    args.push_back(settingsFilePath.wstr());
  }
  return args;
}

struct MavenRun {
  std::wstring errorMessage;    // empty when Maven succeeded
  // one line naming the goal, the module and the first error; empty when Maven could not be run at all
  std::wstring warning;
};

std::wstring describeFailure(const std::wstring& goal, std::shared_ptr<const TextAccess> mavenOutput, int exitCode) {
  std::string firstError;
  std::string module;
  for(const std::string& line : mavenOutput->getAllLines()) {
    const std::string trimmedLine = utility::trim(line);
    std::string text;
    if(utility::isPrefix<std::string>("[ERROR]", trimmedLine) || utility::isPrefix<std::string>("[FATAL]", trimmedLine)) {
      text = utility::trim(trimmedLine.substr(7));
    }
    if(firstError.empty()) {
      firstError = text;
    }
    if(const size_t position = text.find("on project "); module.empty() && position != std::string::npos) {
      const std::string rest = text.substr(position + 11);
      module = rest.substr(0, rest.find_first_of(": "));
    }
  }
  return L"Maven: " + goal + L" failed in " + (module.empty() ? L"the project" : utility::decodeFromUtf8(module)) + L": " +
      (firstError.empty() ? L"exit code " + std::to_wstring(exitCode) : utility::decodeFromUtf8(firstError)) +
      L"; indexing with partial results";
}

// Runs Maven with the given goal and its arguments. Fails when Maven could not be run, printed an error or exited
// with a non-zero code.
MavenRun runMaven(const FilePath& mavenPath,
                  const FilePath& settingsFilePath,
                  const FilePath& projectDirectoryPath,
                  const std::vector<std::wstring>& goalArgs) {
  utility::setJavaHomeVariableIfNotExists();

  std::vector<std::wstring> args = getMavenArgs(settingsFilePath);
  utility::append(args, goalArgs);

  const auto process = utility::executeProcess(mavenPath.wstr(), args, projectDirectoryPath, true, 60000);
  std::shared_ptr<TextAccess> outputAccess = TextAccess::createFromString(utility::encodeToUtf8(process.output));

  MavenRun run;
  if(outputAccess->isEmpty()) {
    run.errorMessage =
        L"Sourcetrail was unable to locate Maven on this machine.\n"
        "Please make sure to provide the correct Maven Path in the preferences.";
    return run;
  }

  run.errorMessage = getErrorMessageFromMavenOutput(outputAccess);
  if(run.errorMessage.empty() && process.exitCode != 0) {
    run.errorMessage = L"The Maven command \"" + goalArgs.front() + L"\" failed with exit code " +
        std::to_wstring(process.exitCode) + L".";
  }
  if(!run.errorMessage.empty()) {
    run.warning = describeFailure(goalArgs.front(), outputAccess, process.exitCode);
  }
  return run;
}

// Every directory the effective POM names, existing or not; nullopt, with errorMessage set, when Maven failed.
std::optional<std::vector<FilePath>> getCandidateDirectoriesFromEffectivePom(const FilePath& mavenPath,
                                                                             const FilePath& settingsFilePath,
                                                                             const FilePath& projectDirectoryPath,
                                                                             const FilePath& outputDirectoryPath,
                                                                             bool addTestDirectories,
                                                                             std::wstring& errorMessage) {
  FilePath outputPath = outputDirectoryPath.getConcatenated(FilePath("/effective-pom.xml"));

  errorMessage =
      runMaven(mavenPath, settingsFilePath, projectDirectoryPath, {L"help:effective-pom", L"-Doutput=" + outputPath.wstr()}).errorMessage;
  if(!errorMessage.empty()) {
    return std::nullopt;
  } else if(!outputPath.recheckExists()) {
    LOG_ERROR("Maven effective-pom didn't generate an output file: {}", outputPath.str());
    errorMessage = L"Maven effective-pom didn't generate an output file: " + outputPath.wstr();
    return std::nullopt;
  }

  std::shared_ptr<TextAccess> xmlAccess = TextAccess::createFromFile(outputPath);

  std::vector<FilePath> uncheckedDirectories;
  fetchDirectories(
      uncheckedDirectories, xmlAccess, utility::createVectorFromElements<std::string>("project", "build", "sourceDirectory"));
  fetchDirectories(uncheckedDirectories,
                   xmlAccess,
                   utility::createVectorFromElements<std::string>("projects", "project", "build", "sourceDirectory"));
  fetchDirectories(uncheckedDirectories,
                   xmlAccess,
                   utility::createVectorFromElements<std::string>("project", "build", "directory"),
                   FilePath(L"generated-sources"));
  fetchDirectories(uncheckedDirectories,
                   xmlAccess,
                   utility::createVectorFromElements<std::string>("projects", "project", "build", "directory"),
                   FilePath(L"generated-sources"));

  if(addTestDirectories) {
    fetchDirectories(uncheckedDirectories,
                     xmlAccess,
                     utility::createVectorFromElements<std::string>("project", "build", "testSourceDirectory"));
    fetchDirectories(uncheckedDirectories,
                     xmlAccess,
                     utility::createVectorFromElements<std::string>("projects", "project", "build", "testSourceDirectory"));
    fetchDirectories(uncheckedDirectories,
                     xmlAccess,
                     utility::createVectorFromElements<std::string>("project", "build", "directory"),
                     FilePath(L"generated-test-sources"));
    fetchDirectories(uncheckedDirectories,
                     xmlAccess,
                     utility::createVectorFromElements<std::string>("projects", "project", "build", "directory"),
                     FilePath(L"generated-test-sources"));
  }

  return uncheckedDirectories;
}

std::vector<FilePath> filterExistingDirectories(const std::vector<FilePath>& uncheckedDirectories) {
  std::vector<FilePath> directories;
  for(const FilePath& uncheckedDirectory : uncheckedDirectories) {
    if(uncheckedDirectory.recheckExists()) {
      directories.push_back(uncheckedDirectory);
    }
  }

  LOG_INFO("Found {} of {} directories on system.", directories.size(), uncheckedDirectories.size());

  return directories;
}

}    // namespace

namespace utility {
std::wstring mavenGenerateSources(const FilePath& mavenPath, const FilePath& settingsFilePath, const FilePath& projectDirectoryPath) {
  const MavenRun run = runMaven(mavenPath, settingsFilePath, projectDirectoryPath, {L"generate-sources"});
  if(run.warning.empty()) {
    return run.errorMessage;
  }
  LOG_WARNING(run.errorMessage);
  MessageStatus(run.warning).dispatch();
  return {};
}

std::vector<FilePath> mavenGetAllDirectoriesFromEffectivePom(const FilePath& mavenPath,
                                                             const FilePath& settingsFilePath,
                                                             const FilePath& projectDirectoryPath,
                                                             const FilePath& outputDirectoryPath,
                                                             bool addTestDirectories) {
  std::wstring errorMessage;
  const std::optional<std::vector<FilePath>> candidates = getCandidateDirectoriesFromEffectivePom(
      mavenPath, settingsFilePath, projectDirectoryPath, outputDirectoryPath, addTestDirectories, errorMessage);
  if(!candidates) {
    LOG_ERROR(L"{}", errorMessage);
    return {};
  }
  return filterExistingDirectories(*candidates);
}

MavenPreparation mavenPrepareProject(const FilePath& mavenPath,
                                     const FilePath& settingsFilePath,
                                     const FilePath& projectDirectoryPath,
                                     const FilePath& outputDirectoryPath,
                                     bool addTestDirectories,
                                     bool fullRefresh,
                                     const std::function<void(const std::wstring&)>& onStep) {
  MavenPreparation preparation;
  const std::string fingerprint = mavenBuildFingerprint(mavenPath, settingsFilePath, projectDirectoryPath, addTestDirectories);
  if(!fullRefresh && mavenDependenciesUpToDate(outputDirectoryPath, fingerprint, &preparation.warnings)) {
    LOG_INFO("Maven build files unchanged, reusing generated sources and exported dependencies.");
    return preparation;
  }

  // a run that fails half way must not leave a marker claiming its outputs are current
  if(fullRefresh) {
    removeMavenCache(outputDirectoryPath);
  } else {
    removeMavenDependenciesExported(outputDirectoryPath);
  }

  // Like a build without Sourcetrail, a failing module (one whose sibling modules are not installed, say) leaves the
  // others' outputs in place: that is a warning, indexed with what there is.
  const std::vector<std::pair<std::wstring, std::vector<std::wstring>>> steps = {
      {L"Generating Source Files", {L"generate-sources"}},
      {L"Exporting Dependencies", {L"dependency:copy-dependencies", L"-DoutputDirectory=" + outputDirectoryPath.wstr()}}};
  for(const auto& [description, goalArgs] : steps) {
    onStep(description);
    const MavenRun run = runMaven(mavenPath, settingsFilePath, projectDirectoryPath, goalArgs);
    if(run.errorMessage.empty()) {
      continue;
    } else if(run.warning.empty()) {
      preparation.errorMessage = run.errorMessage;
      return preparation;
    }
    LOG_WARNING(run.errorMessage);
    preparation.warnings.push_back(run.warning);
  }

  // without the source directories there is nothing to index. The marker records which ones existed, so that a
  // `mvn clean` runs generate-sources again.
  std::optional<std::vector<FilePath>> candidates = loadMavenSourceDirectories(outputDirectoryPath, fingerprint);
  if(!candidates) {
    onStep(L"Fetching Source Directories");
    candidates = getCandidateDirectoriesFromEffectivePom(
        mavenPath, settingsFilePath, projectDirectoryPath, outputDirectoryPath, addTestDirectories, preparation.errorMessage);
    if(!candidates) {
      return preparation;
    }
    saveMavenSourceDirectories(outputDirectoryPath, fingerprint, *candidates);
  }

  markMavenDependenciesExported(outputDirectoryPath, fingerprint, filterExistingDirectories(*candidates), preparation.warnings);
  return preparation;
}

std::vector<FilePath> mavenGetSourceDirectories(const FilePath& mavenPath,
                                                const FilePath& settingsFilePath,
                                                const FilePath& projectDirectoryPath,
                                                const FilePath& outputDirectoryPath,
                                                bool addTestDirectories,
                                                std::wstring& errorMessage) {
  const std::string fingerprint = mavenBuildFingerprint(mavenPath, settingsFilePath, projectDirectoryPath, addTestDirectories);
  if(const std::optional<std::vector<FilePath>> cached = loadMavenSourceDirectories(outputDirectoryPath, fingerprint)) {
    LOG_INFO("Maven build files unchanged, reusing the source directories of the effective POM.");
    return filterExistingDirectories(*cached);
  }

  const std::optional<std::vector<FilePath>> candidates = getCandidateDirectoriesFromEffectivePom(
      mavenPath, settingsFilePath, projectDirectoryPath, outputDirectoryPath, addTestDirectories, errorMessage);
  if(!candidates) {
    return {};
  }
  saveMavenSourceDirectories(outputDirectoryPath, fingerprint, *candidates);
  return filterExistingDirectories(*candidates);
}
}    // namespace utility
