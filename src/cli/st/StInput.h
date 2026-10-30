#ifndef ST_INPUT_H
#define ST_INPUT_H

#include <istream>
#include <string>
#include <vector>

namespace st
{
// First column of each non-blank line, "\r" stripped, duplicates removed in order.
std::vector<std::wstring> readKeys(std::istream& in);
}

#endif
