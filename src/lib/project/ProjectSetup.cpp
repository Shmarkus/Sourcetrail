#include "ProjectSetup.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <sstream>

#include <system_error>

#include "language_packages.h"
#include "logging.h"
#include "ProjectSettings.h"
#include "SourceGroupSettingsJavaEmpty.h"
#include "SourceGroupSettingsJavaGradle.h"
#include "SourceGroupSettingsJavaMaven.h"
#include "SourceGroupSettingsPython.h"
#include "SourceGroupSettingsTypeScript.h"
#include "utilityString.h"
#include "utilityUuid.h"
#if BUILD_CXX_LANGUAGE_PACKAGE
#  include "SourceGroupSettingsCxxCdb.h"
#endif    // BUILD_CXX_LANGUAGE_PACKAGE

namespace fs = std::filesystem;

namespace {

constexpr auto ProjectFolderName = L".sourcetrail";
constexpr auto TsconfigName = L"tsconfig.json";
constexpr auto CompilationDatabaseName = L"compile_commands.json";

// The Java standard of a project whose build names no Java version.
constexpr auto DefaultJavaStandard = 21;

fs::path toPath(const FilePath& path) {
  return fs::path{path.wstr()}.lexically_normal();
}

// The folder without a trailing separator, so its file name is the folder's name.
fs::path folderPath(const FilePath& folder) {
  fs::path path = toPath(folder);
  if(!path.has_filename() && path.has_parent_path()) {
    path = path.parent_path();
  }
  return path;
}

bool isFile(const fs::path& path) {
  std::error_code error;
  return fs::is_regular_file(path, error);
}

// The Gradle file the source group points at; Gradle builds from its directory, so any of them will do.
fs::path gradleFile(const fs::path& folder) {
  for(const auto* name : {L"build.gradle", L"build.gradle.kts", L"settings.gradle", L"settings.gradle.kts"}) {
    if(isFile(folder / name)) {
      return folder / name;
    }
  }
  return {};
}

std::string readFile(const fs::path& path) {
  std::ifstream file{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

// The text without what lies between each opening and the following closing marker, markers included.
std::string withoutComments(std::string text, const std::string& opening, const std::string& closing) {
  for(size_t start = text.find(opening); start != std::string::npos; start = text.find(opening, start)) {
    const size_t end = text.find(closing, start + opening.size());
    text.erase(start, end == std::string::npos ? std::string::npos : end + closing.size() - start);
  }
  return text;
}

// "25" and "VERSION_25" give 25, "1.8" and "VERSION_1_8" give 8; anything else gives 0.
int javaLevel(std::string value) {
  if(value.starts_with("VERSION_")) {
    value.erase(0, 8);
  }
  if(value.starts_with("1.") || value.starts_with("1_")) {
    value.erase(0, 2);
  }
  if(value.empty() || value.size() > 3 || !std::ranges::all_of(value, [](char c) { return c >= '0' && c <= '9'; })) {
    return 0;
  }
  return std::stoi(value);
}

// The Java version a root build.gradle(.kts) names, or 0: the source compatibility, else the toolchain's language
// version, else the target compatibility, else any JavaVersion.VERSION_<n>. Plain text matching, line by line, without
// comments; Gradle does not run.
int gradleJavaLevel(const fs::path& folder) {
  // a value: 25, '1.8', "21", JavaVersion.VERSION_17
  const std::string value = R"([=(\s]\s*(?:JavaVersion\.)?['"]?(VERSION_[0-9_]+|[0-9]+(?:\.[0-9]+)?)\b)";
  const std::regex patterns[] = {
      std::regex{R"(\bsourceCompatibility\s*)" + value},
      std::regex{R"(\b(?:JavaLanguageVersion\.of|jvmToolchain)\s*\(\s*['"]?([0-9]+)\b)"},
      std::regex{R"(\btargetCompatibility\s*)" + value},
      std::regex{R"(\bJavaVersion\.(VERSION_[0-9_]+)\b)"},
  };

  for(const auto* name : {L"build.gradle", L"build.gradle.kts"}) {
    if(!isFile(folder / name)) {
      continue;
    }
    std::vector<std::string> lines;
    std::istringstream text{withoutComments(readFile(folder / name), "/*", "*/")};
    for(std::string line; std::getline(text, line);) {
      // a // that starts the line or follows a blank, so that "https://" stays
      for(size_t slashes = line.find("//"); slashes != std::string::npos; slashes = line.find("//", slashes + 2)) {
        if(slashes == 0 || line[slashes - 1] == ' ' || line[slashes - 1] == '\t') {
          line.erase(slashes);
          break;
        }
      }
      lines.push_back(std::move(line));
    }

    for(const std::regex& pattern : patterns) {
      for(const std::string& line : lines) {
        // std::regex recurses per character; a build script line is short
        if(line.size() > 1000) {
          continue;
        }
        if(std::smatch match; std::regex_search(line, match, pattern) && javaLevel(match[1].str()) > 0) {
          return javaLevel(match[1].str());
        }
      }
    }
  }
  return 0;
}

