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
// Generated line index -> original line index; LCS over whitespace-trimmed, non-empty lines.
// Unmatched lines map to nullopt, as does everything above 5000 x 5000 lines.
std::vector<std::optional<size_t>> alignLines(
	const std::vector<std::string>& generated, const std::vector<std::string>& original);

struct LombokRoots
{
	// Delombok output directory -> the declared original roots of the same source set.
	std::vector<std::pair<FilePath, std::vector<FilePath>>> delombokToOriginalRoots;
	// Alignments by delombok file for the duration of one command.
	mutable std::map<std::wstring, std::pair<FilePath, std::vector<std::optional<size_t>>>> alignments;
	mutable std::map<std::wstring, std::vector<std::string>> originalLines;
	mutable std::map<std::wstring, std::vector<std::optional<size_t>>> reverseAlignments;
};

LombokRoots lombokRoots(const ProjectSettings& settings);

// Lines outside every delombok directory are returned unchanged; unmatched ones keep their delombok
// location and gain " (generated)". readLines returns no lines for a missing file.
ResultLine toOriginal(const ResultLine& line, const LombokRoots& roots,
	const std::function<std::vector<std::string>(const FilePath&)>& readLines);

// The original file behind a delombok file (a path lookup only, nothing is read); nullopt outside the delombok directories or without exactly one original.
std::optional<FilePath> toOriginalFile(const FilePath& generated, const LombokRoots& roots);

// The delombok copy of an original file; nullopt outside the original roots or when several delombok roots hold one.
std::optional<FilePath> toGeneratedFile(const FilePath& original, const LombokRoots& roots);

// 1-based original line -> 1-based line of the delombok file; nullopt when the line has no counterpart there.
// With `cache` the alignment of a delombok file is computed once.
std::optional<size_t> toGeneratedLine(const FilePath& original, const FilePath& generated, size_t line,
	const std::function<std::vector<std::string>(const FilePath&)>& readLines, const LombokRoots* cache = nullptr);

std::vector<std::string> readLinesFromDisk(const FilePath& file);
}

#endif
