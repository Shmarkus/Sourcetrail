#include "utilityGradleCache.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "logging.h"
#include "utilityString.h"

namespace utility {
namespace {
const char* const CACHE_FILE_NAME = "sourcetrail-gradle-project.cache";
const char* const EXPORTED_FILE_NAME = "sourcetrail-gradle-exported.cache";
const char* const CACHE_VERSION = "3";
const char* const MANIFEST_VERSION = "4";    // 4: one output directory per root

const std::set<std::string> BUILD_FILE_NAMES = {
    "build.gradle", "build.gradle.kts", "settings.gradle", "settings.gradle.kts", "gradle.properties"};

const std::set<std::string> SKIPPED_DIRECTORY_NAMES = {"build", ".gradle", ".git", "node_modules"};

std::filesystem::path getCachePath(const FilePath& outputDirectory) {
  return std::filesystem::path(outputDirectory.str()) / CACHE_FILE_NAME;
}

std::filesystem::path getExportedPath(const FilePath& outputDirectory) {
  return std::filesystem::path(outputDirectory.str()) / EXPORTED_FILE_NAME;
}

std::string exportedMarker(const std::string& fingerprint, bool includesTests) {
  return std::string(CACHE_VERSION) + "\n" + (includesTests ? "1" : "0") + "\n" + fingerprint;
}

void writeFileAtomically(const std::filesystem::path& path, const std::string& content, const FilePath& outputDirectory) {
  const std::filesystem::path temporaryPath = path.string() + ".tmp";
  {
    std::ofstream stream(temporaryPath, std::ios::binary | std::ios::trunc);
    stream << content;
    if(!stream) {
      LOG_ERROR(L"Failed to write Gradle cache: {}", outputDirectory.wstr());
      return;
    }
  }
  std::error_code ec;
  std::filesystem::rename(temporaryPath, path, ec);
  if(ec) {
    LOG_ERROR(L"Failed to store Gradle cache: {}", outputDirectory.wstr());
  }
}

bool isInBuildSrc(const std::filesystem::path& relativePath) {
  return std::any_of(
      relativePath.begin(), relativePath.end(), [](const std::filesystem::path& component) { return component == "buildSrc"; });
}

std::filesystem::path getUserPropertiesPath() {
  std::filesystem::path home;
  if(const char* userHome = std::getenv("GRADLE_USER_HOME"); userHome != nullptr && *userHome != '\0') {
    home = userHome;
  } else if(const char* homeDirectory = std::getenv("HOME"); homeDirectory != nullptr && *homeDirectory != '\0') {
    home = std::filesystem::path(homeDirectory) / ".gradle";
  } else {
    return {};
  }
  return home / "gradle.properties";
}

bool isBuildFile(const std::filesystem::path& path) {
  const std::string name = path.filename().string();
  if(BUILD_FILE_NAMES.count(name) > 0) {
    return true;
  }

  const std::string parentName = path.parent_path().filename().string();
  return (parentName == "gradle" && name == "libs.versions.toml") ||
      (parentName == "wrapper" && name == "gradle-wrapper.properties" && path.parent_path().parent_path().filename() == "gradle");
}

std::string joinPath(const FilePath& path) {
  return path.str();
}

std::filesystem::path getManifestPath(const FilePath& delombokOutputDirectory) {
  return std::filesystem::path(delombokOutputDirectory.str() + ".manifest");
}

std::optional<FileStamp> stampFile(const std::filesystem::path& path) {
  std::error_code sizeEc;
  std::error_code timeEc;
  const auto size = std::filesystem::file_size(path, sizeEc);
  const auto mtime = std::filesystem::last_write_time(path, timeEc);
  if(sizeEc || timeEc) {
    return std::nullopt;
  }
  return FileStamp{size, std::chrono::duration_cast<std::chrono::nanoseconds>(mtime.time_since_epoch()).count()};
}

std::string formatStamp(const FileStamp& stamp) {
  return std::to_string(stamp.size) + "\t" + std::to_string(stamp.mtime);
}

// FNV-1a, so the stored value does not depend on the standard library's std::hash
std::string hashFingerprint(const std::string& fingerprint) {
  uint64_t hash = 14695981039346656037ull;
  for(const unsigned char c : fingerprint) {
    hash = (hash ^ c) * 1099511628211ull;
  }
  std::ostringstream out;
  out << std::hex << hash;
  return out.str();
}

// @Delegate and @ExtensionMethod generate code from other types, and lombok.val and lombok.var
// resolve to a concrete type on Java 8, so their output changes with those types
bool usesCrossFileLombok(const std::filesystem::path& file) {
  std::ifstream stream(file, std::ios::binary);
  std::ostringstream content;
  content << stream.rdbuf();
  const std::string text = content.str();
  for(const char* marker : {"@Delegate",
                            "lombok.experimental.Delegate",
                            "lombok.Delegate",
                            "@ExtensionMethod",
                            "lombok.experimental.ExtensionMethod",
                            "lombok.val",
                            "lombok.var"}) {
    if(text.find(marker) != std::string::npos) {
      return true;
    }
  }
  // val through a wildcard import; any "val " counts, a false hit only costs a whole-tree run
  return text.find("import lombok.*;") != std::string::npos && text.find("val ") != std::string::npos;
}

// Reuses the previous answer for an unchanged file and reads the file otherwise.
bool isCrossFileOriginal(const std::optional<DelombokManifest>& previous,
                         const std::pair<size_t, std::wstring>& original,
                         const FileStamp& stamp,
                         const std::filesystem::path& file) {
  if(previous) {
    const auto it = previous->files.find(original);
    if(it != previous->files.end() && it->second == stamp) {
      return previous->crossFileOriginals.count(original) > 0;
    }
  }
  return file.extension() == ".java" && usesCrossFileLombok(file);
}

std::optional<FileStamp> parseStamp(const std::string& size, const std::string& mtime) {
  try {
    return FileStamp{std::stoull(size), std::stoll(mtime)};
  } catch(const std::exception&) {
    return std::nullopt;
  }
}
}    // namespace

std::vector<FilePath> gradleBuildFiles(const FilePath& projectDirectory) {
  std::vector<FilePath> files;
  std::error_code ec;
  std::filesystem::recursive_directory_iterator it(
      projectDirectory.str(), std::filesystem::directory_options::skip_permission_denied, ec);
  const std::filesystem::recursive_directory_iterator end;
  for(; !ec && it != end; it.increment(ec)) {
    std::error_code entryEc;
    if(it->is_directory(entryEc)) {
      if(SKIPPED_DIRECTORY_NAMES.count(it->path().filename().string()) > 0) {
        it.disable_recursion_pending();
      }
    } else if((isInBuildSrc(it->path().lexically_relative(projectDirectory.str())) || isBuildFile(it->path())) &&
              it->is_regular_file(entryEc)) {
      files.push_back(FilePath(it->path().wstring()));
    }
  }

  std::sort(files.begin(), files.end(), [](const FilePath& a, const FilePath& b) { return a.str() < b.str(); });

  const std::filesystem::path userProperties = getUserPropertiesPath();
  std::error_code propertiesEc;
  if(!userProperties.empty() && std::filesystem::is_regular_file(userProperties, propertiesEc)) {
    files.push_back(FilePath(userProperties.wstring()));
  }
  return files;
}

std::string gradleBuildFingerprint(const FilePath& projectDirectory) {
  std::string fingerprint;
  for(const FilePath& file : gradleBuildFiles(projectDirectory)) {
    std::error_code ec;
    const std::filesystem::path path(file.str());
    const auto size = std::filesystem::file_size(path, ec);
    const auto mtime = std::filesystem::last_write_time(path, ec).time_since_epoch().count();
    fingerprint += file.str() + "|" + std::to_string(size) + "|" + std::to_string(mtime) + "\n";
  }
  return fingerprint;
}

std::optional<GradleProjectInfo> loadGradleProjectInfo(const FilePath& outputDirectory,
                                                       const FilePath& projectDirectory,
                                                       bool includesTests) {
  std::ifstream stream(getCachePath(outputDirectory), std::ios::binary);
  if(!stream) {
    return std::nullopt;
  }

  GradleProjectInfo info;
  bool versionOk = false;
  std::vector<std::pair<FilePath, bool>> recordedExistence;
  std::string line;
  while(std::getline(stream, line)) {
    const std::vector<std::string> parts = splitToVector(line, "\t");
    if(parts.empty()) {
      continue;
    }

    if(parts[0] == "version" && parts.size() == 2) {
      versionOk = parts[1] == CACHE_VERSION;
    } else if(parts[0] == "includesTests" && parts.size() == 2) {
      info.includesTests = parts[1] == "1";
    } else if(parts[0] == "fp" && parts.size() >= 2) {
      info.fingerprint += line.substr(3) + "\n";
    } else if(parts[0] == "lombok" && parts.size() == 3) {
      info.lombokJars[decodeFromUtf8(parts[1])] = FilePath(decodeFromUtf8(parts[2]));
    } else if(parts[0] == "root" && parts.size() == 4) {
      const FilePath root(decodeFromUtf8(parts[2]));
      info.sourceDirectories[decodeFromUtf8(parts[1])].push_back(root);
      recordedExistence.emplace_back(root, parts[3] == "1");
    }
  }

  if(!versionOk || info.includesTests != includesTests || info.fingerprint != gradleBuildFingerprint(projectDirectory)) {
    return std::nullopt;
  }

  for(const auto& [root, existed] : recordedExistence) {
    if(root.recheckExists() != existed) {
      return std::nullopt;
    }
  }

  return info;
}

void saveGradleProjectInfo(const FilePath& outputDirectory, const GradleProjectInfo& info) {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(outputDirectory.str()), ec);

