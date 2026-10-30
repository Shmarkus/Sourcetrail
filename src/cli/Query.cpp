#include "Query.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>

#include <iostream>
#include <stdexcept>

#include "ErrorCountInfo.h"
#include "FilePath.h"
#include "NameHierarchy.h"
#include "NodeTypeSet.h"
#include "PersistentStorage.h"
#include "SearchMatch.h"
#include "SourceLocation.h"
#include "SourceLocationCollection.h"
#include "StorageStats.h"
#include "TextAccess.h"
#include "TooltipInfo.h"
#include "TooltipOrigin.h"

namespace cli
{
QJsonObject symbol(PersistentStorage& storage, Id id)
{
	const auto name = storage.getNameHierarchyForNodeId(id);
	if (name.getQualifiedName().empty())
	{
		throw std::runtime_error("Symbol not found in this index");
	}
	return {{"id", QString::number(id)},
		{"name", QString::fromStdWString(name.getQualifiedName())},
		{"signature", QString::fromStdWString(name.getQualifiedNameWithSignature())},
		{"kind", QString::fromStdString(storage.getNodeTypeForNodeWithId(id).getReadableTypeString())}};
}

QJsonObject search(PersistentStorage& storage, const QString& query, int limit)
{
	if (query.trimmed().isEmpty())
	{
		throw std::runtime_error("Search query must not be empty");
	}
	QJsonArray results;
	bool truncated = false;
	for (const auto& match : storage.getAutocompletionSymbolMatches(
		query.toStdWString(), NodeTypeSet::all(), limit + 1, 0))
	{
		for (Id id : match.tokenIds)
		{
			if (results.size() == limit)
			{
				truncated = true;
				break;
			}
			results.append(symbol(storage, id));
		}
	}
	return {{"symbols", results}, {"truncated", truncated}};
}

QJsonArray locations(PersistentStorage& storage, Id id, int limit)
{
	QJsonArray results;
	storage.getSourceLocationsForTokenIds({id})->forEachSourceLocation([&](SourceLocation* location) {
		if (!location->isStartLocation() || results.size() >= limit)
		{
			return;
		}
		const auto* end = location->getEndLocation();
		results.append(QJsonObject{
			{"file", QString::fromStdWString(location->getFilePath().wstr())},
			{"start_line", static_cast<int>(location->getLineNumber())},
			{"start_column", static_cast<int>(location->getColumnNumber())},
			{"end_line", static_cast<int>(end ? end->getLineNumber() : location->getLineNumber())},
			{"end_column", static_cast<int>(end ? end->getColumnNumber() : location->getColumnNumber())},
			{"scope", location->isScopeLocation()}});
	});
	return results;
}

QJsonObject describe(PersistentStorage& storage, Id id, int limit)
{
	QJsonObject result = symbol(storage, id);
	result["locations"] = locations(storage, id, limit);
	const auto tooltip = storage.getTooltipInfoForTokenIds({id}, TOOLTIP_ORIGIN_CODE);
	if (!tooltip.snippets.empty())
	{
		result["signature"] = QString::fromStdWString(tooltip.snippets.front().code).trimmed();
	}
	QJsonObject definition;
	for (const auto location : result["locations"].toArray())
	{
		const auto candidate = location.toObject();
		if (definition.isEmpty() || candidate["scope"].toBool()) definition = candidate;
		if (candidate["scope"].toBool()) break;
	}
	if (!definition.isEmpty())
	{
		const int start = definition["start_line"].toInt();
		const int end = definition["end_line"].toInt();
		const auto source = readSource(storage, definition["file"].toString(), start, std::min(end, start + 199));
		definition["content"] = source["content"];
		definition["source"] = source["source"];
		definition["truncated"] = end - start >= 200;
		result["definition"] = definition;
	}
	else result["definition"] = QJsonValue::Null;
	return result;
}

QJsonObject readSource(PersistentStorage& storage, const QString& path, int start, int end)
{
	const FilePath file(path.toStdWString());
	if (!storage.getNodeIdForFileNode(file))
	{
		throw std::runtime_error("File is not present in this index; use a path returned by describe/references");
	}
	const auto content = storage.getFileContent(file, false);
	const int count = static_cast<int>(content->getLineCount());
	if (start > count)
	{
		throw std::runtime_error("Start line exceeds indexed file length");
	}
	const int stop = std::min(end, count);
	QString text;
	for (int line = start; line <= stop; ++line)
	{
		text += QString::fromStdString(content->getLine(line));
	}
	return {{"file", path}, {"start_line", start}, {"end_line", stop},
		{"content", text}, {"source", storage.hasContentForFile(file) ? "index" : "disk"}};
}

QJsonObject status(PersistentStorage& storage)
{
	const auto stats = storage.getStorageStats();
	const auto errors = storage.getErrorCount();
	return {{"nodes", static_cast<double>(stats.nodeCount)},
		{"edges", static_cast<double>(stats.edgeCount)},
		{"files", static_cast<double>(stats.fileCount)},
		{"completed_files", static_cast<double>(stats.completedFileCount)},
		{"indexed_at", QString::fromStdString(stats.timestamp.toString())},
		{"errors", static_cast<double>(errors.total)},
		{"fatal_errors", static_cast<double>(errors.fatal)},
		{"freshness", "last_completed_index; source changes require reindexing"}};
}

void reindex(const QString& project, bool full, int timeoutMs)
{
	QString executable = QCoreApplication::applicationDirPath() + "/Sourcetrail";
#ifdef _WIN32
	executable += ".exe";
#endif
	QProcess process;
	process.setWorkingDirectory(QCoreApplication::applicationDirPath());
	process.setProcessChannelMode(QProcess::MergedChannels);
	QStringList args{"index"};
	if (full)
	{
		args << "--full";
	}
	args << project;
	process.start(executable, args);
	if (!process.waitForStarted())
	{
		throw std::runtime_error("Cannot start adjacent Sourcetrail executable: " + process.errorString().toStdString());
	}
	QElapsedTimer timer;
	timer.start();
	while (process.state() != QProcess::NotRunning)
	{
		process.waitForReadyRead(1000);
		std::cerr << process.readAll().constData() << std::flush;
		if (timer.elapsed() > timeoutMs)
		{
			process.terminate();
			if (!process.waitForFinished(5000))
			{
				process.kill();
				process.waitForFinished();
			}
			throw std::runtime_error("Indexing timed out; inspect logs before retrying");
		}
	}
	std::cerr << process.readAll().constData();
	if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
	{
		throw std::runtime_error("Indexing failed; see stderr for Gradle, delombok or indexer diagnostics");
	}
}
}
