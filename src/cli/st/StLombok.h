#ifndef ST_LOMBOK_H
#define ST_LOMBOK_H

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "FilePath.h"
#include "StOutput.h"

class ProjectSettings;

namespace st
{
// Original lines first..last (0-based) behind one generated line; last > first where delombok joined several lines.
struct LineSpan
{
	size_t first = 0;
	size_t last = 0;
};

// Generated line index -> the original lines it stands for; the one alignment both mapping directions use.
// Byte-identical files map line to line. Otherwise an LCS over whitespace-trimmed, non-empty lines, refusing repeated
// text, and inside each gap between its matches a generated line takes the one run of 2-12 non-blank original lines
// whose whitespace-free text equals its own, if no other generated line in the gap has that text.
// Unmatched lines map to nullopt, as does everything above 5000 x 5000 lines.
std::vector<std::optional<LineSpan>> alignSpans(
	const std::vector<std::string>& generated, const std::vector<std::string>& original);

// The first original line of each alignSpans entry.
std::vector<std::optional<size_t>> alignLines(
	const std::vector<std::string>& generated, const std::vector<std::string>& original);

struct LombokRoots
{
	// Delombok output directory of each root -> that original root.
	std::vector<std::pair<FilePath, std::vector<FilePath>>> delombokToOriginalRoots;
	// By delombok file for the duration of one command: its original (empty without exactly one), alignSpans and lines.
	mutable std::map<std::wstring, FilePath> originalFiles;
	mutable std::map<std::wstring, std::vector<std::optional<LineSpan>>> alignments;
	mutable std::map<std::wstring, std::vector<std::string>> originalLines;
	mutable std::map<std::wstring, std::vector<std::string>> generatedLines;
};

LombokRoots lombokRoots(const ProjectSettings& settings);

// Lines outside every delombok directory are returned unchanged. An unmatched line is marked `generated` and names
// the original file with line 0; without one it keeps its delombok location and its kind gains " (generated)".
// A line delombok joined from several original lines maps to the one holding the character at `column`, and
// stays generated when the column is unknown or can't be placed.
// readLines returns no lines for a missing file.
ResultLine toOriginal(const ResultLine& line, const LombokRoots& roots,
	const std::function<std::vector<std::string>(const FilePath&)>& readLines);

// The original file behind a delombok file (a path lookup only, nothing is read); nullopt outside the delombok directories or without exactly one original.
std::optional<FilePath> toOriginalFile(const FilePath& generated, const LombokRoots& roots);

// The delombok copy of an original file; nullopt outside the original roots or when several delombok roots hold one.
std::optional<FilePath> toGeneratedFile(const FilePath& original, const LombokRoots& roots);

// Original line index -> line index of the delombok file, the reverse of alignSpans.
// With `cache` the alignment of a delombok file is computed once.
std::vector<std::optional<size_t>> originalToGenerated(const FilePath& original, const FilePath& generated,
	const std::function<std::vector<std::string>(const FilePath&)>& readLines, const LombokRoots* cache = nullptr);

// 1-based original line -> 1-based line of the delombok file; nullopt when the line has no counterpart there.
std::optional<size_t> toGeneratedLine(const FilePath& original, const FilePath& generated, size_t line,
	const std::function<std::vector<std::string>(const FilePath&)>& readLines, const LombokRoots* cache = nullptr);

std::vector<std::string> readLinesFromDisk(const FilePath& file);
}

#endif