  std::ostringstream out;
  out << "version\t" << CACHE_VERSION << "\n";
  out << "includesTests\t" << (info.includesTests ? "1" : "0") << "\n";
  for(const auto& [sourceSet, jar] : info.lombokJars) {
    out << "lombok\t" << encodeToUtf8(sourceSet) << "\t" << joinPath(jar) << "\n";
  }
  for(const auto& entry : info.sourceDirectories) {
    for(const FilePath& root : entry.second) {
      out << "root\t" << encodeToUtf8(entry.first) << "\t" << joinPath(root) << "\t" << (root.recheckExists() ? "1" : "0") << "\n";
    }
  }
  for(const std::string& fingerprintLine : splitToVector(info.fingerprint, "\n")) {
    if(!fingerprintLine.empty()) {
      out << "fp\t" << fingerprintLine << "\n";
    }
  }

  writeFileAtomically(getCachePath(outputDirectory), out.str(), outputDirectory);
}

void markGradleDependenciesExported(const FilePath& outputDirectory, const std::string& fingerprint, bool includesTests) {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(outputDirectory.str()), ec);
  writeFileAtomically(getExportedPath(outputDirectory), exportedMarker(fingerprint, includesTests), outputDirectory);
}

bool gradleDependenciesUpToDate(const FilePath& outputDirectory, const FilePath& projectDirectory, bool includesTests) {
  std::ifstream stream(getExportedPath(outputDirectory), std::ios::binary);
  if(!stream) {
    return false;
  }
  std::ostringstream content;
  content << stream.rdbuf();
  return content.str() == exportedMarker(gradleBuildFingerprint(projectDirectory), includesTests);
}

