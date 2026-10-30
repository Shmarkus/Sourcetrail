#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QProcess>

#include <iostream>
#include <limits>
#include <stdexcept>

#include "CppSQLite3.h"
#include "PersistentStorage.h"
#include "ProjectSettings.h"
#include "Query.h"

namespace
{
void output(const QJsonObject& object)
{
	std::cout << QJsonDocument(object).toJson(QJsonDocument::Compact).constData() << std::endl;
}

int number(const QCommandLineParser& parser, const QString& name, int max)
{
	bool valid = false;
	const int result = parser.value(name).toInt(&valid);
	if (!valid || result < 1 || result > max)
	{
		throw std::runtime_error(("Invalid --" + name + ": expected 1.." + QString::number(max)).toStdString());
	}
	return result;
}

Id symbolId(const QString& text)
{
	bool valid = false;
	const qulonglong result = text.toULongLong(&valid);
	if (!valid || !result || result > std::numeric_limits<Id>::max())
	{
		throw std::runtime_error("Symbol id must be a positive integer returned by search");
	}
	return static_cast<Id>(result);
}

void reindex(const QString& project, bool full, int timeout)
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
		if (timer.elapsed() > timeout)
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

int main(int argc, char** argv)
{
	QCoreApplication app(argc, argv);
	QCoreApplication::setApplicationName("Sourcetrail_cli");
	QCommandLineParser parser;
	parser.setApplicationDescription("Headless frontend to Sourcetrail's shared indexing and storage APIs. JSON on stdout, diagnostics on stderr.");
	parser.addHelpOption();
	parser.addPositionalArgument("command", "index | status | search | describe | references | read-source");
	parser.addPositionalArgument("project", "Existing .srctrlprj project");
	parser.addPositionalArgument("value", "Search text, symbol id or indexed source path", "[value]");
	parser.addOption({"full", "Reindex the whole project instead of changed files"});
	parser.addOption({"limit", "Maximum results/locations (1..1000)", "count", "50"});
	parser.addOption({"kind", "Filter references, e.g. CALL, USAGE, MEMBER", "kind"});
	parser.addOption({"start-line", "First source line (1-based)", "line", "1"});
	parser.addOption({"end-line", "Last source line (maximum 1000 lines per request)", "line"});
	parser.addOption({"timeout", "Indexing timeout in milliseconds", "ms", "600000"});
	try
	{
		if (!parser.parse(app.arguments()))
		{
			throw std::runtime_error(parser.errorText().toStdString());
		}
		if (parser.isSet("help") || parser.isSet("help-all"))
		{
			std::cout << parser.helpText().toStdString();
			return 0;
		}
		const auto args = parser.positionalArguments();
		const QStringList commands{"index", "status", "search", "describe", "references", "read-source"};
		if (args.size() < 2 || !commands.contains(args[0]) ||
			args.size() != ((args[0] == "index" || args[0] == "status") ? 2 : 3))
		{
			throw std::runtime_error("Usage: Sourcetrail_cli <command> <project.srctrlprj> [value]; see --help");
		}
		const QFileInfo file(args[1]);
		if (!file.isFile() || file.suffix() != "srctrlprj")
		{
			throw std::runtime_error("Project must be an existing .srctrlprj file");
		}
		const QString project = file.canonicalFilePath();
		const int limit = number(parser, "limit", 1000);
		ProjectSettings settings;
		if (!settings.load(FilePath(project.toStdWString()), true))
		{
			throw std::runtime_error("Unable to load project settings");
		}
		if (args[0] == "index")
		{
			reindex(project, parser.isSet("full"), number(parser, "timeout", std::numeric_limits<int>::max()));
		}
		QLockFile lock(project + ".lock");
		lock.setStaleLockTime(0);
		if (!lock.tryLock())
		{
			throw std::runtime_error("Project is busy indexing or being queried; retry after completion");
		}
		if (settings.getTempDBFilePath().exists())
		{
			throw std::runtime_error("An unfinished index exists; recover/reindex the project before querying");
		}
		if (!settings.getDBFilePath().exists())
		{
			throw std::runtime_error("Project has no index; run the index command first");
		}
		PersistentStorage storage(settings.getDBFilePath(), settings.getBookmarkDBFilePath(), true);
		if (storage.isEmpty() || storage.isIncompatible())
		{
			throw std::runtime_error("Index is empty or incompatible; reindex with --full");
		}
		storage.buildCaches();
		QJsonObject result;
		if (args[0] == "index" || args[0] == "status") result = cli::status(storage);
		else if (args[0] == "search") result = cli::search(storage, args[2], limit);
		else if (args[0] == "describe") result = cli::describe(storage, symbolId(args[2]), limit);
		else if (args[0] == "references") result = cli::references(storage, symbolId(args[2]), parser.value("kind").toUpper(), limit);
		else
		{
			const int start = number(parser, "start-line", std::numeric_limits<int>::max() - 1000);
			const int end = parser.isSet("end-line") ? number(parser, "end-line", std::numeric_limits<int>::max()) : start + 199;
			if (end < start || end - start >= 1000) throw std::runtime_error("Request a range of 1..1000 source lines");
			result = cli::readSource(storage, args[2], start, end);
		}
		const bool ok = args[0] != "index" || result["errors"].toDouble() == 0;
		QJsonObject response{{"ok", ok}, {"command", args[0]}, {"project", project}, {"result", result}};
		if (!ok) response["error"] = "Indexing completed with parse errors; inspect stderr and status";
		output(response);
		return ok ? 0 : 1;
	}
	catch (CppSQLite3Exception& error)
	{
		output({{"ok", false}, {"error", QString::fromUtf8(error.errorMessage())}});
	}
	catch (const std::exception& error)
	{
		output({{"ok", false}, {"error", QString::fromUtf8(error.what())}});
	}
	return 1;
}
