#include "JavaPathDetector.h"

#include <cwctype>

namespace {
int readNumber(const std::wstring& text, std::size_t& pos) {
  int number = 0;
  bool found = false;
  while(pos < text.size() && std::iswdigit(text[pos]) != 0) {
    number = number * 10 + (text[pos] - L'0');
    found = true;
    ++pos;
  }
  return found ? number : -1;
}
}    // namespace

JavaPathDetector::JavaPathDetector(const std::string& name, int minimumMajorVersion)
    : PathDetector(name), m_minimumMajorVersion(minimumMajorVersion) {}

int JavaPathDetector::majorVersion(const std::wstring& versionOutput) {
  const std::wstring marker = L"version \"";
  std::size_t pos = versionOutput.find(marker);
  if(pos == std::wstring::npos) {
    return 0;
  }
  pos += marker.size();

  const int first = readNumber(versionOutput, pos);
  if(first < 0) {
    return 0;
  }
  // before Java 9 the major version came second: "1.8.0_412"
  if(first == 1 && pos < versionOutput.size() && versionOutput[pos] == L'.') {
    ++pos;
    const int second = readNumber(versionOutput, pos);
    return second < 0 ? 0 : second;
  }
  return first;
}