bool FileStamp::operator==(const FileStamp& other) const {
  return size == other.size && mtime == other.mtime;
}

bool DelombokManifest::operator==(const DelombokManifest& other) const {
  return fingerprint == other.fingerprint && sourceDirectories == other.sourceDirectories &&
      lombokConfigs == other.lombokConfigs && files == other.files && crossFileOriginals == other.crossFileOriginals;
}

DelombokManifest scanDelombokOriginals(const std::vector<FilePath>& sourceDirectories,
                                       const std::string& fingerprint,
                                       const std::optional<DelombokManifest>& previous) {
  DelombokManifest manifest;
  manifest.fingerprint = hashFingerprint(fingerprint);
  manifest.sourceDirectories = sourceDirectories;
  const bool comparable = previous && previous->sourceDirectories == sourceDirectories;

  for(size_t index = 0; index < sourceDirectories.size(); index++) {
    const std::filesystem::path root(sourceDirectories[index].str());

    // Lombok looks for lombok.config in every directory up to the file system root
    for(std::filesystem::path directory = root.parent_path(); !directory.empty(); directory = directory.parent_path()) {
      const std::filesystem::path config = directory / "lombok.config";
      if(const std::optional<FileStamp> stamp = stampFile(config)) {
        manifest.lombokConfigs[config.wstring()] = *stamp;
      }
      if(directory == directory.parent_path()) {
        break;
      }
    }

    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::follow_directory_symlink, ec);
    const std::filesystem::recursive_directory_iterator end;
    for(; !ec && it != end; it.increment(ec)) {
      std::error_code entryEc;
      if(it->is_regular_file(entryEc)) {
        const std::optional<FileStamp> stamp = stampFile(it->path());
        if(!stamp) {
          continue;
        }
        const std::pair<size_t, std::wstring> original = {index, it->path().lexically_relative(root).generic_wstring()};
        manifest.files[original] = *stamp;
        if(isCrossFileOriginal(comparable ? previous : std::nullopt, original, *stamp, it->path())) {
          manifest.crossFileOriginals.insert(original);
        }
      }
    }
  }
  return manifest;
}

