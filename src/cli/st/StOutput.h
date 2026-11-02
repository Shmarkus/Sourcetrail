#ifndef ST_OUTPUT_H
#define ST_OUTPUT_H

#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "FilePath.h"

namespace st
{
enum ExitCode
{
	EXIT_RESULTS = 0,
	EXIT_EMPTY = 1,
	EXIT_AMBIGUOUS = 2,
	EXIT_NOT_FOUND = 3,
	EXIT_NO_INDEX = 4,
	EXIT_STALE = 5,
	EXIT_USAGE = 64,
	EXIT_INTERNAL = 70
};

struct ResultLine
{
	std::wstring key;
	std::wstring kind;
	FilePath file;
	size_t line;
	std::string source;
	int tier = 0;
	// Last line of a symbol's extent; 0 when it is a single line or unknown.
	size_t endLine = 0;
	// A delombok location without an original line: `file` is the original file and `line` is 0.
	bool generated = false;
	// 1-based column of the location on `line`; 0 when unknown.
	size_t column = 0;
};

struct OutputOptions
{
	size_t limit = 50;
	bool showSource = true;
	// Drops lines that stay generated after mapLocation, and says how many on stderr.
	bool hideGenerated = false;
	FilePath cwd;
	// Where the hidden-lines notice goes.
	std::ostream* err = &std::cerr;
	// Base name of the project file; ends the stderr count line so a query against the wrong project shows.
	std::string project;
	// Applied to every line before sorting; identity when empty.
	std::function<ResultLine(const ResultLine&)> mapLocation;
};

// Keeps the best tiers (then path/line order) under the limit and prints those sorted by path then line; returns EXIT_RESULTS or EXIT_EMPTY.
int printResults(std::vector<ResultLine> lines, const OutputOptions& options, std::ostream& out);

// Relative to cwd when the file is below it, otherwise absolute.
std::wstring displayPath(const FilePath& file, const FilePath& cwd);

// `<path>:<line>`, `<path>:<line>-<endLine>` for a range, or `<path> (generated)` for a generated line without one.
std::wstring displayLocation(const ResultLine& line, const FilePath& cwd);
}

#endif