// The text of the first <tag>...</tag> in the XML, trimmed; empty when there is none.
std::string xmlValue(const std::string& xml, const std::string& tag) {
  const std::string opening = "<" + tag + ">";
  const size_t start = xml.find(opening);
  if(start == std::string::npos) {
    return {};
  }
  const size_t end = xml.find("</" + tag + ">", start);
  if(end == std::string::npos) {
    return {};
  }
  std::string value = xml.substr(start + opening.size(), end - start - opening.size());
  const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while(!value.empty() && blank(value.back())) {
    value.pop_back();
  }
  const auto first = std::ranges::find_if_not(value, blank);
  return {first, value.end()};
}

// The Java version the root pom.xml names, or 0: maven.compiler.release, else the compiler plugin's <release>, else
// maven.compiler.source, else java.version. A value ${name} is the pom's <name>; Maven does not run.
int mavenJavaLevel(const fs::path& folder) {
  if(!isFile(folder / "pom.xml")) {
    return 0;
  }
  const std::string pom = withoutComments(readFile(folder / "pom.xml"), "<!--", "-->");
  for(const auto* tag : {"maven.compiler.release", "release", "maven.compiler.source", "java.version"}) {
    std::string value = xmlValue(pom, tag);
    if(value.starts_with("${") && value.ends_with("}")) {
      value = xmlValue(pom, value.substr(2, value.size() - 3));
    }
    if(const int level = javaLevel(value); level > 0) {
      return level;
    }
  }
  return 0;
}

// The Java standard for a new source group: the version the build names, at most the newest one Sourcetrail
// supports, else DefaultJavaStandard.
std::wstring javaStandardFor(const fs::path& folder, JavaBuildKind kind, const SourceGroupSettingsWithJavaStandard& settings) {
  int level = 0;
  if(kind == JavaBuildKind::Gradle) {
    level = gradleJavaLevel(folder);
  } else if(kind == JavaBuildKind::Maven) {
    level = mavenJavaLevel(folder);
  }
  if(level <= 0) {
    level = DefaultJavaStandard;
  }
  return std::to_wstring(std::min(level, javaLevel(utility::encodeToUtf8(settings.getAvailableJavaStandards().front()))));
}

// Relative to the project file's directory, so the checkout can move.
FilePath relativeToProject(const fs::path& path, const fs::path& projectDirectory) {
  return FilePath{path.lexically_relative(projectDirectory).wstring()};
}

std::shared_ptr<SourceGroupSettings> createJavaSourceGroup(const fs::path& folder,
                                                           JavaBuildKind kind,
                                                           const ProjectSettings* projectSettings) {
  const fs::path projectDirectory = folder / ProjectFolderName;
  const std::string id = utility::getUuidString();

  switch(kind) {
  case JavaBuildKind::Gradle: {
    auto settings = std::make_shared<SourceGroupSettingsJavaGradle>(id, projectSettings);
    settings->setName("Gradle");
    settings->setGradleProjectFilePath(relativeToProject(gradleFile(folder), projectDirectory));
    settings->setShouldIndexGradleTests(true);
    settings->setJavaStandard(javaStandardFor(folder, kind, *settings));
    return settings;
  }
  case JavaBuildKind::Maven: {
    auto settings = std::make_shared<SourceGroupSettingsJavaMaven>(id, projectSettings);
    settings->setName("Maven");
    settings->setMavenProjectFilePath(relativeToProject(folder / "pom.xml", projectDirectory));
    settings->setShouldIndexMavenTests(true);
    settings->setJavaStandard(javaStandardFor(folder, kind, *settings));
    return settings;
  }
  case JavaBuildKind::None: {
    auto settings = std::make_shared<SourceGroupSettingsJavaEmpty>(id, projectSettings);
    settings->setName("Java");
    // absolute: the settings page checks it before .sourcetrail/ exists, which a relative ".." needs
    settings->setSourcePaths({FilePath{folder.wstring()}});
    settings->setExcludeFilterStrings({L"**/" + std::wstring{ProjectFolderName} + L"/**"});
    settings->setJavaStandard(javaStandardFor(folder, kind, *settings));
    return settings;
  }
  }
  return nullptr;
}

