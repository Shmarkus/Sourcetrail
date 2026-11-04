#ifndef UTILITY_DELOMBOK_PATH_ALIASES_H
#define UTILITY_DELOMBOK_PATH_ALIASES_H

#include <filesystem>
#include <utility>
#include <vector>

#include "FilePath.h"

namespace utility {
// Lombok's delombok writes the files of a directory input without their package directories when the input's path
// holds a non-ASCII character: it looks each parsed file's input up by the file's URI, which javac and java.io.File
// encode differently for such characters. Such paths reach it through ASCII-named links in a private directory below
// the temporary directory instead, each to a path's deepest non-ASCII directory, so that lombok.config files from
// there downwards still apply. The links go when the aliases do. A link that cannot be made leaves its paths as they
// are, with a warning.
class DelombokPathAliases final {
public:
  // An empty temporaryDirectory is the system's.
  explicit DelombokPathAliases(const std::vector<FilePath>& paths, const std::filesystem::path& temporaryDirectory = {});
  ~DelombokPathAliases();

  DelombokPathAliases(const DelombokPathAliases&) = delete;
  DelombokPathAliases& operator=(const DelombokPathAliases&) = delete;

  FilePath map(const FilePath& path) const;

private:
  bool addLink(const std::filesystem::path& target, const std::filesystem::path& temporaryDirectory);

  std::filesystem::path m_directory;
  std::vector<std::pair<std::filesystem::path, std::filesystem::path>> m_links;    // target, link
};
}    // namespace utility

#endif    // UTILITY_DELOMBOK_PATH_ALIASES_H
