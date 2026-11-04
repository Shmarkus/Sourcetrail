#include "utilityMavenCache.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

#if defined(_WIN32)
#  include <process.h>
#  define getpid _getpid
#else
#  include <unistd.h>
#endif

#include "logging.h"
#include "utilityString.h"

namespace utility {
namespace {
const char* const SOURCES_FILE_NAME = "sourcetrail-maven-sources.cache";
const char* const EXPORTED_FILE_NAME = "sourcetrail-maven-exported.cache";
const char* const CACHE_VERSION = "2";    // 2: an end line, source directories in the export marker

const std::set<std::string> SKIPPED_DIRECTORY_NAMES = {"target", "node_modules", ".git", ".sourcetrail"};

std::filesystem::path getSourcesPath(const FilePath& outputDirectory) {
  return std::filesystem::path(outputDirectory.str()) / SOURCES_FILE_NAME;
}

std::filesystem::path getExportedPath(const FilePath& outputDirectory) {
  return std::filesystem::path(outputDirectory.str()) / EXPORTED_FILE_NAME;
}

void writeFileAtomically(const std::filesystem::path& path, const std::string& content) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  // one temporary file per process, so that two refreshes writing at once cannot interleave
  const std::filesystem::path temporaryPath = path.string() + "." + std::to_string(getpid()) + ".tmp";
  {
    std::ofstream stream(temporaryPath, std::ios::binary | std::ios::trunc);
    stream << content;
    if(!stream) {
      LOG_ERROR(L"Failed to write Maven cache: {}", path.wstring());
      return;
    }
  }
  std::filesystem::rename(temporaryPath, path, ec);
  if(ec) {
    LOG_ERROR(L"Failed to store Maven cache: {}", path.wstring());
  }
}

// The entries of a cache file, or nullopt when it is missing, cut short (no end line) or was written by another
// version or for another fingerprint.
std::optional<std::vector<std::vector<std::string>>> readCache(const std::filesystem::path& path,
                                                               const std::optional<std::string>& fingerprint) {
  std::ifstream stream(path, std::ios::binary);
  if(!stream) {
    return std::nullopt;
  }

  bool versionOk = false;
  bool ended = false;
  std::string storedFingerprint;
  std::vector<std::vector<std::string>> entries;
  std::string line;
  while(std::getline(stream, line)) {
    std::vector<std::string> parts = splitToVector(line, "\t");
    if(parts.empty()) {
      continue;
    }
    if(ended) {
      return std::nullopt;
    }
    if(parts[0] == "end" && parts.size() == 1) {
      ended = true;
    } else if(parts[0] == "version" && parts.size() == 2) {
      versionOk = parts[1] == CACHE_VERSION;
    } else if(parts[0] == "fp" && parts.size() >= 2) {
      storedFingerprint += line.substr(3) + "\n";
    } else {
      entries.push_back(std::move(parts));
    }
  }

  if(!ended || !versionOk || (fingerprint && storedFingerprint != *fingerprint)) {
    return std::nullopt;
  }
  return entries;
}

std::string formatHeader(const std::string& fingerprint) {
  std::string out = std::string("version\t") + CACHE_VERSION + "\n";
  for(const std::string& fingerprintLine : splitToVector(fingerprint, "\n")) {
    if(!fingerprintLine.empty()) {
      out += "fp\t" + fingerprintLine + "\n";
    }
  }
  return out;
}

std::filesystem::path getUserSettingsPath() {
  const char* homeDirectory = std::getenv("HOME");
  if(homeDirectory == nullptr || *homeDirectory == '\0') {
    return {};
  }
  return std::filesystem::path(homeDirectory) / ".m2" / "settings.xml";
}

bool isUnderDotMvn(const std::filesystem::path& relativePath) {
  return !relativePath.empty() && *relativePath.begin() == ".mvn";
}
}    // namespace

std::vector<FilePath> mavenBuildFiles(const FilePath& projectDirectory, const FilePath& settingsFilePath) {
  std::vector<FilePath> files;
  const std::filesystem::path root(projectDirectory.str());
  std::error_code ec;
  std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec);
  const std::filesystem::recursive_directory_iterator end;
  for(; !ec && it != end; it.increment(ec)) {
    std::error_code entryEc;
    if(it->is_directory(entryEc)) {
      if(SKIPPED_DIRECTORY_NAMES.count(it->path().filename().string()) > 0) {
        it.disable_recursion_pending();
      }
    } else if((it->path().filename() == "pom.xml" || isUnderDotMvn(it->path().lexically_relative(root))) &&
              it->is_regular_file(entryEc)) {
      files.push_back(FilePath(it->path().wstring()));
    }
  }

  std::sort(files.begin(), files.end(), [](const FilePath& a, const FilePath& b) { return a.str() < b.str(); });

  for(const std::filesystem::path& settings : {std::filesystem::path(settingsFilePath.str()), getUserSettingsPath()}) {
    std::error_code settingsEc;
    if(!settings.empty() && std::filesystem::is_regular_file(settings, settingsEc)) {
      files.push_back(FilePath(settings.wstring()));
    }
  }
  return files;
}