std::shared_ptr<SourceGroupSettings> createTypeScriptSourceGroup(const fs::path& folder, const ProjectSettings* projectSettings) {
  auto settings = std::make_shared<SourceGroupSettingsTypeScript>(utility::getUuidString(), projectSettings);
  settings->setName("TypeScript");
  settings->setTsconfigPath(relativeToProject(folder / TsconfigName, folder / ProjectFolderName));
  return settings;
}

std::shared_ptr<SourceGroupSettings> createPythonSourceGroup(const fs::path& folder, const ProjectSettings* projectSettings) {
  auto settings = std::make_shared<SourceGroupSettingsPython>(utility::getUuidString(), projectSettings);
  settings->setName("Python");
  // the indexer finds the folder's environment itself
  settings->setProjectRoot(relativeToProject(folder, folder / ProjectFolderName));
  return settings;
}

#if BUILD_CXX_LANGUAGE_PACKAGE
std::shared_ptr<SourceGroupSettings> createCxxSourceGroup(const fs::path& folder,
                                                          const FilePath& compilationDatabase,
                                                          const ProjectSettings* projectSettings) {
  auto settings = std::make_shared<SourceGroupSettingsCxxCdb>(utility::getUuidString(), projectSettings);
  settings->setName("C/C++");
  const fs::path projectDirectory = folder / ProjectFolderName;
  settings->setCompilationDatabasePath(relativeToProject(toPath(compilationDatabase), projectDirectory));
  // Only the headers below an indexed header path are indexed; the folder's own are part of the project.
  settings->setIndexedHeaderPaths({relativeToProject(folder, projectDirectory)});
  // but not the dependencies CMake's FetchContent puts into the build folder the database sits in
  if(const fs::path buildFolder = toPath(compilationDatabase).parent_path(); buildFolder != folder) {
    settings->setExcludeFilterStrings({relativeToProject(buildFolder / "_deps", projectDirectory).wstr() + L"/**"});
  }
  return settings;
}

// The subfolders of a folder, by name.
std::vector<fs::path> subfolders(const fs::path& folder) {
  std::vector<fs::path> folders;
  std::error_code error;
  for(const auto& entry : fs::directory_iterator{folder, error}) {
    std::error_code typeError;
    if(entry.is_directory(typeError)) {
      folders.push_back(entry.path());
    }
  }
  std::ranges::sort(folders);
  return folders;
}

FilePath findCompilationDatabase(const fs::path& folder) {
  std::vector<fs::path> candidates = {folder, folder / "build"};
  for(const fs::path& subfolder : subfolders(folder)) {
    if(subfolder.filename().wstring().starts_with(L"cmake-build-")) {
      candidates.push_back(subfolder);
    }
  }
  for(const fs::path& subfolder : subfolders(folder / "out" / "build")) {
    candidates.push_back(subfolder);
  }
  for(const fs::path& candidate : candidates) {
    if(isFile(candidate / CompilationDatabaseName)) {
      return FilePath{(candidate / CompilationDatabaseName).wstring()};
    }
  }
  return {};
}
#endif    // BUILD_CXX_LANGUAGE_PACKAGE

// A pyproject.toml, setup.py, setup.cfg or requirements*.txt at the folder's top level.
bool hasPythonProjectFile(const fs::path& folder) {
  for(const auto* name : {L"pyproject.toml", L"setup.py", L"setup.cfg"}) {
    if(isFile(folder / name)) {
      return true;
    }
  }
  std::error_code error;
  for(const auto& entry : fs::directory_iterator{folder, error}) {
    const std::wstring name = entry.path().filename().wstring();
    if(name.starts_with(L"requirements") && name.ends_with(L".txt") && isFile(entry.path())) {
      return true;
    }
  }
  return false;
}

bool isCxxSource(const fs::path& path) {
  static const std::set<std::wstring> extensions = {L".c", L".cc", L".cpp", L".cxx", L".h", L".hh", L".hpp", L".hxx"};
  return extensions.contains(path.extension().wstring());
}

// .py files below the folder, outside the folders the Python indexer skips, and no .java file there (nor a C/C++ source
// or header when `cxxSourcesCount`): the sources of a folder without a build are Python.
bool hasOnlyPythonSources(const fs::path& folder, bool cxxSourcesCount) {
  bool python = false;
  std::error_code error;
  for(auto it = fs::recursive_directory_iterator{folder, fs::directory_options::skip_permission_denied, error};
      !error && it != fs::recursive_directory_iterator{};
      it.increment(error)) {
    const fs::path& path = it->path();
    std::error_code typeError;
    if(it->is_directory(typeError)) {
      if(SourceGroupSettingsPython::isUnwalkedDirectory(path.filename().wstring())) {
        it.disable_recursion_pending();
      }
    } else if(path.extension() == ".java" || (cxxSourcesCount && isCxxSource(path))) {
      return false;
    } else if(path.extension() == ".py") {
      python = true;
    }
  }
  return python;
}
}    // namespace

