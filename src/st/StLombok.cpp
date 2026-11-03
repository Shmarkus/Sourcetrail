#include "StLombok.h"

#include <algorithm>
#include <map>
#include <tuple>

#include <QFile>

#include "ProjectSettings.h"
#include "SourceGroupSettingsJavaGradle.h"
#include "utilityGradle.h"
#include "utilityGradleCache.h"

namespace st {
namespace {
const size_t MAX_ALIGNED_LINES = 5000;

std::string trimmed(const std::string& text) {
  const size_t first = text.find_first_not_of(" \t\r\n");
  if(first == std::string::npos) {
    return "";
  }
  return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

// The path below `root` when `file` lies inside it, with a trailing separator-free relative form.
std::optional<std::wstring> relativeTo(const FilePath& file, const FilePath& root) {
  std::wstring base = root.wstr();
  while(base.size() > 1 && (base.back() == L'/' || base.back() == L'\\')) {
    base.pop_back();
  }
  const std::wstring path = file.wstr();
  if(path.size() > base.size() + 1 && path.compare(0, base.size(), base) == 0 &&
     (path[base.size()] == L'/' || path[base.size()] == L'\\')) {
    return path.substr(base.size() + 1);
  }
  return std::nullopt;
}

// Resolves symlinks through the existing parent directories, so the file itself may be missing.
FilePath canonical(const FilePath& file) {
  return file.getParentDirectory().getCanonical().getConcatenated(file.fileName());
}

ResultLine generated(ResultLine line) {
  line.kind += L" (generated)";
  line.generated = true;
  return line;
}

bool isSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

std::string squeezed(const std::string& text) {
  std::string result;
  for(const char c : text) {
    if(!isSpace(c)) {
      result += c;
    }
  }
  return result;
}

// The line of `span` holding the token at 1-based `column` of the generated line, found by counting non-whitespace
// bytes; nullopt when the column is unknown or lands on whitespace or past the line. The indexer counts UTF-16
// units, so a tab is one column and a character outside the basic plane two.
std::optional<size_t> lineAtColumn(const std::string& generatedLine,
                                   const std::vector<std::string>& original,
                                   const LineSpan& span,
                                   size_t column) {
  size_t units = 0;
  size_t before = 0;
  size_t i = 0;
  for(; i < generatedLine.size(); i++) {
    const unsigned char c = generatedLine[i];
    if((c & 0xC0) != 0x80) {
      if(units + 1 == column) {
        break;
      }
      units += c >= 0xF0 ? 2 : 1;
    }
    before += !isSpace(c);
  }
  if(column == 0 || i == generatedLine.size() || isSpace(generatedLine[i])) {
    return std::nullopt;
  }
  for(size_t o = span.first; o <= span.last; o++) {
    const size_t size = squeezed(original[o]).size();
    if(before < size) {
      return o;
    }
    before -= size;
  }
  return std::nullopt;
}

// Generated line index -> original line index of the LCS matches that pass the guards against repeated lines.
std::vector<std::optional<size_t>> matchLines(const std::vector<std::string>& generatedLines,
                                              const std::vector<std::string>& original) {
  const size_t n = generatedLines.size();
  const size_t m = original.size();
  std::vector<std::optional<size_t>> result(n);
  std::vector<std::string> a;
  std::vector<std::string> b;
  for(const std::string& text : generatedLines) {
    a.push_back(trimmed(text));
  }
  for(const std::string& text : original) {
    b.push_back(trimmed(text));
  }
  const auto neighbour = [](const std::vector<std::string>& lines, size_t index, bool forward) -> std::optional<size_t> {
    for(size_t k = index; forward ? ++k < lines.size() : k-- > 0;) {
      if(!lines[k].empty()) {
        return k;
      }
    }
    return std::nullopt;
  };
  const auto agree = [&](std::optional<size_t> x, std::optional<size_t> y) {
    return x.has_value() == y.has_value() && (!x || a[*x] == b[*y]);
  };
  // Equal text is worth more when the nearest non-blank neighbours repeat as well, which settles repeated lines.
  const auto weight = [&](size_t i, size_t j) {
    return 4 + agree(neighbour(a, i, false), neighbour(b, j, false)) + agree(neighbour(a, i, true), neighbour(b, j, true));
  };
  std::vector<std::vector<uint16_t>> suffix(n + 1, std::vector<uint16_t>(m + 1, 0));
  for(size_t i = n; i-- > 0;) {
    for(size_t j = m; j-- > 0;) {
      suffix[i][j] = std::max(suffix[i + 1][j], suffix[i][j + 1]);
      if(!a[i].empty() && a[i] == b[j]) {
        suffix[i][j] = std::max<uint16_t>(suffix[i][j], suffix[i + 1][j + 1] + weight(i, j));
      }
    }
  }
  std::vector<std::optional<size_t>> raw(n);
  size_t i = 0;
  size_t j = 0;
  while(i < n && j < m) {
    if(!a[i].empty() && a[i] == b[j] && suffix[i][j] == suffix[i + 1][j + 1] + weight(i, j)) {
      raw[i++] = j++;
    } else if(suffix[i + 1][j] >= suffix[i][j + 1]) {
      i++;
    } else {
      j++;
    }
  }

  std::map<std::string, size_t> inOriginal;
  std::map<std::string, size_t> inGenerated;
  for(const std::string& text : b) {
    inOriginal[text]++;
  }
  for(const std::string& text : a) {
    inGenerated[text]++;
  }
  const auto surroundings = [&](size_t g) {
    const auto before = neighbour(a, g, false);
    const auto after = neighbour(a, g, true);
    return std::make_tuple(before ? a[*before] : std::string(), a[g], after ? a[*after] : std::string());
  };
  std::map<std::tuple<std::string, std::string, std::string>, size_t> triples;
  for(size_t g = 0; g < n; g++) {
    if(!a[g].empty()) {
      triples[surroundings(g)]++;
    }
  }
  const auto matchedTo = [&](std::optional<size_t> generatedIndex, std::optional<size_t> originalIndex) {
    return generatedIndex.has_value() == originalIndex.has_value() && (!generatedIndex || raw[*generatedIndex] == originalIndex);
  };
  for(size_t g = 0; g < n; g++) {
    if(!raw[g]) {
      continue;
    }
    const size_t o = *raw[g];
    const bool contextual = agree(neighbour(a, g, false), neighbour(b, o, false)) ||
        agree(neighbour(a, g, true), neighbour(b, o, true));
    const bool unique = inOriginal[b[o]] == 1 && (inGenerated[a[g]] == 1 || contextual);
    const bool neighboursMatched = matchedTo(neighbour(a, g, false), neighbour(b, o, false)) &&
        matchedTo(neighbour(a, g, true), neighbour(b, o, true));
    if((unique || neighboursMatched) && triples[surroundings(g)] == 1) {
      result[g] = o;
    }
  }
  return result;
}

// Maps generated lines in [gBegin, gEnd) to runs of non-blank original lines in [oBegin, oEnd) that delombok joined.
void tileGap(const std::vector<std::string>& a,
             const std::vector<std::string>& b,
             size_t gBegin,
             size_t gEnd,
             size_t oBegin,
             size_t oEnd,
             std::vector<std::optional<LineSpan>>& spans) {
  const size_t MAX_JOINED_LINES = 12;
  std::map<std::string, size_t> uses;
  size_t longest = 0;
  for(size_t g = gBegin; g < gEnd; g++) {
    if(!a[g].empty()) {
      uses[a[g]]++;
      longest = std::max(longest, a[g].size());
    }
  }
  std::vector<size_t> lines;
  for(size_t o = oBegin; o < oEnd; o++) {
    if(!b[o].empty()) {
      lines.push_back(o);
    }
  }
  // Single lines never tile, but one with the same text makes a run ambiguous.
  std::map<std::string, std::vector<LineSpan>> runs;
  for(size_t start = 0; start < lines.size(); start++) {
    std::string text = b[lines[start]];
    if(uses.count(text)) {
      runs[text].push_back({lines[start], lines[start]});
    }
    for(size_t end = start + 1; end < lines.size() && end - start < MAX_JOINED_LINES && text.size() < longest; end++) {
      text += b[lines[end]];
      if(uses.count(text)) {
        runs[text].push_back({lines[start], lines[end]});
      }
    }
  }
  std::vector<std::pair<size_t, LineSpan>> tiles;
  for(size_t g = gBegin; g < gEnd; g++) {
    const auto run = runs.find(a[g]);
    if(run != runs.end() && run->second.size() == 1 && run->second.front().last > run->second.front().first && uses[a[g]] == 1) {
      tiles.emplace_back(g, run->second.front());
    }
  }
  // Tiles keep the order of both files and never share an original line.
  for(const auto& tile : tiles) {
    const bool consistent = std::all_of(tiles.begin(), tiles.end(), [&](const std::pair<size_t, LineSpan>& other) {
      return other.first == tile.first ||
          (other.first < tile.first ? other.second.last < tile.second.first : other.second.first > tile.second.last);
    });
    if(consistent) {
      spans[tile.first] = tile.second;
    }
  }
}
}    // namespace

std::vector<std::optional<LineSpan>> alignSpans(const std::vector<std::string>& generatedLines,
                                                const std::vector<std::string>& original) {
  const size_t n = generatedLines.size();
  const size_t m = original.size();
  std::vector<std::optional<LineSpan>> spans(n);
  if(generatedLines == original) {
    for(size_t i = 0; i < n; i++) {
      spans[i] = LineSpan{i, i};
    }
    return spans;
  }
  if(n > MAX_ALIGNED_LINES || m > MAX_ALIGNED_LINES) {
    return spans;
  }
  const std::vector<std::optional<size_t>> matched = matchLines(generatedLines, original);
  std::vector<std::string> a;
  std::vector<std::string> b;
  for(const std::string& text : generatedLines) {
    a.push_back(squeezed(text));
  }
  for(const std::string& text : original) {
    b.push_back(squeezed(text));
  }
  size_t gBegin = 0;
  size_t oBegin = 0;
  for(size_t g = 0; g <= n; g++) {
    if(g < n && !matched[g]) {
      continue;
    }
    tileGap(a, b, gBegin, g, oBegin, g < n ? *matched[g] : m, spans);
    if(g < n) {
      spans[g] = LineSpan{*matched[g], *matched[g]};
      gBegin = g + 1;
      oBegin = *matched[g] + 1;
    }
  }
  return spans;
}

std::vector<std::optional<size_t>> alignLines(const std::vector<std::string>& generatedLines,
                                              const std::vector<std::string>& original) {
  std::vector<std::optional<size_t>> lines;
  for(const auto& span : alignSpans(generatedLines, original)) {
    lines.push_back(span ? std::optional<size_t>(span->first) : std::nullopt);
  }
  return lines;
}

LombokRoots lombokRoots(const ProjectSettings& settings) {
  LombokRoots roots;
  for(const auto& group : settings.getAllSourceGroupSettings()) {
    const auto gradle = std::dynamic_pointer_cast<SourceGroupSettingsJavaGradle>(group);
    if(!gradle || group->getStatus() != SOURCE_GROUP_STATUS_ENABLED) {
      continue;
    }
    const FilePath output = gradle->getGradleDependenciesDirectoryPath();
    const FilePath project = gradle->getGradleProjectFilePathExpandedAndAbsolute().getParentDirectory();
    const auto info = utility::loadGradleProjectInfo(output, project, gradle->getShouldIndexGradleTests());
    if(!info) {
      continue;
    }
    for(const auto& sourceSet : info->sourceDirectories) {
      std::vector<FilePath> existing;
      for(const FilePath& root : sourceSet.second) {
        if(root.exists()) {
          existing.push_back(root);
        }
      }
      // every root has its own delombok directory, named the way the indexer names it
      const std::vector<FilePath> generated = utility::getDelombokRootDirectories(
          utility::getDelombokOutputDirectory(output, sourceSet.first), existing, project);
      for(size_t i = 0; i < existing.size(); i++) {
        roots.delombokToOriginalRoots.emplace_back(generated[i], std::vector<FilePath>{existing[i]});
      }
    }
  }
  return roots;
}

namespace {
struct Counterpart {
  bool inside = false;
  // Empty unless exactly one original root holds the file.
  FilePath file;
  std::vector<std::string> lines;
};

Counterpart findOriginal(const FilePath& generatedFile,
                         const LombokRoots& roots,
                         const std::function<std::vector<std::string>(const FilePath&)>& readLines) {
  Counterpart result;
  for(const auto& entry : roots.delombokToOriginalRoots) {
    const auto relative = relativeTo(canonical(generatedFile), entry.first.getCanonical());
    if(!relative) {
      continue;
    }
    result.inside = true;
    size_t found = 0;
    for(const FilePath& root : entry.second) {
      const FilePath candidate = root.getConcatenated(*relative);
      std::vector<std::string> lines = readLines(candidate);
      if(!lines.empty() && found++ == 0) {
        result.file = candidate;
        result.lines = std::move(lines);
      }
    }
    if(found != 1) {
      result.file = FilePath();
      result.lines.clear();
    }
    break;
  }
  return result;
}
}    // namespace

ResultLine toOriginal(const ResultLine& line,
                      const LombokRoots& roots,
                      const std::function<std::vector<std::string>(const FilePath&)>& readLines) {
  auto cached = roots.originalFiles.find(line.file.wstr());
  if(cached == roots.originalFiles.end()) {
    Counterpart counterpart = findOriginal(line.file, roots, readLines);
    if(!counterpart.inside) {
      return line;
    }
    if(!counterpart.file.empty()) {
      std::vector<std::string> generatedLines = readLines(line.file);
      if(!roots.alignments.count(line.file.wstr())) {
        roots.alignments[line.file.wstr()] = alignSpans(generatedLines, counterpart.lines);
      }
      roots.generatedLines[line.file.wstr()] = std::move(generatedLines);
      roots.originalLines[counterpart.file.wstr()] = std::move(counterpart.lines);
    }
    cached = roots.originalFiles.emplace(line.file.wstr(), counterpart.file).first;
  }
  const FilePath& originalFile = cached->second;
  if(originalFile.empty()) {
    return generated(line);
  }
  const auto& alignment = roots.alignments[line.file.wstr()];
  const auto& originalLines = roots.originalLines[originalFile.wstr()];
  ResultLine mapped = line;
  mapped.file = originalFile;
  mapped.endLine = 0;
  std::optional<size_t> original;
  if(line.line > 0 && line.line <= alignment.size() && alignment[line.line - 1]) {
    const LineSpan& span = *alignment[line.line - 1];
    const auto& generatedLines = roots.generatedLines[line.file.wstr()];
    if(span.last == span.first) {
      original = span.first;
    } else if(line.line <= generatedLines.size()) {
      // A joined line maps by the column of its token, never to a guess.
      original = lineAtColumn(generatedLines[line.line - 1], originalLines, span, line.column);
    }
  }
  if(!original) {
    mapped.line = 0;
    mapped.generated = true;
    return mapped;
  }
  mapped.line = *original + 1;
  if(line.endLine > line.line && line.endLine <= alignment.size() && alignment[line.endLine - 1]) {
    mapped.endLine = alignment[line.endLine - 1]->last + 1;
  }
  mapped.source = originalLines[mapped.line - 1];
  return mapped;
}

std::optional<FilePath> toOriginalFile(const FilePath& generatedFile, const LombokRoots& roots) {
  // Only the path matters here: any non-empty reply stands for an existing file.
  const Counterpart counterpart = findOriginal(generatedFile, roots, [](const FilePath& file) {
    return file.exists() ? std::vector<std::string>{""} : std::vector<std::string>{};
  });
  if(counterpart.file.empty()) {
    return std::nullopt;
  }
  return counterpart.file;
}

std::optional<FilePath> toGeneratedFile(const FilePath& original, const LombokRoots& roots) {
  std::vector<FilePath> found;
  for(const auto& entry : roots.delombokToOriginalRoots) {
    for(const FilePath& root : entry.second) {
      const auto relative = relativeTo(canonical(original), root.getCanonical());
      if(!relative) {
        continue;
      }
      const FilePath candidate = entry.first.getConcatenated(*relative);
      if(candidate.exists() && std::find(found.begin(), found.end(), candidate) == found.end()) {
        found.push_back(candidate);
      }
    }
  }
  if(found.size() != 1) {
    return std::nullopt;
  }
  return found.front();
}

std::vector<std::optional<size_t>> originalToGenerated(const FilePath& originalFile,
                                                       const FilePath& generatedFile,
                                                       const std::function<std::vector<std::string>(const FilePath&)>& readLines,
                                                       const LombokRoots* cache) {
  std::vector<std::optional<LineSpan>> computed;
  const std::vector<std::optional<LineSpan>>* spans = &computed;
  if(cache) {
    auto it = cache->alignments.find(generatedFile.wstr());
    if(it == cache->alignments.end()) {
      it = cache->alignments.emplace(generatedFile.wstr(), alignSpans(readLines(generatedFile), readLines(originalFile))).first;
    }
    spans = &it->second;
  } else {
    computed = alignSpans(readLines(generatedFile), readLines(originalFile));
  }
  std::vector<std::optional<size_t>> lines;
  for(size_t g = 0; g < spans->size(); g++) {
    if(const auto& span = (*spans)[g]) {
      lines.resize(std::max(lines.size(), span->last + 1));
      for(size_t o = span->first; o <= span->last; o++) {
        lines[o] = g;
      }
    }
  }
  return lines;
}

std::optional<size_t> toGeneratedLine(const FilePath& originalFile,
                                      const FilePath& generatedFile,
                                      size_t line,
                                      const std::function<std::vector<std::string>(const FilePath&)>& readLines,
                                      const LombokRoots* cache) {
  const auto lines = originalToGenerated(originalFile, generatedFile, readLines, cache);
  if(line == 0 || line > lines.size() || !lines[line - 1]) {
    return std::nullopt;
  }
  return *lines[line - 1] + 1;
}

std::vector<std::string> readLinesFromDisk(const FilePath& file) {
  std::vector<std::string> lines;
  QFile input(QString::fromStdWString(file.wstr()));
  if(!input.open(QIODevice::ReadOnly)) {
    return lines;
  }
  while(!input.atEnd()) {
    lines.push_back(input.readLine().toStdString());
  }
  return lines;
}
}    // namespace st
