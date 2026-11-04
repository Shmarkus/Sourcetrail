#include "utilityDelombokPathAliases.h"

#include <algorithm>
#include <random>
#include <string>
#include <system_error>

#ifndef _WIN32
#  include <cstdlib>
#endif

#include "logging.h"
#include "utilityString.h"

namespace utility {
namespace {
std::filesystem::path getDeepestNonAsciiDirectory(const FilePath& path) {
  std::filesystem::path deepest;
  std::filesystem::path prefix;
  for(const std::filesystem::path& part : std::filesystem::path(path.wstr())) {
    prefix /= part;
    const std::wstring name = part.wstring();
    if(std::any_of(name.begin(), name.end(), [](wchar_t c) { return c > 0x7F; })) {
      deepest = prefix;
    }
  }
  return deepest;
}

// A new directory only its owner can enter, or an empty path.
std::filesystem::path createPrivateDirectory(const std::filesystem::path& parent) {
#ifdef _WIN32
  std::error_code ec;
  const std::filesystem::path directory = parent / ("sourcetrail-delombok-" + std::to_string(std::random_device()()));
  return std::filesystem::create_directory(directory, ec) && !ec ? directory : std::filesystem::path();
#else
  std::string pattern = (parent / "sourcetrail-delombok-XXXXXX").string();
  return mkdtemp(pattern.data()) != nullptr ? std::filesystem::path(pattern) : std::filesystem::path();
#endif
}
}    // namespace

DelombokPathAliases::DelombokPathAliases(const std::vector<FilePath>& paths, const std::filesystem::path& temporaryDirectory) {
  for(const FilePath& path : paths) {
    const std::filesystem::path target = getDeepestNonAsciiDirectory(path);
    const bool linked = std::any_of(
        m_links.begin(), m_links.end(), [&target](const auto& link) { return link.first == target; });
    if(!target.empty() && !linked && !addLink(target, temporaryDirectory)) {
      return;
    }
  }
}

DelombokPathAliases::~DelombokPathAliases() {
  if(!m_directory.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(m_directory, ec);
  }
}

FilePath DelombokPathAliases::map(const FilePath& path) const {
  const std::filesystem::path original(path.wstr());
  for(const auto& [target, link] : m_links) {
    const std::filesystem::path relative = original.lexically_relative(target);
    if(!relative.empty() && *relative.begin() != "..") {
      return FilePath((relative == "." ? link : link / relative).wstring());
    }
  }
  return path;
}

bool DelombokPathAliases::addLink(const std::filesystem::path& target, const std::filesystem::path& temporaryDirectory) {
  std::error_code ec;
  if(m_directory.empty()) {
    const std::filesystem::path parent = temporaryDirectory.empty() ? std::filesystem::temp_directory_path(ec) :
                                                                      temporaryDirectory;
    m_directory = ec ? std::filesystem::path() : createPrivateDirectory(parent);
    if(m_directory.empty()) {
      LOG_WARNING(L"Cannot create a directory below {} for delombok's links to non-ASCII paths; delombok gets them as "
                  L"they are.",
                  parent.wstring());
      return false;
    }
  }
  const std::filesystem::path link = m_directory / std::to_string(m_links.size());
  std::filesystem::create_directory_symlink(target, link, ec);
  if(ec) {
    LOG_WARNING(L"Cannot link {} to the non-ASCII path {}: {}; delombok gets it as it is.",
                link.wstring(),
                target.wstring(),
                utility::decodeFromUtf8(ec.message()));
    return false;
  }
  m_links.emplace_back(target, link);
  return true;
}
}    // namespace utility