std::optional<DelombokManifest> loadDelombokManifest(const FilePath& delombokOutputDirectory) {
  std::ifstream stream(getManifestPath(delombokOutputDirectory), std::ios::binary);
  if(!stream) {
    return std::nullopt;
  }

  DelombokManifest manifest;
  bool versionOk = false;
  std::string line;
  while(std::getline(stream, line)) {
    const std::vector<std::string> parts = splitToVector(line, "\t");
    if(parts.empty()) {
      continue;
    }

    if(parts[0] == "version" && parts.size() == 2) {
      versionOk = parts[1] == MANIFEST_VERSION;
    } else if(parts[0] == "fp" && parts.size() == 2) {
      manifest.fingerprint = parts[1];
    } else if(parts[0] == "root" && parts.size() == 2) {
      manifest.sourceDirectories.push_back(FilePath(decodeFromUtf8(parts[1])));
    } else if(parts[0] == "config" && parts.size() == 4) {
      const std::optional<FileStamp> stamp = parseStamp(parts[2], parts[3]);
      if(!stamp) {
        return std::nullopt;
      }
      manifest.lombokConfigs[decodeFromUtf8(parts[1])] = *stamp;
    } else if(parts[0] == "file" && parts.size() == 6) {
      const std::optional<FileStamp> stamp = parseStamp(parts[3], parts[4]);
      size_t index = 0;
      try {
        index = std::stoul(parts[1]);
      } catch(const std::exception&) {
        return std::nullopt;
      }
      if(!stamp || index >= manifest.sourceDirectories.size()) {
        return std::nullopt;
      }
      manifest.files[{index, decodeFromUtf8(parts[2])}] = *stamp;
      if(parts[5] == "1") {
        manifest.crossFileOriginals.insert({index, decodeFromUtf8(parts[2])});
      }
    } else {
      return std::nullopt;
    }
  }

  if(!versionOk) {
    return std::nullopt;
  }
  return manifest;
}

void saveDelombokManifest(const FilePath& delombokOutputDirectory, const DelombokManifest& manifest) {
  const std::filesystem::path path = getManifestPath(delombokOutputDirectory);
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);

  std::ostringstream out;
  out << "version\t" << MANIFEST_VERSION << "\n";
  out << "fp\t" << manifest.fingerprint << "\n";
  for(const FilePath& root : manifest.sourceDirectories) {
    out << "root\t" << joinPath(root) << "\n";
  }
  for(const auto& [config, stamp] : manifest.lombokConfigs) {
    out << "config\t" << encodeToUtf8(config) << "\t" << formatStamp(stamp) << "\n";
  }
  for(const auto& [file, stamp] : manifest.files) {
    out << "file\t" << file.first << "\t" << encodeToUtf8(file.second) << "\t" << formatStamp(stamp) << "\t"
        << (manifest.crossFileOriginals.count(file) > 0 ? "1" : "0") << "\n";
  }

  writeFileAtomically(path, out.str(), delombokOutputDirectory);
}

void removeDelombokManifest(const FilePath& delombokOutputDirectory) {
  std::error_code ec;
  std::filesystem::remove(getManifestPath(delombokOutputDirectory), ec);
}
}    // namespace utility
