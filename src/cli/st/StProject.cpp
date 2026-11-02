#include "StProject.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSaveFile>
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

QString canonical(const QString& path)
{
	const QString resolved = QFileInfo(path).canonicalFilePath();
	return resolved.isEmpty() ? normalized(path) : resolved;
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
	std::vector<std::pair<QString, QString>> registered;
	for (const QString& line: readLines(registryFile))
	{
		const QStringList columns = line.trimmed().split('\t');
		if (columns.size() == 2)
		{
			registered.emplace_back(canonical(columns[0]), columns[1]);
		}
	}
	const QString given = normalized(toQt(cwd));
	QStringList starts{given};
	if (const QString resolved = canonical(given); resolved != given)
	{
		starts << resolved;
	}
	for (const QString& start: starts)
	{
		for (QDir dir(start); ; )
		{
			const QStringList projects = dir.entryList({"*.srctrlprj"}, QDir::Files, QDir::Name);
			if (!projects.isEmpty())
			{
				return fromQt(dir.absoluteFilePath(projects.first()));
			}
			const QString here = canonical(dir.path());
			for (const auto& [root, project]: registered)
			{
				if (root == here && QFile::exists(project))
				{
					return fromQt(project);
				}
			}
			if (!dir.cdUp())
			{
				break;
			}
		}
	}
	return FilePath();
}

QString projectLockFile(const FilePath& project)
{
	return toQt(project.getCanonical()) + ".lock";
}

void registerProject(const FilePath& registryFile, const FilePath& project, std::ostream& err)
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
		err << "st: project has no Gradle source group to register; run from its directory or pass --project\n";
		return;
	}
	const QString projectPath = toQt(project.getAbsolute());
	QDir().mkpath(QFileInfo(toQt(registryFile)).absolutePath());
	QLockFile lock(toQt(registryFile) + ".lock");
	if (!lock.tryLock(10000))
	{
		err << "st: cannot lock " << registryFile.str() << '\n';
		return;
	}
	const QStringList existing = readLines(registryFile);
	QStringList lines;
	for (const QString& line: existing)
	{
		const QString root = normalized(line.split('\t').first());
		if (!roots.contains(root) || line == root + '\t' + projectPath)
		{
			lines << line;
		}
	}
	for (const QString& root: roots)
	{
		const QString entry = root + '\t' + projectPath;
		if (!lines.contains(entry))
		{
			lines << entry;
		}
	}
	if (lines == existing)
	{
		return;
	}
	QSaveFile out(toQt(registryFile));
	if (!out.open(QIODevice::WriteOnly | QIODevice::Text) || out.write((lines.join('\n') + '\n').toUtf8()) < 0 || !out.commit())
	{
		err << "st: cannot write " << registryFile.str() << '\n';
	}
}
}
