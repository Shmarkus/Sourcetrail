#include "StCommand.h"

#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLockFile>

#include <algorithm>
#include <iomanip>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>

#include "CppSQLite3.h"
#include "NodeType.h"
#include "NodeTypeSet.h"
#include "FileInfo.h"
#include "PersistentStorage.h"
#include "ProjectSettings.h"
#include "Query.h"
#include "StArchitecture.h"
#include "StFiles.h"
#include "StFreshness.h"
#include "StFilter.h"
#include "StGaps.h"
#include "StGraph.h"
#include "StInput.h"
#include "StKey.h"
#include "StLombok.h"
#include "StOutput.h"
#include "StProject.h"
#include "StServer.h"
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

// The --project file, or an empty path when it is not an existing .srctrlprj.
FilePath givenProject(const QCommandLineParser& parser, const QString& cwd)
{
	const QFileInfo file(QDir(cwd), parser.value("project"));
	if (!file.isFile() || file.suffix() != "srctrlprj")
	{
		return FilePath();
	}
	return FilePath(file.canonicalFilePath().toStdWString());
}

FilePath locateProject(const st::Io& io, const QCommandLineParser& parser, const QString& cwd)
{
	if (parser.isSet("project"))
	{
		const FilePath project = givenProject(parser, cwd);
		if (project.empty())
		{
			throw UsageError("--project must be an existing .srctrlprj file");
		}
		st::registerProject(registryFile(), project, io.err);
		return project;
	}
	const FilePath project = st::findProject(FilePath(cwd.toStdWString()), registryFile());
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

void countLine(const st::Io& io, const st::OutputOptions& options, const std::string& count)
{
	io.err << count << " (project " << options.project << ")\n";
}

int find(const st::Io& io, PersistentStorage& storage, const std::vector<std::wstring>& names, NodeKindMask kinds,
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
	countLine(io, options, "find: " + std::to_string(lines.size()));
	return st::printResults(std::move(lines), options, io.out);
}

const QStringList GRAPH_COMMANDS = {"refs", "callers", "callees", "impls", "supers", "path", "impact", "deps"};

// Prints on stderr the gaps of the index the results touch, or that they could not be checked.
void printGaps(const st::Io& io, PersistentStorage& storage, const st::Walked& walked, const std::vector<st::ResultLine>& lines,
	st::GapInputs* inputs)
{
	st::printGaps([&]() { return st::findGaps(storage, walked, lines, inputs); }, io.err);
}

// Prints the results, then on stderr the gaps of the index they touch.
int printWithGaps(const st::Io& io, PersistentStorage& storage, const std::vector<st::ResultLine>& lines, const st::Walked& walked,
	const st::OutputOptions& options)
{
	const int code = st::printResults(lines, options, io.out);
	printGaps(io, storage, walked, lines, options.gapInputs);
	return code;
}

// Prints the definition lines of `ids` on stderr, all of them.
void printCandidates(const st::Io& io, PersistentStorage& storage, const std::vector<Id>& ids, const st::OutputOptions& options)
{
	std::vector<st::ResultLine> candidates;
	for (const Id id: ids)
	{
		for (auto& line: st::definitionLines(storage, id, st::symbolKey(storage.getNameHierarchyForNodeId(id))))
		{
			candidates.push_back(std::move(line));
		}
	}
	st::OutputOptions all = options;
	all.limit = candidates.size();
	all.hideGenerated = false;
	st::printResults(std::move(candidates), all, io.err);
}

// Resolves every name; ambiguous names list their candidates on stderr, and so does a parameter list matching none
// of the name's overloads.
int resolve(const st::Io& io, st::IndexCache& cache, PersistentStorage& storage, const std::vector<std::wstring>& names,
	NodeKindMask kinds, const st::OutputOptions& options, std::vector<Id>* ids)
{
	std::vector<Id> candidates;
	for (const std::wstring& name: names)
	{
		const st::Resolved resolved = cache.resolve(storage, name, kinds);
		if (resolved.status == st::Resolution::NotFound)
		{
			io.err << "st: symbol not found: " << utility::encodeToUtf8(name) << '\n';
			printCandidates(io, storage, st::overloadsOf(storage, name, kinds), options);
		}
		else if (resolved.status == st::Resolution::Ambiguous)
		{
			io.err << "st: ambiguous: " << utility::encodeToUtf8(name) << '\n';
			candidates.insert(candidates.end(), resolved.ids.begin(), resolved.ids.end());
		}
		else
		{
			ids->insert(ids->end(), resolved.ids.begin(), resolved.ids.end());
		}
	}
	if (!candidates.empty())
	{
		printCandidates(io, storage, candidates, options);
		return st::EXIT_AMBIGUOUS;
	}
	return ids->empty() ? st::EXIT_NOT_FOUND : st::EXIT_RESULTS;
}

int path(const st::Io& io, PersistentStorage& storage, const std::vector<Id>& from, const std::vector<Id>& to, size_t depth,
	Edge::TypeMask types, const st::OutputOptions& options)
{
	const std::vector<Id> origins = st::expandClasses(storage, from);
	const std::vector<Id> targets = st::expandClasses(storage, to);
	if (origins.size() * targets.size() > 200)
	{
		io.err << "st: path searches only the first 200 of " << origins.size() * targets.size()
				  << " symbol pairs\n";
	}
	const size_t found = 1000;
	st::Walked walked;
	const auto chains = st::paths(storage, origins, targets, depth, std::max(found, options.limit + 1), types, &walked);
	std::vector<st::ResultLine> hops;
	for (const auto& chain: chains)
	{
		for (const st::PathHop& hop: chain)
		{
			hops.push_back({L"", L"", hop.file, hop.line, ""});
		}
	}
	countLine(io, options, "path: " + std::to_string(chains.size()));
	const auto location = [&](const FilePath& file, size_t line, size_t column) {
		st::ResultLine mapped{L"", L"", file, line, ""};
		mapped.column = column;
		if (options.mapLocation)
		{
			mapped = options.mapLocation(mapped);
		}
		return utility::encodeToUtf8(st::displayLocation(mapped, options.cwd) + mapped.kind);
	};
	for (size_t i = 0; i < chains.size() && i < options.limit; i++)
	{
		const Id start = chains[i].front().from;
		const std::wstring startKey = st::symbolKey(storage.getNameHierarchyForNodeId(start));
		io.out << i + 1 << ". " << utility::encodeToUtf8(startKey);
		const auto definitions = st::definitionLines(storage, start, startKey);
		if (!definitions.empty())
		{
			io.out << '\t' << location(definitions.front().file, definitions.front().line, definitions.front().column);
		}
		io.out << '\n';
		for (const st::PathHop& hop: chains[i])
		{
			io.out << "   -" << utility::encodeToUtf8(st::linkKind(hop.type)) << "-> "
					  << utility::encodeToUtf8(st::symbolKey(storage.getNameHierarchyForNodeId(hop.to))) << '\t'
					  << location(hop.file, hop.line, hop.column) << '\n';
		}
	}
	if (chains.size() > options.limit)
	{
		io.out << "(+" << chains.size() - options.limit << " more paths, --limit)\n";
	}
	printGaps(io, storage, walked, hops, options.gapInputs);
	return chains.empty() ? st::EXIT_EMPTY : st::EXIT_RESULTS;
}

int impact(const st::Io& io, PersistentStorage& storage, const std::vector<Id>& ids, size_t depth, Edge::TypeMask types, st::Group group,
	const st::LombokRoots& lombok, const st::OutputOptions& options)
{
	st::ImpactResult result = st::impact(storage, ids, depth, types, group);
	countLine(io, options, "impact: " + std::to_string(result.symbols) + " symbols in " + std::to_string(result.files) +
			" files, " + std::to_string(result.packages) + " packages");
	// A file line is keyed by its displayed path: the original of a delombok file.
	for (st::ResultLine& line: result.lines)
	{
		line.key = group == st::Group::File
			? st::displayPath(st::toOriginalFile(line.file, lombok).value_or(line.file), options.cwd)
			: line.key;
	}
	return printWithGaps(io, storage, result.lines, result.walked, options);
}

int deps(const st::Io& io, PersistentStorage& storage, const std::vector<Id>& ids, st::Direction direction, const st::OutputOptions& options)
{
	st::Walked walked;
	std::vector<st::ResultLine> lines = st::deps(storage, ids, direction, &walked);
	countLine(io, options, "deps: " + std::to_string(lines.size()));
	return printWithGaps(io, storage, lines, walked, options);
}

int graph(const st::Io& io, PersistentStorage& storage, const QString& command, const std::vector<Id>& ids, size_t depth,
	const st::OutputOptions& options)
{
	std::vector<st::ResultLine> lines;
	st::Walked walked;
	if (command == "refs")
	{
		lines = st::references(storage, ids, &walked);
	}
	else if (command == "callers" || command == "callees")
	{
		lines = st::calls(storage, st::expandClasses(storage, ids),
			command == "callers" ? st::Direction::Incoming : st::Direction::Outgoing, depth, &walked);
	}
	else
	{
		lines = st::hierarchy(storage, ids, command == "impls" ? st::Direction::Incoming : st::Direction::Outgoing, &walked);
	}
	countLine(io, options, command.toStdString() + ": " + std::to_string(lines.size()));
	return printWithGaps(io, storage, lines, walked, options);
}

// Whether the symbol named by `key` passes; a key that doesn't resolve exactly is reported, fails and sets `unknown`.
bool keeps(const st::Io& io, PersistentStorage& storage, const std::wstring& key, const st::FilterOptions& filterOptions,
	const st::OutputOptions& options, bool& unknown)
{
	const st::Resolved resolved = st::resolveSymbol(storage, key, ~NodeKindMask(0));
	if (resolved.status != st::Resolution::Found || resolved.ids.size() != 1)
	{
		io.err << "st: symbol not found: " << utility::encodeToUtf8(key) << '\n';
		unknown = true;
		return false;
	}
	const Id id = resolved.ids.front();
	// a symbol declared in several files is in a file pattern when one of them is
	std::vector<std::wstring> paths;
	if (!filterOptions.filePattern.empty())
	{
		for (st::ResultLine definition: st::definitionLines(storage, id, key))
		{
			if (options.mapLocation)
			{
				definition = options.mapLocation(definition);
			}
			paths.push_back(st::displayPath(definition.file, options.cwd));
		}
	}
	return st::matchesFilter(storage, id, filterOptions, paths);
}

// Whether a result line printed by st stays generated: the marker ends its kind or its location column.
bool isGeneratedRow(const std::string& raw)
{
	const std::string marker = " (generated)";
	std::stringstream columns(raw);
	std::string column;
	for (int i = 0; i < 3 && std::getline(columns, column, '\t'); i++)
	{
		if (i > 0 && column.size() >= marker.size() && column.compare(column.size() - marker.size(), marker.size(), marker) == 0)
		{
			return true;
		}
	}
	return false;
}

// Prints the input lines whose symbol passes; keys that don't resolve exactly are reported, skipped and exit 3.
int filter(const st::Io& io, PersistentStorage& storage, const std::vector<st::InputLine>& input, const st::FilterOptions& filterOptions,
	const st::OutputOptions& options)
{
	std::vector<const st::InputLine*> passing;
	bool unknown = false;
	std::map<std::wstring, bool> verdicts;	// per distinct key: refs-style input repeats keys across many lines
	for (const st::InputLine& line: input)
	{
		if (options.hideGenerated && isGeneratedRow(line.raw))
		{
			continue;
		}
		auto verdict = verdicts.find(line.key);
		if (verdict == verdicts.end())
		{
			verdict = verdicts.emplace(line.key, keeps(io, storage, line.key, filterOptions, options, unknown)).first;
		}
		if (verdict->second)
		{
			passing.push_back(&line);
		}
	}
	countLine(io, options, "filter: " + std::to_string(passing.size()));
	for (size_t i = 0; i < passing.size() && i < options.limit; i++)
	{
		io.out << passing[i]->raw << '\n';
	}
	if (passing.size() > options.limit)
	{
		io.out << '+' << passing.size() - options.limit << " more (--limit)\n";
	}
	if (unknown)
	{
		return st::EXIT_NOT_FOUND;
	}
	return passing.empty() ? st::EXIT_EMPTY : st::EXIT_RESULTS;
}

bool isGiven(const FilePath& indexed, const std::wstring& path, const FilePath& cwd)
{
	const FilePath given(path);
	const FilePath resolved = given.isAbsolute() ? given : cwd.getConcatenated(path);
	return resolved.wstr() == indexed.wstr() || resolved.getCanonical().wstr() == indexed.wstr();
}

// `<path>:<line>`, anything after a second colon ignored.
void splitLocation(const std::wstring& input, std::wstring* path, size_t* line)
{
	const size_t colon = input.find(L':');
	const size_t end = colon == std::wstring::npos ? colon : input.find(L':', colon + 1);
	const std::wstring digits = colon == std::wstring::npos ? L"" : input.substr(colon + 1, end - colon - 1);
	if (colon == 0 || digits.empty() || digits.find_first_not_of(L"0123456789") != std::wstring::npos ||
		digits.size() > 9 || std::stoul(digits) == 0)
	{
		throw UsageError("Expected <file>:<line>, got '" + utility::encodeToUtf8(input) + "'");
	}
	*path = input.substr(0, colon);
	*line = std::stoul(digits);
}

enum class FileLookup
{
	Found,
	NotIndexed,
	Ambiguous
};

// The one indexed file a user-typed path denotes; reports on stderr when there is none or several.
FileLookup lookupFile(const st::Io& io, const st::IndexedFiles& indexed, const std::wstring& path, const st::LombokRoots& lombok,
	const st::OutputOptions& options, FilePath* file)
{
	const std::vector<FilePath> matches = st::matchIndexedFiles(indexed, path, options.cwd, &lombok);
	if (matches.empty())
	{
		io.err << "st: not indexed: " << utility::encodeToUtf8(path) << '\n';
		return FileLookup::NotIndexed;
	}
	if (matches.size() > 1)
	{
		io.err << "st: ambiguous: " << utility::encodeToUtf8(path) << '\n';
		for (const FilePath& match: matches)
		{
			io.err << utility::encodeToUtf8(st::displayPath(match, options.cwd)) << '\n';
		}
		return FileLookup::Ambiguous;
	}
	*file = matches.front();
	return FileLookup::Found;
}

// symbols-in: the symbols defined in each file. at: the symbol at each <file>:<line>.
int inFiles(const st::Io& io, PersistentStorage& storage, const QString& command, const std::vector<std::wstring>& inputs,
	const st::LombokRoots& lombok, const st::OutputOptions& options)
{
	std::map<Id, std::set<FilePath>> ids;	// symbols-in: the files each is listed for
	const st::IndexedFiles indexed = st::indexedFiles(storage, &lombok);
	size_t skipped = 0;
	bool ambiguous = false;
	for (const std::wstring& input: inputs)
	{
		std::wstring path = input;
		size_t line = 0;
		if (command == "at")
		{
			splitLocation(input, &path, &line);
		}
		FilePath file;
		const FileLookup found = lookupFile(io, indexed, path, lombok, options, &file);
		if (found == FileLookup::NotIndexed)
		{
			skipped++;
			continue;
		}
		if (found == FileLookup::Ambiguous)
		{
			ambiguous = true;
			continue;
		}
		if (command == "symbols-in")
		{
			for (const Id id: st::definedInFile(storage, file))
			{
				ids[id].insert(file);
			}
			continue;
		}
		// A user reading the original source gives original line numbers.
		const std::optional<FilePath> original = st::toOriginalFile(file, lombok);
		const std::optional<Id> id = original && !isGiven(file, path, options.cwd)
			? st::symbolAtOriginal(storage, file, *original, line, st::readLinesFromDisk, &lombok)
			: st::symbolAt(storage, file, line);
		if (!id)
		{
			io.err << "st: " << utility::encodeToUtf8(path) << ':' << line
					  << " has no counterpart in the indexed (delombok) file\n";
			skipped++;
			continue;
		}
		if (*id)
		{
			ids[*id].insert(file);
		}
	}
	if (ambiguous)
	{
		return st::EXIT_AMBIGUOUS;
	}
	if (skipped == inputs.size())
	{
		return st::EXIT_NOT_FOUND;
	}
	std::vector<st::ResultLine> lines;
	for (const auto& [id, files]: ids)
	{
		const std::wstring key = st::symbolKey(storage.getNameHierarchyForNodeId(id));
		std::vector<st::ResultLine> own = st::definitionLines(storage, id, key, &files);
		// `at` on a use of a symbol declared elsewhere
		for (auto& line: own.empty() ? st::definitionLines(storage, id, key) : own)
		{
			lines.push_back(std::move(line));
		}
	}
	countLine(io, options, command.toStdString() + ": " + std::to_string(lines.size()));
	return st::printResults(std::move(lines), options, io.out);
}

// outline: the symbols defined in each file, and the direct members of each type (any other symbol outlines itself).
int outlineInputs(const st::Io& io, st::IndexCache& cache, PersistentStorage& storage, const std::vector<std::wstring>& inputs,
	const st::LombokRoots& lombok, const st::OutputOptions& options)
{
	const st::IndexedFiles indexed = st::indexedFiles(storage, &lombok);
	std::vector<Id> ids;
	std::map<Id, std::set<FilePath>> inFiles;	// symbols listed for a file show only that file's lines
	std::vector<std::wstring> symbolNames;
	size_t skipped = 0;
	bool ambiguous = false;
	for (const std::wstring& input: inputs)
	{
		const FilePath given(input);
		const bool isFile = !st::matchIndexedFiles(indexed, input, options.cwd, &lombok).empty() ||
			QFileInfo(QString::fromStdWString((given.isAbsolute() ? given : options.cwd.getConcatenated(input)).wstr())).isFile();
		if (!isFile)
		{
			symbolNames.push_back(input);
			continue;
		}
		FilePath file;
		const FileLookup found = lookupFile(io, indexed, input, lombok, options, &file);
		if (found == FileLookup::Found)
		{
			const std::vector<Id> defined = st::definedInFile(storage, file);
			ids.insert(ids.end(), defined.begin(), defined.end());
			for (const Id id: defined)
			{
				inFiles[id].insert(file);
			}
		}
		skipped += found == FileLookup::NotIndexed;
		ambiguous |= found == FileLookup::Ambiguous;
	}
	std::vector<Id> symbols;
	const int resolved = symbolNames.empty() ? st::EXIT_RESULTS : resolve(io, cache, storage, symbolNames, ~NodeKindMask(0), options, &symbols);
	if (resolved == st::EXIT_AMBIGUOUS || ambiguous)
	{
		return st::EXIT_AMBIGUOUS;
	}
	if (ids.empty() && symbols.empty() && (resolved == st::EXIT_NOT_FOUND || skipped == inputs.size()))
	{
		return st::EXIT_NOT_FOUND;
	}
	for (const Id id: symbols)
	{
		if (st::isTypeSymbol(storage, id))
		{
			const std::vector<Id> members = st::directMembers(storage, id);
			ids.insert(ids.end(), members.begin(), members.end());
			for (const Id member: members)
			{
				inFiles.erase(member);
			}
		}
		else
		{
			ids.push_back(id);
			inFiles.erase(id);
		}
	}
	std::set<Id> seen;
	ids.erase(std::remove_if(ids.begin(), ids.end(), [&](Id id) { return !seen.insert(id).second; }), ids.end());
	std::vector<st::ResultLine> lines = st::outline(storage, ids, inFiles);
	countLine(io, options, "outline: " + std::to_string(lines.size()));
	return st::printResults(std::move(lines), options, io.out);
}

int status(const st::Io& io, PersistentStorage& storage, size_t unresolved, const st::Freshness& freshness, bool serverRunning)
{
	const StorageStats stats = storage.getStorageStats();
	io.out << "indexed_at\t" << stats.timestamp.toString() << '\n'
			  << "files\t" << stats.fileCount << '\n'
			  << "errors\t" << storage.getErrorCount().total << '\n'
			  << "files_with_errors\t" << storage.getIncompleteFiles().size() << '\n'
			  << "unresolved_symbols\t" << unresolved << '\n'
			  << "stale_files\t" << freshness.changedFiles << '\n'
			  << "server\t" << (serverRunning ? "running" : "off") << '\n';
	return st::EXIT_RESULTS;
}

std::unique_ptr<QLockFile> lockProject(const st::Io& io, const QString& lockFile, size_t wait)
{
	auto lock = std::make_unique<QLockFile>(lockFile);
	lock->setStaleLockTime(0);
	if (!lock->tryLock(static_cast<int>(wait)))
	{
		io.err << "st: project is busy; gave up after " << wait << " ms (see --wait)\n";
		return nullptr;
	}
	return lock;
}

// EXIT_RESULTS when the index can be opened for queries.
int checkIndex(const st::Io& io, const ProjectSettings& settings)
{
	if (settings.getTempDBFilePath().exists())
	{
		io.err << "st: an unfinished index exists; reindex the project\n";
		return st::EXIT_NO_INDEX;
	}
	if (!settings.getDBFilePath().exists())
	{
		io.err << "st: project has no index; index it first\n";
		return st::EXIT_NO_INDEX;
	}
	return st::EXIT_RESULTS;
}

// Reads the index time under the project lock and checks the sources against it.
int inspect(const st::Io& io, const ProjectSettings& settings, const QString& lockFile, size_t wait, TimeStamp* indexedAt,
	st::Freshness* freshness)
{
	const auto lock = lockProject(io, lockFile, wait);
	if (!lock)
	{
		return st::EXIT_STALE;
	}
	if (const int code = checkIndex(io, settings))
	{
		return code;
	}
	PersistentStorage storage(settings.getDBFilePath(), settings.getBookmarkDBFilePath(), true);
	if (storage.isEmpty() || storage.isIncompatible())
	{
		io.err << "st: index is empty or incompatible; reindex with --full\n";
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

// Re-indexes a stale index: EXIT_RESULTS when the index is fresh afterwards, else the exit code to stop with.
int refreshIfStale(const st::Io& io, const ProjectSettings& settings, const QString& project, const QString& lockFile,
	size_t wait, bool noRefresh, TimeStamp* indexedAt, st::Freshness* freshness)
{
	if (noRefresh)
	{
		io.err << "st: index is out of date (" << freshness->changedFiles << " changed files); refresh turned off\n";
		return st::EXIT_STALE;
	}
	QElapsedTimer timer;
	timer.start();
	const TimeStamp started = st::refreshStart();
	const size_t changedFiles = freshness->changedFiles;
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
	if (const int code = inspect(io, settings, lockFile, wait, indexedAt, freshness))
	{
		return code;
	}
	if (failure.empty())
	{
		st::recordRefresh(settings, {started, *indexedAt}, io.err);
		io.err << "refreshed " << changedFiles << " files in " << std::fixed << std::setprecision(1)
			  << timer.elapsed() / 1000.0 << "s\n";
	}
	else if (freshness->stale)
	{
		io.err << log << "st: refresh failed: " << failure << '\n';
		return st::EXIT_STALE;
	}
	else
	{
		io.err << "st: index was refreshed by another process\n";
	}
	*freshness = st::Freshness();
	return st::EXIT_RESULTS;
}

void addOptions(QCommandLineParser& parser)
{
	parser.setApplicationDescription(
		"Agent-facing code navigation over a Sourcetrail index. Results on stdout, progress on stderr.\n"
		"refs prints one line per reference; its kind column is the link kind: call, usage, type usage, "
		"override, inheritance, annotation usage, ...");
	parser.addHelpOption();
	parser.addPositionalArgument("command", "find | status | serve | refs | callers | callees | impls | supers | path | impact | deps | filter | symbols-in | at | outline");
	parser.addPositionalArgument("symbol", "Symbol name or key; read from stdin when omitted. symbols-in takes files "
		"(stdin: one path per line), at takes <file>:<line> (stdin: path:line[:...]), outline takes files or symbols", "[symbol]");
	parser.addPositionalArgument("target", "path only: the symbol to reach", "[target]");
	parser.addOption({"project", "Project file (.srctrlprj); registers it for later use from its source tree", "file"});
	parser.addOption({"limit", "Maximum result lines (default 50)", "count"});
	parser.addOption({"depth", "Call levels for callers/callees (default 1), links for path (default 6) and impact (default 0); 0 = unlimited", "n"});
	parser.addOption({"edges", "path and impact only: link kinds to follow, comma-separated: call, usage, type-usage, override, "
		"inheritance, import, annotation (path default call,usage,override; impact default all but import)", "list"});
	parser.addOption({"group", "impact only: one line per file or package", "file|package"});
	parser.addOption({"in", "deps only: dependencies on the symbol instead of its own"});
	parser.addOption({"kind", "Only symbols of this kind, e.g. class, method", "kind"});
	parser.addOption({"package", "filter only: keep symbols whose key starts with this package", "package"});
	parser.addOption({"annotated", "filter only: keep symbols annotated with this type, or inside a type that is", "annotation"});
	parser.addOption({"file", "filter only: wildcard on the definition file; a pattern with / matches the displayed path, else the file name", "glob"});
	parser.addOption({"not", "filter only: keep the symbols that do not meet the criteria"});
	parser.addOption({"no-source", "Omit the source line column"});
	parser.addOption({"no-generated", "Drop result lines that stay generated: Lombok-made code without an original line"});
	parser.addOption({"no-refresh", "Do not refresh a stale index before querying"});
	parser.addOption({"wait", "Milliseconds to wait for the project lock (default 30000)", "ms"});
	parser.addOption({"detach", "serve only: start the server in the background"});
	parser.addOption({"stop", "serve only: stop the running server"});
	parser.addOption({"no-watch", "serve only: do not watch the sources and refresh in the background"});
	parser.addOption({"no-server", "Answer in this process even when st serve runs for the project"});
}

bool knownCommand(const QString& command)
{
	return command == "find" || command == "status" || command == "filter" || GRAPH_COMMANDS.contains(command) ||
		command == "symbols-in" || command == "at" || command == "outline";
}

// Whether a command with this many names given reads further names from stdin.
bool takesStdin(const QString& command, int named)
{
	return knownCommand(command) && command != "status" && named == (command == "path" ? 1 : 0);
}

int runImpl(const st::Request& request, const st::Io& io, st::IndexCache& cache)
{
	QCommandLineParser parser;
	addOptions(parser);
	if (!parser.parse(request.arguments))
	{
		throw UsageError(parser.errorText().toStdString());
	}
	if (parser.isSet("help"))
	{
		io.out << parser.helpText().toStdString();
		return st::EXIT_RESULTS;
	}
	const QStringList args = parser.positionalArguments();
	const QString command = args.isEmpty() ? QString() : args[0];
	if (command == "serve")
	{
		const char* usage = "Usage: st serve [--detach|--stop] [--no-watch] [--project P]";
		if (args.size() != 1 || (parser.isSet("stop") && (parser.isSet("detach") || parser.isSet("no-watch"))))
		{
			throw UsageError(usage);
		}
		st::ServeOptions options;
		options.watch = !parser.isSet("no-watch");
		if (parser.isSet("stop"))
		{
			return st::stopServer(locateProject(io, parser, request.cwd), io.err);
		}
		if (parser.isSet("detach"))
		{
			return st::detachServer(locateProject(io, parser, request.cwd), options, io.err);
		}
		return st::serve(locateProject(io, parser, request.cwd), options, io.err);
	}
	const bool isFiles = command == "symbols-in" || command == "at";
	const bool takesMany = command == "symbols-in" || command == "outline";
	if (!knownCommand(command) ||
		(args.size() > (command == "path" ? 3 : 2) && !takesMany) || (command == "status" && args.size() != 1))
	{
		throw UsageError(
			"Usage: st <find|status|refs|callers|callees|impls|supers|path|impact|deps|filter|symbols-in|at|outline> [symbol|file[:line]] [target] "
			"[--project P] [--limit N] [--depth N] [--kind K] [--edges L] [--group file|package] [--in] [--package P] [--annotated A] "
			"[--file G] [--not] [--no-source] [--no-generated] [--no-refresh]; see --help");
	}
	st::OutputOptions options;
	options.limit = number(parser, "limit", options.limit);
	options.showSource = !parser.isSet("no-source");
	options.hideGenerated = parser.isSet("no-generated");
	options.cwd = FilePath(request.cwd.toStdWString());
	options.err = &io.err;
	const size_t wait = number(parser, "wait", 30000);
	const size_t depth = number(parser, "depth", command == "path" ? 6 : command == "impact" ? 0 : 1);
	const NodeKindMask kinds = kindMask(parser);
	for (const QString& name: {"package", "annotated", "file", "not"})
	{
		if (command != "filter" && parser.isSet(name))
		{
			throw UsageError("--" + name.toStdString() + " applies to filter only");
		}
	}
	if (command != "deps" && parser.isSet("in"))
	{
		throw UsageError("--in applies to deps only");
	}
	if (command != "impact" && parser.isSet("group"))
	{
		throw UsageError("--group applies to impact only");
	}
	if (command != "path" && command != "impact" && parser.isSet("edges"))
	{
		throw UsageError("--edges applies to path and impact only");
	}
	if ((isFiles || command == "outline") && parser.isSet("kind"))
	{
		throw UsageError("--kind does not apply to " + command.toStdString() + "; pipe into st filter --kind");
	}
	Edge::TypeMask edges = command == "path" ? st::PATH_EDGES : st::IMPACT_EDGES;
	if (parser.isSet("edges"))
	{
		try
		{
			edges = st::parseEdges(parser.value("edges").toStdWString());
		}
		catch (const std::invalid_argument& error)
		{
			throw UsageError(error.what());
		}
	}
	st::Group group = st::Group::None;
	if (parser.isSet("group"))
	{
		const QString value = parser.value("group");
		if (value != "file" && value != "package")
		{
			throw UsageError("Unknown --group '" + value.toStdString() + "': expected file or package");
		}
		group = value == "file" ? st::Group::File : st::Group::Package;
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
		if (takesMany && named >= 1)
		{
			for (int i = 1; i < args.size(); i++)
			{
				names.push_back(args[i].toStdWString());
			}
		}
		else if (named == (command == "path" ? 2 : 1))
		{
			names.push_back(args[1].toStdWString());
			inputLines.push_back({names.back(), utility::encodeToUtf8(names.back())});
		}
		else if (command == "path" && named == 0)
		{
			throw UsageError("path needs a target: st path <from> <to>, or <keys> | st path <to>");
		}
		else if (takesStdin(command, named) && !request.stdinIsTerminal)
		{
			size_t truncated = 0;
			inputLines = st::readLines(io.in, &truncated);
			for (const st::InputLine& line: inputLines)
			{
				if (std::find(names.begin(), names.end(), line.key) == names.end())
				{
					names.push_back(line.key);
				}
			}
			if (truncated)
			{
				io.err << "st: input truncated upstream (+" << truncated << ", raise --limit)\n";
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

	const FilePath projectFile = locateProject(io, parser, request.cwd);
	ProjectSettings settings;
	if (!settings.load(projectFile, true))
	{
		throw UsageError("Unable to load project settings");
	}
	const QString project = QString::fromStdWString(projectFile.wstr());
	options.project = QFileInfo(project).completeBaseName().toStdString();

	// Never hold the lock while Sourcetrail refreshes: the indexer takes it without waiting.
	const QString lockFile = st::projectLockFile(projectFile);
	TimeStamp indexedAt;
	st::Freshness freshness;
	if (const int code = inspect(io, settings, lockFile, wait, &indexedAt, &freshness))
	{
		return code;
	}
	if (!freshness.reason.empty())
	{
		io.err << "st: " << freshness.reason << '\n';
	}
	if (freshness.stale && command != "status")
	{
		if (const int code = refreshIfStale(io, settings, project, lockFile, wait, parser.isSet("no-refresh"), &indexedAt, &freshness))
		{
			return code;
		}
	}

	const auto lock = lockProject(io, lockFile, wait);
	if (!lock)
	{
		return st::EXIT_STALE;
	}
	if (const int code = checkIndex(io, settings))
	{
		return code;
	}
	PersistentStorage& storage = cache.storage(settings);
	options.gapInputs = &cache.gapInputs();
	const st::LombokRoots lombok = cache.lombok(settings);
	if (!lombok.delombokToOriginalRoots.empty())
	{
		options.mapLocation = [&lombok](const st::ResultLine& line) {
			return st::toOriginal(line, lombok, st::readLinesFromDisk);
		};
	}
	if (storage.isEmpty() || storage.isIncompatible())
	{
		io.err << "st: index is empty or incompatible; reindex with --full\n";
		return st::EXIT_NO_INDEX;
	}
	if (command == "status")
	{
		return status(io, storage, cache.unresolved(storage), freshness, request.served || (!request.serverDeclined && st::serverRunning(projectFile)));
	}
	if (command == "find")
	{
		return find(io, storage, names, kinds, options);
	}
	if (isFiles)
	{
		return inFiles(io, storage, command, names, lombok, options);
	}
	if (command == "outline")
	{
		return outlineInputs(io, cache, storage, names, lombok, options);
	}
	if (command == "filter")
	{
		return filter(io, storage, inputLines, filterOptions, options);
	}
	std::vector<Id> ids;
	if (const int code = resolve(io, cache, storage, names, kinds, options, &ids))
	{
		return code;
	}
	if (command == "impact")
	{
		return impact(io, storage, ids, depth, edges, group, lombok, options);
	}
	if (command == "deps")
	{
		return deps(io, storage, ids, parser.isSet("in") ? st::Direction::Incoming : st::Direction::Outgoing, options);
	}
	if (command != "path")
	{
		return graph(io, storage, command, ids, depth, options);
	}
	std::vector<Id> targetIds;
	if (const int code = resolve(io, cache, storage, targets, ~0, options, &targetIds))
	{
		return code;
	}
	return path(io, storage, ids, targetIds, depth, edges, options);
}
}

namespace st
{
Route route(const Request& request)
{
	QCommandLineParser parser;
	addOptions(parser);
	Route result;
	if (!parser.parse(request.arguments))
	{
		return result;
	}
	const QStringList args = parser.positionalArguments();
	result.command = args.isEmpty() ? QString() : args[0];
	result.noServer = parser.isSet("no-server");
	result.readsStdin = !parser.isSet("help") && takesStdin(result.command, args.size() - 1);
	result.project = parser.isSet("project") ? givenProject(parser, request.cwd)
											 : st::findProject(FilePath(request.cwd.toStdWString()), registryFile());
	return result;
}

int runCommand(const Request& request, Io io, IndexCache& cache)
{
	try
	{
		return runImpl(request, io, cache);
	}
	catch (const UsageError& error)
	{
		io.err << "st: " << error.what() << '\n';
		return EXIT_USAGE;
	}
	catch (CppSQLite3Exception& error)
	{
		io.err << "st: " << error.errorMessage() << '\n';
	}
	catch (const std::exception& error)
	{
		io.err << "st: " << error.what() << '\n';
	}
	return EXIT_INTERNAL;
}
}
