#include "StFreshness.h"

#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <iostream>
#include <set>

#include "FileSystem.h"
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

bool changedSince(const FilePath& path, const TimeStamp& indexedAt)
{
	TimeStamp written = FileSystem::getLastWriteTime(path);
	return written >= indexedAt;
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
			fallback = true;
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
		freshness.reason = "no Gradle source roots cached; checked indexed files only";
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
	const std::string text = record.startedAt.toString() + "\n" + record.indexedAt.toString() + "\n";
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
}
