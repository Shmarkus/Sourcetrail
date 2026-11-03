#include "StInput.h"

#include <algorithm>
#include <regex>

#include "utilityString.h"

namespace st {
std::vector<InputLine> readLines(std::istream& in, size_t* truncated) {
  std::vector<InputLine> lines;
  std::vector<std::string> seen;
  if(truncated) {
    *truncated = 0;
  }
  static const std::regex overflow(R"(\+(\d+) more \(--limit\))");
  std::string line;
  while(std::getline(in, line)) {
    std::smatch marker;
    const std::string text = line.substr(0, line.size() - (!line.empty() && line.back() == '\r'));
    if(std::regex_match(text, marker, overflow)) {
      if(truncated) {
        *truncated += std::stoull(marker[1].str());
      }
      continue;
    }
    const std::string column = line.substr(0, line.find('\t'));
    const size_t first = column.find_first_not_of(" \r");
    if(first == std::string::npos) {
      continue;
    }
    if(std::find(seen.begin(), seen.end(), text) != seen.end()) {
      continue;
    }
    seen.push_back(text);
    lines.push_back({utility::decodeFromUtf8(column.substr(first, column.find_last_not_of(" \r") - first + 1)), text});
  }
  return lines;
}

std::vector<std::wstring> readKeys(std::istream& in, size_t* truncated) {
  std::vector<std::wstring> keys;
  for(const InputLine& line : readLines(in, truncated)) {
    if(std::find(keys.begin(), keys.end(), line.key) == keys.end()) {
      keys.push_back(line.key);
    }
  }
  return keys;
}
}    // namespace st
