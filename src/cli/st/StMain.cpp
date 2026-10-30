#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>

#include <iostream>
#include <set>
#include <stdexcept>

#ifdef _WIN32
#include <io.h>
#define ST_ISATTY(fd) _isatty(fd)
#else
#include <unistd.h>
#define ST_ISATTY(fd) isatty(fd)
#endif

#include "CppSQLite3.h"
#include "NodeType.h"
#include "NodeTypeSet.h"
#include "PersistentStorage.h"
#include "ProjectSettings.h"
#include "SearchMatch.h"
#include "SourceLocation.h"
#include "SourceLocationCollection.h"
#include "StInput.h"
#include "StKey.h"
#include "StOutput.h"
#include "StProject.h"
#include "StorageStats.h"
#include "TextAccess.h"
#include "utilityString.h"

namespace
{
struct UsageError: std::runtime_error
{
	using std::runtime_error::runtime_error;
};

size_t number(const QCommandLineParser& parser, const QString& name, size_t fallback)
{
	if (!parser.isSet(name))
	{
		return fallback;
	}
	bool valid = false;
	const int result = parser.value(name).toInt(&valid);
	if (!valid || result < 0)
	{
		throw UsageError(("Invalid --" + name + ": expected a non-negative integer").toStdString());
	}
	return static_cast<size_t>(result);
}

FilePath registryFile()
{
	return FilePath((QDir::homePath() + "/.config/sourcetrail/st-projects.tsv").toStdWString());
}

FilePath locateProject(const QCommandLineParser& parser, const FilePath& cwd)
{
	if (parser.isSet("project"))
	{
		const QFileInfo file(parser.value("project"));
		if (!file.isFile() || file.suffix() != "srctrlprj")
		{
			throw UsageError("--project must be an existing .srctrlprj file");
		}
		const FilePath project(file.canonicalFilePath().toStdWString());
		st::registerProject(registryFile(), project);
		return project;
	}
	const FilePath project = st::findProject(cwd, registryFile());
	if (project.empty())
	{
		throw UsageError("No Sourcetrail project found from here; pass --project <file.srctrlprj> once to register it");
	}
	return project;
}

NodeKindMask kindMask(const QCommandLineParser& parser)
{
	if (!parser.isSet("kind"))
	{
		return ~0;
	}
	const std::wstring name = parser.value("kind").toLower().toStdWString();
	const NodeKind kind = getNodeKindForReadableNodeKindString(name);
	if (getReadableNodeKindWString(kind) != name)
	{
		throw UsageError("Unknown --kind '" + parser.value("kind").toStdString() + "'");
	}
	return kind;
}

bool isIndexed(const SearchMatch& match)
{
	return match.typeName.compare(0, 12, L"non-indexed ") != 0;
}

// Scope location when there is one, else the first token location.
std::vector<st::ResultLine> definitionLines(PersistentStorage& storage, Id id, const std::wstring& key)
{
	const auto locations = storage.getSourceLocationsForTokenIds({id});
	const SourceLocation* best = nullptr;
	locations->forEachSourceLocation([&](SourceLocation* location) {
		if (!location->isStartLocation())
		{
			return;
		}
		if (!best || (location->isScopeLocation() && !best->isScopeLocation()))
		{
			best = location;
		}
	});
	if (!best)
	{
		return {};
	}
	std::string source;
	const auto content = storage.getFileContent(best->getFilePath(), false);
	if (content && best->getLineNumber() >= 1 && best->getLineNumber() <= content->getLineCount())
	{
		source = content->getLine(static_cast<unsigned int>(best->getLineNumber()));
	}
	return {{key, storage.getNodeTypeForNodeWithId(id).getReadableTypeWString(), best->getFilePath(),
		best->getLineNumber(), source}};
}

int find(PersistentStorage& storage, const std::vector<std::wstring>& names, NodeKindMask kinds,
	const st::OutputOptions& options)
{
	std::vector<st::ResultLine> lines;
	for (const std::wstring& name: names)
	{
		for (const auto& match: storage.getAutocompletionSymbolMatches(name, NodeTypeSet::all(), 1000, 0))
		{
			if (!isIndexed(match))
			{
				continue;
			}
			for (Id id: match.tokenIds)
			{
				if (!(storage.getNodeTypeForNodeWithId(id).getKind() & kinds))
				{
					continue;
				}
				const NameHierarchy hierarchy = storage.getNameHierarchyForNodeId(id);
				if (hierarchy.size() == 0)
				{
					continue;
				}
				for (auto& line: definitionLines(storage, id, st::symbolKey(hierarchy)))
				{
					lines.push_back(std::move(line));
				}
			}
		}
	}
	std::cerr << "find: " << lines.size() << '\n';
	return st::printResults(std::move(lines), options, std::cout);
}

int status(PersistentStorage& storage)
{
	const StorageStats stats = storage.getStorageStats();
	std::cout << "indexed_at\t" << stats.timestamp.toString() << '\n'
			  << "files\t" << stats.fileCount << '\n'
			  << "errors\t" << storage.getErrorCount().total << '\n'
			  << "stale_files\tunknown\n";
	return st::EXIT_RESULTS;
}

int run(QCoreApplication& app)
{
	QCommandLineParser parser;
	parser.setApplicationDescription("Agent-facing code navigation over a Sourcetrail index. Results on stdout, progress on stderr.");
	parser.addHelpOption();
	parser.addPositionalArgument("command", "find | status");
	parser.addPositionalArgument("symbol", "Symbol name or key; read from stdin when omitted", "[symbol]");
	parser.addOption({"project", "Project file (.srctrlprj); registers it for later use from its source tree", "file"});
	parser.addOption({"limit", "Maximum result lines (default 50)", "count"});
	parser.addOption({"depth", "Traversal depth for graph commands", "n"});
	parser.addOption({"kind", "Only symbols of this kind, e.g. class, method", "kind"});
	parser.addOption({"no-source", "Omit the source line column"});
	parser.addOption({"no-refresh", "Do not refresh a stale index before querying"});
	parser.addOption({"wait", "Milliseconds to wait for the project lock (default 30000)", "ms"});
	if (!parser.parse(app.arguments()))
	{
		throw UsageError(parser.errorText().toStdString());
	}
	if (parser.isSet("help"))
	{
		std::cout << parser.helpText().toStdString();
		return st::EXIT_RESULTS;
	}
	const QStringList args = parser.positionalArguments();
	if (args.isEmpty() || (args[0] != "find" && args[0] != "status") || args.size() > 2 ||
		(args[0] == "status" && args.size() != 1))
	{
		throw UsageError("Usage: st <find|status> [symbol] [--project P] [--limit N] [--kind K] [--no-source]; see --help");
	}
	const QString command = args[0];
	st::OutputOptions options;
	options.limit = number(parser, "limit", options.limit);
	options.showSource = !parser.isSet("no-source");
	options.cwd = FilePath(QDir::currentPath().toStdWString());
	const size_t wait = number(parser, "wait", 30000);
	const NodeKindMask kinds = kindMask(parser);

	std::vector<std::wstring> names;
	if (command == "find")
	{
		if (args.size() == 2)
		{
			names.push_back(args[1].toStdWString());
		}
		else if (!ST_ISATTY(0))
		{
			names = st::readKeys(std::cin);
		}
		if (names.empty())
		{
			return args.size() == 2 ? st::EXIT_USAGE : st::EXIT_EMPTY;
		}
	}

	const FilePath projectFile = locateProject(parser, options.cwd);
	ProjectSettings settings;
	if (!settings.load(projectFile, true))
	{
		throw UsageError("Unable to load project settings");
	}
	const QString project = QString::fromStdWString(projectFile.wstr());
	QLockFile lock(project + ".lock");
	lock.setStaleLockTime(0);
	if (!lock.tryLock(static_cast<int>(wait)))
	{
		std::cerr << "st: project is busy; gave up after " << wait << " ms (see --wait)\n";
		return st::EXIT_STALE;
	}
	if (settings.getTempDBFilePath().exists())
	{
		std::cerr << "st: an unfinished index exists; reindex the project\n";
		return st::EXIT_NO_INDEX;
	}
	if (!settings.getDBFilePath().exists())
	{
		std::cerr << "st: project has no index; index it first\n";
		return st::EXIT_NO_INDEX;
	}
	PersistentStorage storage(settings.getDBFilePath(), settings.getBookmarkDBFilePath(), true);
	if (storage.isEmpty() || storage.isIncompatible())
	{
		std::cerr << "st: index is empty or incompatible; reindex with --full\n";
		return st::EXIT_NO_INDEX;
	}
	storage.buildCaches();
	return command == "find" ? find(storage, names, kinds, options) : status(storage);
}
}

int main(int argc, char** argv)
{
	QCoreApplication app(argc, argv);
	QCoreApplication::setApplicationName("st");
	try
	{
		return run(app);
	}
	catch (const UsageError& error)
	{
		std::cerr << "st: " << error.what() << '\n';
		return st::EXIT_USAGE;
	}
	catch (CppSQLite3Exception& error)
	{
		std::cerr << "st: " << error.errorMessage() << '\n';
	}
	catch (const std::exception& error)
	{
		std::cerr << "st: " << error.what() << '\n';
	}
	return st::EXIT_NO_INDEX;
}
