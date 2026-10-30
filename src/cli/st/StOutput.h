#ifndef ST_OUTPUT_H
#define ST_OUTPUT_H

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
	EXIT_USAGE = 64
};

struct ResultLine
{
	std::wstring key;
	std::wstring kind;
	FilePath file;
	size_t line;
	std::string source;
};

struct OutputOptions
{
	size_t limit = 50;
	bool showSource = true;
	FilePath cwd;
};

// Sorts by path then line, drops duplicates, applies the limit; returns EXIT_RESULTS or EXIT_EMPTY.
int printResults(std::vector<ResultLine> lines, const OutputOptions& options, std::ostream& out);

// Relative to cwd when the file is below it, otherwise absolute.
std::wstring displayPath(const FilePath& file, const FilePath& cwd);
}

#endif
