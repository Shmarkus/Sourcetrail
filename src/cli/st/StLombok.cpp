#include "StLombok.h"

#include <QFile>

#include <algorithm>
#include <map>
#include <tuple>

#include "ProjectSettings.h"
#include "SourceGroupSettingsJavaGradle.h"
#include "utilityGradle.h"
#include "utilityGradleCache.h"

namespace st
{
namespace
{
const size_t MAX_ALIGNED_LINES = 5000;

std::string trimmed(const std::string& text)
{
	const size_t first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
	{
		return "";
	}
	return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

// The path below `root` when `file` lies inside it, with a trailing separator-free relative form.
std::optional<std::wstring> relativeTo(const FilePath& file, const FilePath& root)
{
	std::wstring base = root.wstr();
	while (base.size() > 1 && (base.back() == L'/' || base.back() == L'\\'))
	{
		base.pop_back();
	}
	const std::wstring path = file.wstr();
	if (path.size() > base.size() + 1 && path.compare(0, base.size(), base) == 0 &&
		(path[base.size()] == L'/' || path[base.size()] == L'\\'))
	{
		return path.substr(base.size() + 1);
	}
	return std::nullopt;
}

// Resolves symlinks through the existing parent directories, so the file itself may be missing.
FilePath canonical(const FilePath& file)
{
	return file.getParentDirectory().getCanonical().getConcatenated(file.fileName());
}

ResultLine generated(ResultLine line)
{
	line.kind += L" (generated)";
	return line;
}
}

std::vector<std::optional<size_t>> alignLines(
	const std::vector<std::string>& generatedLines, const std::vector<std::string>& original)
{
	const size_t n = generatedLines.size();
	const size_t m = original.size();
	std::vector<std::optional<size_t>> result(n);
	if (n > MAX_ALIGNED_LINES || m > MAX_ALIGNED_LINES)
	{
		return result;
	}
	std::vector<std::string> a;
	std::vector<std::string> b;
	for (const std::string& text: generatedLines)
	{
		a.push_back(trimmed(text));
	}
	for (const std::string& text: original)
	{
		b.push_back(trimmed(text));
	}
	const auto neighbour = [](const std::vector<std::string>& lines, size_t index, bool forward) -> std::optional<size_t> {
		for (size_t k = index; forward ? ++k < lines.size() : k-- > 0;)
		{
			if (!lines[k].empty())
			{
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
	for (size_t i = n; i-- > 0;)
	{
		for (size_t j = m; j-- > 0;)
		{
			suffix[i][j] = std::max(suffix[i + 1][j], suffix[i][j + 1]);
			if (!a[i].empty() && a[i] == b[j])
			{
				suffix[i][j] = std::max<uint16_t>(suffix[i][j], suffix[i + 1][j + 1] + weight(i, j));
			}
		}
	}
	std::vector<std::optional<size_t>> raw(n);
	size_t i = 0;
	size_t j = 0;
	while (i < n && j < m)
	{
		if (!a[i].empty() && a[i] == b[j] && suffix[i][j] == suffix[i + 1][j + 1] + weight(i, j))
		{
			raw[i++] = j++;
		}
		else if (suffix[i + 1][j] >= suffix[i][j + 1])
		{
			i++;
		}
		else
		{
			j++;
		}
	}

	std::map<std::string, size_t> inOriginal;
	std::map<std::string, size_t> inGenerated;
	for (const std::string& text: b)
	{
		inOriginal[text]++;
	}
	for (const std::string& text: a)
	{
		inGenerated[text]++;
	}
	const auto surroundings = [&](size_t g) {
		const auto before = neighbour(a, g, false);
		const auto after = neighbour(a, g, true);
		return std::make_tuple(before ? a[*before] : std::string(), a[g], after ? a[*after] : std::string());
	};
	std::map<std::tuple<std::string, std::string, std::string>, size_t> triples;
	for (size_t g = 0; g < n; g++)
	{
		if (!a[g].empty())
		{
			triples[surroundings(g)]++;
		}
	}
	const auto matchedTo = [&](std::optional<size_t> generatedIndex, std::optional<size_t> originalIndex) {
		return generatedIndex.has_value() == originalIndex.has_value() &&
			(!generatedIndex || raw[*generatedIndex] == originalIndex);
	};
	for (size_t g = 0; g < n; g++)
	{
		if (!raw[g])
		{
			continue;
		}
		const size_t o = *raw[g];
		const bool contextual = agree(neighbour(a, g, false), neighbour(b, o, false)) ||
			agree(neighbour(a, g, true), neighbour(b, o, true));
		const bool unique = inOriginal[b[o]] == 1 && (inGenerated[a[g]] == 1 || contextual);
		const bool neighboursMatched = matchedTo(neighbour(a, g, false), neighbour(b, o, false)) &&
			matchedTo(neighbour(a, g, true), neighbour(b, o, true));
		if ((unique || neighboursMatched) && triples[surroundings(g)] == 1)
		{
			result[g] = o;
		}
	}
	return result;
}

LombokRoots lombokRoots(const ProjectSettings& settings)
{
	LombokRoots roots;
	for (const auto& group: settings.getAllSourceGroupSettings())
	{
		const auto gradle = std::dynamic_pointer_cast<SourceGroupSettingsJavaGradle>(group);
		if (!gradle || group->getStatus() != SOURCE_GROUP_STATUS_ENABLED)
		{
			continue;
		}
		const FilePath output = gradle->getGradleDependenciesDirectoryPath();
		const auto info = utility::loadGradleProjectInfo(output,
			gradle->getGradleProjectFilePathExpandedAndAbsolute().getParentDirectory(), gradle->getShouldIndexGradleTests());
		if (!info)
		{
			continue;
		}
		for (const auto& sourceSet: info->sourceDirectories)
		{
			std::vector<FilePath> existing;
			for (const FilePath& root: sourceSet.second)
			{
				if (root.exists())
				{
					existing.push_back(root);
				}
			}
			roots.delombokToOriginalRoots.emplace_back(
				utility::getDelombokOutputDirectory(output, sourceSet.first), existing);
		}
	}
	return roots;
}

namespace
{
struct Counterpart
{
	bool inside = false;
	// Empty unless exactly one original root holds the file.
	FilePath file;
	std::vector<std::string> lines;
};

Counterpart findOriginal(const FilePath& generatedFile, const LombokRoots& roots,
	const std::function<std::vector<std::string>(const FilePath&)>& readLines)
{
	Counterpart result;
	for (const auto& entry: roots.delombokToOriginalRoots)
	{
		const auto relative = relativeTo(canonical(generatedFile), entry.first.getCanonical());
		if (!relative)
		{
			continue;
		}
		result.inside = true;
		size_t found = 0;
		for (const FilePath& root: entry.second)
		{
			const FilePath candidate = root.getConcatenated(*relative);
			std::vector<std::string> lines = readLines(candidate);
			if (!lines.empty() && found++ == 0)
			{
				result.file = candidate;
				result.lines = std::move(lines);
			}
		}
		if (found != 1)
		{
			result.file = FilePath();
			result.lines.clear();
		}
		break;
	}
	return result;
}
}

ResultLine toOriginal(const ResultLine& line, const LombokRoots& roots,
	const std::function<std::vector<std::string>(const FilePath&)>& readLines)
{
	auto cached = roots.alignments.find(line.file.wstr());
	if (cached == roots.alignments.end())
	{
		Counterpart counterpart = findOriginal(line.file, roots, readLines);
		if (!counterpart.inside)
		{
			return line;
		}
		std::vector<std::optional<size_t>> alignment;
		if (!counterpart.file.empty())
		{
			alignment = alignLines(readLines(line.file), counterpart.lines);
			roots.originalLines[counterpart.file.wstr()] = std::move(counterpart.lines);
		}
		cached = roots.alignments.emplace(line.file.wstr(), std::make_pair(counterpart.file, std::move(alignment))).first;
	}
	const FilePath& originalFile = cached->second.first;
	const auto& alignment = cached->second.second;
	if (originalFile.empty() || line.line == 0 || line.line > alignment.size() || !alignment[line.line - 1])
	{
		return generated(line);
	}
	ResultLine mapped = line;
	mapped.file = originalFile;
	mapped.line = *alignment[line.line - 1] + 1;
	mapped.endLine = 0;
	if (line.endLine > line.line && line.endLine <= alignment.size() && alignment[line.endLine - 1])
	{
		mapped.endLine = *alignment[line.endLine - 1] + 1;
	}
	mapped.source = roots.originalLines[originalFile.wstr()][mapped.line - 1];
	return mapped;
}

std::optional<FilePath> toOriginalFile(const FilePath& generatedFile, const LombokRoots& roots)
{
	// Only the path matters here: any non-empty reply stands for an existing file.
	const Counterpart counterpart = findOriginal(generatedFile, roots, [](const FilePath& file) {
		return file.exists() ? std::vector<std::string>{""} : std::vector<std::string>{};
	});
	if (counterpart.file.empty())
	{
		return std::nullopt;
	}
	return counterpart.file;
}

std::optional<FilePath> toGeneratedFile(const FilePath& original, const LombokRoots& roots)
{
	std::vector<FilePath> found;
	for (const auto& entry: roots.delombokToOriginalRoots)
	{
		for (const FilePath& root: entry.second)
		{
			const auto relative = relativeTo(canonical(original), root.getCanonical());
			if (!relative)
			{
				continue;
			}
			const FilePath candidate = entry.first.getConcatenated(*relative);
			if (candidate.exists() && std::find(found.begin(), found.end(), candidate) == found.end())
			{
				found.push_back(candidate);
			}
		}
	}
	if (found.size() != 1)
	{
		return std::nullopt;
	}
	return found.front();
}

std::optional<size_t> toGeneratedLine(const FilePath& originalFile, const FilePath& generatedFile, size_t line,
	const std::function<std::vector<std::string>(const FilePath&)>& readLines, const LombokRoots* cache)
{
	std::vector<std::optional<size_t>> computed;
	const std::vector<std::optional<size_t>>* aligned = &computed;
	if (cache)
	{
		auto it = cache->reverseAlignments.find(generatedFile.wstr());
		if (it == cache->reverseAlignments.end())
		{
			it = cache->reverseAlignments.emplace(generatedFile.wstr(),
				alignLines(readLines(generatedFile), readLines(originalFile))).first;
		}
		aligned = &it->second;
	}
	else
	{
		computed = alignLines(readLines(generatedFile), readLines(originalFile));
	}
	const auto& alignment = *aligned;
	for (size_t i = 0; i < alignment.size(); i++)
	{
		if (line > 0 && alignment[i] && *alignment[i] == line - 1)
		{
			return i + 1;
		}
	}
	return std::nullopt;
}

std::vector<std::string> readLinesFromDisk(const FilePath& file)
{
	std::vector<std::string> lines;
	QFile input(QString::fromStdWString(file.wstr()));
	if (!input.open(QIODevice::ReadOnly))
	{
		return lines;
	}
	while (!input.atEnd())
	{
		lines.push_back(input.readLine().toStdString());
	}
	return lines;
}
}
