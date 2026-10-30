#include "StOutput.h"

#include <algorithm>
#include <tuple>

#include "utilityString.h"

namespace st
{
namespace
{
auto order(const ResultLine& line)
{
	return std::make_tuple(line.file.wstr(), line.line, line.key, line.kind);
}

std::string clean(const std::string& source)
{
	std::string text = source;
	std::replace(text.begin(), text.end(), '\t', ' ');
	const size_t first = text.find_first_not_of(" \r\n");
	if (first == std::string::npos)
	{
		return "";
	}
	return text.substr(first, text.find_last_not_of(" \r\n") - first + 1);
}
}

std::wstring displayPath(const FilePath& file, const FilePath& cwd)
{
	const std::wstring path = file.wstr();
	std::wstring base = cwd.wstr();
	while (base.size() > 1 && (base.back() == L'/' || base.back() == L'\\'))
	{
		base.pop_back();
	}
	if (!base.empty() && path.size() > base.size() + 1 && path.compare(0, base.size(), base) == 0 &&
		(path[base.size()] == L'/' || path[base.size()] == L'\\'))
	{
		return path.substr(base.size() + 1);
	}
	return path;
}

int printResults(std::vector<ResultLine> lines, const OutputOptions& options, std::ostream& out)
{
	std::stable_sort(lines.begin(), lines.end(), [](const ResultLine& a, const ResultLine& b) {
		return order(a) < order(b);
	});
	lines.erase(
		std::unique(lines.begin(), lines.end(), [](const ResultLine& a, const ResultLine& b) {
			return order(a) == order(b);
		}),
		lines.end());
	if (lines.empty())
	{
		return EXIT_EMPTY;
	}
	const size_t shown = std::min(lines.size(), options.limit);
	for (size_t i = 0; i < shown; i++)
	{
		const ResultLine& line = lines[i];
		out << utility::encodeToUtf8(line.key) << '\t' << utility::encodeToUtf8(line.kind) << '\t'
			<< utility::encodeToUtf8(displayPath(line.file, options.cwd)) << ':' << line.line;
		if (options.showSource)
		{
			out << '\t' << clean(line.source);
		}
		out << '\n';
	}
	if (shown < lines.size())
	{
		out << '+' << lines.size() - shown << " more (--limit)\n";
	}
	return EXIT_RESULTS;
}
}