JavaBuildKind detectJavaBuild(const FilePath& folder) {
  const fs::path path = folderPath(folder);
  if(!gradleFile(path).empty()) {
    return JavaBuildKind::Gradle;
  }
  if(isFile(path / "pom.xml")) {
    return JavaBuildKind::Maven;
  }
  return JavaBuildKind::None;
}

DetectedBuild detectBuild(const FilePath& folder) {
  const fs::path path = folderPath(folder);
  DetectedBuild build{.java = detectJavaBuild(folder), .typeScript = isFile(path / TsconfigName)};
#if BUILD_CXX_LANGUAGE_PACKAGE
  build.compilationDatabase = findCompilationDatabase(path);
  build.cmakeWithoutDatabase = build.compilationDatabase.empty() && isFile(path / "CMakeLists.txt");
#endif    // BUILD_CXX_LANGUAGE_PACKAGE
  // Loose .py files next to a compilation database are its scripts; next to a CMakeLists.txt alone they are the project
  // unless C/C++ sources are there too.
  build.python = hasPythonProjectFile(path) ||
      (build.java == JavaBuildKind::None && !build.typeScript && build.compilationDatabase.empty() &&
       hasOnlyPythonSources(path, build.cmakeWithoutDatabase));
  return build;
}

std::wstring compilationDatabaseCommand() {
  return L"cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON";
}

FilePath projectFileFor(const FilePath& folder) {
  const fs::path path = folderPath(folder);
  return FilePath{(path / ProjectFolderName / path.filename()).wstring() + L".srctrlprj"};
}

FilePath existingProjectFile(const FilePath& folder) {
  std::error_code error;
  fs::directory_iterator entries{folderPath(folder) / ProjectFolderName, error};
  if(error) {
    return {};
  }

  std::vector<fs::path> projectFiles;
  for(const auto& entry : entries) {
    if(entry.path().extension() == ".srctrlprj" && isFile(entry.path())) {
      projectFiles.push_back(entry.path());
    }
  }
  if(projectFiles.empty()) {
    return {};
  }
  return FilePath{std::ranges::min(projectFiles).wstring()};
}

FilePath rootTsconfigFor(const FilePath& projectDirectory) {
  const fs::path directory = folderPath(projectDirectory);
  if(!directory.is_absolute()) {
    return {};    // no project location yet, which would read the working directory
  }
  for(const fs::path& candidate : {directory, directory.parent_path()}) {
    if(isFile(candidate / TsconfigName) && (candidate == directory || directory.filename() == ProjectFolderName)) {
      return FilePath{(candidate / TsconfigName).wstring()};
    }
  }
  return {};
}

FilePath createProject(const FilePath& folder, const DetectedBuild& build) {
  if(!prepareProjectFolder(folder)) {
    return {};
  }

  const auto settings = projectSettingsFor(folder, build);
  if(!settings->save(settings->getFilePath())) {
    LOG_ERROR("Cannot write {}", settings->getFilePath().str());
    return {};
  }
  return settings->getFilePath();
}

std::shared_ptr<ProjectSettings> projectSettingsFor(const FilePath& folder, const DetectedBuild& build) {
  const fs::path path = folderPath(folder);
  auto settings = std::make_shared<ProjectSettings>();
  settings->setProjectFilePath(path.filename().wstring(), FilePath{(path / ProjectFolderName).wstring()});
  settings->setVersion(ProjectSettings::VERSION);

  std::vector<std::shared_ptr<SourceGroupSettings>> groups;
  if(build.java != JavaBuildKind::None || (!build.typeScript && !build.python && build.compilationDatabase.empty())) {
    groups.push_back(createJavaSourceGroup(path, build.java, settings.get()));
  }
  if(build.typeScript) {
    groups.push_back(createTypeScriptSourceGroup(path, settings.get()));
  }
  if(build.python) {
    groups.push_back(createPythonSourceGroup(path, settings.get()));
  }
#if BUILD_CXX_LANGUAGE_PACKAGE
  if(!build.compilationDatabase.empty()) {
    groups.push_back(createCxxSourceGroup(path, build.compilationDatabase, settings.get()));
  }
#endif    // BUILD_CXX_LANGUAGE_PACKAGE
  settings->setAllSourceGroupSettings(groups);
  return settings;
}

