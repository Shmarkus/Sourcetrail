#include "StProject.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTextStream>

#include <iostream>

#include "ProjectSettings.h"
#include "SourceGroupSettingsJavaGradle.h"
#include "utilityString.h"

namespace st
{
namespace
{
QString toQt(const FilePath& path)
{
	return QString::fromStdWString(path.wstr());
}

FilePath fromQt(const QString& path)
{
	return FilePath(path.toStdWString());
}

QString normalized(const QString& path)
{
	return QDir::cleanPath(path);
}

bool isBelow(const QString& path, const QString& root)
{
	return path == root || path.startsWith(root.endsWith('/') ? root : root + '/');
}

QStringList readLines(const FilePath& file)
{
	QFile in(toQt(file));
	if (!in.open(QIODevice::ReadOnly | QIODevice::Text))
	{
		return {};
	}
	return QString::fromUtf8(in.readAll()).split('\n', Qt::SkipEmptyParts);
}
}

FilePath findProject(const FilePath& cwd, const FilePath& registryFile)
{
	const QString start = normalized(toQt(cwd));
	for (QDir dir(start); ; )
	{
		const QStringList projects = dir.entryList({"*.srctrlprj"}, QDir::Files, QDir::Name);
		if (!projects.isEmpty())
		{
			return fromQt(dir.absoluteFilePath(projects.first()));
		}
		if (!dir.cdUp())
		{
			break;
		}
	}
	QString best;
	QString bestRoot;
	for (const QString& line: readLines(registryFile))
	{
		const QStringList columns = line.trimmed().split('\t');
		if (columns.size() != 2)
		{
			continue;
		}
		const QString root = normalized(columns[0]);
		if (isBelow(start, root) && root.size() > bestRoot.size() && QFile::exists(columns[1]))
		{
			best = columns[1];
			bestRoot = root;
		}
	}
	return best.isEmpty() ? FilePath() : fromQt(best);
}

void registerProject(const FilePath& registryFile, const FilePath& project)
{
	ProjectSettings settings;
	if (!settings.load(project, true))
	{
		return;
	}
	QStringList roots;
	for (const auto& group: settings.getAllSourceGroupSettings())
	{
		const auto gradle = std::dynamic_pointer_cast<SourceGroupSettingsJavaGradle>(group);
		if (gradle && gradle->getStatus() == SOURCE_GROUP_STATUS_ENABLED)
		{
			const QString root = normalized(toQt(gradle->getGradleProjectFilePathExpandedAndAbsolute().getParentDirectory()));
			if (!root.isEmpty() && !roots.contains(root))
			{
				roots << root;
			}
		}
	}
	if (roots.isEmpty())
	{
		std::cerr << "st: project has no Gradle source group to register; run from its directory or pass --project\n";
		return;
	}
	const QString projectPath = toQt(project.getAbsolute());
	QStringList lines;
	for (const QString& line: readLines(registryFile))
	{
		if (!roots.contains(normalized(line.split('\t').first())))
		{
			lines << line;
		}
	}
	for (const QString& root: roots)
	{
		lines << root + '\t' + projectPath;
	}
	QDir().mkpath(QFileInfo(toQt(registryFile)).absolutePath());
	QFile out(toQt(registryFile));
	if (out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
	{
		out.write((lines.join('\n') + '\n').toUtf8());
	}
	else
	{
		std::cerr << "st: cannot write " << registryFile.str() << '\n';
	}
}
}
