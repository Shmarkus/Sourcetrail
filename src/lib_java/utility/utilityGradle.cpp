#include "utilityGradle.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <set>

#include "Application.h"
#include "ApplicationSettings.h"
#include "FileSystem.h"
#include "JavaEnvironment.h"
#include "JavaEnvironmentFactory.h"
#include "logging.h"
#include "ResourcePaths.h"
#include "TextAccess.h"
#include "type/MessageStatus.h"
#include "utility.h"
#include "utilityApp.h"
#include "utilityDirectorySync.h"
#include "utilityGradleCache.h"
#include "utilityJava.h"
#include "utilityString.h"

namespace utility {
namespace {
FilePath getGradleInitScriptPath() {
  return ResourcePaths::getJavaDirectoryPath().concatenate(L"gradle/init.gradle");
}

std::shared_ptr<JavaEnvironment> createPreparedJavaEnvironment() {
  utility::setJavaHomeVariableIfNotExists();
  utility::prepareJavaEnvironment();
  return JavaEnvironmentFactory::getInstance()->createEnvironment();
}

bool showGradleErrorIfNeeded(const std::string& output) {
  if(!utility::isPrefix<std::string>("[ERROR]", utility::trim(output))) {
    return false;
  }

  const std::wstring dialogMessage = L"The following error occurred while executing a Gradle command:\n\n" +
      utility::decodeFromUtf8(utility::replace(output, "\r\n", "\n"));
  MessageStatus(dialogMessage, true, false).dispatch();
  if(Application::getInstance()) {
    Application::getInstance()->handleDialog(dialogMessage);
  }
  return true;
}

std::string callGradleStringMethod(std::shared_ptr<JavaEnvironment> javaEnvironment,
                                   const std::string& methodName,
                                   const FilePath& projectDirectoryPath,
                                   const FilePath& gradleInitScriptPath) {
  std::string output = "";
  javaEnvironment->callStaticStringMethod("com/sourcetrail/gradle/InfoRetriever",
                                          methodName,
                                          output,
                                          utility::encodeToUtf8(projectDirectoryPath.wstr()),
                                          utility::encodeToUtf8(gradleInitScriptPath.wstr()));
  return output;
}

// Source set name -> lombok jar, for every source set that uses Lombok.
std::optional<std::map<std::wstring, FilePath>> getLombokJars(const FilePath& projectDirectoryPath) {
  std::shared_ptr<JavaEnvironment> javaEnvironment = createPreparedJavaEnvironment();
  const std::string output = callGradleStringMethod(
      javaEnvironment, "getLombokInfo", projectDirectoryPath, getGradleInitScriptPath());

  if(showGradleErrorIfNeeded(output)) {
    return std::nullopt;
  }

  std::map<std::wstring, FilePath> jars;
  for(const std::string& line : utility::splitToVector(output, "\n")) {
    const std::string trimmedLine = utility::trim(line);
    if(trimmedLine.empty()) {
      continue;
    }

    const std::vector<std::string> parts = utility::splitToVector(trimmedLine, "|");
    if(parts.size() != 2) {
      continue;
    }

    jars[utility::decodeFromUtf8(parts[0])] = FilePath(utility::decodeFromUtf8(parts[1]));
  }

  return jars;
}

bool clearDirectoryContents(const FilePath& directoryPath) {
  if(!directoryPath.recheckExists() || !directoryPath.isDirectory()) {
    return true;
  }

  const std::vector<FilePath> files = FileSystem::getFilePathsFromDirectory(directoryPath);
  for(const FilePath& file : files) {
    if(!FileSystem::remove(file)) {
      return false;
    }
  }

  std::vector<FilePath> subDirectories = FileSystem::getRecursiveSubDirectories(directoryPath);
  for(auto it = subDirectories.rbegin(); it != subDirectories.rend(); ++it) {
    if(!FileSystem::remove(*it)) {
      return false;
    }
  }

  return true;
}

bool prepareTemporaryDirectory(const FilePath& temporaryOutputPath) {
  FileSystem::createDirectory(temporaryOutputPath);
  if(!clearDirectoryContents(temporaryOutputPath)) {
    LOG_ERROR(L"Failed to clear delombok temporary directory: {}", temporaryOutputPath.wstr());
    return false;
  }
  return true;
}

void removeTemporaryDirectory(const FilePath& temporaryOutputPath) {
  clearDirectoryContents(temporaryOutputPath);
  FileSystem::remove(temporaryOutputPath);
}

DelombokRunner createDelombokRunner(const FilePath& projectDirectoryPath, const FilePath& lombokJarPath) {
  return [projectDirectoryPath, lombokJarPath](
             const std::vector<FilePath>& inputs, const std::vector<FilePath>& sourcePath, const FilePath& outputDirectory) {
    const FilePath javaPath = ApplicationSettings::getInstance()->getJavaPath();
    if(javaPath.empty()) {
      LOG_ERROR("Cannot run delombok because configured Java path is empty.");
      return false;
    }

    FilePath javaExecutablePath = javaPath.getParentDirectory().getParentDirectory().getParentDirectory();
    javaExecutablePath = javaExecutablePath.getConcatenated(L"bin/java");
    if(!javaExecutablePath.exists()) {
      LOG_ERROR(L"Cannot run delombok because Java executable is missing at \"{}\".", javaExecutablePath.wstr());
      return false;
    }

    std::vector<std::wstring> args;
    // A short run: the JIT's top tier and a parallel collector only add start-up work.
    args.push_back(L"-XX:TieredStopAtLevel=1");
    args.push_back(L"-XX:+UseSerialGC");
    args.push_back(L"-jar");
    args.push_back(lombokJarPath.wstr());
    args.push_back(L"delombok");
    for(const FilePath& input : inputs) {
      args.push_back(input.wstr());
    }
    if(!sourcePath.empty()) {
#ifdef _WIN32
      const std::wstring separator = L";";
#else
      const std::wstring separator = L":";
#endif
      std::wstring joined;
      for(const FilePath& directory : sourcePath) {
        joined += (joined.empty() ? L"" : separator) + directory.wstr();
      }
      args.push_back(L"-s");
      args.push_back(joined);
    }
    args.push_back(L"-f");
    args.push_back(L"generateDelombokComment:skip");
    args.push_back(L"-d");
    args.push_back(outputDirectory.wstr());

    const utility::ProcessOutput output = utility::executeProcess(
        javaExecutablePath.wstr(), args, projectDirectoryPath, true, 60000, true);

    if(output.exitCode != 0) {
      LOG_ERROR("Delombok failed with exit code {}: {}", output.exitCode, utility::encodeToUtf8(output.output));
      return false;
    }
    return true;
  };
}

// Lombok writes all directory inputs of a run below one output directory, so roots sharing a relative path run in
// separate groups. Returns the root indices of each group.
std::vector<std::vector<size_t>> groupRootsByRelativePath(const DelombokManifest& manifest) {
  std::vector<std::set<std::wstring>> rootPaths(manifest.sourceDirectories.size());
  for(const auto& entry : manifest.files) {
    rootPaths[entry.first.first].insert(entry.first.second);
  }

  std::vector<std::vector<size_t>> groups;
  std::vector<std::set<std::wstring>> groupPaths;
  for(size_t root = 0; root < rootPaths.size(); root++) {
    size_t group = 0;
    while(group < groups.size() && std::any_of(rootPaths[root].begin(), rootPaths[root].end(), [&](const std::wstring& path) {
            return groupPaths[group].count(path) > 0;
          })) {
      group++;
    }
    if(group == groups.size()) {
      groups.emplace_back();
      groupPaths.emplace_back();
    }
    groups[group].push_back(root);
    groupPaths[group].insert(rootPaths[root].begin(), rootPaths[root].end());
  }
  return groups;
}

// Moves each file of a run's output into the staging directory of the root holding its relative path.
bool distributeDelombokOutput(const FilePath& runOutputPath,
                              const std::vector<size_t>& group,
                              const DelombokManifest& manifest,
                              const std::vector<FilePath>& stagingDirectories) {
  std::map<std::wstring, size_t> owners;
  for(const auto& entry : manifest.files) {
    if(std::count(group.begin(), group.end(), entry.first.first)) {
      owners[entry.first.second] = entry.first.first;
    }
  }

  namespace fs = std::filesystem;
  const fs::path runOutput(runOutputPath.wstr());
  std::error_code ec;
  for(fs::recursive_directory_iterator it(runOutput, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code entryEc;
    if(!it->is_regular_file(entryEc)) {
      continue;
    }
    const std::wstring relativePath = it->path().lexically_relative(runOutput).generic_wstring();
    const auto owner = owners.find(relativePath);
    if(owner == owners.end() && group.size() > 1) {
      // added after the scan, so the next run sees it as changed
      LOG_WARNING(L"Delombok output without an original in the scanned roots: {}", relativePath);
      continue;
    }

    const fs::path target = fs::path(stagingDirectories[owner != owners.end() ? owner->second : group.front()].wstr()) /
        relativePath;
    fs::create_directories(target.parent_path(), entryEc);
    if(!entryEc) {
      fs::rename(it->path(), target, entryEc);
    }
    if(entryEc) {
      LOG_ERROR(L"Failed to move delombok output to: {}", target.wstring());
      return false;
    }
  }
  return !ec;
}

bool delombokWholeTree(const std::vector<FilePath>& sourceDirectories,
                       const FilePath& outputDirectoryPath,
                       const FilePath& projectDirectoryPath,
                       const DelombokManifest& manifest,
                       const DelombokRunner& runDelombok) {
  const FilePath temporaryOutputPath(outputDirectoryPath.wstr() + L".tmp");
  const FilePath runOutputPath(outputDirectoryPath.wstr() + L".run");
  const std::vector<FilePath> stagingDirectories = getDelombokRootDirectories(
      temporaryOutputPath, sourceDirectories, projectDirectoryPath);
  if(!prepareTemporaryDirectory(temporaryOutputPath)) {
    return false;
  }

  const std::vector<std::vector<size_t>> groups = groupRootsByRelativePath(manifest);
  for(const std::vector<size_t>& group : groups) {
    std::vector<FilePath> inputs;
    for(size_t root : group) {
      inputs.push_back(sourceDirectories[root]);
    }
    // the other groups' roots stay visible to type resolution
    if(!prepareTemporaryDirectory(runOutputPath) ||
       !runDelombok(inputs, groups.size() > 1 ? sourceDirectories : std::vector<FilePath>(), runOutputPath) ||
       !distributeDelombokOutput(runOutputPath, group, manifest, stagingDirectories)) {
      return false;
    }
  }
  removeTemporaryDirectory(runOutputPath);

  // only touch files whose delombok output changed, so refreshes re-index just those
  const DirectorySyncResult syncResult = syncDirectoryContents(temporaryOutputPath, outputDirectoryPath);
  LOG_INFO("Delombok output synced: {} written, {} unchanged, {} removed in {} runs",
           syncResult.written,
           syncResult.unchanged,
           syncResult.removed,
           groups.size());

  removeTemporaryDirectory(temporaryOutputPath);

  if(syncResult.failed > 0) {
    LOG_ERROR(L"Failed to sync delombok output into: {}", outputDirectoryPath.wstr());
    return false;
  }

  return outputDirectoryPath.recheckExists();
}

using DelombokOriginal = std::pair<size_t, std::wstring>;    // (root index, relative path)

struct DelombokChanges {
  std::vector<DelombokOriginal> changed;    // changed or added
  std::vector<DelombokOriginal> removed;
};

// Returns nullopt when only a whole-tree run is safe.
std::optional<DelombokChanges> getDelombokChanges(const DelombokManifest& previous, const DelombokManifest& current) {
  if(previous.fingerprint != current.fingerprint || previous.sourceDirectories != current.sourceDirectories ||
     previous.lombokConfigs != current.lombokConfigs) {
    return std::nullopt;
  }

  DelombokChanges changes;
  std::vector<DelombokOriginal> touched;
  for(const auto& [original, stamp] : current.files) {
    const auto it = previous.files.find(original);
    if(it == previous.files.end() || !(it->second == stamp)) {
      changes.changed.push_back(original);
      touched.push_back(original);
    }
  }
  for(const auto& [original, stamp] : previous.files) {
    if(current.files.count(original) == 0) {
      changes.removed.push_back(original);
      touched.push_back(original);
    }
  }

  // a long command line costs more than a whole-tree run saves
  if(changes.changed.size() > 200) {
    return std::nullopt;
  }

  // the output of an unchanged @Delegate, @ExtensionMethod or lombok.val user may depend on any changed file
  for(const DelombokOriginal& original : current.crossFileOriginals) {
    if(!touched.empty() && !std::count(touched.begin(), touched.end(), original)) {
      return std::nullopt;
    }
  }

  for(const DelombokOriginal& original : touched) {
    // a lombok.config changes other files' output, and only a whole-tree run copies non-Java files
    if(!utility::isPostfix<std::wstring>(L".java", original.second)) {
      return std::nullopt;
    }
  }
  return changes;
}

bool delombokChangedFiles(const std::vector<FilePath>& sourceDirectories,
                          const FilePath& outputDirectoryPath,
                          const std::vector<FilePath>& rootOutputDirectories,
                          const DelombokChanges& changes,
                          const DelombokRunner& runDelombok) {
  // Lombok writes a file input as <target>/<file name>, so files sharing a name run separately
  std::vector<std::map<std::wstring, DelombokOriginal>> batches;
  for(const DelombokOriginal& original : changes.changed) {
    const std::wstring fileName = FilePath(original.second).fileName();
    auto batch = std::find_if(
        batches.begin(), batches.end(), [&fileName](const auto& names) { return names.count(fileName) == 0; });
    if(batch == batches.end()) {
      batch = batches.insert(batches.end(), std::map<std::wstring, DelombokOriginal>());
    }
    (*batch)[fileName] = original;
  }

  const FilePath temporaryOutputPath(outputDirectoryPath.wstr() + L".tmp");
  for(const auto& batch : batches) {
    std::vector<FilePath> inputs;
    std::vector<std::pair<FilePath, FilePath>> outputs;
    for(const auto& [fileName, original] : batch) {
      inputs.push_back(sourceDirectories[original.first].getConcatenated(original.second));
      outputs.emplace_back(
          temporaryOutputPath.getConcatenated(fileName), rootOutputDirectories[original.first].getConcatenated(original.second));
    }

    if(!prepareTemporaryDirectory(temporaryOutputPath) || !runDelombok(inputs, sourceDirectories, temporaryOutputPath)) {
      return false;
    }

    const DirectorySyncResult syncResult = syncFiles(outputs);
    LOG_INFO("Delombok output of changed files synced: {} written, {} unchanged", syncResult.written, syncResult.unchanged);
    if(syncResult.failed > 0) {
      return false;
    }
  }
  removeTemporaryDirectory(temporaryOutputPath);

  for(const auto& [root, relativePath] : changes.removed) {
    const FilePath outputFile = rootOutputDirectories[root].getConcatenated(relativePath);
    if(outputFile.recheckExists() && !FileSystem::remove(outputFile)) {
      LOG_ERROR(L"Failed to remove delombok output: {}", outputFile.wstr());
      return false;
    }

    // non-recursive remove fails on non-empty directories, which is what we want here
    for(FilePath directory = outputFile.getParentDirectory();
        directory.wstr().size() > rootOutputDirectories[root].wstr().size() && FileSystem::remove(directory);
        directory = directory.getParentDirectory()) {}
  }
  return true;
}

std::optional<std::map<std::wstring, std::vector<FilePath>>> getSourceDirectoriesBySourceSet(const FilePath& projectDirectoryPath,
                                                                                             const FilePath& gradleInitScriptPath,
                                                                                             bool addTestDirectories) {
  std::map<std::wstring, std::vector<FilePath>> directories;
  std::shared_ptr<JavaEnvironment> javaEnvironment = createPreparedJavaEnvironment();

  const std::string mainOutput = callGradleStringMethod(
      javaEnvironment, "getMainSrcDirs", projectDirectoryPath, gradleInitScriptPath);
  if(showGradleErrorIfNeeded(mainOutput)) {
    return std::nullopt;
  }

  for(const std::string& mainSrcDir : utility::splitToVector(mainOutput, ";")) {
    if(!mainSrcDir.empty()) {
      directories[L"main"].push_back(FilePath(utility::decodeFromUtf8(mainSrcDir)));
    }
  }

  if(addTestDirectories) {
    const std::string testOutput = callGradleStringMethod(
        javaEnvironment, "getTestSrcDirs", projectDirectoryPath, gradleInitScriptPath);
    if(showGradleErrorIfNeeded(testOutput)) {
      return std::nullopt;
    }

    // every other source set, as "<sourceSetName>|<dir>"
    for(const std::string& testSrcDir : utility::splitToVector(testOutput, ";")) {
      const size_t separator = testSrcDir.find('|');
      if(separator != std::string::npos && separator + 1 < testSrcDir.size()) {
        directories[utility::decodeFromUtf8(testSrcDir.substr(0, separator))].push_back(
            FilePath(utility::decodeFromUtf8(testSrcDir.substr(separator + 1))));
      }
    }
  }

  return directories;
}
std::map<std::wstring, std::vector<FilePath>> filterExistingDirectories(
    const std::map<std::wstring, std::vector<FilePath>>& directoriesBySourceSet) {
  std::map<std::wstring, std::vector<FilePath>> existing;
  for(const auto& [sourceSet, directories] : directoriesBySourceSet) {
    for(const FilePath& directory : directories) {
      if(directory.exists()) {
        existing[sourceSet].push_back(directory);
      }
    }
  }
  return existing;
}

bool outputDirectoryHoldsJars(const FilePath& outputDirectoryPath) {
  return outputDirectoryPath.recheckExists() && !FileSystem::getFilePathsFromDirectory(outputDirectoryPath, {L".jar"}).empty();
}

// Asks Gradle for lombok and source directory info. Only stores it when both calls succeeded.
GradleProjectInfo queryGradleProjectInfo(const FilePath& projectDirectoryPath,
                                         const FilePath& outputDirectoryPath,
                                         bool includesTests) {
  GradleProjectInfo info;
  info.fingerprint = gradleBuildFingerprint(projectDirectoryPath);
  info.includesTests = includesTests;

  const std::optional<std::map<std::wstring, FilePath>> lombokJars = getLombokJars(projectDirectoryPath);
  if(!lombokJars) {
    return info;
  }
  info.lombokJars = *lombokJars;

  const auto sourceDirectories = getSourceDirectoriesBySourceSet(projectDirectoryPath, getGradleInitScriptPath(), includesTests);
  if(!sourceDirectories) {
    return info;
  }
  info.sourceDirectories = *sourceDirectories;

  saveGradleProjectInfo(outputDirectoryPath, info);
  return info;
}

GradleProjectInfo getGradleProjectInfo(const FilePath& projectDirectoryPath,
                                       const FilePath& outputDirectoryPath,
                                       bool includesTests) {
  if(const std::optional<GradleProjectInfo> cached = loadGradleProjectInfo(
         outputDirectoryPath, projectDirectoryPath, includesTests)) {
    return *cached;
  }
  return queryGradleProjectInfo(projectDirectoryPath, outputDirectoryPath, includesTests);
}
}    // namespace

FilePath getDelombokOutputDirectory(const FilePath& outputDirectoryPath, const std::wstring& sourceSetName) {
  return outputDirectoryPath.getConcatenated(L"delombok").getConcatenated(sourceSetName);
}

std::vector<FilePath> getDelombokRootDirectories(const FilePath& delombokOutputDirectory,
                                                 const std::vector<FilePath>& sourceDirectories,
                                                 const FilePath& projectDirectoryPath) {
  namespace fs = std::filesystem;
  const fs::path project = fs::absolute(fs::path(projectDirectoryPath.wstr())).lexically_normal();

  std::set<std::wstring> taken;
  std::vector<FilePath> directories;
  for(const FilePath& root : sourceDirectories) {
    const fs::path absoluteRoot = fs::absolute(fs::path(root.wstr())).lexically_normal();
    fs::path relativeRoot = absoluteRoot.lexically_relative(project);
    if(relativeRoot.empty() || *relativeRoot.begin() == "..") {
      relativeRoot = absoluteRoot.relative_path();
    }

    std::wstring name;
    for(const fs::path& part : relativeRoot) {
      if(!part.empty() && part != ".") {
        name += (name.empty() ? L"" : L"_") + part.wstring();
      }
    }
    if(name.empty()) {
      name = L"root";
    }

    std::wstring uniqueName = name;
    for(size_t suffix = 2; !taken.insert(uniqueName).second; suffix++) {
      uniqueName = name + L"_" + std::to_wstring(suffix);
    }
    directories.push_back(delombokOutputDirectory.getConcatenated(uniqueName));
  }
  return directories;
}

bool delombokSourceSet(const std::vector<FilePath>& sourceDirectories,
                       const FilePath& outputDirectory,
                       const FilePath& projectDirectoryPath,
                       const std::string& fingerprint,
                       const DelombokRunner& runDelombok,
                       bool wholeTree) {
  if(sourceDirectories.empty()) {
    return true;
  }

  const std::optional<DelombokManifest> previous = loadDelombokManifest(outputDirectory);
  const DelombokManifest current = scanDelombokOriginals(sourceDirectories, fingerprint, previous);
  const bool outputExists = outputDirectory.recheckExists();
  if(!wholeTree && previous && outputExists && *previous == current) {
    LOG_INFO(L"Delombok output is up to date: {}", outputDirectory.wstr());
    return true;
  }

  // a run that fails half way must not leave a manifest claiming its outputs are current
  removeDelombokManifest(outputDirectory);

  const std::optional<DelombokChanges> changes = !wholeTree && previous && outputExists ? getDelombokChanges(*previous, current) :
                                                                                          std::nullopt;
  if(changes) {
    LOG_INFO("Delomboking {} changed files, removing {} outputs", changes->changed.size(), changes->removed.size());
  }
  if(!changes ||
     !delombokChangedFiles(sourceDirectories,
                           outputDirectory,
                           getDelombokRootDirectories(outputDirectory, sourceDirectories, projectDirectoryPath),
                           *changes,
                           runDelombok)) {
    if(changes) {
      LOG_WARNING("Delombok of changed files failed, delomboking the whole tree.");
    }
    if(!delombokWholeTree(sourceDirectories, outputDirectory, projectDirectoryPath, current, runDelombok)) {
      return false;
    }
  }

  saveDelombokManifest(outputDirectory, current);
  return true;
}

bool gradleCopyDependencies(const FilePath& projectDirectoryPath, const FilePath& outputDirectoryPath, bool addTestDependencies) {
  if(gradleDependenciesUpToDate(outputDirectoryPath, projectDirectoryPath, addTestDependencies) &&
     outputDirectoryHoldsJars(outputDirectoryPath)) {
    LOG_INFO("Gradle build files unchanged, reusing exported dependencies.");
    return true;
  }

  const std::string exportedFingerprint = gradleBuildFingerprint(projectDirectoryPath);
  const FilePath gradleInitScriptPath = getGradleInitScriptPath();
  std::shared_ptr<JavaEnvironment> javaEnvironment = createPreparedJavaEnvironment();
  bool success = javaEnvironment->callStaticVoidMethod("com/sourcetrail/gradle/InfoRetriever",
                                                       "copyCompileLibs",
                                                       utility::encodeToUtf8(projectDirectoryPath.wstr()),
                                                       utility::encodeToUtf8(gradleInitScriptPath.wstr()),
                                                       utility::encodeToUtf8(outputDirectoryPath.wstr()));

  if(success && addTestDependencies) {
    success = javaEnvironment->callStaticVoidMethod("com/sourcetrail/gradle/InfoRetriever",
                                                    "copyTestCompileLibs",
                                                    utility::encodeToUtf8(projectDirectoryPath.wstr()),
                                                    utility::encodeToUtf8(gradleInitScriptPath.wstr()),
                                                    utility::encodeToUtf8(outputDirectoryPath.wstr()));
  }

  if(success) {
    // refresh the cached info next to the freshly exported jars
    queryGradleProjectInfo(projectDirectoryPath, outputDirectoryPath, addTestDependencies);
    markGradleDependenciesExported(outputDirectoryPath, exportedFingerprint, addTestDependencies);
  }

  return success;
}

bool gradlePrepareLombokSources(const FilePath& projectDirectoryPath,
                                const FilePath& outputDirectoryPath,
                                bool addTestSources,
                                bool wholeTree) {
  const GradleProjectInfo info = getGradleProjectInfo(projectDirectoryPath, outputDirectoryPath, addTestSources);
  if(info.lombokJars.empty()) {
    return true;
  }

  // the cached source sets are main only, or every source set when tests are included
  for(const auto& [sourceSet, directories] : filterExistingDirectories(info.sourceDirectories)) {
    const auto jarIt = info.lombokJars.find(sourceSet);
    if(jarIt != info.lombokJars.end() &&
       !delombokSourceSet(directories,
                          getDelombokOutputDirectory(outputDirectoryPath, sourceSet),
                          projectDirectoryPath,
                          info.fingerprint,
                          createDelombokRunner(projectDirectoryPath, jarIt->second),
                          wholeTree)) {
      return false;
    }
  }

  return true;
}

std::vector<FilePath> gradleGetAllSourceDirectories(const FilePath& projectDirectoryPath,
                                                    const FilePath& outputDirectoryPath,
                                                    bool addTestDirectories) {
  const GradleProjectInfo info = getGradleProjectInfo(projectDirectoryPath, outputDirectoryPath, addTestDirectories);

  std::vector<FilePath> directories;
  for(const auto& [sourceSet, sourceSetDirectories] : filterExistingDirectories(info.sourceDirectories)) {
    if(info.lombokJars.count(sourceSet) > 0) {
      for(const FilePath& delombokDirectory : getDelombokRootDirectories(
              getDelombokOutputDirectory(outputDirectoryPath, sourceSet), sourceSetDirectories, projectDirectoryPath)) {
        if(delombokDirectory.exists()) {
          directories.push_back(delombokDirectory);
        }
      }
    } else {
      utility::append(directories, sourceSetDirectories);
    }
  }

  LOG_INFO("Found {} directories on system.", directories.size());
  return directories;
}
}    // namespace utility
