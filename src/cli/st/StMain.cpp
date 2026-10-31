#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLockFile>

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
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
#include "FileInfo.h"
#include "PersistentStorage.h"
#include "ProjectSettings.h"
#include "Query.h"
#include "StFreshness.h"
#include "StFilter.h"
#include "StGraph.h"
#include "StInput.h"
#include "StKey.h"
#include "StLombok.h"
#include "StOutput.h"
#include "StProject.h"
#include "StorageStats.h"
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

int find(PersistentStorage& storage, const std::vector<std::wstring>& names, NodeKindMask kinds,
	const st::OutputOptions& options)
{
	std::vector<st::ResultLine> lines;
	for (const std::wstring& name: names)
	{
		for (const auto& symbol: st::rankSymbols(storage, name, kinds))
		{
			for (auto& line: st::definitionLines(storage, symbol.id, st::symbolKey(storage.getNameHierarchyForNodeId(symbol.id))))
			{
				line.tier = symbol.tier;
				lines.push_back(std::move(line));
			}
		}
	}
	std::cerr << "find: " << lines.size() << '\n';
	return st::printResults(std::move(lines), options, std::cout);
}

const QStringList GRAPH_COMMANDS = {"refs", "callers", "callees", "impls", "supers", "path"};

// Resolves every name; ambiguous names list their candidates on stderr.
int resolve(PersistentStorage& storage, const std::vector<std::wstring>& names, NodeKindMask kinds,
	const st::OutputOptions& options, std::vector<Id>* ids)
{
	std::vector<st::ResultLine> candidates;
	for (const std::wstring& name: names)
	{
		const st::Resolved resolved = st::resolveSymbol(storage, name, kinds);
		if (resolved.status == st::Resolution::NotFound)
		{
			std::cerr << "st: symbol not found: " << utility::encodeToUtf8(name) << '\n';
		}
		else if (resolved.status == st::Resolution::Ambiguous)
		{
			std::cerr << "st: ambiguous: " << utility::encodeToUtf8(name) << '\n';
			for (const Id id: resolved.ids)
			{
				for (auto& line: st::definitionLines(storage, id, st::symbolKey(storage.getNameHierarchyForNodeId(id))))
				{
					candidates.push_back(std::move(line));
				}
			}
		}
		else
		{
			ids->insert(ids->end(), resolved.ids.begin(), resolved.ids.end());
		}
	}
	if (!candidates.empty())
	{
		st::OutputOptions all = options;
		all.limit = candidates.size();
		st::printResults(std::move(candidates), all, std::cerr);
		return st::EXIT_AMBIGUOUS;
	}
	return ids->empty() ? st::EXIT_NOT_FOUND : st::EXIT_RESULTS;
}

int path(PersistentStorage& storage, const std::vector<Id>& from, const std::vector<Id>& to, size_t depth,
	const st::OutputOptions& options)
{
	const std::vector<Id> origins = st::expandClasses(storage, from);
	const std::vector<Id> targets = st::expandClasses(storage, to);
	if (origins.size() * targets.size() > 200)
	{
		std::cerr << "st: path searches only the first 200 of " << origins.size() * targets.size()
				  << " symbol pairs\n";
	}
	const size_t found = 1000;
	const auto chains = st::paths(storage, origins, targets, depth, std::max(found, options.limit + 1));
	std::cerr << "path: " << chains.size() << '\n';
	const auto location = [&](const FilePath& file, size_t line) {
		st::ResultLine mapped{L"", L"", file, line, ""};
		if (options.mapLocation)
		{
			mapped = options.mapLocation(mapped);
		}
		return utility::encodeToUtf8(st::displayPath(mapped.file, options.cwd)) + ':' + std::to_string(mapped.line) +
			utility::encodeToUtf8(mapped.kind);
	};
	for (size_t i = 0; i < chains.size() && i < options.limit; i++)
	{
		const Id start = chains[i].front().from;
		const std::wstring startKey = st::symbolKey(storage.getNameHierarchyForNodeId(start));
		std::cout << i + 1 << ". " << utility::encodeToUtf8(startKey);
		for (const auto& line: st::definitionLines(storage, start, startKey))
		{
			std::cout << '\t' << location(line.file, line.line);
		}
		std::cout << '\n';
		for (const st::PathHop& hop: chains[i])
		{
			std::cout << "   -" << utility::encodeToUtf8(st::linkKind(hop.type)) << "-> "
					  << utility::encodeToUtf8(st::symbolKey(storage.getNameHierarchyForNodeId(hop.to))) << '\t'
					  << location(hop.file, hop.line) << '\n';
		}
	}
	if (chains.size() > options.limit)
	{
		std::cout << "(+" << chains.size() - options.limit << " more paths, --limit)\n";
	}
	return chains.empty() ? st::EXIT_EMPTY : st::EXIT_RESULTS;
}

