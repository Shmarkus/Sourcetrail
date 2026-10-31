#include "StFreshness.h"

#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <set>

#include <boost/date_time/c_local_time_adjustor.hpp>

#include "ProjectSettings.h"
#include "SourceGroupSettingsJavaGradle.h"
#include "utilityGradleCache.h"
#include "utilityString.h"

namespace st
{
namespace
{
QString refreshFile(const ProjectSettings& settings)
{
	return QString::fromStdWString(settings.getFilePath().replaceExtension(L".srctrlst").wstr());
}

// Sub-second, unlike FileSystem::getLastWriteTime: an edit and the refresh it triggers often share a second.
bool changedSince(const FilePath& path, const TimeStamp& indexedAt)
{
	std::error_code ec;
	const auto written = std::filesystem::last_write_time(path.str(), ec);
	if (ec)
	{
		return true;
	}
	const auto utc = std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now() +
		std::chrono::duration_cast<std::chrono::system_clock::duration>(written - std::filesystem::file_time_type::clock::now()));
	const auto seconds = std::chrono::floor<std::chrono::seconds>(utc);
	TimeStamp local(boost::date_time::c_local_adjustor<boost::posix_time::ptime>::utc_to_local(
		boost::posix_time::from_time_t(std::chrono::system_clock::to_time_t(seconds)) +
		boost::posix_time::microseconds((utc - seconds).count())));
	return local >= indexedAt;
}

// TimeStamp::toString drops the fraction of a second.
std::string toPreciseString(const TimeStamp& time)
{
	const std::string seconds = time.toString();
	char fraction[8];
	std::snprintf(fraction, sizeof(fraction), ".%03zu", time.deltaMS(TimeStamp(seconds)) % 1000);
	return seconds + fraction;
}

// Changed source files, plus each changed directory without a changed file in it (a deletion or rename).
size_t changesBelow(const FilePath& root, const std::set<std::wstring>& extensions, const TimeStamp& indexedAt)
{
	const QString rootPath = QString::fromStdWString(root.wstr());
	if (!QFileInfo(rootPath).isDir())
	{
		return 0;
	}
	std::set<QString> changedDirectories;
	std::set<QString> directoriesWithChangedFiles;
	size_t changedFiles = 0;
	const auto visitDirectory = [&](const QString& path) {
		if (changedSince(FilePath(path.toStdWString()), indexedAt))
		{
			changedDirectories.insert(path);
		}
	};
	visitDirectory(rootPath);
	QDirIterator it(rootPath, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
	while (it.hasNext())
	{
		const QString path = it.next();
		const QFileInfo info = it.fileInfo();
		if (info.isDir())
		{
			visitDirectory(path);
		}
		else if (extensions.count(utility::toLowerCase(FilePath(path.toStdWString()).extension())) &&
				 changedSince(FilePath(path.toStdWString()), indexedAt))
		{
			changedFiles++;
			directoriesWithChangedFiles.insert(info.absolutePath());
		}
	}
	for (const QString& directory: changedDirectories)
	{
		if (!directoriesWithChangedFiles.count(directory))
		{
			changedFiles++;
		}
	}
	return changedFiles;
}
}

Freshness checkFreshness(const ProjectSettings& settings, const TimeStamp& indexedAt, const std::vector<FilePath>& indexedFiles)
{
	Freshness freshness;
	bool fallback = false;
	for (const auto& group: settings.getAllSourceGroupSettings())
	{
		if (group->getStatus() != SOURCE_GROUP_STATUS_ENABLED)
		{
			continue;
		}
		const auto gradle = std::dynamic_pointer_cast<SourceGroupSettingsJavaGradle>(group);
		if (!gradle)
		{
			fallback = true;
			continue;
		}
		const FilePath projectDirectory = gradle->getGradleProjectFilePathExpandedAndAbsolute().getParentDirectory();
		for (const FilePath& buildFile: utility::gradleBuildFiles(projectDirectory))
		{
			if (changedSince(buildFile, indexedAt))
			{
				freshness.changedFiles++;
			}
		}
		const auto info = utility::loadGradleProjectInfo(
			gradle->getGradleDependenciesDirectoryPath(), projectDirectory, gradle->getShouldIndexGradleTests());
		if (!info)
		{
			// Indexed files may be delombok copies that only change on refresh; a refresh re-queries Gradle.
			if (gradle->getGradleProjectFilePathExpandedAndAbsolute().exists())
			{
				freshness.changedFiles++;
				freshness.reason = "Gradle source information missing or outdated; next query refreshes";
			}
			else
			{
				fallback = true;
			}
			continue;
		}
		std::set<std::wstring> extensions;
		for (const std::wstring& extension: gradle->getSourceExtensions())
		{
			extensions.insert(utility::toLowerCase(extension));
		}
		for (const auto& sourceSet: info->sourceDirectories)
		{
			for (const FilePath& root: sourceSet.second)
			{
				freshness.changedFiles += changesBelow(root, extensions, indexedAt);
			}
		}
	}
	if (fallback)
	{
		if (freshness.reason.empty())
		{
			freshness.reason = "no Gradle source roots cached; checked indexed files only";
		}
		for (const FilePath& file: indexedFiles)
		{
			if (!file.recheckExists() || changedSince(file, indexedAt))
			{
				freshness.changedFiles++;
			}
		}
	}
	freshness.stale = freshness.changedFiles > 0;
	return freshness;
}

std::optional<RefreshRecord> lastRefresh(const ProjectSettings& settings)
{
	QFile file(refreshFile(settings));
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
	{
		return std::nullopt;
	}
	const QStringList lines = QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
	if (lines.size() != 2)
	{
		return std::nullopt;
	}
	try
	{
		RefreshRecord record{TimeStamp(lines[0].trimmed().toStdString()), TimeStamp(lines[1].trimmed().toStdString())};
		if (!record.startedAt.isValid() || !record.indexedAt.isValid())
		{
			return std::nullopt;
		}
		return record;
	}
	catch (const std::exception&)
	{
		return std::nullopt;
	}
}

void recordRefresh(const ProjectSettings& settings, const RefreshRecord& record)
{
	QSaveFile file(refreshFile(settings));
	const std::string text = toPreciseString(record.startedAt) + "\n" + record.indexedAt.toString() + "\n";
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text) || file.write(text.c_str()) < 0 || !file.commit())
	{
		std::cerr << "st: cannot write " << file.fileName().toStdString() << '\n';
	}
}

TimeStamp freshnessBaseline(const TimeStamp& indexedAt, const std::optional<RefreshRecord>& last)
{
	if (last && last->indexedAt.toString() == indexedAt.toString())
	{
		return last->startedAt;
	}
	return indexedAt;
}

TimeStamp refreshStart()
{
	// File times come from a coarse kernel clock that can trail the precise one by a tick: a file
	// written right after this call may carry an earlier time.
	return TimeStamp(boost::posix_time::microsec_clock::local_time() - boost::posix_time::milliseconds(20));
}
}
