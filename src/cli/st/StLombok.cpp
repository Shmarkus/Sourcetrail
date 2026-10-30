#include "StLombok.h"

#include <QFile>

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
	// suffix[i][j] = LCS length of a[i..] and b[j..]; blank lines never match.
	std::vector<std::vector<uint16_t>> suffix(n + 1, std::vector<uint16_t>(m + 1, 0));
	for (size_t i = n; i-- > 0;)
	{
		for (size_t j = m; j-- > 0;)
		{
			suffix[i][j] = (!a[i].empty() && a[i] == b[j]) ? suffix[i + 1][j + 1] + 1
														   : std::max(suffix[i + 1][j], suffix[i][j + 1]);
		}
	}
	size_t i = 0;
	size_t j = 0;
	while (i < n && j < m)
	{
		if (!a[i].empty() && a[i] == b[j])
		{
			result[i++] = j++;
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

ResultLine toOriginal(const ResultLine& line, const LombokRoots& roots,
	const std::function<std::vector<std::string>(const FilePath&)>& readLines)
{
	for (const auto& entry: roots.delombokToOriginalRoots)
	{
		const auto relative = relativeTo(line.file, entry.first);
		if (!relative)
		{
			continue;
		}
		auto cached = roots.alignments.find(line.file.wstr());
		if (cached == roots.alignments.end())
		{
			FilePath originalFile;
			std::vector<std::string> original;
			for (const FilePath& root: entry.second)
			{
				const FilePath candidate = root.getConcatenated(*relative);
				original = readLines(candidate);
				if (!original.empty())
				{
					originalFile = candidate;
					break;
				}
			}
			std::vector<std::optional<size_t>> alignment;
			if (!original.empty())
			{
				alignment = alignLines(readLines(line.file), original);
				roots.originalLines[originalFile.wstr()] = std::move(original);
			}
			cached = roots.alignments.emplace(line.file.wstr(), std::make_pair(originalFile, std::move(alignment))).first;
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
		mapped.source = roots.originalLines[originalFile.wstr()][mapped.line - 1];
		return mapped;
	}
	return line;
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