int graph(PersistentStorage& storage, const QString& command, const std::vector<Id>& ids, size_t depth,
	const st::OutputOptions& options)
{
	std::vector<st::ResultLine> lines;
	if (command == "refs")
	{
		lines = st::references(storage, ids);
	}
	else if (command == "callers" || command == "callees")
	{
		lines = st::calls(storage, st::expandClasses(storage, ids),
			command == "callers" ? st::Direction::Incoming : st::Direction::Outgoing, depth);
	}
	else
	{
		lines = st::hierarchy(storage, ids, command == "impls" ? st::Direction::Incoming : st::Direction::Outgoing);
	}
	std::cerr << command.toStdString() << ": " << lines.size() << '\n';
	return st::printResults(std::move(lines), options, std::cout);
}

// Whether the symbol named by `key` passes; a key that doesn't resolve exactly is reported and fails.
bool keeps(PersistentStorage& storage, const std::wstring& key, const st::FilterOptions& filterOptions,
	const st::OutputOptions& options)
{
	const st::Resolved resolved = st::resolveSymbol(storage, key, ~NodeKindMask(0));
	if (resolved.status != st::Resolution::Found || resolved.ids.size() != 1)
	{
		std::cerr << "st: symbol not found: " << utility::encodeToUtf8(key) << '\n';
		return false;
	}
	const Id id = resolved.ids.front();
	std::wstring path;
	if (!filterOptions.filePattern.empty())
	{
		const auto definitions = st::definitionLines(storage, id, key);
		if (!definitions.empty())
		{
			st::ResultLine definition = definitions.front();
			if (options.mapLocation)
			{
				definition = options.mapLocation(definition);
			}
			path = st::displayPath(definition.file, options.cwd);
		}
	}
	return st::matchesFilter(storage, id, filterOptions, path);
}

// Prints the input lines whose symbol passes; keys that don't resolve exactly are reported and skipped.
int filter(PersistentStorage& storage, const std::vector<st::InputLine>& input, const st::FilterOptions& filterOptions,
	const st::OutputOptions& options)
{
	std::vector<const st::InputLine*> passing;
	std::map<std::wstring, bool> verdicts;	// per distinct key: refs-style input repeats keys across many lines
	for (const st::InputLine& line: input)
	{
		auto verdict = verdicts.find(line.key);
		if (verdict == verdicts.end())
		{
			verdict = verdicts.emplace(line.key, keeps(storage, line.key, filterOptions, options)).first;
		}
		if (verdict->second)
		{
			passing.push_back(&line);
		}
	}
	std::cerr << "filter: " << passing.size() << '\n';
	for (size_t i = 0; i < passing.size() && i < options.limit; i++)
	{
		std::cout << passing[i]->raw << '\n';
	}
	if (passing.size() > options.limit)
	{
		std::cout << '+' << passing.size() - options.limit << " more (--limit)\n";
	}
	return passing.empty() ? st::EXIT_EMPTY : st::EXIT_RESULTS;
}

int status(PersistentStorage& storage, const st::Freshness& freshness)
{
	const StorageStats stats = storage.getStorageStats();
	std::cout << "indexed_at\t" << stats.timestamp.toString() << '\n'
			  << "files\t" << stats.fileCount << '\n'
			  << "errors\t" << storage.getErrorCount().total << '\n'
			  << "stale_files\t" << freshness.changedFiles << '\n';
	return st::EXIT_RESULTS;
}

std::unique_ptr<QLockFile> lockProject(const QString& lockFile, size_t wait)
{
	auto lock = std::make_unique<QLockFile>(lockFile);
	lock->setStaleLockTime(0);
	if (!lock->tryLock(static_cast<int>(wait)))
	{
		std::cerr << "st: project is busy; gave up after " << wait << " ms (see --wait)\n";
		return nullptr;
	}
	return lock;
}

// EXIT_RESULTS when the index can be opened for queries.
int checkIndex(const ProjectSettings& settings)
{
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
	return st::EXIT_RESULTS;
}