std::string mavenBuildFingerprint(const FilePath& mavenPath,
                                  const FilePath& settingsFilePath,
                                  const FilePath& projectDirectory,
                                  bool includesTests) {
  std::string fingerprint = "maven|" + mavenPath.str() + "\nsettings|" + settingsFilePath.str() + "\ntests|" +
      (includesTests ? "1" : "0") + "\n";
  for(const FilePath& file : mavenBuildFiles(projectDirectory, settingsFilePath)) {
    std::error_code ec;
    const std::filesystem::path path(file.str());
    const auto size = std::filesystem::file_size(path, ec);
    const auto mtime = std::filesystem::last_write_time(path, ec).time_since_epoch().count();
    fingerprint += file.str() + "|" + std::to_string(size) + "|" + std::to_string(mtime) + "\n";
  }
  return fingerprint;
}

std::optional<std::vector<FilePath>> loadMavenSourceDirectories(const FilePath& outputDirectory, const std::string& fingerprint) {
  const auto entries = readCache(getSourcesPath(outputDirectory), fingerprint);
  if(!entries) {
    return std::nullopt;
  }

  std::vector<FilePath> sourceDirectories;
  for(const std::vector<std::string>& parts : *entries) {
    if(parts[0] != "root" || parts.size() != 2) {
      return std::nullopt;
    }
    sourceDirectories.push_back(FilePath(decodeFromUtf8(parts[1])));
  }
  return sourceDirectories;
}

void saveMavenSourceDirectories(const FilePath& outputDirectory,
                                const std::string& fingerprint,
                                const std::vector<FilePath>& sourceDirectories) {
  std::string out = formatHeader(fingerprint);
  for(const FilePath& directory : sourceDirectories) {
    out += "root\t" + directory.str() + "\n";
  }
  writeFileAtomically(getSourcesPath(outputDirectory), out + "end\n");
}

void markMavenDependenciesExported(const FilePath& outputDirectory,
                                   const std::string& fingerprint,
                                   const std::vector<FilePath>& existingSourceDirectories,
                                   const std::vector<std::wstring>& warnings) {
  const std::filesystem::path directory(outputDirectory.str());
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);

  std::vector<std::string> jars;
  for(const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
    if(entry.path().extension() == ".jar") {
      jars.push_back(entry.path().filename().string());
    }
  }
  std::sort(jars.begin(), jars.end());

  std::string out = formatHeader(fingerprint);
  for(const std::string& jar : jars) {
    out += "jar\t" + jar + "\n";
  }
  for(const FilePath& directory : existingSourceDirectories) {
    out += "dir\t" + directory.str() + "\n";
  }
  for(const std::wstring& warning : warnings) {
    std::string line = encodeToUtf8(warning);
    std::replace_if(
        line.begin(), line.end(), [](char c) { return c == '\t' || c == '\n' || c == '\r'; }, ' ');
    out += "warning\t" + line + "\n";
  }
  writeFileAtomically(getExportedPath(outputDirectory), out + "end\n");
}

bool mavenDependenciesUpToDate(const FilePath& outputDirectory, const std::string& fingerprint, std::vector<std::wstring>* warnings) {
  const auto entries = readCache(getExportedPath(outputDirectory), fingerprint);
  if(!entries) {
    return false;
  }

  std::vector<std::wstring> storedWarnings;
  for(const std::vector<std::string>& parts : *entries) {
    std::error_code ec;
    if(parts.size() != 2) {
      return false;
    } else if(parts[0] == "warning") {
      storedWarnings.push_back(decodeFromUtf8(parts[1]));
    } else if(parts[0] == "jar") {
      if(!std::filesystem::is_regular_file(std::filesystem::path(outputDirectory.str()) / parts[1], ec)) {
        return false;
      }
    } else if(parts[0] == "dir") {
      // gone after a `mvn clean`, say, for generated sources
      if(!std::filesystem::is_directory(std::filesystem::path(parts[1]), ec)) {
        LOG_INFO("Maven source directory missing, running Maven again: {}", parts[1]);
        return false;
      }
    } else {
      return false;
    }
  }
  if(warnings) {
    *warnings = storedWarnings;
  }
  return true;
}

std::optional<std::vector<std::wstring>> loadMavenWarnings(const FilePath& outputDirectory) {
  const auto entries = readCache(getExportedPath(outputDirectory), std::nullopt);
  if(!entries) {
    return std::nullopt;
  }
  std::vector<std::wstring> warnings;
  for(const std::vector<std::string>& parts : *entries) {
    if(parts[0] == "warning" && parts.size() == 2) {
      warnings.push_back(decodeFromUtf8(parts[1]));
    }
  }
  return warnings;
}

void removeMavenDependenciesExported(const FilePath& outputDirectory) {
  std::error_code ec;
  std::filesystem::remove(getExportedPath(outputDirectory), ec);
}

void removeMavenCache(const FilePath& outputDirectory) {
  removeMavenDependenciesExported(outputDirectory);
  std::error_code ec;
  std::filesystem::remove(getSourcesPath(outputDirectory), ec);
}
}    // namespace utility