namespace {
bool addSourceGroup(const FilePath& projectFile,
                    const std::function<std::shared_ptr<SourceGroupSettings>(const ProjectSettings*)>& createGroup) {
  ProjectSettings settings;
  if(!settings.load(projectFile)) {
    LOG_ERROR("Cannot read {}", projectFile.str());
    return false;
  }

  auto groups = settings.getAllSourceGroupSettings();
  groups.push_back(createGroup(&settings));
  settings.setAllSourceGroupSettings(groups);
  if(!settings.save(projectFile)) {
    LOG_ERROR("Cannot write {}", projectFile.str());
    return false;
  }
  return true;
}

bool hasSourceGroup(const FilePath& projectFile, SourceGroupType type) {
  ProjectSettings settings;
  return settings.load(projectFile) &&
      std::ranges::any_of(settings.getAllSourceGroupSettings(), [type](const auto& group) { return group->getType() == type; });
}
}    // namespace

bool addTypeScriptGroup(const FilePath& projectFile, const FilePath& folder) {
  return addSourceGroup(projectFile, [&folder](const ProjectSettings* settings) {
    return createTypeScriptSourceGroup(folderPath(folder), settings);
  });
}

bool addPythonGroup(const FilePath& projectFile, const FilePath& folder) {
  return addSourceGroup(
      projectFile, [&folder](const ProjectSettings* settings) { return createPythonSourceGroup(folderPath(folder), settings); });
}

bool addCxxGroup(const FilePath& projectFile, const FilePath& folder) {
#if BUILD_CXX_LANGUAGE_PACKAGE
  const FilePath compilationDatabase = findCompilationDatabase(folderPath(folder));
  if(compilationDatabase.empty()) {
    LOG_ERROR("No compilation database in {}", folder.str());
    return false;
  }
  return addSourceGroup(projectFile, [&folder, &compilationDatabase](const ProjectSettings* settings) {
    return createCxxSourceGroup(folderPath(folder), compilationDatabase, settings);
  });
#else
  LOG_ERROR("Cannot add C/C++ to {}: built without C/C++ support", projectFile.str());
  return false;
#endif    // BUILD_CXX_LANGUAGE_PACKAGE
}

FilePath pythonRootFor(const FilePath& projectDirectory) {
  if(projectDirectory.empty() || folderPath(projectDirectory).filename() != ProjectFolderName) {
    return {};
  }
  return FilePath{L".."};
}

bool prepareProjectFolder(const FilePath& folder) {
  const fs::path projectDirectory = folderPath(folder) / ProjectFolderName;

  std::error_code error;
  fs::create_directories(projectDirectory, error);
  if(error) {
    LOG_ERROR("Cannot create {}: {}", projectDirectory.string(), error.message());
    return false;
  }

  std::ofstream gitignore{projectDirectory / ".gitignore", std::ios::binary | std::ios::trunc};
  gitignore << "*\n";
  if(!gitignore) {
    LOG_ERROR("Cannot write {}", (projectDirectory / ".gitignore").string());
    return false;
  }
  return true;
}

ProjectChoice openOrCreateProject(const FilePath& folder, const std::function<bool()>& acceptPlainJava) {
  const DetectedBuild build = detectBuild(folder);

  if(FilePath existing = existingProjectFile(folder); !existing.empty()) {
    const bool canAddTypeScript = build.typeScript && !hasSourceGroup(existing, SOURCE_GROUP_TYPESCRIPT);
    const bool canAddPython = build.python && !hasSourceGroup(existing, SOURCE_GROUP_PYTHON);
#if BUILD_CXX_LANGUAGE_PACKAGE
    const bool canAddCxx = !build.compilationDatabase.empty() && !hasSourceGroup(existing, SOURCE_GROUP_CXX_CDB);
#else
    const bool canAddCxx = false;
#endif    // BUILD_CXX_LANGUAGE_PACKAGE
    return {.projectFile = existing,
            .canAddTypeScript = canAddTypeScript,
            .canAddPython = canAddPython,
            .canAddCxx = canAddCxx,
            .build = build};
  }

  if(build.java == JavaBuildKind::None && !build.typeScript && !build.python && build.compilationDatabase.empty()) {
    if(build.cmakeWithoutDatabase) {
      return {.compilationDatabaseMissing = true, .build = build};
    }
    if(!acceptPlainJava()) {
      return {};
    }
    return {.projectFile = projectFileFor(folder), .needsSetup = true, .build = build};
  }

  FilePath projectFile = createProject(folder, build);
  const bool created = !projectFile.empty();
  return {.projectFile = std::move(projectFile), .created = created, .build = build};
}