// Reads the index time under the project lock and checks the sources against it.
int inspect(const ProjectSettings& settings, const QString& lockFile, size_t wait, TimeStamp* indexedAt,
	st::Freshness* freshness)
{
	const auto lock = lockProject(lockFile, wait);
	if (!lock)
	{
		return st::EXIT_STALE;
	}
	if (const int code = checkIndex(settings))
	{
		return code;
	}
	PersistentStorage storage(settings.getDBFilePath(), settings.getBookmarkDBFilePath(), true);
	if (storage.isEmpty() || storage.isIncompatible())
	{
		std::cerr << "st: index is empty or incompatible; reindex with --full\n";
		return st::EXIT_NO_INDEX;
	}
	std::vector<FilePath> indexedFiles;
	for (const FileInfo& file: storage.getFileInfoForAllFiles())
	{
		indexedFiles.push_back(file.path);
	}
	*indexedAt = storage.getStorageStats().timestamp;
	*freshness = st::checkFreshness(settings, st::freshnessBaseline(*indexedAt, st::lastRefresh(settings)), indexedFiles);
	return st::EXIT_RESULTS;
}

int run(QCoreApplication& app)
{
	QCommandLineParser parser;
	parser.setApplicationDescription(
		"Agent-facing code navigation over a Sourcetrail index. Results on stdout, progress on stderr.\n"
		"refs prints one line per reference; its kind column is the link kind: call, usage, type usage, "
		"override, inheritance, annotation usage, ...");
	parser.addHelpOption();
	parser.addPositionalArgument("command", "find | status | refs | callers | callees | impls | supers | path | filter");
	parser.addPositionalArgument("symbol", "Symbol name or key; read from stdin when omitted", "[symbol]");
	parser.addPositionalArgument("target", "path only: the symbol to reach", "[target]");
	parser.addOption({"project", "Project file (.srctrlprj); registers it for later use from its source tree", "file"});
	parser.addOption({"limit", "Maximum result lines (default 50)", "count"});
	parser.addOption({"depth", "Call levels for callers/callees (default 1) and links for path (default 6); 0 = unlimited", "n"});
	parser.addOption({"kind", "Only symbols of this kind, e.g. class, method", "kind"});
	parser.addOption({"package", "filter only: keep symbols whose key starts with this package", "package"});
	parser.addOption({"annotated", "filter only: keep symbols annotated with this type, or inside a type that is", "annotation"});
	parser.addOption({"file", "filter only: wildcard on the definition file; a pattern with / matches the displayed path, else the file name", "glob"});
	parser.addOption({"not", "filter only: keep the symbols that do not meet the criteria"});
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
	const QString command = args.isEmpty() ? QString() : args[0];
	const bool isGraph = GRAPH_COMMANDS.contains(command);
	if ((command != "find" && command != "status" && command != "filter" && !isGraph) || args.size() > (command == "path" ? 3 : 2) ||
		(command == "status" && args.size() != 1))
	{
		throw UsageError(
			"Usage: st <find|status|refs|callers|callees|impls|supers|path|filter> [symbol] [target] [--project P] "
			"[--limit N] [--depth N] [--kind K] [--package P] [--annotated A] [--file G] [--not] [--no-source] "
			"[--no-refresh]; see --help");
	}
	st::OutputOptions options;
	options.limit = number(parser, "limit", options.limit);
	options.showSource = !parser.isSet("no-source");
	options.cwd = FilePath(QDir::currentPath().toStdWString());
	const size_t wait = number(parser, "wait", 30000);
	const size_t depth = number(parser, "depth", command == "path" ? 6 : 1);
	const NodeKindMask kinds = kindMask(parser);
	for (const QString& name: {"package", "annotated", "file", "not"})
	{
		if (command != "filter" && parser.isSet(name))
		{
			throw UsageError("--" + name.toStdString() + " applies to filter only");
		}
	}
	st::FilterOptions filterOptions;
	if (command == "filter")
	{
		filterOptions.kinds = kinds;
		filterOptions.package = parser.value("package").toStdWString();
		filterOptions.annotation = parser.value("annotated").toStdWString();
		filterOptions.filePattern = parser.value("file").toStdWString();
		filterOptions.invert = parser.isSet("not");
		if (!parser.isSet("kind") && !parser.isSet("package") && !parser.isSet("annotated") && !parser.isSet("file"))
		{
			throw UsageError("filter needs at least one of --kind, --package, --annotated, --file");
		}
	}

	// path reads its origins from stdin when given only the target.
	std::vector<std::wstring> names;
	std::vector<st::InputLine> inputLines;
	std::vector<std::wstring> targets;
	if (command != "status")
	{
		const int named = args.size() - 1;
		if (command == "path" && named >= 1)
		{
			targets.push_back(args.last().toStdWString());
		}
		if (named == (command == "path" ? 2 : 1))
		{
			names.push_back(args[1].toStdWString());
			inputLines.push_back({names.back(), utility::encodeToUtf8(names.back())});
		}
		else if (command == "path" && named == 0)
		{
			throw UsageError("path needs a target: st path <from> <to>, or <keys> | st path <to>");
		}
		else if (!ST_ISATTY(0))
		{
			size_t truncated = 0;
			inputLines = st::readLines(std::cin, &truncated);
			for (const st::InputLine& line: inputLines)
			{
				if (std::find(names.begin(), names.end(), line.key) == names.end())
				{
					names.push_back(line.key);
				}
			}
			if (truncated)
			{
				std::cerr << "st: input truncated upstream (+" << truncated << ", raise --limit)\n";
			}
		}
		if (names.empty())
		{
			return named > (command == "path" ? 1 : 0) ? st::EXIT_USAGE : st::EXIT_EMPTY;
		}
		if (std::count(names.begin(), names.end(), L"") || std::count(targets.begin(), targets.end(), L""))
		{
			throw UsageError("Symbol names must not be empty");
		}
	}

	const FilePath projectFile = locateProject(parser, options.cwd);
	ProjectSettings settings;
	if (!settings.load(projectFile, true))
	{
		throw UsageError("Unable to load project settings");
	}
	const QString project = QString::fromStdWString(projectFile.wstr());

	// Never hold the lock while Sourcetrail refreshes: the indexer takes it without waiting.
	const QString lockFile = st::projectLockFile(projectFile);
	TimeStamp indexedAt;
	st::Freshness freshness;
	if (const int code = inspect(settings, lockFile, wait, &indexedAt, &freshness))
	{
		return code;
	}
	if (!freshness.reason.empty())
	{
		std::cerr << "st: " << freshness.reason << '\n';
	}
	if (freshness.stale && command != "status")
	{
		if (parser.isSet("no-refresh"))
		{
			std::cerr << "st: index is out of date (" << freshness.changedFiles << " changed files); refresh turned off\n";
			return st::EXIT_STALE;
		}
		QElapsedTimer timer;
		timer.start();
		const TimeStamp started = TimeStamp::now();
		const size_t changedFiles = freshness.changedFiles;
		std::string log;
		std::string failure;
		try
		{
			cli::reindex(project, false, 600000, &log);
		}
		catch (const std::exception& error)
		{
			failure = error.what();
		}
		// Waits out a refresh another st process may be running, then looks again.
		if (const int code = inspect(settings, lockFile, wait, &indexedAt, &freshness))
		{
			return code;
		}
		if (failure.empty())
		{
			st::recordRefresh(settings, {started, indexedAt});
			std::cerr << "refreshed " << changedFiles << " files in " << std::fixed << std::setprecision(1)
					  << timer.elapsed() / 1000.0 << "s\n";
		}
		else if (freshness.stale)
		{
			std::cerr << log << "st: refresh failed: " << failure << '\n';
			return st::EXIT_STALE;
		}
		else
		{
			std::cerr << "st: index was refreshed by another process\n";
		}
		freshness = st::Freshness();
	}

	const auto lock = lockProject(lockFile, wait);
	if (!lock)
	{
		return st::EXIT_STALE;
	}
	if (const int code = checkIndex(settings))
	{
		return code;
	}
	const st::LombokRoots lombok = st::lombokRoots(settings);
	if (!lombok.delombokToOriginalRoots.empty())
	{
		options.mapLocation = [&lombok](const st::ResultLine& line) {
			return st::toOriginal(line, lombok, st::readLinesFromDisk);
		};
	}
	PersistentStorage storage(settings.getDBFilePath(), settings.getBookmarkDBFilePath(), true);
	if (storage.isEmpty() || storage.isIncompatible())
	{
		std::cerr << "st: index is empty or incompatible; reindex with --full\n";
		return st::EXIT_NO_INDEX;
	}
	storage.buildCaches();
	if (command == "status")
	{
		return status(storage, freshness);
	}
	if (command == "find")
	{
		return find(storage, names, kinds, options);
	}
	if (command == "filter")
	{
		return filter(storage, inputLines, filterOptions, options);
	}
	std::vector<Id> ids;
	if (const int code = resolve(storage, names, kinds, options, &ids))
	{
		return code;
	}
	if (command != "path")
	{
		return graph(storage, command, ids, depth, options);
	}
	std::vector<Id> targetIds;
	if (const int code = resolve(storage, targets, ~0, options, &targetIds))
	{
		return code;
	}
	return path(storage, ids, targetIds, depth, options);
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
	return st::EXIT_INTERNAL;
}
