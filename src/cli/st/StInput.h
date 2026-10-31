#ifndef ST_INPUT_H
#define ST_INPUT_H

#include <istream>
#include <string>
#include <vector>

namespace st
{
struct InputLine
{
	std::wstring key;
	std::string raw;
};

// Non-blank lines with "\r" stripped, identical lines once, in order; `key` is the first column. A
// "+N more (--limit)" marker is not a line; its N is stored in `truncated` (0 when there was none).
std::vector<InputLine> readLines(std::istream& in, size_t* truncated = nullptr);

// First column of each non-blank line, "\r" stripped, duplicates removed in order. A "+N more (--limit)"
// marker is not a key; its N is stored in `truncated` (0 when there was none).
std::vector<std::wstring> readKeys(std::istream& in, size_t* truncated = nullptr);
}

#endif
