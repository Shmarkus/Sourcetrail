#ifndef ST_OUTPUT_H
#define ST_OUTPUT_H

#include <functional>
#include <ostream>
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
};

struct OutputOptions
{
	size_t limit = 50;
	bool showSource = true;
	FilePath cwd;
	// Applied to every line before sorting; identity when empty.
	std::function<ResultLine(const ResultLine&)> mapLocation;
};

// Keeps the best tiers (then path/line order) under the limit and prints those sorted by path then line; returns EXIT_RESULTS or EXIT_EMPTY.
int printResults(std::vector<ResultLine> lines, const OutputOptions& options, std::ostream& out);

// Relative to cwd when the file is below it, otherwise absolute.
std::wstring displayPath(const FilePath& file, const FilePath& cwd);
}

#endif
