#ifndef ST_COMMAND_H
#define ST_COMMAND_H

#include <iostream>

#include <QString>
#include <QStringList>

#include "StIndexCache.h"

namespace st
{
struct Request
{
	QStringList arguments;	// argv including argv[0]
	QString cwd;	// absolute
	bool stdinIsTerminal = true;
};

struct Io
{
	std::istream& in;
	std::ostream& out;
	std::ostream& err;
};

// Runs one st command and returns its exit code; throws nothing. Needs a QCoreApplication.
int runCommand(const Request& request, Io io, IndexCache& cache);
}

#endif
