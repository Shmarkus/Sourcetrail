#include "StInput.h"

#include <algorithm>
#include <regex>

#include "utilityString.h"

namespace st
{
std::vector<std::wstring> readKeys(std::istream& in)
{
	std::vector<std::wstring> keys;
	static const std::regex overflow(R"(\+\d+ more \(--limit\))");
	std::string line;
	while (std::getline(in, line))
	{
		line = line.substr(0, line.find('\t'));
		if (std::regex_match(line.begin(), line.end() - (!line.empty() && line.back() == '\r'), overflow))
		{
			continue;
		}
		const size_t first = line.find_first_not_of(" \r");
		if (first == std::string::npos)
		{
			continue;
		}
		const std::wstring key = utility::decodeFromUtf8(
			line.substr(first, line.find_last_not_of(" \r") - first + 1));
		if (std::find(keys.begin(), keys.end(), key) == keys.end())
		{
			keys.push_back(key);
		}
	}
	return keys;
}
}
